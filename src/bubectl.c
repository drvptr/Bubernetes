#define _GNU_SOURCE
#include "apiserver.h"
#include "wire.h"
#include "tls.h"
#include "manifest.h"
#include "sha256.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * bubectl: the command-line client. It speaks the same wire protocol nodes use
 * among themselves - there is no privileged admin channel - and it can talk to
 * any node, because every node holds the whole desired state. Where it reaches
 * the cluster is read from .bube/config, exactly as kubectl reads .kube/config.
 *
 *   bubectl apply -f file.yaml     define or update resources
 *   bubectl get [nodes|deploy|static|all]
 *   bubectl delete NAME
 *   bubectl dump                   the whole cluster's desired state as YAML
 */

static char g_ip[64] = "127.0.0.1";
static int  g_port = 7700;

static void load_config(void)
{
    const char *env = getenv("BUBE_SERVER");
    char path[1024];
    const char *home = getenv("HOME");
    FILE *f = fopen(".bube/config", "r");
    if (f == NULL && home != NULL) {
        snprintf(path, sizeof path, "%s/.bube/config", home);
        f = fopen(path, "r");
    }
    if (f != NULL) {
        char line[256];
        while (fgets(line, sizeof line, f) != NULL) {
            char *c = strchr(line, ':');
            if (c != NULL && strncmp(line, "server", 6) == 0) {
                char *val = c + 1;
                while (*val == ' ') val++;
                char *nl = strchr(val, '\n');
                if (nl) *nl = '\0';
                char *cc = strrchr(val, ':');
                if (cc != NULL) {
                    *cc = '\0';
                    snprintf(g_ip, sizeof g_ip, "%s", val);
                    g_port = atoi(cc + 1);
                }
            }
        }
        fclose(f);
    }
    if (env != NULL) {
        char *cc = strrchr(env, ':');
        if (cc != NULL) {
            size_t n = (size_t)(cc - env);
            if (n >= sizeof g_ip) n = sizeof g_ip - 1;
            memcpy(g_ip, env, n);
            g_ip[n] = '\0';
            g_port = atoi(cc + 1);
        }
    }
}

static void res_id(int kind, const char *name, size_t nlen, unsigned char out[SHA256_LEN])
{
    struct sha256 s;
    sha256_init(&s);
    const char *kn = KindName(kind);
    sha256_update(&s, kn, strlen(kn));
    sha256_update(&s, "/", 1);
    sha256_update(&s, name, nlen);
    sha256_final(&s, out);
}

/* ----------------------------------------------------------- apply */

static int read_file(const char *path, struct buf *out)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return -1;
    buf_reset(out);
    char tmp[65536];
    size_t r;
    while ((r = fread(tmp, 1, sizeof tmp, f)) > 0)
        buf_append(out, tmp, r);
    fclose(f);
    return 0;
}

static int apply_emit(const struct manifest *m, void *ctx)
{
    (void)ctx;
    int kind = KindFromName(m->kind);
    if (kind < 0)
        return 0;

    res_t *spec = ResCreate();
    unsigned char id[SHA256_LEN];
    res_id(kind, m->name, strlen(m->name), id);
    ResSetBytes(spec, NOUN_ID, id, SHA256_LEN);
    ResSetInt(spec, NOUN_KIND, kind);
    ResSetBytes(spec, NOUN_NAME, m->name, strlen(m->name));
    if (m->has_replicas)
        ResSetInt(spec, NOUN_REPLICAS, m->replicas);
    if (m->argv.len > 0)
        ResSetBytes(spec, NOUN_ARGV, m->argv.data, m->argv.len);

    /* resolve the image: a path becomes bytes we ship so the cluster has them;
     * a 64-hex value is assumed already present somewhere in the cluster */
    struct buf img;
    buf_init(&img);
    int have_img = 0;
    if (m->image[0] != '\0') {
        unsigned char hash[SHA256_LEN];
        if (strlen(m->image) == 2 * SHA256_LEN &&
            hex_decode(hash, sizeof hash, m->image) == SHA256_LEN) {
            ResSetBytes(spec, NOUN_IMAGE, hash, SHA256_LEN);
        } else if (read_file(m->image, &img) == 0) {
            sha256_hash(img.data, img.len, hash);
            ResSetBytes(spec, NOUN_IMAGE, hash, SHA256_LEN);
            have_img = 1;
        } else {
            fprintf(stderr, "warning: cannot read image '%s'\n", m->image);
        }
    }

    struct buf payload;
    buf_init(&payload);
    wire_put_res(&payload, spec, 1);
    buf_append_u32(&payload, have_img ? (uint32_t)img.len : 0);
    if (have_img)
        buf_append(&payload, img.data, img.len);

    int rtype = 0;
    struct buf reply;
    buf_init(&reply);
    int rc = wire_call(g_ip, g_port, "", MSG_APPLY, payload.data, payload.len,
                       &rtype, &reply);
    if (rc == 0 && rtype == MSG_APPLY_RESP && reply.len >= 1 && reply.data[0] == W_OK)
        printf("applied %s '%s' (replicas=%ld)\n", KindName(kind), m->name,
               m->has_replicas ? m->replicas : 1);
    else
        fprintf(stderr, "failed to apply '%s'\n", m->name);

    buf_free(&reply);
    buf_free(&payload);
    buf_free(&img);
    ResDelete(spec);
    return 0;
}

static int cmd_apply(const char *file)
{
    struct buf content;
    buf_init(&content);
    if (read_file(file, &content) != 0) {
        fprintf(stderr, "cannot read %s\n", file);
        return 1;
    }
    manifest_parse((char *)content.data, content.len, apply_emit, NULL);
    buf_free(&content);
    return 0;
}

/* ----------------------------------------------------------- get / dump */

static void print_row(res_t *r)
{
    size_t nlen;
    const char *name = ResGetBytes(r, NOUN_NAME, &nlen);
    int kind = (int)ResGetInt(r, NOUN_KIND);
    char rep[24] = "-";
    if (ResHas(r, NOUN_REPLICAS)) {
        long v = (long)ResGetInt(r, NOUN_REPLICAS);
        if (v < 0) snprintf(rep, sizeof rep, "all");
        else snprintf(rep, sizeof rep, "%ld", v);
    }
    const char *status = ResHas(r, NOUN_STATUS) ?
                         StatusName((int)ResGetInt(r, NOUN_STATUS)) : "-";
    char running[16] = "-";
    if (ResHas(r, NOUN_RUNNING))
        snprintf(running, sizeof running, "%ld", (long)ResGetInt(r, NOUN_RUNNING));

    char extra[96] = "";
    size_t alen;
    const char *addr = ResGetBytes(r, NOUN_ADDRESS, &alen);
    if (addr != NULL)
        snprintf(extra, sizeof extra, "%.*s", (int)alen, addr);
    else {
        size_t olen;
        const char *origin = ResGetBytes(r, NOUN_ORIGIN, &olen);
        if (origin != NULL)
            snprintf(extra, sizeof extra, "@%.*s v%ld", (int)olen, origin,
                     (long)ResGetInt(r, NOUN_VERSION));
    }

    printf("%-16.*s %-8s %-5s %-12s %-8s %s\n",
           (int)(name ? nlen : 1), name ? name : "?",
           KindName(kind), rep, status, running, extra);
}

struct get_ctx { int want_kind; int any; };

static void get_emit(res_t *r, void *vp)
{
    struct get_ctx *g = vp;
    int kind = (int)ResGetInt(r, NOUN_KIND);
    if (g->want_kind >= 0 && kind != g->want_kind)
        return;
    if (g->want_kind < 0 && kind == KIND_NODE)
        return;     /* default view hides nodes; use `get nodes` */
    print_row(r);
    g->any = 1;
}

static int cmd_get(const char *what)
{
    int want = -1;
    if (what != NULL) {
        if (strcmp(what, "nodes") == 0 || strcmp(what, "node") == 0) want = KIND_NODE;
        else if (strcmp(what, "deploy") == 0) want = KIND_DEPLOY;
        else if (strcmp(what, "static") == 0) want = KIND_STATIC;
        else if (strcmp(what, "all") == 0) want = -1;
    }

    int rtype = 0;
    struct buf reply;
    buf_init(&reply);
    if (wire_call(g_ip, g_port, "", MSG_LIST_REQ, NULL, 0, &rtype, &reply) != 0 ||
        rtype != MSG_LIST_RESP) {
        fprintf(stderr, "cannot reach bubelet at %s:%d\n", g_ip, g_port);
        buf_free(&reply);
        return 1;
    }

    printf("%-16s %-8s %-5s %-12s %-8s %s\n",
           "NAME", "KIND", "REPL", "STATUS", "RUNNING", "NODE/ORIGIN");

    struct rdr rd;
    rdr_init(&rd, reply.data, reply.len);
    uint32_t n = rd_u32(&rd);
    struct get_ctx g = { want, 0 };
    for (uint32_t i = 0; i < n && !rd.err; i++) {
        res_t *r = wire_get_res(&rd);
        if (r == NULL)
            break;
        get_emit(r, &g);
        ResDelete(r);
    }
    buf_free(&reply);
    return 0;
}

static int cmd_dump(void)
{
    int rtype = 0;
    struct buf reply;
    buf_init(&reply);
    if (wire_call(g_ip, g_port, "", MSG_LIST_REQ, NULL, 0, &rtype, &reply) != 0 ||
        rtype != MSG_LIST_RESP) {
        fprintf(stderr, "cannot reach bubelet at %s:%d\n", g_ip, g_port);
        buf_free(&reply);
        return 1;
    }

    struct rdr rd;
    rdr_init(&rd, reply.data, reply.len);
    uint32_t n = rd_u32(&rd);
    struct buf out;
    buf_init(&out);
    for (uint32_t i = 0; i < n && !rd.err; i++) {
        res_t *r = wire_get_res(&rd);
        if (r == NULL)
            break;
        int kind = (int)ResGetInt(r, NOUN_KIND);
        if (kind == KIND_STATIC || kind == KIND_DEPLOY)
            manifest_dump_res(&out, r);
        ResDelete(r);
    }
    fwrite(out.data, 1, out.len, stdout);
    buf_free(&out);
    buf_free(&reply);
    return 0;
}

static int cmd_delete(const char *name)
{
    int rtype = 0;
    struct buf reply;
    buf_init(&reply);
    int rc = wire_call(g_ip, g_port, "", MSG_DELETE, name, strlen(name),
                       &rtype, &reply);
    if (rc == 0 && rtype == MSG_DELETE_RESP && reply.len >= 1 &&
        reply.data[0] == W_OK)
        printf("deleted '%s'\n", name);
    else
        fprintf(stderr, "could not delete '%s' (not found?)\n", name);
    buf_free(&reply);
    return 0;
}

static void usage(const char *p)
{
    fprintf(stderr,
        "usage:\n"
        "  %s apply -f FILE\n"
        "  %s get [nodes|deploy|static|all]\n"
        "  %s delete NAME\n"
        "  %s dump\n"
        "server is read from .bube/config or $BUBE_SERVER (ip:port)\n",
        p, p, p, p);
}

int main(int argc, char **argv)
{
    load_config();

    /* allow leading --server IP:PORT and --certs DIR overrides */
    int a = 1;
    while (a + 1 < argc) {
        if (strcmp(argv[a], "--server") == 0) {
            char *cc = strrchr(argv[a + 1], ':');
            if (cc != NULL) {
                *cc = '\0';
                snprintf(g_ip, sizeof g_ip, "%s", argv[a + 1]);
                g_port = atoi(cc + 1);
            }
            a += 2;
        } else if (strcmp(argv[a], "--certs") == 0) {
            tls_setup(argv[a + 1]);     /* talk TLS to nodes that require it */
            a += 2;
        } else {
            break;
        }
    }

    if (a >= argc) {
        usage(argv[0]);
        return 2;
    }

    if (strcmp(argv[a], "apply") == 0) {
        if (a + 2 < argc && strcmp(argv[a + 1], "-f") == 0)
            return cmd_apply(argv[a + 2]);
        usage(argv[0]);
        return 2;
    }
    if (strcmp(argv[a], "get") == 0)
        return cmd_get(a + 1 < argc ? argv[a + 1] : NULL);
    if (strcmp(argv[a], "delete") == 0 && a + 1 < argc)
        return cmd_delete(argv[a + 1]);
    if (strcmp(argv[a], "dump") == 0)
        return cmd_dump();

    usage(argv[0]);
    return 2;
}
