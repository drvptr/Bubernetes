#include "blob.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

struct entry {
    unsigned char hash[SHA256_LEN];
    unsigned char *data;
    size_t len;
    int used;
};

/* small linear table. Workload images per node are few; a tree would be more
 * code than it buys. */
static struct entry *g_tab;
static size_t g_cap;
static size_t g_len;

static struct entry *find(const unsigned char hash[SHA256_LEN])
{
    for (size_t i = 0; i < g_len; i++)
        if (g_tab[i].used && memcmp(g_tab[i].hash, hash, SHA256_LEN) == 0)
            return &g_tab[i];
    return NULL;
}

static struct entry *alloc_slot(void)
{
    for (size_t i = 0; i < g_len; i++)
        if (!g_tab[i].used)
            return &g_tab[i];
    if (g_len == g_cap) {
        g_cap = g_cap ? g_cap * 2 : 8;
        g_tab = xrealloc(g_tab, g_cap * sizeof *g_tab);
    }
    return &g_tab[g_len++];
}

void blob_put(const void *data, size_t len, unsigned char hash[SHA256_LEN])
{
    sha256_hash(data, len, hash);
    if (find(hash) != NULL)
        return;                     /* already have these exact bytes */
    struct entry *e = alloc_slot();
    memcpy(e->hash, hash, SHA256_LEN);
    e->data = xmalloc(len ? len : 1);
    memcpy(e->data, data, len);
    e->len = len;
    e->used = 1;
}

const void *blob_get(const unsigned char hash[SHA256_LEN], size_t *len)
{
    struct entry *e = find(hash);
    if (e == NULL) {
        if (len != NULL)
            *len = 0;
        return NULL;
    }
    if (len != NULL)
        *len = e->len;
    return e->data;
}

int blob_has(const unsigned char hash[SHA256_LEN])
{
    return find(hash) != NULL;
}

int blob_ingest_file(const char *path, unsigned char hash[SHA256_LEN])
{
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return -1;
    struct buf b;
    buf_init(&b);
    char tmp[65536];
    size_t r;
    while ((r = fread(tmp, 1, sizeof tmp, f)) > 0)
        buf_append(&b, tmp, r);
    fclose(f);
    blob_put(b.data, b.len, hash);
    buf_free(&b);
    return 0;
}

void blob_drop(const unsigned char hash[SHA256_LEN])
{
    struct entry *e = find(hash);
    if (e == NULL)
        return;
    free(e->data);
    e->data = NULL;
    e->len = 0;
    e->used = 0;
}

void blob_sweep(int (*keep)(const unsigned char hash[SHA256_LEN], void *ctx),
                void *ctx)
{
    for (size_t i = 0; i < g_len; i++) {
        struct entry *e = &g_tab[i];
        if (!e->used)
            continue;
        if (keep(e->hash, ctx))
            continue;
        free(e->data);
        e->data = NULL;
        e->len = 0;
        e->used = 0;
    }
}

size_t blob_count(void)
{
    size_t n = 0;
    for (size_t i = 0; i < g_len; i++)
        if (g_tab[i].used)
            n++;
    return n;
}
