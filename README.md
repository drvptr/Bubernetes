# Bubernetes
---
![logo](./logo.png)
---
> **Bare Metal Kubernetes** — a peer-to-peer distributed process orchestrator written in pure C.

## Overview

Bubernetes is an experimental research project exploring what a distributed process orchestrator could look like **without containers, without a centralized control plane, and without dozens of cooperating services**. Instead of orchestrating containers, Bubernetes orchestrates **native Linux processes** across a cluster of machines. The project is intentionally minimal and focuses on understanding distributed systems, Linux internals, scheduling, process management, and cluster coordination.

A typical Kubernetes cluster consists of multiple independent components:  API Server, Scheduler, Controller Manager, kubelet, kube-proxy, etcd, Container Runtime. Bubernetes takes a completely different approach. The entire cluster is built around a **single monolithic daemon** called `bubelet`. Every node runs exactly the same executable. There are no dedicated control-plane nodes and no worker-only nodes. Every machine is simultaneously responsible for cluster coordination and workload execution.

Bubernetes is designed as a **peer-to-peer cluster**. Each node participates in cluster coordination, stores cluster state **in memory** (without etcd or other databases), schedules workloads, executes processes, exchanges state with other peers. Instead of separating "control" and "worker" nodes, the cluster forms a distributed quorum where every node has equal responsibility. The failure of a single node should not stop the cluster from operating.

The only required component is: `bubelet`. It combines responsibilities similar to: Kubernetes API Server, Scheduler, kubelet, Controller Manager, basic load balancing. 

Bubernetes is configured using YAML manifests. `bubelet` continuously watches a manifest directory, similar to how Kubernetes watches *static Pod* manifests. Unlike Kubernetes static Pods, however, the behavior is defined by the manifest itself.
For example, a manifest may describre as a single local daemon and as a replicated deployment. The orchestrator determines how many instances should exist and where they should run.

Bubernetes does **not** manage containers. Instead, it manages **native Linux processes**. Applications are expected to be compiled as static executables.

Instead of copying binaries to the filesystem, the design is to transfer statically linked executable images directly between peers, create an anonymous in-memory file using Linux `memfd_create()`, and execute it via `fexecve()`. This allows workloads to be started entirely from memory without leaving artifacts on disk. When multiple replicas of the same workload are required, Bubernetes does not repeatedly load the executable image. Instead, it launches a single executable and creates additional replicas using `fork()`. Since Linux shares read-only executable (.text) pages between parent and child processes using copy-on-write semantics, all replicas execute the same code without duplicating it in physical memory. This makes process replication lightweight while allowing each replica to maintain its own independent address space and runtime state. 

One of the key goals of Bubernetes is to separate process orchestration from the execution environment. Bubernetes manages what processes should run, how many instances should exist, and where they should be placed, while the underlying environment is left completely outside of the orchestrator's responsibility. Unlike Kubernetes, Bubernetes **does not require** a cluster-wide container network, CNI, Pod network, or other networking layer between workloads. Nodes are independent environments that communicate directly with each other through the TCP. A node may therefore be bare metal, a virtual machine, a container, or any other Linux environment capable of running bubelet. This makes the same orchestration layer usable across fundamentally different environments: for example, ten isolated containers can each run bubelet and form a cluster, after which a DaemonSet-like workload can be distributed across all of them. Isolation is therefore something the environment provides, while orchestration remains the responsibility of Bubernetes. Bubernetes does not orchestrate containers. It orchestrates processes across heterogeneous execution environments.

Bubernetes is designed around a flat cluster topology where the cluster itself is the smallest logical unit of topology. Nodes are equal peers rather than members of a hierarchy, and a node is identified by a logical name and a current network address. The name represents the node's identity within the cluster, while the address is only a way to reach it and may change without changing the node's logical identity. This also means that a physical or virtual machine can be replaced without necessarily replacing the logical node: as long as the new machine assumes the same node identity and has valid credentials, the cluster can continue treating it as the same node.

Bubernetes does not require a built-in hierarchy of masters, workers, regions, zones, or sub-clusters. Higher-level topology can instead be implemented externally when needed. For example, several independent Bubernetes clusters could be connected through an external load balancer or another routing mechanism and treated as larger logical units. This makes the cluster itself a composable building block, allowing a cluster of clusters to be constructed without introducing topology-specific concepts into the core orchestration model.

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

## Running a local cluster

Each node needs a name, a port (used for both SWIM/UDP and the TCP wire), and a
manifest directory. Nodes after the first take a `--seed` to join.

```sh
make examples
bin/bubelet --name n1 --port 7701 --manifests /tmp/n1 &
bin/bubelet --name n2 --port 7702 --manifests /tmp/n2 --seed 127.0.0.1:7701 &
bin/bubelet --name n3 --port 7703 --manifests /tmp/n3 --seed 127.0.0.1:7701 &

bin/bubectl --server 127.0.0.1:7701 get nodes
```

Point an `image:` at the built workload (an absolute path, or one relative to
where bubelet runs), then:

```sh
bin/bubectl --server 127.0.0.1:7701 apply -f examples/web.yaml    # replicas: 3
bin/bubectl --server 127.0.0.1:7701 apply -f examples/agent.yaml  # replicas: -1
bin/bubectl --server 127.0.0.1:7702 get deploy     # any node has the full state
bin/bubectl --server 127.0.0.1:7703 dump           # whole desired state as YAML
bin/bubectl --server 127.0.0.1:7701 delete web
```

Kill a node and its replicas are rescheduled onto the survivors — placement is a
pure function of the alive set, so every node agrees without electing a leader.
`scripts/demo.sh` runs this whole scenario (join, deploy, daemonset, dump, node
failure + reschedule, delete) end to end.

For TLS: `examples/gen-certs.sh certs n1 n2 n3` writes a CA and per-node certs;
start each node with `--certs certs/<name>`, and pass `--certs certs/<name>` to
`bubectl` too.

## bubectl

```
bubectl apply -f FILE          define or update resources
bubectl get [nodes|deploy|static|all]
bubectl delete NAME
bubectl dump                   whole-cluster desired state as YAML
```

The server address is read from `--server IP:PORT`, `$BUBE_SERVER`, or
`.bube/config` (a line `server: 127.0.0.1:7701`).

## Manifests

```yaml
kind: Deploy        # Node | Static | Deploy
name: web
replicas: 3         # -1 every node (daemonset), 1 single, N copies
image: ./bin/workload   # a path to ingest, or a 64-hex image hash
argv: [--serve, web]
```

The manifest directory is the only storage there is: a file means the resource
should exist, removing it retracts the resource from the whole cluster. See
`examples/` for Deploy, DaemonSet (`replicas: -1`) and Static manifests.

## Repository layout

```
src/        the daemon and client (docs/DESIGN.md has the file-by-file map)
examples/   sample workload, manifests, cert generator
scripts/    demo.sh — a scripted, asserted 3-node scenario
tests/      run.sh — in-process decoder/validator tests under AddressSanitizer
docs/       DESIGN.md — architecture and simplifications
            AUDIT.md  — the code review and what each finding's fix was
```

See **docs/DESIGN.md** for how each idea above maps to the code and what is
deliberately simplified, and **docs/AUDIT.md** for the review pass and fixes.
`bash tests/run.sh` runs the sanitizer tests; `bash scripts/demo.sh` runs the
full cluster scenario with assertions.
