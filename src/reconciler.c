#define _GNU_SOURCE
#include "reconciler.h"
#include "apiserver.h"
#include "scheduler.h"
#include "membership.h"
#include "blob.h"
#include "exec.h"
#include "sha256.h"
#include "log.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#define MAX_MEMBERS 256

static char g_self[128];

/* per-resource runtime: the replicas this node is actually running */
struct run {
    unsigned char id[64];
    size_t idlen;
    pid_t *pids;
    int npids;
    int cap;
    int used;
};
static struct run *g_runs;
static size_t g_runcap, g_runlen;

void reconcile_init(const char *self_name)
{
    snprintf(g_self, sizeof g_self, "%s", self_name);
}

static struct run *run_find(const void *id, size_t idlen)
{
    for (size_t i = 0; i < g_runlen; i++)
        if (g_runs[i].used && g_runs[i].idlen == idlen &&
            memcmp(g_runs[i].id, id, idlen) == 0)
            return &g_runs[i];
    return NULL;
}

static struct run *run_alloc(const void *id, size_t idlen)
{
    struct run *r = run_find(id, idlen);
    if (r != NULL)
        return r;
    for (size_t i = 0; i < g_runlen; i++)
        if (!g_runs[i].used) { r = &g_runs[i]; break; }
    if (r == NULL) {
        if (g_runlen == g_runcap) {
            g_runcap = g_runcap ? g_runcap * 2 : 16;
            g_runs = xrealloc(g_runs, g_runcap * sizeof *g_runs);
        }
        r = &g_runs[g_runlen++];
    }
    memset(r, 0, sizeof *r);
    memcpy(r->id, id, idlen);
    r->idlen = idlen;
    r->used = 1;
    return r;
}

static void run_add_pid(struct run *r, pid_t pid)
{
    if (r->npids == r->cap) {
        r->cap = r->cap ? r->cap * 2 : 8;
        r->pids = xrealloc(r->pids, r->cap * sizeof *r->pids);
    }
    r->pids[r->npids++] = pid;
}

static void run_remove_pid(struct run *r, pid_t pid)
{
    for (int i = 0; i < r->npids; i++)
        if (r->pids[i] == pid) {
            r->pids[i] = r->pids[--r->npids];
            return;
        }
}

/* ------------------------------------------------------- spawning */

/* build argv: program name then the manifest's argv entries */
static int build_argv(res_t *r, char **argv, int cap, char *namebuf, size_t nbcap)
{
    size_t nlen;
    const char *name = ResGetBytes(r, NOUN_NAME, &nlen);
    snprintf(namebuf, nbcap, "%.*s", (int)(name ? nlen : 0), name ? name : "wl");
    int n = 0;
    argv[n++] = namebuf;

    size_t alen;
    const char *a = ResGetBytes(r, NOUN_ARGV, &alen);
    if (a != NULL) {
        size_t off = 0;
        while (off < alen && n < cap - 1) {
            argv[n++] = (char *)(a + off);     /* NUL-separated already */
            off += strnlen(a + off, alen - off) + 1;
        }
    }
    argv[n] = NULL;
    return n;
}

static void spawn_more(res_t *r, struct run *run, int count)
{
    size_t ilen;
    const unsigned char *hash = ResGetBytes(r, NOUN_IMAGE, &ilen);
    size_t nlen;
    const char *name = ResGetBytes(r, NOUN_NAME, &nlen);
    char nm[160];
    snprintf(nm, sizeof nm, "%.*s", (int)(name ? nlen : 0), name ? name : "wl");

    if (hash == NULL || ilen != SHA256_LEN) {
        WARN("resource '%s' has no image, cannot start", nm);
        ResSetInt(r, NOUN_STATUS, ST_PENDING);
        return;
    }
    /* The reconciler never touches the network. If we only hold the hash and
     * not the bytes, we mark the resource Pending; the daemon's worker fetches
     * the image from a peer and the next pass starts it. This is what keeps the
     * event loop free to answer other nodes instead of blocking on one. */
    if (!blob_has(hash)) {
        ResSetInt(r, NOUN_STATUS, ST_PENDING);
        return;
    }

    size_t imglen;
    const void *img = blob_get(hash, &imglen);
    int memfd = exec_memfd(nm, img, imglen);
    if (memfd < 0) {
        ResSetInt(r, NOUN_STATUS, ST_UNHEALTHY);
        return;
    }

    char *argv[64];
    char namebuf[160];
    build_argv(r, argv, 64, namebuf, sizeof namebuf);

    for (int i = 0; i < count; i++) {
        int idx = run->npids;      /* replica index for the environment */
        char e_name[192], e_node[160], e_rep[32];
        snprintf(e_name, sizeof e_name, "BUBE_NAME=%s", nm);
        snprintf(e_node, sizeof e_node, "BUBE_NODE=%s", g_self);
        snprintf(e_rep, sizeof e_rep, "BUBE_REPLICA=%d", idx);
        char *envp[] = { e_name, e_node, e_rep,
                         "PATH=/usr/bin:/bin", NULL };
        pid_t pid = exec_spawn(memfd, argv, envp);
        if (pid > 0) {
            run_add_pid(run, pid);
            INFO("started '%s' replica %d pid %ld", nm, idx, (long)pid);
        }
    }
    close(memfd);       /* replicas keep running; their .text is the memfd's */
}

static void kill_some(struct run *run, int count, const char *name)
{
    for (int i = 0; i < count && run->npids > 0; i++) {
        pid_t pid = run->pids[run->npids - 1];
        kill(pid, SIGTERM);
        run->npids--;
        INFO("stopping '%s' replica pid %ld (scaled down)", name, (long)pid);
    }
}

/* ------------------------------------------------------- the pass */

struct pass_ctx {
    struct member_view *members;
    int nmembers;
};

static void reconcile_one(res_t *r, void *vp)
{
    struct pass_ctx *pc = vp;
    size_t idlen;
    const void *id = ResGetBytes(r, NOUN_ID, &idlen);
    if (id == NULL)
        return;

    int kind = (int)ResGetInt(r, NOUN_KIND);
    int want;

    if (kind == KIND_STATIC) {
        size_t olen;
        const char *origin = ResGetBytes(r, NOUN_ORIGIN, &olen);
        int mine = origin != NULL && olen == strlen(g_self) &&
                   memcmp(origin, g_self, olen) == 0;
        want = mine ? 1 : 0;
    } else if (kind == KIND_DEPLOY) {
        int replicas = ResHas(r, NOUN_REPLICAS) ?
                       (int)ResGetInt(r, NOUN_REPLICAS) : 1;
        want = schedule_deploy_here(id, idlen, replicas,
                                    pc->members, pc->nmembers, g_self);
    } else {
        return;     /* NODE or unknown: not something we run */
    }

    struct run *run = run_find(id, idlen);
    int have = run ? run->npids : 0;

    if (want > have) {
        if (run == NULL)
            run = run_alloc(id, idlen);
        spawn_more(r, run, want - have);
    } else if (want < have) {
        size_t nlen;
        const char *name = ResGetBytes(r, NOUN_NAME, &nlen);
        char nm[160];
        snprintf(nm, sizeof nm, "%.*s", (int)(name ? nlen : 0), name ? name : "wl");
        kill_some(run, have - want, nm);
    }

    /* publish runtime state (local nouns only) */
    int now = run ? run->npids : 0;
    if (want > 0) {
        ResSetInt(r, NOUN_RUNNING, now);
        ResSetInt(r, NOUN_STATUS, now >= want ? ST_READY : ST_PENDING);
        if (now > 0)
            ResSetInt(r, NOUN_PID, run->pids[0]);
    } else {
        ResSetInt(r, NOUN_RUNNING, 0);
    }
}

/* stop replicas of resources that no longer exist or are no longer desired */
static void sweep_orphans(void)
{
    for (size_t i = 0; i < g_runlen; i++) {
        struct run *run = &g_runs[i];
        if (!run->used || run->npids == 0)
            continue;
        res_t *r = StoreGet(run->id, run->idlen);
        if (r == NULL || !ResIsDesired(r)) {
            INFO("resource gone; stopping its %d replica(s)", run->npids);
            while (run->npids > 0) {
                kill(run->pids[run->npids - 1], SIGTERM);
                run->npids--;
            }
        }
    }
}

void reconcile_pass(void)
{
    struct member_view members[MAX_MEMBERS];
    int nm = membership_snapshot(members, MAX_MEMBERS);

    struct pass_ctx pc = { members, nm };
    DesiredForEach(reconcile_one, &pc);
    sweep_orphans();
}

void reconcile_on_child_exit(pid_t pid, int status)
{
    (void)status;
    for (size_t i = 0; i < g_runlen; i++) {
        if (!g_runs[i].used)
            continue;
        for (int j = 0; j < g_runs[i].npids; j++) {
            if (g_runs[i].pids[j] == pid) {
                run_remove_pid(&g_runs[i], pid);
                /* next reconcile pass notices want>have and restarts it */
                return;
            }
        }
    }
}

void reconcile_stop_all(void)
{
    for (size_t i = 0; i < g_runlen; i++) {
        struct run *run = &g_runs[i];
        if (!run->used)
            continue;
        for (int j = 0; j < run->npids; j++)
            kill(run->pids[j], SIGTERM);
        run->npids = 0;
    }
}
