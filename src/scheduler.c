#include "scheduler.h"
#include "util.h"

#include <string.h>

/* weight of placing resource <id> on node <name>: a hash of both, so it is
 * stable, uniform, and independent of any ordering */
static uint64_t weight(const void *id, size_t idlen, const char *name)
{
    uint64_t h = hash64(id, idlen);
    return hash64_seed(h, name, strlen(name));
}

/* this node's rank in the rendezvous ordering: how many nodes outrank it.
 * Ties (equal weight) break on the name, consistently on every node. */
static int rank_of(const void *id, size_t idlen,
                   struct member_view *members, int nmembers,
                   const char *self_name)
{
    uint64_t self_w = weight(id, idlen, self_name);
    int rank = 0;
    for (int i = 0; i < nmembers; i++) {
        if (strcmp(members[i].name, self_name) == 0)
            continue;
        uint64_t w = weight(id, idlen, members[i].name);
        if (w > self_w || (w == self_w && strcmp(members[i].name, self_name) > 0))
            rank++;
    }
    return rank;
}

int schedule_deploy_here(const void *id, size_t idlen, int replicas,
                         struct member_view *members, int nmembers,
                         const char *self_name)
{
    if (nmembers <= 0)
        return 0;

    if (replicas < 0)
        return 1;               /* daemonset: one here, one everywhere */
    if (replicas == 0)
        return 0;

    int rank = rank_of(id, idlen, members, nmembers, self_name);

    /* N replicas over M nodes: each node gets N/M, and the first N%M nodes in
     * the ranking get one extra. Deterministic on every node. */
    int base = replicas / nmembers;
    int extra = replicas % nmembers;
    int here = base + (rank < extra ? 1 : 0);
    return here;
}
