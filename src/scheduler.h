#ifndef SCHEDULER_H
#define SCHEDULER_H

#include "membership.h"
#include <stddef.h>

/*
 * Placement, with no scheduler process and no leader.
 *
 * Every node runs this same function over the same inputs - the resource id and
 * the alive member set, which gossip and SWIM have already made (near) identical
 * everywhere - and so every node independently reaches the same answer about who
 * runs what. Agreement falls out of determinism, not election.
 *
 * The method is rendezvous (highest-random-weight) hashing: for a resource and a
 * node, weight = hash(id, node). Rank the nodes by weight and the top ones win.
 * Add or remove a node and only the resources whose top-ranked node changed move;
 * everything else stays put.
 *
 *   replicas == -1 : one on every node          (a daemonset)
 *   replicas == 1  : one, on the single winner  (a lone process)
 *   replicas == N  : N, spread over the ranking; if N exceeds the node count the
 *                    surplus is handed out by rank, so the split is even and the
 *                    same on every node.
 *
 * STATIC resources are not placed here - they are pinned to their origin node by
 * the reconciler - so this concerns DEPLOY only.
 */
int schedule_deploy_here(const void *id, size_t idlen, int replicas,
                         struct member_view *members, int nmembers,
                         const char *self_name);

#endif
