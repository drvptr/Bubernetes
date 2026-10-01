#ifndef SHA256_H
#define SHA256_H

#include <stddef.h>
#include <stdint.h>

/*
 * A self-contained SHA-256. It exists for one job: give every executable image
 * a stable content identity, so a node can ask the cluster for "the image whose
 * hash is X" instead of trusting a path or a name. Standard FIPS 180-4; written
 * out plainly rather than as a table of macros.
 */

#define SHA256_LEN 32

struct sha256 {
    uint32_t state[8];
    uint64_t bitlen;
    unsigned char block[64];
    size_t fill;
};

void sha256_init(struct sha256 *s);
void sha256_update(struct sha256 *s, const void *data, size_t len);
void sha256_final(struct sha256 *s, unsigned char out[SHA256_LEN]);

/* one-shot convenience */
void sha256_hash(const void *data, size_t len, unsigned char out[SHA256_LEN]);

#endif
