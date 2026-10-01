#ifndef BLOB_H
#define BLOB_H

#include "sha256.h"
#include <stddef.h>

/*
 * The content-addressed image store.
 *
 * A spec never carries a path or a name for its executable; it carries the
 * sha256 of the image bytes. That hash is what gossip replicates. The bytes
 * themselves are heavy, so they are kept out of the desired state entirely and
 * fetched on demand: a node that holds a spec but not the image asks peers for
 * "the blob with this hash" (the DHT-flavoured lookup), and any peer that has
 * it answers. Identity is the content, so it does not matter which peer answers
 * or how the bytes arrive - a wrong answer simply fails to hash back.
 *
 * Kept in memory: these are ephemeral workload images, not a package cache.
 */

/* store bytes; writes their sha256 into hash[]. Idempotent. */
void blob_put(const void *data, size_t len, unsigned char hash[SHA256_LEN]);

/* look up by hash; returns a pointer owned by the store (do not free), or NULL.
 * *len receives the length. The pointer stays valid until blob_drop/exit. */
const void *blob_get(const unsigned char hash[SHA256_LEN], size_t *len);

int  blob_has(const unsigned char hash[SHA256_LEN]);

/* read a file into the store; writes its hash. Returns 0, or -1 on read error. */
int  blob_ingest_file(const char *path, unsigned char hash[SHA256_LEN]);

/* drop a blob we no longer need (no workload references the hash). */
void blob_drop(const unsigned char hash[SHA256_LEN]);

size_t blob_count(void);

#endif
