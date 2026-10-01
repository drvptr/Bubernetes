#include "sha256.h"
#include <string.h>

/* round constants (first 32 bits of the fractional parts of the cube roots
 * of the first 64 primes) */
static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

static uint32_t ror(uint32_t x, int n)
{
    return (x >> n) | (x << (32 - n));
}

static void sha256_block(struct sha256 *s, const unsigned char *p)
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[4 * i] << 24) |
               ((uint32_t)p[4 * i + 1] << 16) |
               ((uint32_t)p[4 * i + 2] << 8) |
               ((uint32_t)p[4 * i + 3]);
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = s->state[0], b = s->state[1], c = s->state[2], d = s->state[3];
    uint32_t e = s->state[4], f = s->state[5], g = s->state[6], h = s->state[7];

    for (int i = 0; i < 64; i++) {
        uint32_t s1 = ror(e, 6) ^ ror(e, 11) ^ ror(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + s1 + ch + K[i] + w[i];
        uint32_t s0 = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = s0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    s->state[0] += a; s->state[1] += b; s->state[2] += c; s->state[3] += d;
    s->state[4] += e; s->state[5] += f; s->state[6] += g; s->state[7] += h;
}

void sha256_init(struct sha256 *s)
{
    s->state[0] = 0x6a09e667; s->state[1] = 0xbb67ae85;
    s->state[2] = 0x3c6ef372; s->state[3] = 0xa54ff53a;
    s->state[4] = 0x510e527f; s->state[5] = 0x9b05688c;
    s->state[6] = 0x1f83d9ab; s->state[7] = 0x5be0cd19;
    s->bitlen = 0;
    s->fill = 0;
}

void sha256_update(struct sha256 *s, const void *data, size_t len)
{
    const unsigned char *p = data;
    s->bitlen += (uint64_t)len * 8;
    while (len > 0) {
        size_t take = 64 - s->fill;
        if (take > len)
            take = len;
        memcpy(s->block + s->fill, p, take);
        s->fill += take;
        p += take;
        len -= take;
        if (s->fill == 64) {
            sha256_block(s, s->block);
            s->fill = 0;
        }
    }
}

void sha256_final(struct sha256 *s, unsigned char out[SHA256_LEN])
{
    uint64_t bitlen = s->bitlen;

    unsigned char pad = 0x80;
    sha256_update(s, &pad, 1);
    pad = 0x00;
    while (s->fill != 56)
        sha256_update(s, &pad, 1);

    unsigned char lenbuf[8];
    for (int i = 0; i < 8; i++)
        lenbuf[i] = (unsigned char)(bitlen >> (56 - 8 * i));
    sha256_update(s, lenbuf, 8);

    for (int i = 0; i < 8; i++) {
        out[4 * i]     = (unsigned char)(s->state[i] >> 24);
        out[4 * i + 1] = (unsigned char)(s->state[i] >> 16);
        out[4 * i + 2] = (unsigned char)(s->state[i] >> 8);
        out[4 * i + 3] = (unsigned char)(s->state[i]);
    }
}

void sha256_hash(const void *data, size_t len, unsigned char out[SHA256_LEN])
{
    struct sha256 s;
    sha256_init(&s);
    sha256_update(&s, data, len);
    sha256_final(&s, out);
}
