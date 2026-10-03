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
 *
 * Every spec that arrives from outside - a peer, bubectl, a file - is checked
 * with ResSpecValid() before it is stored; nothing malformed gets past here.
 */

/* Lamport clock. The daemon names a file to persist it in, so a restarted node
 * resumes past everything it ever authored instead of starting again at 1 and
 * losing every edit it makes until gossip catches it up. */
uint64_t gossip_now(void);
uint64_t gossip_tick(void);          /* increment, return new value */
void     gossip_witness(uint64_t v); /* advance past a value we have seen */
void     gossip_clock_file(const char *path);   /* load now, save on change */

/* The loader has put spec nouns on r (already in the store). Stamp it as ours
 * with a fresh version and author, clear any tombstone, and announce the change.
 * Call only when the spec actually changed, or versions climb forever. */
void gossip_author(res_t *r);

/* Merge one complete spec (with version and origin) into the store under the
 * same last-writer-wins and tombstone rules a gossiped spec gets. Used for
 * peers' specs and for state files the daemon itself persisted. Takes ownership
 * of `in` (it is a detached resource) and frees it. Returns 1 if the store
 * changed. */
int  gossip_merge_res(res_t *in);

/* Locally retract a resource by id: tombstone it and remove it from the store. */
void gossip_delete(const void *id, size_t idlen);

/* is there a live tombstone for this id? (so an unchanged file does not quietly
 * resurrect something that was deleted through the API) */
int  gossip_is_tombstoned(const void *id, size_t idlen);

/* Are the spec nouns of a and b identical? The loader uses this to avoid
 * re-authoring an unchanged manifest. */
int  gossip_spec_equal(res_t *a, res_t *b);

/* Anti-entropy wire core: serialise our whole desired state (+ tombstones), and
 * merge a peer's. Both sides of a round use these; the daemon's worker thread
 * drives the round so the blocking call happens off the event loop. */
void gossip_build_state(struct buf *out);
void gossip_merge_state(const void *data, size_t len);

/* Change notifications, so the daemon can persist desired state to the manifest
 * directory and (re)reconcile. For GOSSIP_DELETE, r is the resource about to be
 * removed (still readable), so the callback can find its file. */
enum { GOSSIP_UPSERT, GOSSIP_DELETE };
void gossip_on_change(void (*fn)(int op, res_t *r,
                                 const void *id, size_t idlen, void *ctx),
                      void *ctx);

#endif
