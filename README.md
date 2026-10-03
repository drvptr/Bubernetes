# Bubernetes
---
![logo](./logo.png)
---
> **Bare Metal Kubernetes** — a peer-to-peer distributed process orchestrator written in pure C.

## Overview

Bubernetes is an experimental research project exploring what a distributed process orchestrator could look like **without containers, without a centralized control plane, and without dozens of cooperating services**. Instead of orchestrating containers, Bubernetes orchestrates **native Linux processes** across a cluster of machines. The project is intentionally minimal and focuses on understanding distributed systems, Linux internals, scheduling, process management, and cluster coordination.

A typical Kubernetes cluster consists of multiple independent components:  API Server, Scheduler, Controller Manager, kubelet, kube-proxy, etcd, Container Runtime. Bubernetes takes a completely different approach. The entire cluster is built around a **single monolithic daemon** called `bubelet`. Every node runs exactly the same executable. There are no dedicated control-plane nodes and no worker-only nodes. Every machine is simultaneously responsible for cluster coordination and workload execution.

Bubernetes is designed as a **peer-to-peer cluster**. Each node participates in cluster coordination, stores cluster state **in memory** (without etcd or other databases), schedules workloads, executes processes, exchanges state with other peers. Instead of separating "control" and "worker" nodes, the cluster forms a distributed quorum where every node has equal responsibility. The failure of a single node should not stop the cluster from operating.

Bubernetes definetly is **not** Kubernetes. The command syntax are intentionally inspired by Kubernetes because its CLI/API model is convenient. Architecturally, Bubernetes is some like to a combination of Kubernetes, Nomad, HashiCorp Serf, Nefele, Fleet. 

Bubernetes is configured using YAML manifests. `bubelet` continuously watches a manifest directory, similar to how Kubernetes watches *static Pod* manifests. Unlike Kubernetes static Pods, however, the behavior is defined by the manifest itself.
For example, a manifest may describre as a single local daemon and as a replicated deployment. The orchestrator determines how many instances should exist and where they should run.

Bubernetes does **not** manage containers. Instead, it manages **native Linux processes**. Applications are expected to be compiled as static executables.

Instead of copying binaries to the filesystem, the design is to transfer statically linked executable images directly between peers, create an anonymous in-memory file using Linux `memfd_create()`, and execute it via `fexecve()`. This allows workloads to be started entirely from memory without leaving artifacts on disk. When multiple replicas of the same workload are required, Bubernetes does not repeatedly load the executable image. Instead, it launches a single executable and creates additional replicas using `fork()`. Since Linux shares read-only executable (.text) pages between parent and child processes using copy-on-write semantics, all replicas execute the same code without duplicating it in physical memory. This makes process replication lightweight while allowing each replica to maintain its own independent address space and runtime state. 

One of the key goals of Bubernetes is to separate process orchestration from the execution environment. Bubernetes manages what processes should run, how many instances should exist, and where they should be placed, while the underlying environment is left completely outside of the orchestrator's responsibility. Unlike Kubernetes, Bubernetes **does not require** a cluster-wide container network, CNI, Pod network, or other networking layer between workloads. Nodes are independent environments that communicate directly with each other through the TCP. A node may therefore be bare metal, a virtual machine, a container, or any other Linux environment capable of running bubelet. This makes the same orchestration layer usable across fundamentally different environments: for example, ten isolated containers can each run bubelet and form a cluster, after which a DaemonSet-like workload can be distributed across all of them. Isolation is therefore something the environment provides, while orchestration remains the responsibility of Bubernetes. Bubernetes does not orchestrate containers. It orchestrates processes across heterogeneous execution environments.

Bubernetes is designed around a flat cluster topology where the cluster itself is the smallest logical unit of topology. Nodes are equal peers rather than members of a hierarchy, and a node is identified by a logical name and a current network address. The name represents the node's identity within the cluster, while the address is only a way to reach it and may change without changing the node's logical identity. This also means that a physical or virtual machine can be replaced without necessarily replacing the logical node: as long as the new machine assumes the same node identity and has valid credentials, the cluster can continue treating it as the same node.

Bubernetes does not require a built-in hierarchy of masters, workers, regions, zones, or sub-clusters. Higher-level topology can instead be implemented externally when needed. For example, several independent Bubernetes clusters could be connected through an external load balancer or another routing mechanism and treated as larger logical units. This makes the cluster itself a composable building block, allowing a cluster of clusters to be constructed without introducing topology-specific concepts into the core orchestration model.

ConfigMaps are not a Bubernetes object. Ansible can configure the node, install packages and libraries, distribute configuration files, create users and directories, install certificates, and prepare the environment before `bubelet` starts.

Containerization is not required either. A container environment can be prepared separately with `runc run`, with its network connected to the cluster through an L2 bridge without NAT. Routing between networks can also be handled independently, for example with OSPF. From Bubernetes' point of view, the resulting container is still just a IP. The same workload can run directly on a physical machine, inside a VM, inside a container, on a BSD systems.

At the same time, there is no fundamental reason why `bubelet` could not create such an environment itself. Its **API is essentially a remote interface for performing syscalls**. Since a process is already created through `execve()`, the same interface can be extended with operations such as `ulimit`/`setrlimit()`, `chroot()`, namespaces, signals, priorities, environment variables, and other process properties. A limited set of typed operations is preferable to exposing arbitrary syscalls directly. It means - you can set analog of `resource limit` by `ulimit` directly!

Traffic balancing is **deliberately** outside of Bubernetes. Bubernetes balances workload placement, not user requests. If several nginx instances run on different nodes, HAProxy can balance traffic between them and perform *health checks* independently of the orchestrator.

Monitoring follows the same principle. Prometheus can collect metrics from nodes and workloads, while Grafana and Alertmanager can provide visualization and alerting. Bubernetes only needs to expose the state required by these tools.

The idea is to keep the orchestration layer small and compose it with existing Unix-like tools instead of implementing configuration management. **We can deploy the real KISS unix-way infrastructure with: bubernetes, ansible, runc, haproxy, keepalived, GeoDNS, Prometheus **

---

## Building

One static binary per command; no runtime dependencies, exactly like the
workloads it runs.

```sh
make                 # bubelet + bubectl, static, cleartext transport
make examples        # the sample static workload (examples/workload.c)
make TLS=1           # node-to-node traffic over static mutual-auth TLS (OpenSSL)
```

Everything lands in `bin/`. The only external dependency is libc (and OpenSSL
when `TLS=1`), both linked statically.

## Running

Two hosts, `10.0.0.7` and `10.0.0.161`, no shared storage, no container
runtime, no cluster network. One of them is where you work.

**1. Build nginx statically** (on the working host). Bubernetes ships the
executable's bytes between nodes and runs them from memory, so the binary
must carry everything it needs:

```sh
apt install build-essential libpcre2-dev zlib1g-dev git
git clone https://github.com/nginx/nginx && cd nginx
./auto/configure --with-cc-opt="-static" --with-ld-opt="-static"
make
file objs/nginx          # must say: statically linked
install objs/nginx /usr/local/bin/nginx
```

**2. Give nginx an environment on each host.** The orchestrator runs the
process; what the process reads is the environment's job. nginx wants a config
and a place for logs, and here each host also gets its own `index.html` so you
can see which one answered:

```sh
mkdir -p /usr/local/nginx/conf /usr/local/nginx/logs
cat > /usr/local/nginx/conf/nginx.conf <<'EOF'
worker_processes 1;
events { worker_connections 1024; }
http {
    default_type text/html;
    server {
        listen 80;
        location / { root /; index index.html; }
    }
}
EOF
echo "Hello from $(hostname)" > /index.html

ssh root@10.0.0.161 'mkdir -p /usr/local/nginx/conf /usr/local/nginx/logs; echo "Hello from $(hostname)" > /index.html'
scp /usr/local/nginx/conf/nginx.conf root@10.0.0.161:/usr/local/nginx/conf/
```

Nothing about nginx itself is copied to the second host.

**3. Install Bubernetes on both hosts:**

```sh
git clone https://github.com/drvptr/Bubernetes && cd Bubernetes
make && make install
scp bin/bubelet bin/bubectl root@10.0.0.161:/usr/local/bin/
```

**4. Start a node on each host.** `--bind` is both where a node listens and
the address it tells the others to reach it at, so it must be the host's
routable address. One port carries both the UDP membership protocol and the
TCP wire; open both on any firewall between the hosts. The node's name
defaults to its hostname.

```sh
bubelet --bind 10.0.0.7 &
ssh root@10.0.0.161 'bubelet --bind 10.0.0.161 --seed 10.0.0.7:7700 &'
```

That `--seed` is the whole join procedure. No TLS is needed for it; TLS is a
separate, optional layer on the TCP wire (see below).

**5. Point bubectl at any node** — every node holds the whole desired state:

```sh
mkdir -p ~/.bube && echo 'server: 10.0.0.7' > ~/.bube/config
bubectl get nodes
NAME             KIND     REPL  STATUS       RUNNING  NODE/ORIGIN
node1            Node     -     Ready        -        10.0.0.161:7700
node2            Node     -     Ready        -        10.0.0.7:7700
```

**6. Deploy.** `replicas: -1` means one on every node, now and whenever a node
joins. The `argv` matters — see the next section.

```sh
cat > web.yaml <<'EOF'
kind: Deploy
name: web
replicas: -1
image: /usr/local/bin/nginx
argv: [-g, "daemon off;"]
EOF

bubectl apply -f web.yaml
applied Deploy 'web' (replicas=-1)

bubectl get
NAME             KIND     REPL  STATUS       RUNNING  NODE/ORIGIN
web              Deploy   all   Ready        1        @node2 v2

curl http://10.0.0.7/      # Hello from node2
curl http://10.0.0.161/    # Hello from node1
```

What happened: `bubectl` read `/usr/local/bin/nginx`, sent its bytes to
`10.0.0.7` with the spec, and from then on the cluster knows the image only by
its sha256. `10.0.0.161` learned the spec by gossip, had no image with that
hash, fetched the bytes from its peer, wrote them into an anonymous memory file
and `fexecve`'d it. nginx on the second host never touched its disk — delete
`/usr/local/bin/nginx` there and re-apply; it still serves.

Scale it, retract it:

```sh
sed -i 's/replicas: -1/replicas: 1/' web.yaml && bubectl apply -f web.yaml   # one copy, placed by hash
bubectl dump > all.yaml       # the whole cluster's desired state, with versions
bubectl delete web            # stopped on every node, tombstoned so it stays gone
```

Instead of `bubectl apply`, a manifest can simply be placed in a node's
`/etc/bubernetes/manifests/` (and removed to retract it); that directory *is*
the storage.

## Conclusion

Bubernetes supervises the process it started. A daemon that forks and lets its
original process exit looks like a workload that died a few milliseconds after
starting: it gets restarted, the restart fails to bind the port the real daemon
already holds, and the resource shows `Unhealthy` with restarts backing off:

```
started 'web' replica 0 pid 12921
nginx: [emerg] bind() to 0.0.0.0:80 failed (98: Address already in use)
pid 12921 exited after 31ms (exit 0), crash 1; next start in 2s
```

That is the manifest without `argv: [-g, "daemon off;"]`. Every daemon has
such a switch; the rule is the same as `systemd`'s `Type=simple`: the process
the supervisor started is the process it supervise

---

## TLS

```sh
make TLS=1
examples/gen-certs.sh certs node1 node2      # a CA and a cert per node name
bubelet --name node2 --bind 10.0.0.7   --certs certs/node2
bubelet --name node1 --bind 10.0.0.161 --certs certs/node1 --seed 10.0.0.7:7700
bubectl --certs certs/node2 get nodes
```

Mutual authentication on the TCP wire: a node (or `bubectl`) without a cert
signed by the cluster CA is refused. All-or-nothing per cluster. Built plain,
the wire is cleartext and anyone who can reach a node's port can define
workloads — use plain clusters on trusted networks only.

