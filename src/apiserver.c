#include "apiserver.h"
#include "sha256.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>

/*
 * The concrete resource. Outside this file nobody knows it looks like this; it
 * could become a flat byte array, a b-tree, anything, and ResGet/ResSet would
 * hide the change. A resource is a set of fields indexed by noun, where a field
 * is one tagged value. "res is just a buffer" in practice.
 */

struct field {
    int set;
    int is_int;
    int64_t i;
    unsigned char *bytes;
    size_t len;
};

struct res {
    struct field f[NOUN__COUNT];
    unsigned int watch_mask;    /* bit per noun someone asked to watch */
    struct res *next;           /* hash-bucket chain in the store */
    int in_store;
};

struct resp {
    int is_int;
    int64_t i;
    unsigned char *bytes;
    size_t len;
};

/* one optional observer, woken when a watched noun changes. Enough for the
 * reconciler to react to spec changes instead of only polling. */
static void (*g_on_change)(res_t *r, int noun, void *ctx);
static void *g_on_change_ctx;

void ApiOnChange(void (*fn)(res_t *r, int noun, void *ctx), void *ctx)
{
    g_on_change = fn;
    g_on_change_ctx = ctx;
}

/* --------------------------------------------------------- noun metadata */

int NounIsSpec(int noun)
{
    switch (noun) {
    case NOUN_ID:
    case NOUN_NAME:
    case NOUN_KIND:
    case NOUN_REPLICAS:
    case NOUN_IMAGE:
    case NOUN_ARGV:
    case NOUN_VERSION:
    case NOUN_ORIGIN:
        return 1;
    default:
        return 0;       /* STATUS, ADDRESS, PID, RUNNING, MEM, CPU are local */
    }
}

const char *NounName(int noun)
{
    switch (noun) {
    case NOUN_ID:       return "id";
    case NOUN_NAME:     return "name";
    case NOUN_KIND:     return "kind";
    case NOUN_REPLICAS: return "replicas";
    case NOUN_IMAGE:    return "image";
    case NOUN_ARGV:     return "argv";
    case NOUN_VERSION:  return "version";
    case NOUN_ORIGIN:   return "origin";
    case NOUN_STATUS:   return "status";
    case NOUN_ADDRESS:  return "address";
    case NOUN_PID:      return "pid";
    case NOUN_RUNNING:  return "running";
    case NOUN_MEM:      return "mem";
    case NOUN_CPU:      return "cpu";
    default:            return "?";
    }
}

const char *KindName(int kind)
{
    switch (kind) {
    case KIND_NODE:   return "Node";
    case KIND_STATIC: return "Static";
    case KIND_DEPLOY: return "Deploy";
    default:          return "?";
    }
}

int KindFromName(const char *s)
{
    if (strcmp(s, "Node") == 0)   return KIND_NODE;
    if (strcmp(s, "Static") == 0) return KIND_STATIC;
    if (strcmp(s, "Deploy") == 0) return KIND_DEPLOY;
    return -1;
}

const char *StatusName(int status)
{
    switch (status) {
    case ST_PENDING:     return "Pending";
    case ST_READY:       return "Ready";
    case ST_UNHEALTHY:   return "Unhealthy";
    case ST_UNREACHABLE: return "Unreachable";
    case ST_GONE:        return "Gone";
    default:             return "?";
    }
}

/* --------------------------------------------------------- field helpers */

static void field_clear(struct field *fl)
{
    free(fl->bytes);
    fl->bytes = NULL;
    fl->len = 0;
    fl->i = 0;
    fl->is_int = 0;
    fl->set = 0;
}

static void notify(res_t *r, int noun)
{
    if (g_on_change != NULL && (r->watch_mask & (1u << noun)) != 0)
        g_on_change(r, noun, g_on_change_ctx);
}

/* --------------------------------------------------------- local verbs */

res_t *ResCreate(void)
{
    return xcalloc(1, sizeof(struct res));
}

void ResDelete(res_t *r)
{
    if (r == NULL)
        return;
    for (int n = 0; n < NOUN__COUNT; n++)
        field_clear(&r->f[n]);
    free(r);
}

resp_t *ResGet(res_t *r, int noun)
{
    if (r == NULL || noun < 0 || noun >= NOUN__COUNT)
        return NULL;
    struct field *fl = &r->f[noun];
    if (!fl->set)
        return NULL;
    if (fl->is_int)
        return RespFromInt(fl->i);
    return RespFromBytes(fl->bytes, fl->len);
}

int ResSet(res_t *r, int noun, const resp_t *v)
{
    if (r == NULL || v == NULL || noun < 0 || noun >= NOUN__COUNT)
        return -1;
    if (v->is_int)
        ResSetInt(r, noun, v->i);
    else
        ResSetBytes(r, noun, v->bytes, v->len);
    return 0;
}

int ResWatch(res_t *r, int noun)
{
    if (r == NULL || noun < 0 || noun >= NOUN__COUNT)
        return -1;
    r->watch_mask |= (1u << noun);
    return 0;
}

int64_t ResGetInt(res_t *r, int noun)
{
    if (r == NULL || noun < 0 || noun >= NOUN__COUNT)
        return 0;
    struct field *fl = &r->f[noun];
    if (fl->set && fl->is_int)
        return fl->i;
    return 0;
}

void ResSetInt(res_t *r, int noun, int64_t x)
{
    if (r == NULL || noun < 0 || noun >= NOUN__COUNT)
        return;
    struct field *fl = &r->f[noun];
    field_clear(fl);
    fl->set = 1;
    fl->is_int = 1;
    fl->i = x;
    notify(r, noun);
}

const void *ResGetBytes(res_t *r, int noun, size_t *len)
{
    if (r == NULL || noun < 0 || noun >= NOUN__COUNT) {
        if (len != NULL)
            *len = 0;
        return NULL;
    }
    struct field *fl = &r->f[noun];
    if (fl->set && !fl->is_int) {
        if (len != NULL)
            *len = fl->len;
        return fl->bytes;
    }
    if (len != NULL)
        *len = 0;
    return NULL;
}

void ResSetBytes(res_t *r, int noun, const void *p, size_t n)
{
    if (r == NULL || noun < 0 || noun >= NOUN__COUNT)
        return;
    struct field *fl = &r->f[noun];
    field_clear(fl);
    fl->set = 1;
    fl->is_int = 0;
    fl->bytes = xmalloc(n ? n : 1);
    memcpy(fl->bytes, p, n);
    fl->len = n;
    notify(r, noun);
}

int ResHas(res_t *r, int noun)
{
    if (r == NULL || noun < 0 || noun >= NOUN__COUNT)
        return 0;
    return r->f[noun].set;
}

void ResClear(res_t *r, int noun)
{
    if (r == NULL || noun < 0 || noun >= NOUN__COUNT)
        return;
    if (!r->f[noun].set)
        return;
    field_clear(&r->f[noun]);
    notify(r, noun);
}

void ResComputeId(int kind, const char *name, size_t nlen,
                  unsigned char out[RES_ID_LEN])
{
    struct sha256 s;
    sha256_init(&s);
    const char *kn = KindName(kind);
    sha256_update(&s, kn, strlen(kn));
    sha256_update(&s, "/", 1);
    sha256_update(&s, name, nlen);
    sha256_final(&s, out);
}

int ResNameValid(const char *name, size_t nlen)
{
    if (name == NULL || nlen == 0 || nlen > 63)
        return 0;
    for (size_t i = 0; i < nlen; i++) {
        char c = name[i];
        int ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                 (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
        if (!ok)
            return 0;
    }
    return 1;
}

int ResSpecValid(res_t *r, const char **why)
{
    const char *reason = NULL;

    int kind = (int)ResGetInt(r, NOUN_KIND);
    size_t nlen;
    const char *name = ResGetBytes(r, NOUN_NAME, &nlen);

    if (!ResHas(r, NOUN_KIND) || (kind != KIND_STATIC && kind != KIND_DEPLOY))
        reason = "kind must be Static or Deploy";
    else if (!ResNameValid(name, nlen))
        reason = "name must be 1..63 chars of [A-Za-z0-9._-]";
    else if (ResHas(r, NOUN_REPLICAS)) {
        int64_t rep = ResGetInt(r, NOUN_REPLICAS);
        if (rep < -1 || rep > RES_MAX_REPLICAS)
            reason = "replicas must be -1 or 0..65535";
    }
    if (reason == NULL && ResHas(r, NOUN_IMAGE)) {
        size_t il;
        ResGetBytes(r, NOUN_IMAGE, &il);
        if (il != RES_ID_LEN)
            reason = "image must be a 32-byte hash";
    }
    if (reason == NULL && ResHas(r, NOUN_ARGV)) {
        size_t al;
        const unsigned char *a = ResGetBytes(r, NOUN_ARGV, &al);
        if (al == 0 || a[al - 1] != '\0')
            reason = "argv must be NUL-terminated entries";
    }
    if (reason == NULL && ResHas(r, NOUN_ID)) {
        size_t il;
        const void *id = ResGetBytes(r, NOUN_ID, &il);
        unsigned char want[RES_ID_LEN];
        ResComputeId(kind, name, nlen, want);
        if (il != RES_ID_LEN || memcmp(id, want, RES_ID_LEN) != 0)
            reason = "id does not match kind/name";
    }

    if (why != NULL)
        *why = reason;
    return reason == NULL;
}

/* --------------------------------------------------------- resp values */

int RespIsInt(const resp_t *v)
{
    return v != NULL && v->is_int;
}

int64_t RespInt(const resp_t *v)
{
    if (v != NULL && v->is_int)
        return v->i;
    return 0;
}

const void *RespBytes(const resp_t *v, size_t *len)
{
    if (v != NULL && !v->is_int) {
        if (len != NULL)
            *len = v->len;
        return v->bytes;
    }
    if (len != NULL)
        *len = 0;
    return NULL;
}

const void *RespGetValuePtr(const resp_t *v)
{
    if (v == NULL)
        return NULL;
    if (v->is_int)
        return &v->i;
    return v->bytes;
}

resp_t *RespFromInt(int64_t x)
{
    resp_t *v = xcalloc(1, sizeof *v);
    v->is_int = 1;
    v->i = x;
    return v;
}

resp_t *RespFromBytes(const void *p, size_t n)
{
    resp_t *v = xcalloc(1, sizeof *v);
    v->is_int = 0;
    v->bytes = xmalloc(n ? n : 1);
    memcpy(v->bytes, p, n);
    v->len = n;
    return v;
}

void RespFree(resp_t *v)
{
    if (v == NULL)
        return;
    free(v->bytes);
    free(v);
}

/* --------------------------------------------------------------- store */

#define STORE_BUCKETS 1024

static struct res *g_buckets[STORE_BUCKETS];
static size_t g_count;

static size_t bucket_of(const void *id, size_t idlen)
{
    return hash64(id, idlen) & (STORE_BUCKETS - 1);
}

res_t *StoreGet(const void *id, size_t idlen)
{
    size_t b = bucket_of(id, idlen);
    for (res_t *r = g_buckets[b]; r != NULL; r = r->next) {
        size_t len;
        const void *rid = ResGetBytes(r, NOUN_ID, &len);
        if (rid != NULL && len == idlen && memcmp(rid, id, idlen) == 0)
            return r;
    }
    return NULL;
}

res_t *StoreCreate(const void *id, size_t idlen)
{
    res_t *r = StoreGet(id, idlen);
    if (r != NULL)
        return r;
    r = ResCreate();
    ResSetBytes(r, NOUN_ID, id, idlen);
    size_t b = bucket_of(id, idlen);
    r->next = g_buckets[b];
    r->in_store = 1;
    g_buckets[b] = r;
    g_count++;
    return r;
}

void StoreRemove(const void *id, size_t idlen)
{
    size_t b = bucket_of(id, idlen);
    res_t **pp = &g_buckets[b];
    while (*pp != NULL) {
        res_t *r = *pp;
        size_t len;
        const void *rid = ResGetBytes(r, NOUN_ID, &len);
        if (rid != NULL && len == idlen && memcmp(rid, id, idlen) == 0) {
            *pp = r->next;
            g_count--;
            ResDelete(r);
            return;
        }
        pp = &r->next;
    }
}

res_t *StoreGetName(const char *name)
{
    size_t want = strlen(name);
    for (size_t b = 0; b < STORE_BUCKETS; b++) {
        for (res_t *r = g_buckets[b]; r != NULL; r = r->next) {
            size_t len;
            const void *nm = ResGetBytes(r, NOUN_NAME, &len);
            if (nm != NULL && len == want && memcmp(nm, name, want) == 0)
                return r;
        }
    }
    return NULL;
}

res_t *StoreFindDesired(const char *name, int kind, int *matches)
{
    size_t want = strlen(name);
    res_t *found = NULL;
    int n = 0;
    for (size_t b = 0; b < STORE_BUCKETS; b++) {
        for (res_t *r = g_buckets[b]; r != NULL; r = r->next) {
            if (!ResIsDesired(r))
                continue;
            if (kind >= 0 && (int)ResGetInt(r, NOUN_KIND) != kind)
                continue;
            size_t len;
            const void *nm = ResGetBytes(r, NOUN_NAME, &len);
            if (nm != NULL && len == want && memcmp(nm, name, want) == 0) {
                if (found == NULL)
                    found = r;
                n++;
            }
        }
    }
    if (matches != NULL)
        *matches = n;
    return found;
}

void StoreForEach(void (*fn)(res_t *r, void *ctx), void *ctx)
{
    for (size_t b = 0; b < STORE_BUCKETS; b++) {
        res_t *r = g_buckets[b];
        while (r != NULL) {
            res_t *next = r->next;   /* fn may remove r */
            fn(r, ctx);
            r = next;
        }
    }
}

size_t StoreCount(void)
{
    return g_count;
}

/* ----------------------------------------------------- desired state */

int ResIsDesired(res_t *r)
{
    if (!ResHas(r, NOUN_ID))
        return 0;
    int kind = (int)ResGetInt(r, NOUN_KIND);
    return kind == KIND_STATIC || kind == KIND_DEPLOY;
}

struct desired_ctx {
    void (*fn)(res_t *r, void *ctx);
    void *ctx;
};

static void desired_filter(res_t *r, void *vp)
{
    struct desired_ctx *d = vp;
    if (ResIsDesired(r))
        d->fn(r, d->ctx);
}

void DesiredForEach(void (*fn)(res_t *r, void *ctx), void *ctx)
{
    struct desired_ctx d = { fn, ctx };
    StoreForEach(desired_filter, &d);
}

struct fold_ctx {
    void (*fn)(res_t *r, void *acc);
    void *acc;
};

static void fold_filter(res_t *r, void *vp)
{
    struct fold_ctx *d = vp;
    if (ResIsDesired(r))
        d->fn(r, d->acc);
}

void DesiredFold(void (*fn)(res_t *r, void *acc), void *acc)
{
    struct fold_ctx d = { fn, acc };
    StoreForEach(fold_filter, &d);
}

void ResCopySpec(res_t *dst, res_t *src)
{
    for (int n = 0; n < NOUN__COUNT; n++) {
        if (!NounIsSpec(n) || n == NOUN_ID)
            continue;
        if (!ResHas(src, n)) {
            ResClear(dst, n);       /* absent in the newer spec: gone here too */
            continue;
        }
        resp_t *v = ResGet(src, n);
        ResSet(dst, n, v);
        RespFree(v);
    }
}
