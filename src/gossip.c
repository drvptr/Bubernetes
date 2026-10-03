#define _GNU_SOURCE
#include "gossip.h"
#include "wire.h"
#include "membership.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Tombstones live at least this long so a peer that was away cannot resurrect
 * a deleted resource with its stale copy. A day covers an ordinary outage; a
 * node away longer than this should be re-synced from a live node (see
 * docs/DESIGN.md) rather than trusted to rejoin with whatever it remembers. */
#define TOMB_TTL_MS (24LL * 3600 * 1000)

static uint64_t g_lamport = 1;
static char g_clock_path[1024];

static void (*g_on_change)(int, res_t *, const void *, size_t, void *);
static void *g_on_change_ctx;

struct tomb {
    unsigned char *id;
    size_t idlen;
    uint64_t version;
    char origin[128];
    int64_t ts;
    int used;
};
static struct tomb *g_tombs;
static size_t g_tcap, g_tlen;

/* ------------------------------------------------------------ lamport */

static void clock_save(void)
{
    if (g_clock_path[0] == '\0')
        return;
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.tmp", g_clock_path);
    FILE *f = fopen(tmp, "w");
    if (f == NULL)
        return;
    fprintf(f, "%llu\n", (unsigned long long)g_lamport);
    fclose(f);
    rename(tmp, g_clock_path);      /* atomic replace */
}

void gossip_clock_file(const char *path)
{
    snprintf(g_clock_path, sizeof g_clock_path, "%s", path);
    FILE *f = fopen(path, "r");
    if (f == NULL)
        return;
    unsigned long long v = 0;
    if (fscanf(f, "%llu", &v) == 1 && v > g_lamport)
        g_lamport = v;
    fclose(f);
}

uint64_t gossip_now(void)  { return g_lamport; }

uint64_t gossip_tick(void)
{
    g_lamport++;
    clock_save();
    return g_lamport;
}

void gossip_witness(uint64_t v)
{
    if (v >= g_lamport) {
        g_lamport = v + 1;
        clock_save();
    }
}

void gossip_on_change(void (*fn)(int, res_t *, const void *, size_t, void *),
                      void *ctx)
{
    g_on_change = fn;
    g_on_change_ctx = ctx;
}

static void announce(int op, res_t *r, const void *id, size_t idlen)
{
    if (g_on_change != NULL)
        g_on_change(op, r, id, idlen, g_on_change_ctx);
}

/* ------------------------------------------------------------ tombstones */

static struct tomb *tomb_find(const void *id, size_t idlen)
{
    for (size_t i = 0; i < g_tlen; i++)
        if (g_tombs[i].used && g_tombs[i].idlen == idlen &&
            memcmp(g_tombs[i].id, id, idlen) == 0)
            return &g_tombs[i];
    return NULL;
}

static void tomb_reap(void)
{
    int64_t now = monotime_ms();
    for (size_t i = 0; i < g_tlen; i++)
        if (g_tombs[i].used && now - g_tombs[i].ts > TOMB_TTL_MS) {
            free(g_tombs[i].id);
            g_tombs[i].id = NULL;
            g_tombs[i].used = 0;
        }
}

static void tomb_set(const void *id, size_t idlen, uint64_t version,
                     const char *origin)
{
    struct tomb *t = tomb_find(id, idlen);
    if (t != NULL) {
        if (version > t->version) {
            t->version = version;
            snprintf(t->origin, sizeof t->origin, "%s", origin);
            t->ts = monotime_ms();
        }
        return;
    }
    struct tomb *slot = NULL;
    for (size_t i = 0; i < g_tlen; i++)
        if (!g_tombs[i].used) { slot = &g_tombs[i]; break; }
    if (slot == NULL) {
        if (g_tlen == g_tcap) {
            g_tcap = g_tcap ? g_tcap * 2 : 16;
            g_tombs = xrealloc(g_tombs, g_tcap * sizeof *g_tombs);
        }
        slot = &g_tombs[g_tlen++];
    }
    slot->id = xmalloc(idlen);
    memcpy(slot->id, id, idlen);
    slot->idlen = idlen;
    slot->version = version;
    snprintf(slot->origin, sizeof slot->origin, "%s", origin);
    slot->ts = monotime_ms();
    slot->used = 1;
}

static void tomb_clear(const void *id, size_t idlen)
{
    struct tomb *t = tomb_find(id, idlen);
    if (t != NULL) {
        free(t->id);
        t->id = NULL;
        t->used = 0;
    }
}

/* ------------------------------------------------------------ spec compare */

int gossip_spec_equal(res_t *a, res_t *b)
{
    for (int n = 0; n < NOUN__COUNT; n++) {
        if (!NounIsSpec(n))
            continue;
        if (n == NOUN_VERSION || n == NOUN_ORIGIN)
            continue;       /* identity of the spec, not its content */
        int ha = ResHas(a, n), hb = ResHas(b, n);
        if (ha != hb)
            return 0;
        if (!ha)
            continue;
        resp_t *va = ResGet(a, n), *vb = ResGet(b, n);
        int eq;
        if (RespIsInt(va) != RespIsInt(vb)) {
            eq = 0;
        } else if (RespIsInt(va)) {
            eq = RespInt(va) == RespInt(vb);
        } else {
            size_t la, lb;
            const void *pa = RespBytes(va, &la), *pb = RespBytes(vb, &lb);
            eq = la == lb && memcmp(pa, pb, la) == 0;
        }
        RespFree(va);
        RespFree(vb);
        if (!eq)
            return 0;
    }
    return 1;
}

/* a wins over b on (version, origin-name) */
static int newer(uint64_t av, const char *ao, uint64_t bv, const char *bo)
{
    if (av != bv)
        return av > bv;
    return strcmp(ao, bo) > 0;
}

/* ------------------------------------------------------------ author/delete */

void gossip_author(res_t *r)
{
    ResSetBytes(r, NOUN_ORIGIN, membership_self_name(),
                strlen(membership_self_name()));
    ResSetInt(r, NOUN_VERSION, (int64_t)gossip_tick());
    size_t idlen;
    const void *id = ResGetBytes(r, NOUN_ID, &idlen);
    if (id != NULL)
        tomb_clear(id, idlen);
    announce(GOSSIP_UPSERT, r, id, idlen);
}

int gossip_is_tombstoned(const void *id, size_t idlen)
{
    return tomb_find(id, idlen) != NULL;
}

void gossip_delete(const void *id, size_t idlen)
{
    uint64_t v = gossip_tick();
    tomb_set(id, idlen, v, membership_self_name());
    res_t *r = StoreGet(id, idlen);     /* pass it out before we drop it, so the
                                         * daemon can find the file to remove */
    announce(GOSSIP_DELETE, r, id, idlen);
    StoreRemove(id, idlen);
}

/* ------------------------------------------------------------ merge one spec */

int gossip_merge_res(res_t *in)
{
    /* nothing malformed gets past this point, whoever sent it */
    const char *why;
    if (!ResHas(in, NOUN_ID) || !ResHas(in, NOUN_VERSION) ||
        !ResHas(in, NOUN_ORIGIN) || !ResSpecValid(in, &why)) {
        size_t nl;
        const char *nm = ResGetBytes(in, NOUN_NAME, &nl);
        WARN("rejecting spec '%.*s': %s", (int)(nm ? nl : 0), nm ? nm : "",
             ResHas(in, NOUN_VERSION) && ResHas(in, NOUN_ORIGIN)
                 ? why : "missing version/origin");
        ResDelete(in);
        return 0;
    }

    size_t idlen;
    const void *id = ResGetBytes(in, NOUN_ID, &idlen);
    unsigned char idbuf[RES_ID_LEN];
    memcpy(idbuf, id, RES_ID_LEN);          /* ResSpecValid proved the length */

    uint64_t ver = (uint64_t)ResGetInt(in, NOUN_VERSION);
    size_t olen;
    const char *origin = ResGetBytes(in, NOUN_ORIGIN, &olen);
    char ob[128];
    snprintf(ob, sizeof ob, "%.*s", (int)olen, origin);
    gossip_witness(ver);

    struct tomb *t = tomb_find(idbuf, RES_ID_LEN);
    if (t != NULL && t->version >= ver) {
        ResDelete(in);          /* it's deleted, and the delete is newer */
        return 0;
    }

    res_t *cur = StoreGet(idbuf, RES_ID_LEN);
    if (cur == NULL) {
        res_t *r = StoreCreate(idbuf, RES_ID_LEN);
        ResCopySpec(r, in);
        size_t nlen;
        const char *nm = ResGetBytes(r, NOUN_NAME, &nlen);
        INFO("learned resource '%.*s' v%llu from %s",
             (int)nlen, nm, (unsigned long long)ver, ob);
        announce(GOSSIP_UPSERT, r, idbuf, RES_ID_LEN);
        ResDelete(in);
        return 1;
    }

    uint64_t curver = (uint64_t)ResGetInt(cur, NOUN_VERSION);
    size_t colen;
    const char *corigin = ResGetBytes(cur, NOUN_ORIGIN, &colen);
    char cob[128];
    snprintf(cob, sizeof cob, "%.*s", (int)(corigin ? colen : 0),
             corigin ? corigin : "");

    int changed = 0;
    if (newer(ver, ob, curver, cob)) {
        ResCopySpec(cur, in);     /* spec nouns only; runtime stays */
        announce(GOSSIP_UPSERT, cur, idbuf, RES_ID_LEN);
        changed = 1;
    }
    ResDelete(in);
    return changed;
}

/* ------------------------------------------------------------ build/merge */

struct build_ctx { struct buf *out; uint32_t n; };

static void count_spec(res_t *r, void *vp)
{
    (void)r;
    struct build_ctx *c = vp;
    c->n++;
}

static void emit_spec(res_t *r, void *vp)
{
    struct build_ctx *c = vp;
    wire_put_res(c->out, r, 1);     /* spec_only */
}

void gossip_build_state(struct buf *out)
{
    tomb_reap();

    struct build_ctx c = { out, 0 };
    DesiredFold(count_spec, &c);
    buf_append_u32(out, c.n);
    DesiredForEach(emit_spec, &c);

    /* tombstones */
    uint32_t nt = 0;
    for (size_t i = 0; i < g_tlen; i++)
        if (g_tombs[i].used)
            nt++;
    buf_append_u32(out, nt);
    for (size_t i = 0; i < g_tlen; i++) {
        if (!g_tombs[i].used)
            continue;
        buf_append_u32(out, (uint32_t)g_tombs[i].idlen);
        buf_append(out, g_tombs[i].id, g_tombs[i].idlen);
        buf_append_u64(out, g_tombs[i].version);
        buf_append_u16(out, (uint16_t)strlen(g_tombs[i].origin));
        buf_append_str(out, g_tombs[i].origin);
    }
}

void gossip_merge_state(const void *data, size_t len)
{
    struct rdr rd;
    rdr_init(&rd, data, len);

    uint32_t nspecs = rd_u32(&rd);
    for (uint32_t i = 0; i < nspecs && !rd.err; i++) {
        res_t *in = wire_get_res(&rd);
        if (in == NULL)
            break;
        gossip_merge_res(in);
    }

    uint32_t ntombs = rd_u32(&rd);
    for (uint32_t i = 0; i < ntombs && !rd.err; i++) {
        uint32_t idlen = rd_u32(&rd);
        const void *id = rd_bytes(&rd, idlen);
        uint64_t ver = rd_u64(&rd);
        uint16_t olen = rd_u16(&rd);
        const char *origin = rd_bytes(&rd, olen);
        if (rd.err || id == NULL || origin == NULL)
            break;
        if (idlen != RES_ID_LEN)
            continue;           /* not an id we could ever have issued */
        char ob[128];
        snprintf(ob, sizeof ob, "%.*s", (int)olen, origin);
        gossip_witness(ver);

        /* copy the id: rd points into the caller's buffer */
        unsigned char idbuf[RES_ID_LEN];
        memcpy(idbuf, id, RES_ID_LEN);

        res_t *cur = StoreGet(idbuf, RES_ID_LEN);
        if (cur != NULL) {
            uint64_t curver = (uint64_t)ResGetInt(cur, NOUN_VERSION);
            if (ver >= curver) {
                tomb_set(idbuf, RES_ID_LEN, ver, ob);
                announce(GOSSIP_DELETE, cur, idbuf, RES_ID_LEN);
                StoreRemove(idbuf, RES_ID_LEN);
            }
        } else {
            tomb_set(idbuf, RES_ID_LEN, ver, ob);
        }
    }
}
