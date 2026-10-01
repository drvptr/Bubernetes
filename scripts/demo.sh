#!/bin/bash
# A scripted 3-node Bubernetes scenario on loopback:
#   join -> Deploy(3) -> DaemonSet(-1) -> dump -> kill a node + reschedule -> delete
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

pids=()
cleanup() { kill "${pids[@]}" 2>/dev/null; sleep 1; kill -9 "${pids[@]}" 2>/dev/null; rm -rf "$BASE"; }
trap cleanup EXIT

echo "########## starting 3 nodes ##########"
$BIN/bubelet --name n1 --port 7701 --manifests "$BASE/n1" >"$BASE/n1.log" 2>&1 & pids+=($!)
$BIN/bubelet --name n2 --port 7702 --manifests "$BASE/n2" --seed 127.0.0.1:7701 >"$BASE/n2.log" 2>&1 & pids+=($!)
$BIN/bubelet --name n3 --port 7703 --manifests "$BASE/n3" --seed 127.0.0.1:7701 >"$BASE/n3.log" 2>&1 & pids+=($!)
sleep 5

echo "########## membership (asked n1) ##########"
$BIN/bubectl --server 127.0.0.1:7701 get nodes

echo; echo "########## apply Deploy web replicas=3 (to n1) ##########"
cat > "$BASE/web.yaml" <<EOF
kind: Deploy
name: web
replicas: 3
image: $BIN/workload
argv: [--serve, web]
EOF
$BIN/bubectl --server 127.0.0.1:7701 apply -f "$BASE/web.yaml"
sleep 7
echo "########## get deploy (asked n2 - state spread by gossip) ##########"
$BIN/bubectl --server 127.0.0.1:7702 get deploy
echo "replicas per node (leaderless HRW placement):"
for n in n1 n2 n3; do echo "  $n: $(grep -c "started 'web'" "$BASE/$n.log")"; done

echo; echo "########## apply DaemonSet replicas=-1 (to n3) ##########"
cat > "$BASE/agent.yaml" <<EOF
kind: Deploy
name: agent
replicas: -1
image: $BIN/workload
argv: [--agent]
EOF
$BIN/bubectl --server 127.0.0.1:7703 apply -f "$BASE/agent.yaml"
sleep 7
for n in n1 n2 n3; do echo "  $n agent replicas: $(grep -c "started 'agent'" "$BASE/$n.log") (expect 1)"; done

echo; echo "########## bubectl dump (asked n3) ##########"
$BIN/bubectl --server 127.0.0.1:7703 dump

echo; echo "########## kill n2 - its web replica must be rescheduled ##########"
kill -9 "${pids[1]}" 2>/dev/null
sleep 12
echo "nodes as seen by n1:"
$BIN/bubectl --server 127.0.0.1:7701 get nodes
echo "web starts per survivor (one should have gained a replica):"
for n in n1 n3; do echo "  $n: $(grep -c "started 'web'" "$BASE/$n.log")"; done

echo; echo "########## delete web (tombstone across cluster) ##########"
$BIN/bubectl --server 127.0.0.1:7701 delete web
sleep 5
echo "get deploy (n3) - web gone, agent remains:"
$BIN/bubectl --server 127.0.0.1:7703 get deploy
echo "manifest files left on each node (the only storage):"
for n in n1 n3; do echo "  $n: $(ls "$BASE/$n")"; done

echo; echo "########## done ##########"
