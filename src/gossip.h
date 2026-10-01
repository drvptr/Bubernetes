#ifndef GOSSIP_H
#define GOSSIP_H

#include "apiserver.h"
#include "util.h"
#include <stdint.h>

/*
 * Desired state, replicated.
 *
 * No node owns the desired state. Each keeps a full replica and they converge by
 * anti-entropy: now and then a node picks a random peer and the two exchange
 * everything they know, so any fact started anywhere reaches everyone. That is
 * the "collective mind" - pod1 declared here and pod2 declared there become, on
 * both nodes, a desired state of {pod1, pod2}. A node that dies and returns has
 * lost only its runtime; the specs are still held by the others and flow back.
 *
 * Only spec nouns travel (see NounIsSpec). Runtime - pids, status - stays on the
 * node that observed it and is never replicated.
 *
 * Conflicts are settled by a Lamport version with the author's node name as the
 * tiebreak: last writer wins, deterministically, on every node. Deletion is a
 * tombstone (a version that says "gone"), so a removed resource does not get
 * resurrected by a peer that had not yet heard, and the delete reaches every
 * replica the same way a creation does.
 */

/* Lamport clock */
uint64_t gossip_now(void);
uint64_t gossip_tick(void);          /* increment, return new value */
void     gossip_witness(uint64_t v); /* advance past a value we have seen */

/* The loader has put spec nouns on r (already in the store). Stamp it as ours
 * with a fresh version and author, clear any tombstone, and announce the change.
 * Call only when the spec actually changed, or versions climb forever. */
void gossip_author(res_t *r);

/* Locally retract a resource by id: tombstone it and remove it from the store. */
void gossip_delete(const void *id, size_t idlen);

/* Are the spec nouns of a and b identical? The loader uses this to avoid
 * re-authoring an unchanged manifest. */
int  gossip_spec_equal(res_t *a, res_t *b);

/* Anti-entropy wire core: serialise our whole desired state (+ tombstones), and
 * merge a peer's. Both sides of a round use these. */
void gossip_build_state(struct buf *out);
void gossip_merge_state(const void *data, size_t len);

/* Run one anti-entropy round against a random alive peer (uses membership+wire).
 * server_name lets TLS verify the peer; harmless when built plain. */
void gossip_round(void);

/* Change notifications, so the daemon can persist desired state to the manifest
 * directory and (re)reconcile. */
enum { GOSSIP_UPSERT, GOSSIP_DELETE };
void gossip_on_change(void (*fn)(int op, res_t *r,
                                 const void *id, size_t idlen, void *ctx),
                      void *ctx);

#endif
