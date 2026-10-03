#!/bin/bash
# A scripted 3-node Bubernetes scenario on loopback, with checks:
#   join -> Deploy(3) -> DaemonSet(-1) -> scale down -> edit file in/out ->
#   dump -> kill a node (+ its orphans must die) -> restart a node (state
#   files) -> delete -> everything stopped.
#
# Run from the repo root:  bash scripts/demo.sh
set -u
cd "$(dirname "$0")/.."
ROOT=$PWD
BIN=$ROOT/bin

[ -x "$BIN/bubelet" ] && [ -x "$BIN/bubectl" ] || { echo "run 'make' first"; exit 1; }
[ -x "$BIN/workload" ] || make examples >/dev/null

BASE=$(mktemp -d)
mkdir -p "$BASE/n1" "$BASE/n2" "$BASE/n3"
fail=0

# workloads exec from memfds named after the resource; count them that way
count_wl() {   # count_wl name
    local n=0
    for p in /proc/[0-9]*; do
        case "$(readlink "$p/exe" 2>/dev/null)" in */memfd:$1\ *|*/memfd:$1) n=$((n+1));; esac
    done
    echo "$n"
}
check() {   # check "description" actual expected
    if [ "$2" = "$3" ]; then echo "  ok   $1 ($2)"; else echo "  FAIL $1 (got $2, expected $3)"; fail=1; fi
}

pids=()
cleanup() { kill "${pids[@]}" 2>/dev/null; sleep 2; kill -9 "${pids[@]}" 2>/dev/null; rm -rf "$BASE"; }
trap cleanup EXIT

start_node() {   # start_node name port [seed]
    local seed=()
    [ -n "${3:-}" ] && seed=(--seed "$3")
    $BIN/bubelet --name "$1" --port "$2" --manifests "$BASE/$1" "${seed[@]}" >>"$BASE/$1.log" 2>&1 &
    echo $!
}

echo "########## starting 3 nodes ##########"
pids+=("$(start_node n1 7701)")
pids+=("$(start_node n2 7702 127.0.0.1:7701)")
pids+=("$(start_node n3 7703 127.0.0.1:7701)")
sleep 5
$BIN/bubectl --server 127.0.0.1:7701 get nodes

echo; echo "########## Deploy web replicas=3 (apply to n1) ##########"
cat > "$BASE/web.yaml" <<EOF
kind: Deploy
name: web
replicas: 3
image: $BIN/workload
argv: [--serve, "a,b", web]
EOF
$BIN/bubectl --server 127.0.0.1:7701 apply -f "$BASE/web.yaml"
sleep 7
$BIN/bubectl --server 127.0.0.1:7702 get deploy
check "web replicas running in total" "$(count_wl web)" 3
for n in n1 n2 n3; do echo "  $n started $(grep -c "started 'web'" "$BASE/$n.log") web replica(s)"; done
check "argv with a quoted comma survived" "$(cat "$BASE"/n*.log | grep -c 'args=\[--serve a,b web \]' | awk '{if ($1 > 0) print 1; else print 0}')" 1

echo; echo "########## DaemonSet agent replicas=-1 (apply to n3) ##########"
cat > "$BASE/agent.yaml" <<EOF
kind: Deploy
name: agent
replicas: -1
image: $BIN/workload
EOF
$BIN/bubectl --server 127.0.0.1:7703 apply -f "$BASE/agent.yaml"
sleep 6
check "agent replicas (one per node)" "$(count_wl agent)" 3

echo; echo "########## scale web down to 1 ##########"
sed -i 's/replicas: 3/replicas: 1/' "$BASE/web.yaml"
$BIN/bubectl --server 127.0.0.1:7702 apply -f "$BASE/web.yaml"
sleep 7
check "web replicas after scale-down (SIGTERM really stops them)" "$(count_wl web)" 1

echo; echo "########## edit file dropped into n1's manifest dir ##########"
cp "$BIN/workload" "$BASE/n1/logger.bin"
cat > "$BASE/n1/logger.yaml" <<EOF
kind: Static
name: logger
image: logger.bin        # relative to the manifest directory
EOF
sleep 6
check "static 'logger' running on n1 only" "$(count_wl logger)" 1
check "n1 persisted a state file for it" "$(ls "$BASE/n1" | grep -c 'logger.state.yaml')" 1
check "n2 learned it too (state file via gossip)" "$(ls "$BASE/n2" | grep -c 'logger.state.yaml')" 1
echo "  operator file untouched: $(head -c 60 "$BASE/n1/logger.yaml" | tr '\n' ' ')"
rm "$BASE/n1/logger.yaml"
sleep 7
check "removing the file retracted it everywhere" "$(count_wl logger)" 0
check "n2's state file gone too" "$(ls "$BASE/n2" | grep -c 'logger.state.yaml')" 0

echo; echo "########## dump (asked n3) ##########"
$BIN/bubectl --server 127.0.0.1:7703 dump

echo; echo "########## kill -9 n2: orphans must die, web must stay at 1 ##########"
kill -9 "${pids[1]}" 2>/dev/null
sleep 12
$BIN/bubectl --server 127.0.0.1:7701 get nodes
check "agent replicas after n2 died (its own got PDEATHSIG)" "$(count_wl agent)" 2
check "web still exactly 1" "$(count_wl web)" 1

echo; echo "########## restart n3: state files restore it at the same version ##########"
before=$($BIN/bubectl --server 127.0.0.1:7703 get deploy | awk '/^agent/{print $NF}')
kill "${pids[2]}"; sleep 3
pids[2]=$(start_node n3 7703 127.0.0.1:7701)
sleep 8
after=$($BIN/bubectl --server 127.0.0.1:7703 get deploy | awk '/^agent/{print $NF}')
check "agent version unchanged across n3 restart" "$after" "$before"
check "n3 runs its agent again" "$(count_wl agent)" 2

echo; echo "########## delete web (tombstone across cluster) ##########"
$BIN/bubectl --server 127.0.0.1:7701 delete web
sleep 6
$BIN/bubectl --server 127.0.0.1:7703 get deploy
check "web processes gone" "$(count_wl web)" 0
check "web state file gone on n1" "$(ls "$BASE/n1" | grep -c 'web.state.yaml')" 0

echo; echo "########## stop the cluster: nothing may outlive it ##########"
kill "${pids[0]}" "${pids[2]}" 2>/dev/null
sleep 4
check "no workloads left after shutdown" "$(( $(count_wl web) + $(count_wl agent) + $(count_wl logger) ))" 0

echo
if [ $fail = 0 ]; then echo "########## ALL CHECKS PASSED ##########"; else echo "########## SOME CHECKS FAILED ##########"; fi
exit $fail
