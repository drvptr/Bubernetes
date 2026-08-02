# Bubernetes

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

One of the key goals of Bubernetes is to separate process orchestration from the execution environment. Bubernetes manages what processes should run, how many instances should exist, and where they should be placed, while the underlying environment is left completely outside of the orchestrator's responsibility. Unlike Kubernetes, Bubernetes **does not require** a cluster-wide container network, CNI, Pod network, or other networking layer between workloads. Nodes are independent environments that communicate directly with each other through the TCP. A node may therefore be bare metal, a virtual machine, a container, or any other Linux environment capable of running bubelet. This makes the same orchestration layer usable across fundamentally different environments: for example, ten isolated containers can each run bubelet and form a cluster, after which a DaemonSet-like workload can be distributed across all of them. Isolation is therefore something the environment provides, while orchestration remains the responsibility of Bubernetes.
