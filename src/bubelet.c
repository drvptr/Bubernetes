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
 * Concurrency. There are two threads and one lock. The main thread owns the
 * event loop: it answers other nodes (SWIM, inbound TCP), watches the manifest
 * directory, and reconciles - none of which ever blocks on a remote node, so a
 * node is always able to respond. The worker thread owns the only blocking
 * outbound calls there are - gossip rounds and image fetches - so one slow peer
 * can never stall the loop or deadlock two nodes against each other. All shared
 * state sits behind g_lock; it is held only across in-memory work, never across a
 * network call.
 */

#define MAX_EV 64

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

/* ids we saw as files in the previous scan, to detect removals (32-byte ids) */
static struct buf g_prev_ids;

/* ----------------------------------------------------------- helpers */

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

static int id_set_has(struct buf *set, const unsigned char id[SHA256_LEN])
{
    for (size_t off = 0; off + SHA256_LEN <= set->len; off += SHA256_LEN)
        if (memcmp(set->data + off, id, SHA256_LEN) == 0)
            return 1;
    return 0;
}

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

static void safe_name(char *out, size_t cap, const char *name, size_t nlen)
{
    size_t j = 0;
    for (size_t i = 0; i < nlen && j + 1 < cap; i++) {
        char c = name[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_')
            out[j++] = c;
        else
            out[j++] = '_';
    }
    out[j] = '\0';
}

/* ----------------------------------------------------- persistence */

/* write a resource's spec to <dir>/<name>.yaml, but only if the bytes differ,
 * so we do not fight our own inotify watch. Desired state stored as files - the
 * only storage bubelet has. */
static void persist_res(res_t *r)
{
    size_t nlen;
    const char *name = ResGetBytes(r, NOUN_NAME, &nlen);
    if (name == NULL)
        return;
    char sn[256];
    safe_name(sn, sizeof sn, name, nlen);
    char path[1400];
    snprintf(path, sizeof path, "%s/%s.yaml", g_cfg.manifests, sn);

    struct buf doc;
    buf_init(&doc);
    manifest_dump_res(&doc, r);

    FILE *f = fopen(path, "rb");
    if (f != NULL) {
        struct buf cur;
        buf_init(&cur);
        char tmp[4096];
        size_t rd;
        while ((rd = fread(tmp, 1, sizeof tmp, f)) > 0)
            buf_append(&cur, tmp, rd);
        fclose(f);
        int same = cur.len == doc.len && memcmp(cur.data, doc.data, doc.len) == 0;
        buf_free(&cur);
        if (same) {
            buf_free(&doc);
            return;
        }
    }

    f = fopen(path, "wb");
    if (f != NULL) {
        fwrite(doc.data, 1, doc.len, f);
        fclose(f);
    }
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
    char sn[256];
    safe_name(sn, sizeof sn, name, nlen);
    char path[1400];
    snprintf(path, sizeof path, "%s/%s.yaml", g_cfg.manifests, sn);
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

/* ----------------------------------------------------- loader */

static int loader_emit(const struct manifest *m, void *ctx)
{
    struct buf *seen = ctx;

    int kind = KindFromName(m->kind);
    if (kind < 0)
        return 0;

    unsigned char id[SHA256_LEN];
    res_id(kind, m->name, strlen(m->name), id);
    buf_append(seen, id, SHA256_LEN);

    res_t *tmp = ResCreate();
    ResSetBytes(tmp, NOUN_ID, id, SHA256_LEN);
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
        } else if (blob_ingest_file(m->image, hash) == 0) {
            ResSetBytes(tmp, NOUN_IMAGE, hash, SHA256_LEN);   /* a path: ingest */
            INFO("ingested image %s for '%s'", m->image, m->name);
        } else {
            WARN("cannot read image '%s' for '%s'", m->image, m->name);
        }
    }

    res_t *cur = StoreGet(id, SHA256_LEN);
    if (cur == NULL) {
        res_t *r = StoreCreate(id, SHA256_LEN);
        ResCopySpec(r, tmp);
        gossip_author(r);
        INFO("resource '%s' (%s) defined", m->name, KindName(kind));
    } else if (!gossip_spec_equal(cur, tmp)) {
        ResCopySpec(cur, tmp);
        gossip_author(cur);
        INFO("resource '%s' (%s) updated", m->name, KindName(kind));
    }
    ResDelete(tmp);
    return 0;
}

static void loader_scan(void)
{
    struct buf cur_ids;
    buf_init(&cur_ids);
    manifest_scan_dir(g_cfg.manifests, loader_emit, &cur_ids);

    /* a file we had last time but not now means the resource was retracted */
    for (size_t off = 0; off + SHA256_LEN <= g_prev_ids.len; off += SHA256_LEN) {
        const unsigned char *id = g_prev_ids.data + off;
        if (!id_set_has(&cur_ids, id) && StoreGet(id, SHA256_LEN) != NULL) {
            INFO("manifest removed; retracting resource");
            gossip_delete(id, SHA256_LEN);
        }
    }

    buf_free(&g_prev_ids);
    g_prev_ids = cur_ids;
    g_dirty = 1;
}

/* ----------------------------------------------------- apply over wire */

static void ingest_spec(res_t *spec, const void *img, size_t imglen)
{
    int kind = (int)ResGetInt(spec, NOUN_KIND);
    size_t nlen;
    const char *name = ResGetBytes(spec, NOUN_NAME, &nlen);
    if (name == NULL)
        return;

    unsigned char id[SHA256_LEN];
    res_id(kind, name, nlen, id);
    ResSetBytes(spec, NOUN_ID, id, SHA256_LEN);

    if (imglen > 0) {
        unsigned char hash[SHA256_LEN];
        blob_put(img, imglen, hash);
        ResSetBytes(spec, NOUN_IMAGE, hash, SHA256_LEN);
    }

    res_t *cur = StoreGet(id, SHA256_LEN);
    if (cur == NULL) {
        res_t *r = StoreCreate(id, SHA256_LEN);
        ResCopySpec(r, spec);
        gossip_author(r);
        INFO("applied '%.*s' (%s)", (int)nlen, name, KindName(kind));
    } else if (!gossip_spec_equal(cur, spec)) {
        ResCopySpec(cur, spec);
        gossip_author(cur);
        INFO("applied update to '%.*s'", (int)nlen, name);
    }
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
        if (spec != NULL && !rd.err) {
            ingest_spec(spec, img, imglen);
            st = W_OK;
        }
        if (spec != NULL)
            ResDelete(spec);
        buf_append_byte(reply, st);
        *rtype = MSG_APPLY_RESP;
        return 1;

    } else if (type == MSG_DELETE) {
        char name[256];
        snprintf(name, sizeof name, "%.*s", (int)payload->len,
                 (char *)payload->data);
        res_t *r = StoreGetName(name);
        unsigned char st = W_NOTFOUND;
        if (r != NULL) {
            size_t idlen;
            const void *id = ResGetBytes(r, NOUN_ID, &idlen);
            unsigned char idbuf[SHA256_LEN];
            if (id != NULL && idlen == SHA256_LEN) {
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
        } else if (verb == V_DELETE && r != NULL && idlen <= SHA256_LEN) {
            unsigned char idbuf[SHA256_LEN];
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

static void accept_all(void)
{
    for (;;) {
        struct conn c;
        if (wire_accept(g_tcp_fd, &c) != 0)
            break;      /* EAGAIN or handshake failure */
        int type;
        struct buf payload;
        buf_init(&payload);
        if (wire_recv(&c, &type, &payload) == 0) {
            int rtype = 0;
            struct buf reply;
            buf_init(&reply);
            lock();
            int send = process_req(type, &payload, &rtype, &reply);
            unlock();
            if (send)
                wire_send(&c, rtype, reply.data, reply.len);
            buf_free(&reply);
        }
        buf_free(&payload);
        wire_close(&c);
    }
}

/* ----------------------------------------------------- node mirror */

struct mirror_ctx { struct buf *names; };

static void mirror_one(const struct member_view *m, void *vp)
{
    struct mirror_ctx *mc = vp;
    buf_append(mc->names, m->name, strlen(m->name));
    buf_append_byte(mc->names, '\0');

    unsigned char id[SHA256_LEN];
    res_id(KIND_NODE, m->name, strlen(m->name), id);
    res_t *r = StoreCreate(id, SHA256_LEN);
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
    if (id != NULL) {
        unsigned char idbuf[SHA256_LEN];
        memcpy(idbuf, id, idlen < SHA256_LEN ? idlen : SHA256_LEN);
        StoreRemove(idbuf, idlen);
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

/* ----------------------------------------------------- main-thread tasks */

static void task_swim(void *ctx)      { (void)ctx; lock(); membership_tick(g_udp_fd); unlock(); }
static void task_reconcile(void *ctx) { (void)ctx; lock(); reconcile_pass();          unlock(); }
static void task_mirror(void *ctx)    { (void)ctx; lock(); mirror_nodes();            unlock(); }
static void task_rescan(void *ctx)    { (void)ctx; lock(); loader_scan();             unlock(); }

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
                        unsigned char h[SHA256_LEN];
                        lock();
                        blob_put(bytes, len, h);
                        unlock();
                        if (memcmp(h, need[i], SHA256_LEN) == 0) {
                            INFO("fetched image from %s (%u bytes)",
                                 peers[j].name, len);
                            got = 1;
                        } else {
                            lock();
                            blob_drop(h);
                            unlock();
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
    buf_init(&g_prev_ids);

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
    sigprocmask(SIG_BLOCK, &mask, NULL);
    int sig_fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);

    int epfd = epoll_create1(EPOLL_CLOEXEC);
    struct epoll_event ev;
    ev.events = EPOLLIN; ev.data.fd = g_udp_fd; epoll_ctl(epfd, EPOLL_CTL_ADD, g_udp_fd, &ev);
    ev.events = EPOLLIN; ev.data.fd = g_tcp_fd; epoll_ctl(epfd, EPOLL_CTL_ADD, g_tcp_fd, &ev);
    ev.events = EPOLLIN; ev.data.fd = sig_fd;   epoll_ctl(epfd, EPOLL_CTL_ADD, sig_fd, &ev);
    if (inotify_fd >= 0) {
        ev.events = EPOLLIN; ev.data.fd = inotify_fd;
        epoll_ctl(epfd, EPOLL_CTL_ADD, inotify_fd, &ev);
    }

    periodic_add(200,  task_swim,      NULL, "swim");
    periodic_add(1000, task_reconcile, NULL, "reconcile");
    periodic_add(1000, task_mirror,    NULL, "mirror");
    periodic_add(5000, task_rescan,    NULL, "rescan");

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
    usleep(200000);
    lock();
    exec_reap(on_child, NULL);
    unlock();
    close(epfd);
    close(g_udp_fd);
    close(g_tcp_fd);
    if (inotify_fd >= 0)
        close(inotify_fd);
    close(sig_fd);
    INFO("bubelet '%s' stopped", g_cfg.name);
    return 0;
}
