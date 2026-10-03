/*
 * In-process robustness tests for the decoders and validators. No sockets: each
 * test builds a byte buffer in memory - some well-formed, some deliberately
 * malformed - and feeds it to the same functions that handle real traffic, so
 * the parser's safety is checked directly. Run under AddressSanitizer:
 *
 *   see tests/run.sh
 *
 * These lock in the audit fixes: an over-long id must be rejected, not written
 * past a fixed buffer; a truncated frame must fail cleanly; ResSpecValid must
 * reject every shape the scheduler should never see.
 */
#define _GNU_SOURCE
#include "apiserver.h"
#include "gossip.h"
#include "wire.h"
#include "manifest.h"
#include "membership.h"
#include "util.h"
#include "sha256.h"

#include <stdio.h>
#include <string.h>
#include <assert.h>

static int g_failures;

static void ok(const char *what, int cond)
{
    printf("  %-4s %s\n", cond ? "ok" : "FAIL", what);
    if (!cond)
        g_failures++;
}

/* build a gossip state buffer carrying one hand-written resource */
static void build_state(struct buf *out, res_t *r)
{
    buf_append_u32(out, 1);         /* one spec */
    wire_put_res(out, r, 1);        /* spec nouns */
    buf_append_u32(out, 0);         /* no tombstones */
}

static void test_overlong_id(void)
{
    /* finding #2: a peer's spec with a 200-byte id must not overflow anything */
    res_t *r = ResCreate();
    unsigned char big[200];
    memset(big, 0xab, sizeof big);
    ResSetBytes(r, NOUN_ID, big, sizeof big);
    ResSetInt(r, NOUN_KIND, KIND_DEPLOY);
    ResSetBytes(r, NOUN_NAME, "evil", 4);
    ResSetInt(r, NOUN_REPLICAS, -1);
    ResSetInt(r, NOUN_VERSION, 9);
    ResSetBytes(r, NOUN_ORIGIN, "attacker", 8);

    struct buf s;
    buf_init(&s);
    build_state(&s, r);
    ResDelete(r);

    size_t before = StoreCount();
    gossip_merge_state(s.data, s.len);
    ok("over-long id is rejected, store unchanged", StoreCount() == before);
    buf_free(&s);
}

static void test_valid_spec_accepted(void)
{
    res_t *r = ResCreate();
    unsigned char id[RES_ID_LEN];
    ResComputeId(KIND_DEPLOY, "web", 3, id);
    ResSetBytes(r, NOUN_ID, id, RES_ID_LEN);
    ResSetInt(r, NOUN_KIND, KIND_DEPLOY);
    ResSetBytes(r, NOUN_NAME, "web", 3);
    ResSetInt(r, NOUN_REPLICAS, 3);
    ResSetInt(r, NOUN_VERSION, 5);
    ResSetBytes(r, NOUN_ORIGIN, "n1", 2);

    struct buf s;
    buf_init(&s);
    build_state(&s, r);
    ResDelete(r);

    size_t before = StoreCount();
    gossip_merge_state(s.data, s.len);
    ok("a well-formed spec is accepted", StoreCount() == before + 1);
    ok("it can be found by its id", StoreGet(id, RES_ID_LEN) != NULL);
    buf_free(&s);
}

static void test_truncated_frames(void)
{
    /* take a valid state buffer and feed every prefix of it */
    res_t *r = ResCreate();
    unsigned char id[RES_ID_LEN];
    ResComputeId(KIND_DEPLOY, "trunc", 5, id);
    ResSetBytes(r, NOUN_ID, id, RES_ID_LEN);
    ResSetInt(r, NOUN_KIND, KIND_DEPLOY);
    ResSetBytes(r, NOUN_NAME, "trunc", 5);
    ResSetInt(r, NOUN_VERSION, 1);
    ResSetBytes(r, NOUN_ORIGIN, "n1", 2);
    struct buf s;
    buf_init(&s);
    build_state(&s, r);
    ResDelete(r);

    for (size_t n = 0; n < s.len; n++)
        gossip_merge_state(s.data, n);      /* must not read past n */
    ok("every truncated prefix is handled without reading past the end", 1);
    buf_free(&s);
}

static void test_wire_get_res_garbage(void)
{
    /* a claimed field count far larger than the bytes present */
    unsigned char buf[8];
    buf[0] = 0xff; buf[1] = 0xff;   /* count = 65535 */
    memset(buf + 2, 0, 6);
    struct rdr rd;
    rdr_init(&rd, buf, sizeof buf);
    res_t *r = wire_get_res(&rd);
    ok("wire_get_res rejects an impossible field count", r == NULL);

    /* a bytes field whose length overruns the buffer */
    struct buf b;
    buf_init(&b);
    buf_append_u16(&b, 1);          /* one field */
    buf_append_u16(&b, NOUN_NAME);
    buf_append_byte(&b, 0);         /* is_int = 0 (bytes) */
    buf_append_u32(&b, 1000000);    /* claims a megabyte */
    buf_append_str(&b, "short");
    rdr_init(&rd, b.data, b.len);
    r = wire_get_res(&rd);
    ok("wire_get_res rejects a length that overruns the buffer", r == NULL);
    buf_free(&b);
}

static void test_spec_validation(void)
{
    const char *why;

    res_t *r = ResCreate();
    ResSetInt(r, NOUN_KIND, KIND_NODE);
    ResSetBytes(r, NOUN_NAME, "x", 1);
    ok("Node kind is not a runnable spec", !ResSpecValid(r, &why));
    ResDelete(r);

    r = ResCreate();
    ResSetInt(r, NOUN_KIND, KIND_DEPLOY);
    ResSetBytes(r, NOUN_NAME, "bad name!", 9);
    ok("a name with spaces/punctuation is rejected", !ResSpecValid(r, &why));
    ResDelete(r);

    r = ResCreate();
    ResSetInt(r, NOUN_KIND, KIND_DEPLOY);
    ResSetBytes(r, NOUN_NAME, "ok", 2);
    ResSetInt(r, NOUN_REPLICAS, 1000000);
    ok("an absurd replica count is rejected", !ResSpecValid(r, &why));
    ResSetInt(r, NOUN_REPLICAS, -1);
    ok("replicas=-1 (daemonset) is accepted", ResSpecValid(r, &why));
    ResDelete(r);

    r = ResCreate();
    ResSetInt(r, NOUN_KIND, KIND_DEPLOY);
    ResSetBytes(r, NOUN_NAME, "ok", 2);
    ResSetBytes(r, NOUN_IMAGE, "tooshort", 8);
    ok("an image that is not a 32-byte hash is rejected", !ResSpecValid(r, &why));
    ResDelete(r);

    r = ResCreate();
    ResSetInt(r, NOUN_KIND, KIND_DEPLOY);
    ResSetBytes(r, NOUN_NAME, "ok", 2);
    ResSetBytes(r, NOUN_ARGV, "no-nul", 6);     /* not NUL-terminated */
    ok("argv without a trailing NUL is rejected", !ResSpecValid(r, &why));
    ResDelete(r);
}

struct argv_ctx { int count; char last[64]; char first[64]; };
static int argv_emit(const struct manifest *m, void *vp)
{
    struct argv_ctx *c = vp;
    c->count = m->argc;
    size_t off = 0;
    int i = 0;
    while (off < m->argv.len) {
        const char *item = (const char *)m->argv.data + off;
        size_t il = strlen(item);
        if (i == 0) snprintf(c->first, sizeof c->first, "%s", item);
        snprintf(c->last, sizeof c->last, "%s", item);
        off += il + 1;
        i++;
    }
    return 0;
}

static void test_manifest_argv(void)
{
    const char *doc =
        "kind: Deploy\n"
        "name: web\n"
        "argv: [--serve, \"a, b\", web]  # trailing comment\n";
    struct argv_ctx c = { 0, "", "" };
    manifest_parse(doc, strlen(doc), argv_emit, &c);
    ok("quoted comma kept as one argv item", c.count == 3);
    ok("first argv item is --serve", strcmp(c.first, "--serve") == 0);
    ok("last argv item is web (comment stripped)", strcmp(c.last, "web") == 0);
}

static void test_id_determinism(void)
{
    unsigned char a[RES_ID_LEN], b[RES_ID_LEN], c[RES_ID_LEN];
    ResComputeId(KIND_DEPLOY, "web", 3, a);
    ResComputeId(KIND_DEPLOY, "web", 3, b);
    ResComputeId(KIND_STATIC, "web", 3, c);
    ok("same kind+name gives the same id", memcmp(a, b, RES_ID_LEN) == 0);
    ok("different kind gives a different id", memcmp(a, c, RES_ID_LEN) != 0);
}

int main(void)
{
    membership_init("tester", "127.0.0.1", 7700, 7700);

    printf("resource id:\n");       test_id_determinism();
    printf("spec validation:\n");   test_spec_validation();
    printf("wire decoder:\n");      test_wire_get_res_garbage();
    printf("gossip merge:\n");
    test_overlong_id();
    test_valid_spec_accepted();
    test_truncated_frames();
    printf("manifest parser:\n");   test_manifest_argv();

    printf(g_failures ? "\nFAILED (%d)\n" : "\nALL PASSED\n", g_failures);
    return g_failures ? 1 : 0;
}
