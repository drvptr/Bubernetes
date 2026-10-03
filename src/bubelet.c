#define _GNU_SOURCE
#include "apiserver.h"
#include "wire.h"
#include "tls.h"
#include "manifest.h"
#include "membership.h"
#include "gossip.h"
#include "reconciler.h"
#include "blob.h"
#include "exec.h"
#include "event.h"
#include "sha256.h"
#include "log.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <pthread.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/stat.h>

/*
 * bubelet: the whole of Bubernetes in one process. Every node runs this and
 * nothing else - at once the API, the scheduler, the kubelet and the controller,
 * because once placement is a pure function of replicated state there is nothing
 * left for separate services to do.
 *
 * Concurrency. One lock, and three kinds of thread:
 *   - the main thread owns the event loop: SWIM, the manifest directory,
 *     signals, reconcile. It never waits on a remote node.
 *   - one worker thread owns the blocking outbound calls (gossip rounds, image
 *     fetches), so a slow peer can neither stall the loop nor deadlock two
 *     nodes against each other.
 *   - each inbound TCP request is served by a short-lived thread, so a client
 *     that connects and then dawdles costs itself a thread, not the cluster its
 *     node. There is a cap on how many may be in flight at once.
 * Shared state sits behind g_lock, which is held only across in-memory work and
 * never across a network call or a handshake.
 */

#define MAX_EV 64
#define MAX_INFLIGHT 64         /* inbound requests being served at once */

static volatile sig_atomic_t g_stop;
static volatile sig_atomic_t g_dirty;   /* desired state changed; reconcile soon */

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static void lock(void)   { pthread_mutex_lock(&g_lock); }
static void unlock(void) { pthread_mutex_unlock(&g_lock); }

static struct {
    char name[128];
    char ip[64];
    int  port;                  /* one port, used for SWIM/UDP and wire/TCP */
    char manifests[1024];
    char certs[1024];
} g_cfg;

static int g_udp_fd = -1;
static int g_tcp_fd = -1;
static int g_inflight;          /* under g_lock */

/* ----------------------------------------------------------- helpers */

static void mkdir_p(const char *path)
{
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p != '\0'; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

/* write a whole file atomically: nobody - including our own inotify-driven
 * rescan - ever sees a half-written manifest */
static int write_file_atomic(const char *path, const void *data, size_t len)
{
    char tmp[1200];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (f == NULL)
        return -1;
    int ok = fwrite(data, 1, len, f) == len;
    if (fclose(f) != 0)
        ok = 0;
    if (!ok || rename(tmp, path) != 0) {
        remove(tmp);
        return -1;
    }
    return 0;
}

static int read_whole(const char *path, struct buf *out)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return -1;
    buf_reset(out);
    char tmp[8192];
    size_t r;
    while ((r = fread(tmp, 1, sizeof tmp, f)) > 0)
        buf_append(out, tmp, r);
    fclose(f);
    return 0;
}

/* ----------------------------------------------------- persistence
 *
 * Two kinds of file share the manifest directory. Operators write EDIT files
 * (any name, no `version:`), and bubelet never touches those. bubelet writes
 * STATE files, <name>.state.yaml, carrying the version and origin the cluster
 * agreed on; on restart they are reloaded at that version under the same
 * last-writer-wins rules as gossip, so the node resumes rather than re-edits.
 */

static void state_path(char *out, size_t cap, const char *name, size_t nlen)
{
    snprintf(out, cap, "%s/%.*s.state.yaml", g_cfg.manifests, (int)nlen, name);
}

static void persist_res(res_t *r)
{
    size_t nlen;
    const char *name = ResGetBytes(r, NOUN_NAME, &nlen);
    if (name == NULL || !ResHas(r, NOUN_VERSION))
        return;
    char path[1400];
    state_path(path, sizeof path, name, nlen);

    struct buf doc;
    buf_init(&doc);
    manifest_dump_res(&doc, r);

    /* write only if the bytes differ, so we do not fight our own inotify watch */
    struct buf cur;
    buf_init(&cur);
    int same = read_whole(path, &cur) == 0 &&
               cur.len == doc.len && memcmp(cur.data, doc.data, doc.len) == 0;
    buf_free(&cur);
    if (!same && write_file_atomic(path, doc.data, doc.len) != 0)
        PERR("persist %s", path);
    buf_free(&doc);
}

static void remove_res_file(res_t *r)
{
    if (r == NULL)
        return;
    size_t nlen;
    const char *name = ResGetBytes(r, NOUN_NAME, &nlen);
    if (name == NULL)
        return;
    char path[1400];
    state_path(path, sizeof path, name, nlen);
    remove(path);
}

/* gossip / loader tells us the desired state changed (always called under lock) */
static void on_desired_change(int op, res_t *r, const void *id, size_t idlen,
                              void *ctx)
{
    (void)id;
    (void)idlen;
    (void)ctx;
    if (op == GOSSIP_UPSERT && r != NULL)
        persist_res(r);
    else if (op == GOSSIP_DELETE)
        remove_res_file(r);
    g_dirty = 1;
}

/* ----------------------------------------------------- image ingest cache
 *
 * An edit file names its image by path. Hashing a multi-megabyte binary on
 * every look would be silly, so remember (path, mtime, size) -> hash. */

struct ingest_rec {
    char path[2100];            /* manifests dir + relative image path */
    int64_t mtime;
    int64_t size;
    unsigned char hash[SHA256_LEN];
    int used;
};
static struct ingest_rec g_ingest[64];

static int ingest_image(const char *path, unsigned char hash[SHA256_LEN])
{
    struct stat st;
    if (stat(path, &st) != 0)
        return -1;
    for (size_t i = 0; i < sizeof g_ingest / sizeof g_ingest[0]; i++) {
        struct ingest_rec *ir = &g_ingest[i];
        if (ir->used && strcmp(ir->path, path) == 0 &&
            ir->mtime == (int64_t)st.st_mtime && ir->size == (int64_t)st.st_size &&
            blob_has(ir->hash)) {
            memcpy(hash, ir->hash, SHA256_LEN);
            return 0;
        }
    }
    if (blob_ingest_file(path, hash) != 0)
        return -1;
    struct ingest_rec *slot = &g_ingest[0];
    for (size_t i = 0; i < sizeof g_ingest / sizeof g_ingest[0]; i++)
        if (!g_ingest[i].used) { slot = &g_ingest[i]; break; }
    snprintf(slot->path, sizeof slot->path, "%s", path);
    slot->mtime = (int64_t)st.st_mtime;
    slot->size = (int64_t)st.st_size;
    memcpy(slot->hash, hash, SHA256_LEN);
    slot->used = 1;
    INFO("ingested image %s", path);
    return 0;
}

/* ----------------------------------------------------- loader
 *
 * The directory is scanned as a set of files, each remembered by content hash
 * and by the resource ids it defined. A file whose content has not changed is
 * not re-read, and - the important part - never re-authored: the cluster's
 * agreed state stands until somebody actually edits the file. Only a file that
 * disappears retracts the resources it defined (and only if no other file still
 * defines them), and only when the directory could be listed completely; a
 * failed listing is an error, not a mass deletion.
 *
 * The content hashes are persisted to .scan so that after a restart an
 * unchanged edit file is still known to be unchanged. Without that, every
 * restart would re-edit every resource and undo whatever the cluster had since
 * agreed on.
 */

struct filerec {
    char fname[256];
    unsigned char hash[SHA256_LEN];
    struct buf ids;             /* RES_ID_LEN each */
    int have_ids;               /* parsed at least once this run */
    int seen;                   /* present in the current listing */
    int used;
};
static struct filerec *g_files;
static size_t g_nfiles, g_fcap;
static int g_scan_loaded;

static struct filerec *file_find(const char *fname)
{
    for (size_t i = 0; i < g_nfiles; i++)
        if (g_files[i].used && strcmp(g_files[i].fname, fname) == 0)
            return &g_files[i];
    return NULL;
}

static struct filerec *file_alloc(const char *fname)
{
    struct filerec *f = file_find(fname);
    if (f != NULL)
        return f;
    for (size_t i = 0; i < g_nfiles; i++)
        if (!g_files[i].used) { f = &g_files[i]; break; }
    if (f == NULL) {
        if (g_nfiles == g_fcap) {
            g_fcap = g_fcap ? g_fcap * 2 : 16;
            g_files = xrealloc(g_files, g_fcap * sizeof *g_files);
        }
        f = &g_files[g_nfiles++];
    }
    memset(f, 0, sizeof *f);
    snprintf(f->fname, sizeof f->fname, "%s", fname);
    buf_init(&f->ids);
    f->used = 1;
    return f;
}

static int id_in(struct buf *set, const unsigned char id[RES_ID_LEN])
{
    for (size_t off = 0; off + RES_ID_LEN <= set->len; off += RES_ID_LEN)
        if (memcmp(set->data + off, id, RES_ID_LEN) == 0)
            return 1;
    return 0;
}

/* does any other current file still define this id? */
static int id_defined_elsewhere(struct filerec *except,
                                const unsigned char id[RES_ID_LEN])
{
    for (size_t i = 0; i < g_nfiles; i++) {
        struct filerec *f = &g_files[i];
        if (!f->used || !f->seen || f == except)
            continue;
        if (id_in(&f->ids, id))
            return 1;
    }
    return 0;
}

static void scan_save(void)
{
    struct buf out;
    buf_init(&out);
    char line[600];
    for (size_t i = 0; i < g_nfiles; i++) {
        struct filerec *f = &g_files[i];
        if (!f->used)
            continue;
        char hex[2 * SHA256_LEN + 1];
        hex_encode(hex, f->hash, SHA256_LEN);
        snprintf(line, sizeof line, "%s %s\n", hex, f->fname);
        buf_append_str(&out, line);
    }
    char path[1100];
    snprintf(path, sizeof path, "%s/.scan", g_cfg.manifests);
    write_file_atomic(path, out.data, out.len);
    buf_free(&out);
}

static void scan_load(void)
{
    char path[1100];
    snprintf(path, sizeof path, "%s/.scan", g_cfg.manifests);
    FILE *f = fopen(path, "r");
    if (f == NULL)
        return;
    char hex[2 * SHA256_LEN + 1], fname[256];
    while (fscanf(f, "%64s %255s", hex, fname) == 2) {
        unsigned char h[SHA256_LEN];
        if (hex_decode(h, sizeof h, hex) != SHA256_LEN)
            continue;
        struct filerec *fr = file_alloc(fname);
        memcpy(fr->hash, h, SHA256_LEN);
        fr->have_ids = 0;       /* known unchanged, but we must still learn its ids */
    }
    fclose(f);
}

/* context for parsing one file */
struct emit_ctx {
    struct buf *ids;
    int changed;        /* file content differs from last time we saw it */
    int ndocs;
};

static int loader_emit(const struct manifest *m, void *vp)
{
    struct emit_ctx *ec = vp;

    int kind = KindFromName(m->kind);
    if (kind < 0)
        return 0;
    ec->ndocs++;

    res_t *tmp = ResCreate();
    unsigned char id[RES_ID_LEN];
    ResComputeId(kind, m->name, strlen(m->name), id);
    ResSetBytes(tmp, NOUN_ID, id, RES_ID_LEN);
    ResSetInt(tmp, NOUN_KIND, kind);
    ResSetBytes(tmp, NOUN_NAME, m->name, strlen(m->name));
    if (m->has_replicas)
        ResSetInt(tmp, NOUN_REPLICAS, m->replicas);
    if (m->argv.len > 0)
        ResSetBytes(tmp, NOUN_ARGV, m->argv.data, m->argv.len);

    if (m->image[0] != '\0') {
        unsigned char hash[SHA256_LEN];
        if (strlen(m->image) == 2 * SHA256_LEN &&
            hex_decode(hash, sizeof hash, m->image) == SHA256_LEN) {
            ResSetBytes(tmp, NOUN_IMAGE, hash, SHA256_LEN);   /* already a hash */
        } else {
            /* a path: relative ones are relative to the manifest directory */
            char path[2100];
            if (m->image[0] == '/')
                snprintf(path, sizeof path, "%s", m->image);
            else
                snprintf(path, sizeof path, "%s/%s", g_cfg.manifests, m->image);
            if (ingest_image(path, hash) == 0)
                ResSetBytes(tmp, NOUN_IMAGE, hash, SHA256_LEN);
            else
                WARN("cannot read image '%s' for '%s'", path, m->name);
        }
    }

    const char *why;
    if (!ResSpecValid(tmp, &why)) {
        WARN("ignoring manifest '%s': %s", m->name, why);
        ResDelete(tmp);
        return 0;
    }

    if (m->has_version) {
        /* STATE: restore at its own version; last-writer-wins decides. State
         * files are mirrors of what the cluster agreed, not sources of intent,
         * so they are not recorded as definitions: removing one retracts
         * nothing, and an edit file's removal is not shielded by one. */
        ResSetInt(tmp, NOUN_VERSION, (int64_t)m->version);
        ResSetBytes(tmp, NOUN_ORIGIN, m->origin, strlen(m->origin));
        gossip_merge_res(tmp);          /* takes ownership */
        return 0;
    }

    /* EDIT: this file defines the resource; author a new version only if this
     * is a real change */
    buf_append(ec->ids, id, RES_ID_LEN);
    res_t *cur = StoreGet(id, RES_ID_LEN);
    if (cur == NULL) {
        if (gossip_is_tombstoned(id, RES_ID_LEN) && !ec->changed) {
            /* deleted through the API and the file was not touched since */
            ResDelete(tmp);
            return 0;
        }
        res_t *r = StoreCreate(id, RES_ID_LEN);
        ResCopySpec(r, tmp);
        gossip_author(r);
        INFO("resource '%s' (%s) defined", m->name, KindName(kind));
    } else if (ec->changed && !gossip_spec_equal(cur, tmp)) {
        ResCopySpec(cur, tmp);
        gossip_author(cur);
        INFO("resource '%s' (%s) updated", m->name, KindName(kind));
    }
    ResDelete(tmp);
    return 0;
}

static void scan_file(const char *fname, const unsigned char *data, size_t len,
                      void *ctx)
{
    (void)ctx;
    struct filerec *f = file_find(fname);
    if (data == NULL) {
        /* unreadable right now: not a reason to think it was removed */
        if (f != NULL)
            f->seen = 1;
        return;
    }

    unsigned char h[SHA256_LEN];
    sha256_hash(data, len, h);

    int changed = f == NULL || memcmp(f->hash, h, SHA256_LEN) != 0;
    if (f != NULL && !changed && f->have_ids) {
        f->seen = 1;
        return;                 /* nothing new here */
    }
    if (f == NULL)
        f = file_alloc(fname);

    struct buf ids;
    buf_init(&ids);
    struct emit_ctx ec = { &ids, changed, 0 };
    manifest_parse((const char *)data, len, loader_emit, &ec);

    /* a document dropped from a file that still parses is a retraction too */
    if (ec.ndocs > 0 && f->have_ids) {
        for (size_t off = 0; off + RES_ID_LEN <= f->ids.len; off += RES_ID_LEN) {
            const unsigned char *id = f->ids.data + off;
            if (!id_in(&ids, id) && !id_defined_elsewhere(f, id) &&
                StoreGet(id, RES_ID_LEN) != NULL) {
                INFO("document removed from %s; retracting resource", fname);
                gossip_delete(id, RES_ID_LEN);
            }
        }
    }

    buf_free(&f->ids);
    f->ids = ids;
    memcpy(f->hash, h, SHA256_LEN);
    f->have_ids = 1;
    f->seen = 1;
}

static void loader_scan(void)
{
    if (!g_scan_loaded) {
        scan_load();
        g_scan_loaded = 1;
    }
    for (size_t i = 0; i < g_nfiles; i++)
        g_files[i].seen = 0;

    if (manifest_list_dir(g_cfg.manifests, scan_file, NULL) != 0) {
        WARN("could not list %s; keeping previous state", g_cfg.manifests);
        return;         /* never mistake an unreadable directory for an empty one */
    }

    /* files that vanished retract what they alone defined */
    for (size_t i = 0; i < g_nfiles; i++) {
        struct filerec *f = &g_files[i];
        if (!f->used || f->seen)
            continue;
        for (size_t off = 0; off + RES_ID_LEN <= f->ids.len; off += RES_ID_LEN) {
            const unsigned char *id = f->ids.data + off;
            if (!id_defined_elsewhere(f, id) && StoreGet(id, RES_ID_LEN) != NULL) {
                INFO("manifest %s removed; retracting resource", f->fname);
                gossip_delete(id, RES_ID_LEN);
            }
        }
        buf_free(&f->ids);
        f->used = 0;
    }

    scan_save();
    g_dirty = 1;
}

/* ----------------------------------------------------- apply over wire */

static int ingest_spec(res_t *spec, const void *img, size_t imglen)
{
    int kind = (int)ResGetInt(spec, NOUN_KIND);
    size_t nlen;
    const char *name = ResGetBytes(spec, NOUN_NAME, &nlen);
    if (name == NULL)
        return -1;

    unsigned char id[RES_ID_LEN];
    ResComputeId(kind, name, nlen, id);
    ResSetBytes(spec, NOUN_ID, id, RES_ID_LEN);

    if (imglen > 0) {
        unsigned char hash[SHA256_LEN];
        blob_put(img, imglen, hash);
        ResSetBytes(spec, NOUN_IMAGE, hash, SHA256_LEN);
    }

    const char *why;
    if (!ResSpecValid(spec, &why)) {
        WARN("rejecting apply of '%.*s': %s", (int)nlen, name, why);
        return -1;
    }

    res_t *cur = StoreGet(id, RES_ID_LEN);
    if (cur == NULL) {
        res_t *r = StoreCreate(id, RES_ID_LEN);
        ResCopySpec(r, spec);
        gossip_author(r);
        INFO("applied '%.*s' (%s)", (int)nlen, name, KindName(kind));
    } else if (!gossip_spec_equal(cur, spec)) {
        ResCopySpec(cur, spec);
        gossip_author(cur);
        INFO("applied update to '%.*s'", (int)nlen, name);
    }
    return 0;
}

/* ----------------------------------------------------- request processing
 *
 * Pure in-memory handling of one inbound message: the caller holds g_lock and
 * does the socket read before and the socket write after, so the lock never
 * covers network I/O. Returns 1 if a reply was built. */

struct list_ctx { struct buf *out; uint32_t n; };

static void list_emit(res_t *r, void *vp)
{
    struct list_ctx *c = vp;
    wire_put_res(c->out, r, 0);     /* everything: spec + runtime */
    c->n++;
}

static int process_req(int type, struct buf *payload, int *rtype, struct buf *reply)
{
    if (type == MSG_GOSSIP_SYN) {
        gossip_merge_state(payload->data, payload->len);
        gossip_build_state(reply);
        *rtype = MSG_GOSSIP_ACK;
        return 1;

    } else if (type == MSG_LIST_REQ) {
        size_t at = reply->len;
        buf_append_u32(reply, 0);
        struct list_ctx lc = { reply, 0 };
        StoreForEach(list_emit, &lc);
        reply->data[at]     = (unsigned char)(lc.n >> 24);
        reply->data[at + 1] = (unsigned char)(lc.n >> 16);
        reply->data[at + 2] = (unsigned char)(lc.n >> 8);
        reply->data[at + 3] = (unsigned char)(lc.n);
        *rtype = MSG_LIST_RESP;
        return 1;

    } else if (type == MSG_APPLY) {
        struct rdr rd;
        rdr_init(&rd, payload->data, payload->len);
        res_t *spec = wire_get_res(&rd);
        uint32_t imglen = rd_u32(&rd);
        const void *img = rd_bytes(&rd, imglen);
        unsigned char st = W_ERR;
        if (spec != NULL && !rd.err && ingest_spec(spec, img, imglen) == 0)
            st = W_OK;
        if (spec != NULL)
            ResDelete(spec);
        buf_append_byte(reply, st);
        *rtype = MSG_APPLY_RESP;
        return 1;

    } else if (type == MSG_DELETE) {
        /* payload is "name" or "Kind/name" */
        char text[256];
        snprintf(text, sizeof text, "%.*s", (int)payload->len,
                 (char *)payload->data);
        int kind = -1;
        char *name = text;
        char *slash = strchr(text, '/');
        if (slash != NULL) {
            *slash = '\0';
            kind = KindFromName(text);
            name = slash + 1;
        }
        int matches = 0;
        res_t *r = StoreFindDesired(name, kind, &matches);
        unsigned char st = W_NOTFOUND;
        if (matches > 1) {
            st = W_AMBIGUOUS;
        } else if (r != NULL) {
            size_t idlen;
            const void *id = ResGetBytes(r, NOUN_ID, &idlen);
            unsigned char idbuf[RES_ID_LEN];
            if (id != NULL && idlen == RES_ID_LEN) {
                memcpy(idbuf, id, idlen);
                gossip_delete(idbuf, idlen);
                st = W_OK;
            }
        }
        buf_append_byte(reply, st);
        *rtype = MSG_DELETE_RESP;
        return 1;

    } else if (type == MSG_BLOB_REQ) {
        if (payload->len == SHA256_LEN) {
            size_t len;
            const void *bytes = blob_get(payload->data, &len);
            if (bytes != NULL) {
                buf_append_byte(reply, 1);
                buf_append_u32(reply, (uint32_t)len);
                buf_append(reply, bytes, len);
            } else {
                buf_append_byte(reply, 0);
            }
        } else {
            buf_append_byte(reply, 0);
        }
        *rtype = MSG_BLOB_RESP;
        return 1;

    } else if (type == MSG_REQ) {
        struct rdr rd;
        rdr_init(&rd, payload->data, payload->len);
        int verb, noun;
        const void *id, *data;
        size_t idlen, datalen;
        if (wire_get_req(&rd, &verb, &noun, &id, &idlen, &data, &datalen) != 0)
            return 0;
        res_t *r = StoreGet(id, idlen);
        if (verb == V_GET) {
            resp_t *v = r ? ResGet(r, noun) : NULL;
            wire_put_resp(reply, r ? W_OK : W_NOTFOUND, v);
            RespFree(v);
        } else if (verb == V_DELETE && r != NULL && ResIsDesired(r) &&
                   idlen == RES_ID_LEN) {
            unsigned char idbuf[RES_ID_LEN];
            memcpy(idbuf, id, idlen);
            gossip_delete(idbuf, idlen);
            wire_put_resp(reply, W_OK, NULL);
        } else {
            wire_put_resp(reply, W_NOTFOUND, NULL);
        }
        *rtype = MSG_RESP;
        return 1;
    }

    return 0;
}

/* ----------------------------------------------------- inbound threads */

static void *serve_conn(void *arg)
{
    struct conn *c = arg;
    if (wire_accept_handshake(c) == 0) {
        int type;
        struct buf payload;
        buf_init(&payload);
        if (wire_recv(c, &type, &payload) == 0) {
            int rtype = 0;
            struct buf reply;
            buf_init(&reply);
            lock();
            int send = process_req(type, &payload, &rtype, &reply);
            unlock();
            if (send)
                wire_send(c, rtype, reply.data, reply.len);
            buf_free(&reply);
        }
        buf_free(&payload);
    }
    wire_close(c);
    free(c);
    lock();
    g_inflight--;
    unlock();
    return NULL;
}

static void accept_all(void)
{
    for (;;) {
        struct conn c;
        if (wire_accept(g_tcp_fd, &c) != 0)
            break;      /* EAGAIN: drained */

        lock();
        int busy = g_inflight >= MAX_INFLIGHT;
        if (!busy)
            g_inflight++;
        unlock();
        if (busy) {
            wire_close(&c);     /* shed load rather than queue it */
            continue;
        }

        struct conn *heap = xmalloc(sizeof *heap);
        *heap = c;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_attr_setstacksize(&attr, 256 * 1024);
        pthread_t th;
        if (pthread_create(&th, &attr, serve_conn, heap) != 0) {
            wire_close(heap);
            free(heap);
            lock();
            g_inflight--;
            unlock();
        }
        pthread_attr_destroy(&attr);
    }
}

/* ----------------------------------------------------- node mirror */

struct mirror_ctx { struct buf *names; };

static void mirror_one(const struct member_view *m, void *vp)
{
    struct mirror_ctx *mc = vp;
    buf_append(mc->names, m->name, strlen(m->name));
    buf_append_byte(mc->names, '\0');

    unsigned char id[RES_ID_LEN];
    ResComputeId(KIND_NODE, m->name, strlen(m->name), id);
    res_t *r = StoreCreate(id, RES_ID_LEN);
    ResSetInt(r, NOUN_KIND, KIND_NODE);
    ResSetBytes(r, NOUN_NAME, m->name, strlen(m->name));
    char addr[96];
    snprintf(addr, sizeof addr, "%s:%d", m->ip, m->tcp_port);
    ResSetBytes(r, NOUN_ADDRESS, addr, strlen(addr));
    int st = m->state == M_ALIVE ? ST_READY :
             m->state == M_SUSPECT ? ST_UNHEALTHY : ST_UNREACHABLE;
    ResSetInt(r, NOUN_STATUS, st);
}

static void mirror_prune(res_t *r, void *vp)
{
    struct mirror_ctx *mc = vp;
    if ((int)ResGetInt(r, NOUN_KIND) != KIND_NODE)
        return;
    size_t nlen;
    const char *name = ResGetBytes(r, NOUN_NAME, &nlen);
    if (name == NULL)
        return;
    for (size_t off = 0; off < mc->names->len; ) {
        const char *cand = (char *)mc->names->data + off;
        size_t clen = strlen(cand);
        if (clen == nlen && memcmp(cand, name, nlen) == 0)
            return;
        off += clen + 1;
    }
    size_t idlen;
    const void *id = ResGetBytes(r, NOUN_ID, &idlen);
    if (id != NULL && idlen == RES_ID_LEN) {
        unsigned char idbuf[RES_ID_LEN];
        memcpy(idbuf, id, RES_ID_LEN);
        StoreRemove(idbuf, RES_ID_LEN);
    }
}

static void mirror_nodes(void)
{
    struct buf names;
    buf_init(&names);
    struct mirror_ctx mc = { &names };
    membership_foreach(mirror_one, &mc);
    StoreForEach(mirror_prune, &mc);
    buf_free(&names);
}

/* ----------------------------------------------------- blob gc */

static void collect_hash(res_t *r, void *vp)
{
    struct buf *set = vp;
    size_t il;
    const unsigned char *h = ResGetBytes(r, NOUN_IMAGE, &il);
    if (h != NULL && il == SHA256_LEN)
        buf_append(set, h, SHA256_LEN);
}

static int keep_blob(const unsigned char hash[SHA256_LEN], void *vp)
{
    struct buf *set = vp;
    for (size_t off = 0; off + SHA256_LEN <= set->len; off += SHA256_LEN)
        if (memcmp(set->data + off, hash, SHA256_LEN) == 0)
            return 1;
    return 0;
}

static void gc_blobs(void)
{
    struct buf referenced;
    buf_init(&referenced);
    DesiredForEach(collect_hash, &referenced);
    blob_sweep(keep_blob, &referenced);
    buf_free(&referenced);
}

/* ----------------------------------------------------- main-thread tasks */

static void task_swim(void *ctx)      { (void)ctx; lock(); membership_tick(g_udp_fd); unlock(); }
static void task_reconcile(void *ctx) { (void)ctx; lock(); reconcile_pass();          unlock(); }
static void task_mirror(void *ctx)    { (void)ctx; lock(); mirror_nodes();            unlock(); }
static void task_rescan(void *ctx)    { (void)ctx; lock(); loader_scan();             unlock(); }
static void task_gc(void *ctx)        { (void)ctx; lock(); gc_blobs();                unlock(); }

/* ----------------------------------------------------- worker thread */

/* a peer copied out from under the lock so we can call it without holding it */
struct peercopy { char ip[64]; int port; char name[128]; };

static int copy_peers(struct peercopy *out, int max)
{
    struct member_view pv[16];
    if (max > 16)
        max = 16;
    int k = membership_random_peers(pv, max);
    for (int i = 0; i < k; i++) {
        snprintf(out[i].ip, sizeof out[i].ip, "%s", pv[i].ip);
        out[i].port = pv[i].tcp_port;
        snprintf(out[i].name, sizeof out[i].name, "%s", pv[i].name);
    }
    return k;
}

/* images wanted by resources this node marked Pending */
struct cm { unsigned char (*out)[SHA256_LEN]; int max; int n; };

static void cm_cb(res_t *r, void *vp)
{
    struct cm *c = vp;
    if (c->n >= c->max)
        return;
    if ((int)ResGetInt(r, NOUN_STATUS) != ST_PENDING)
        return;
    size_t il;
    const unsigned char *h = ResGetBytes(r, NOUN_IMAGE, &il);
    if (h == NULL || il != SHA256_LEN || blob_has(h))
        return;
    for (int i = 0; i < c->n; i++)
        if (memcmp(c->out[i], h, SHA256_LEN) == 0)
            return;
    memcpy(c->out[c->n++], h, SHA256_LEN);
}

static void worker_gossip(void)
{
    lock();
    struct peercopy peer;
    int have = copy_peers(&peer, 1);
    struct buf syn;
    buf_init(&syn);
    if (have)
        gossip_build_state(&syn);
    unlock();

    if (have) {
        int rt = 0;
        struct buf reply;
        buf_init(&reply);
        if (wire_call(peer.ip, peer.port, peer.name, MSG_GOSSIP_SYN,
                      syn.data, syn.len, &rt, &reply) == 0 &&
            rt == MSG_GOSSIP_ACK) {
            lock();
            gossip_merge_state(reply.data, reply.len);
            unlock();
        }
        buf_free(&reply);
    }
    buf_free(&syn);
}

static void worker_fetch(void)
{
    unsigned char need[16][SHA256_LEN];
    struct peercopy peers[8];

    lock();
    struct cm c = { need, 16, 0 };
    DesiredForEach(cm_cb, &c);
    int nn = c.n;
    int np = copy_peers(peers, 8);
    unlock();

    for (int i = 0; i < nn; i++) {
        for (int j = 0; j < np; j++) {
            int rt = 0;
            struct buf reply;
            buf_init(&reply);
            int ok = wire_call(peers[j].ip, peers[j].port, peers[j].name,
                               MSG_BLOB_REQ, need[i], SHA256_LEN, &rt, &reply);
            int got = 0;
            if (ok == 0 && rt == MSG_BLOB_RESP) {
                struct rdr rd;
                rdr_init(&rd, reply.data, reply.len);
                const unsigned char *found = rd_bytes(&rd, 1);
                if (found != NULL && found[0] == 1) {
                    uint32_t len = rd_u32(&rd);
                    const void *bytes = rd_bytes(&rd, len);
                    if (bytes != NULL) {
                        /* verify before storing: a wrong answer is simply
                         * ignored, never allowed to evict anything we hold */
                        unsigned char h[SHA256_LEN];
                        sha256_hash(bytes, len, h);
                        if (memcmp(h, need[i], SHA256_LEN) == 0) {
                            lock();
                            blob_put(bytes, len, h);
                            unlock();
                            INFO("fetched image from %s (%u bytes)",
                                 peers[j].name, len);
                            got = 1;
                        } else {
                            WARN("peer %s sent bytes that do not match the hash",
                                 peers[j].name);
                        }
                    }
                }
            }
            buf_free(&reply);
            if (got) {
                g_dirty = 1;
                break;
            }
        }
    }
}

static void *worker_main(void *arg)
{
    (void)arg;
    while (!g_stop) {
        worker_gossip();
        worker_fetch();
        for (int i = 0; i < 6 && !g_stop; i++)
            usleep(100000);     /* ~600ms between rounds, responsive to stop */
    }
    return NULL;
}

/* ----------------------------------------------------- child reaping */

static void on_child(pid_t pid, int status, void *ctx)
{
    (void)ctx;
    reconcile_on_child_exit(pid, status);   /* called with g_lock held */
    g_dirty = 1;
}

/* ----------------------------------------------------- config */

static void parse_hostport(const char *s, char *ip, size_t ipcap, int *port)
{
    const char *colon = strrchr(s, ':');
    if (colon == NULL) {
        snprintf(ip, ipcap, "%.*s", (int)ipcap - 1, s);
        return;
    }
    size_t n = (size_t)(colon - s);
    if (n >= ipcap)
        n = ipcap - 1;
    memcpy(ip, s, n);
    ip[n] = '\0';
    *port = atoi(colon + 1);
}

static void usage(const char *p)
{
    fprintf(stderr,
        "usage: %s [--name N] [--bind IP] [--port P] [--manifests DIR]\n"
        "          [--seed IP:PORT ...] [--certs DIR]\n", p);
}

int main(int argc, char **argv)
{
    snprintf(g_cfg.name, sizeof g_cfg.name, "node-%d", (int)getpid());
    snprintf(g_cfg.ip, sizeof g_cfg.ip, "127.0.0.1");
    g_cfg.port = 7700;
    snprintf(g_cfg.manifests, sizeof g_cfg.manifests, "/etc/bubernetes/manifests");
    g_cfg.certs[0] = '\0';

    char seeds[8][96];
    int nseeds = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--name") == 0 && i + 1 < argc)
            snprintf(g_cfg.name, sizeof g_cfg.name, "%s", argv[++i]);
        else if (strcmp(argv[i], "--bind") == 0 && i + 1 < argc)
            snprintf(g_cfg.ip, sizeof g_cfg.ip, "%s", argv[++i]);
        else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc)
            g_cfg.port = atoi(argv[++i]);
        else if (strcmp(argv[i], "--manifests") == 0 && i + 1 < argc)
            snprintf(g_cfg.manifests, sizeof g_cfg.manifests, "%s", argv[++i]);
        else if (strcmp(argv[i], "--certs") == 0 && i + 1 < argc)
            snprintf(g_cfg.certs, sizeof g_cfg.certs, "%s", argv[++i]);
        else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc && nseeds < 8)
            snprintf(seeds[nseeds++], sizeof seeds[0], "%s", argv[++i]);
        else {
            usage(argv[0]);
            return 2;
        }
    }

    if (!ResNameValid(g_cfg.name, strlen(g_cfg.name))) {
        ERR("node name must be 1..63 chars of [A-Za-z0-9._-]");
        return 2;
    }

    mkdir_p(g_cfg.manifests);

    if (g_cfg.certs[0] != '\0' && tls_setup(g_cfg.certs) != 0) {
        ERR("TLS setup failed");
        return 1;
    }

    membership_init(g_cfg.name, g_cfg.ip, g_cfg.port, g_cfg.port);
    for (int i = 0; i < nseeds; i++) {
        char sip[96] = "127.0.0.1";
        int sport = 7700;
        parse_hostport(seeds[i], sip, sizeof sip, &sport);
        membership_seed(sip, sport);
    }

    reconcile_init(g_cfg.name);
    gossip_on_change(on_desired_change, NULL);
    char clock_path[1100];
    snprintf(clock_path, sizeof clock_path, "%s/.lamport", g_cfg.manifests);
    gossip_clock_file(clock_path);

    g_udp_fd = membership_udp_socket();
    g_tcp_fd = wire_listen(g_cfg.ip, g_cfg.port);
    if (g_udp_fd < 0 || g_tcp_fd < 0)
        return 1;
    fcntl(g_tcp_fd, F_SETFL, fcntl(g_tcp_fd, F_GETFL, 0) | O_NONBLOCK);

    int inotify_fd = manifest_watch_init(g_cfg.manifests);

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGCHLD);
    sigaddset(&mask, SIGPIPE);      /* a peer closing mid-write must not kill us */
    sigprocmask(SIG_BLOCK, &mask, NULL);
    sigset_t want;
    sigemptyset(&want);
    sigaddset(&want, SIGINT);
    sigaddset(&want, SIGTERM);
    sigaddset(&want, SIGCHLD);
    int sig_fd = signalfd(-1, &want, SFD_NONBLOCK | SFD_CLOEXEC);

    int epfd = epoll_create1(EPOLL_CLOEXEC);
    struct epoll_event ev;
    ev.events = EPOLLIN; ev.data.fd = g_udp_fd; epoll_ctl(epfd, EPOLL_CTL_ADD, g_udp_fd, &ev);
    ev.events = EPOLLIN; ev.data.fd = g_tcp_fd; epoll_ctl(epfd, EPOLL_CTL_ADD, g_tcp_fd, &ev);
    ev.events = EPOLLIN; ev.data.fd = sig_fd;   epoll_ctl(epfd, EPOLL_CTL_ADD, sig_fd, &ev);
    if (inotify_fd >= 0) {
        ev.events = EPOLLIN; ev.data.fd = inotify_fd;
        epoll_ctl(epfd, EPOLL_CTL_ADD, inotify_fd, &ev);
    }

    periodic_add(200,   task_swim,      NULL, "swim");
    periodic_add(1000,  task_reconcile, NULL, "reconcile");
    periodic_add(1000,  task_mirror,    NULL, "mirror");
    periodic_add(5000,  task_rescan,    NULL, "rescan");
    periodic_add(30000, task_gc,        NULL, "blob-gc");

    INFO("bubelet '%s' on %s:%d, manifests in %s, %d seed(s)",
         g_cfg.name, g_cfg.ip, g_cfg.port, g_cfg.manifests, nseeds);

    lock();
    loader_scan();
    mirror_nodes();
    unlock();

    pthread_t worker;
    pthread_create(&worker, NULL, worker_main, NULL);

    struct epoll_event evs[MAX_EV];
    while (!g_stop) {
        int timeout = (int)periodic_next_delay();
        int n = epoll_wait(epfd, evs, MAX_EV, timeout);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            PERR("epoll_wait");
            break;
        }
        for (int i = 0; i < n; i++) {
            int fd = evs[i].data.fd;
            if (fd == g_udp_fd) {
                lock();
                membership_handle(g_udp_fd);
                unlock();
            } else if (fd == g_tcp_fd) {
                accept_all();
            } else if (fd == inotify_fd) {
                if (manifest_watch_drain(inotify_fd)) {
                    lock();
                    loader_scan();
                    unlock();
                }
            } else if (fd == sig_fd) {
                struct signalfd_siginfo si;
                while (read(sig_fd, &si, sizeof si) == (ssize_t)sizeof si) {
                    if (si.ssi_signo == SIGCHLD) {
                        lock();
                        exec_reap(on_child, NULL);
                        unlock();
                    } else {
                        g_stop = 1;
                    }
                }
            }
        }
        periodic_run_due();
        if (g_dirty) {
            g_dirty = 0;
            lock();
            reconcile_pass();
            unlock();
        }
    }

    INFO("shutting down, stopping workloads");
    pthread_join(worker, NULL);
    lock();
    reconcile_stop_all();
    unlock();
    /* give replicas a moment to exit on SIGTERM, reaping as they go */
    for (int i = 0; i < 20; i++) {
        usleep(100000);
        lock();
        exec_reap(on_child, NULL);
        unlock();
    }
    close(epfd);
    close(g_udp_fd);
    close(g_tcp_fd);
    if (inotify_fd >= 0)
        close(inotify_fd);
    close(sig_fd);
    INFO("bubelet '%s' stopped", g_cfg.name);
    return 0;
}
