#ifndef UTIL_H
#define UTIL_H

#include <stddef.h>
#include <stdint.h>

/* --- allocation: a daemon that cannot allocate cannot continue --- */
void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t sz);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);

/* --- time --- */
int64_t monotime_ms(void);          /* milliseconds on a monotonic clock */
int64_t walltime_ms(void);          /* milliseconds since the epoch       */

/* --- hashing ---
 * hash64 is a fast non-cryptographic mix (FNV-1a). It is used for hash tables
 * and for rendezvous placement, never for content identity. Content identity
 * is sha256 (see sha256.h).
 */
uint64_t hash64(const void *p, size_t n);
uint64_t hash64_seed(uint64_t seed, const void *p, size_t n);

/* --- hex --- */
void  hex_encode(char *out, const void *p, size_t n);   /* out: 2*n+1 bytes */
int   hex_decode(void *out, size_t outcap, const char *s); /* ret bytes, -1  */

/* --- full-transfer fd helpers (handle short reads/writes and EINTR) --- */
int read_full(int fd, void *buf, size_t n);     /* 0 ok, -1 err, 1 eof-early */
int write_full(int fd, const void *buf, size_t n);

/* --- a growable byte buffer. The project's one container; "everything is a
 *     buf". Used by the wire codec, the yaml writer and the blob store. --- */
struct buf {
    unsigned char *data;
    size_t len;
    size_t cap;
};

void buf_init(struct buf *b);
void buf_free(struct buf *b);
void buf_reset(struct buf *b);
void buf_reserve(struct buf *b, size_t need);
void buf_append(struct buf *b, const void *p, size_t n);
void buf_append_byte(struct buf *b, unsigned char c);
void buf_append_str(struct buf *b, const char *s);
void buf_append_u16(struct buf *b, uint16_t v);     /* big-endian */
void buf_append_u32(struct buf *b, uint32_t v);
void buf_append_u64(struct buf *b, uint64_t v);

/* --- a bounds-checked reader over a flat region. Decoding never walks off
 *     the end: past the end every read returns zero and sets err. --- */
struct rdr {
    const unsigned char *p;
    size_t len;
    size_t off;
    int err;
};

void        rdr_init(struct rdr *r, const void *p, size_t len);
uint16_t    rd_u16(struct rdr *r);
uint32_t    rd_u32(struct rdr *r);
uint64_t    rd_u64(struct rdr *r);
const void *rd_bytes(struct rdr *r, size_t n);      /* NULL on short buffer */

#endif
