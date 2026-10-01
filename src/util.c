#define _GNU_SOURCE
#include "util.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>

/* ------------------------------------------------------------------ log */

void bube_log(const char *level, const char *func, int err, const char *fmt, ...)
{
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    int64_t t = walltime_ms();
    time_t secs = (time_t)(t / 1000);
    struct tm tm;
    localtime_r(&secs, &tm);
    char ts[32];
    strftime(ts, sizeof ts, "%H:%M:%S", &tm);

    if (err != 0)
        fprintf(stderr, "%s.%03d %s %s: %s: %s\n",
                ts, (int)(t % 1000), level, func, msg, strerror(err));
    else
        fprintf(stderr, "%s.%03d %s %s: %s\n",
                ts, (int)(t % 1000), level, func, msg);
}

/* ------------------------------------------------------------- allocation */

void *xmalloc(size_t n)
{
    if (n == 0)
        n = 1;
    void *p = malloc(n);
    if (p == NULL) {
        ERR("out of memory (%zu bytes)", n);
        abort();
    }
    return p;
}

void *xcalloc(size_t n, size_t sz)
{
    if (n == 0 || sz == 0) {
        n = 1;
        sz = 1;
    }
    void *p = calloc(n, sz);
    if (p == NULL) {
        ERR("out of memory (%zu x %zu bytes)", n, sz);
        abort();
    }
    return p;
}

void *xrealloc(void *p, size_t n)
{
    if (n == 0)
        n = 1;
    void *q = realloc(p, n);
    if (q == NULL) {
        ERR("out of memory (%zu bytes)", n);
        abort();
    }
    return q;
}

char *xstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = xmalloc(n);
    memcpy(p, s, n);
    return p;
}

/* ------------------------------------------------------------------- time */

int64_t monotime_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int64_t walltime_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---------------------------------------------------------------- hashing */

uint64_t hash64_seed(uint64_t seed, const void *p, size_t n)
{
    const unsigned char *b = p;
    uint64_t h = seed;
    for (size_t i = 0; i < n; i++) {
        h = h ^ b[i];
        h = h * 1099511628211ULL;   /* FNV-1a 64-bit prime */
    }
    return h;
}

uint64_t hash64(const void *p, size_t n)
{
    return hash64_seed(1469598103934665603ULL, p, n);  /* FNV offset basis */
}

/* -------------------------------------------------------------------- hex */

void hex_encode(char *out, const void *p, size_t n)
{
    static const char tab[] = "0123456789abcdef";
    const unsigned char *b = p;
    for (size_t i = 0; i < n; i++) {
        out[2 * i]     = tab[b[i] >> 4];
        out[2 * i + 1] = tab[b[i] & 0x0f];
    }
    out[2 * n] = '\0';
}

static int hex_nib(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

int hex_decode(void *out, size_t outcap, const char *s)
{
    unsigned char *o = out;
    size_t n = 0;
    while (s[0] != '\0' && s[1] != '\0') {
        int hi = hex_nib(s[0]);
        int lo = hex_nib(s[1]);
        if (hi < 0 || lo < 0)
            return -1;
        if (n >= outcap)
            return -1;
        o[n++] = (unsigned char)((hi << 4) | lo);
        s += 2;
    }
    if (s[0] != '\0')
        return -1;      /* odd number of hex digits */
    return (int)n;
}

/* --------------------------------------------------------------- fd I/O */

int read_full(int fd, void *buf, size_t n)
{
    unsigned char *p = buf;
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, p + got, n - got);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (r == 0)
            return 1;   /* peer closed before n bytes arrived */
        got += (size_t)r;
    }
    return 0;
}

int write_full(int fd, const void *buf, size_t n)
{
    const unsigned char *p = buf;
    size_t sent = 0;
    while (sent < n) {
        ssize_t w = write(fd, p + sent, n - sent);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        sent += (size_t)w;
    }
    return 0;
}

/* ---------------------------------------------------------------- buffer */

void buf_init(struct buf *b)
{
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

void buf_free(struct buf *b)
{
    free(b->data);
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

void buf_reset(struct buf *b)
{
    b->len = 0;
}

void buf_reserve(struct buf *b, size_t need)
{
    if (b->len + need <= b->cap)
        return;
    size_t cap = b->cap ? b->cap : 64;
    while (cap < b->len + need)
        cap *= 2;
    b->data = xrealloc(b->data, cap);
    b->cap = cap;
}

void buf_append(struct buf *b, const void *p, size_t n)
{
    buf_reserve(b, n);
    memcpy(b->data + b->len, p, n);
    b->len += n;
}

void buf_append_byte(struct buf *b, unsigned char c)
{
    buf_reserve(b, 1);
    b->data[b->len++] = c;
}

void buf_append_str(struct buf *b, const char *s)
{
    buf_append(b, s, strlen(s));
}

void buf_append_u16(struct buf *b, uint16_t v)
{
    buf_reserve(b, 2);
    b->data[b->len++] = (unsigned char)(v >> 8);
    b->data[b->len++] = (unsigned char)(v);
}

void buf_append_u32(struct buf *b, uint32_t v)
{
    buf_reserve(b, 4);
    b->data[b->len++] = (unsigned char)(v >> 24);
    b->data[b->len++] = (unsigned char)(v >> 16);
    b->data[b->len++] = (unsigned char)(v >> 8);
    b->data[b->len++] = (unsigned char)(v);
}

void buf_append_u64(struct buf *b, uint64_t v)
{
    buf_reserve(b, 8);
    for (int i = 56; i >= 0; i -= 8)
        b->data[b->len++] = (unsigned char)(v >> i);
}

/* ---------------------------------------------------------------- reader */

void rdr_init(struct rdr *r, const void *p, size_t len)
{
    r->p = p;
    r->len = len;
    r->off = 0;
    r->err = 0;
}

uint16_t rd_u16(struct rdr *r)
{
    if (r->off + 2 > r->len) {
        r->err = 1;
        return 0;
    }
    uint16_t v = ((uint16_t)r->p[r->off] << 8) | r->p[r->off + 1];
    r->off += 2;
    return v;
}

uint32_t rd_u32(struct rdr *r)
{
    if (r->off + 4 > r->len) {
        r->err = 1;
        return 0;
    }
    uint32_t v = ((uint32_t)r->p[r->off] << 24) |
                 ((uint32_t)r->p[r->off + 1] << 16) |
                 ((uint32_t)r->p[r->off + 2] << 8) |
                 ((uint32_t)r->p[r->off + 3]);
    r->off += 4;
    return v;
}

uint64_t rd_u64(struct rdr *r)
{
    if (r->off + 8 > r->len) {
        r->err = 1;
        return 0;
    }
    uint64_t v = 0;
    for (int i = 0; i < 8; i++)
        v = (v << 8) | r->p[r->off + i];
    r->off += 8;
    return v;
}

const void *rd_bytes(struct rdr *r, size_t n)
{
    if (r->off + n > r->len) {
        r->err = 1;
        return NULL;
    }
    const void *p = r->p + r->off;
    r->off += n;
    return p;
}
