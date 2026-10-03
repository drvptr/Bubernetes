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
#include <unistd.h>

#define MAX_MEMBERS 256

/* more replicas of one resource than this on one node is a typo or an attack,
 * not a deployment; the spec may ask for 65535 across the cluster, but a single
 * node will not fork past this */
#define MAX_PER_NODE 256

/* how long a replica gets to exit after SIGTERM before it is killed */
#define TERM_GRACE_MS 5000

static char g_self[128];

/* one replica we are responsible for */
struct replica {
    pid_t pid;
    int64_t term_deadline;      /* 0 == running; else SIGTERM sent, kill at this time */
};

/* per-resource runtime: the replicas this node is actually running */
struct run {
    unsigned char id[RES_ID_LEN];
    struct replica *reps;
    int nreps;
    int cap;
    int memfd;                              /* shared image, -1 if none yet */
    unsigned char memfd_hash[SHA256_LEN];   /* which image memfd holds */
    int used;
};
static struct run *g_runs;
static size_t g_runcap, g_runlen;

void reconcile_init(const char *self_name)
{
    snprintf(g_self, sizeof g_self, "%s", self_name);
}

/* ------------------------------------------------------- run table */

static struct run *run_find(const void *id, size_t idlen)
{
    if (idlen != RES_ID_LEN)
        return NULL;
    for (size_t i = 0; i < g_runlen; i++)
        if (g_runs[i].used && memcmp(g_runs[i].id, id, RES_ID_LEN) == 0)
            return &g_runs[i];
    return NULL;
}

static struct run *run_alloc(const void *id, size_t idlen)
{
    if (idlen != RES_ID_LEN)
        return NULL;            /* not an id we issue; never trust its length */
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
    memcpy(r->id, id, RES_ID_LEN);
    r->memfd = -1;
    r->used = 1;
    return r;
}

static void run_free(struct run *r)
{
    free(r->reps);
    if (r->memfd >= 0)
        close(r->memfd);
    memset(r, 0, sizeof *r);
    r->memfd = -1;
}

static void run_add(struct run *r, pid_t pid)
{
    if (r->nreps == r->cap) {
        r->cap = r->cap ? r->cap * 2 : 8;
        r->reps = xrealloc(r->reps, r->cap * sizeof *r->reps);
    }
    r->reps[r->nreps].pid = pid;
    r->reps[r->nreps].term_deadline = 0;
    r->nreps++;
}

static void run_remove_pid(struct run *r, pid_t pid)
{
    for (int i = 0; i < r->nreps; i++)
        if (r->reps[i].pid == pid) {
            r->reps[i] = r->reps[--r->nreps];
            return;
        }
}

static int run_running(struct run *r)
{
    int n = 0;
    for (int i = 0; i < r->nreps; i++)
        if (r->reps[i].term_deadline == 0)
            n++;
    return n;
}

/* ask one replica to stop; it stays accounted for until it is reaped, and is
 * killed outright if it ignores the request */
static void replica_terminate(struct run *r, int i, const char *why)
{
    struct replica *rep = &r->reps[i];
    if (rep->term_deadline != 0)
        return;                 /* already asked */
    kill(rep->pid, SIGTERM);
    rep->term_deadline = monotime_ms() + TERM_GRACE_MS;
    INFO("stopping pid %ld (%s)", (long)rep->pid, why);
}

static void escalate_expired(struct run *r)
{
    int64_t now = monotime_ms();
    for (int i = 0; i < r->nreps; i++) {
        struct replica *rep = &r->reps[i];
        if (rep->term_deadline != 0 && now >= rep->term_deadline) {
            WARN("pid %ld ignored SIGTERM for %d ms, killing",
                 (long)rep->pid, TERM_GRACE_MS);
            kill(rep->pid, SIGKILL);
            rep->term_deadline = now + TERM_GRACE_MS;   /* don't spam */
        }
    }
}

/* ------------------------------------------------------- spawning */

/* argv for a replica: program name, then the manifest's entries. The entries
 * are copied into our own NUL-terminated scratch so exec never reads past the
 * resource's buffer whatever a peer sent. */
static int build_argv(res_t *r, char **argv, int cap, char *scratch, size_t scap)
{
    size_t used = 0;
    int n = 0;

    size_t nlen;
    const char *name = ResGetBytes(r, NOUN_NAME, &nlen);
    if (name == NULL) { name = "wl"; nlen = 2; }
    if (nlen + 1 > scap)
        nlen = scap - 1;
    memcpy(scratch, name, nlen);
    scratch[nlen] = '\0';
    argv[n++] = scratch;
    used = nlen + 1;

    size_t alen;
    const char *a = ResGetBytes(r, NOUN_ARGV, &alen);
    size_t off = 0;
    while (a != NULL && off < alen && n < cap - 1) {
        size_t ilen = strnlen(a + off, alen - off);
        if (used + ilen + 1 > scap)
            break;
        memcpy(scratch + used, a + off, ilen);
        scratch[used + ilen] = '\0';
        argv[n++] = scratch + used;
        used += ilen + 1;
        off += ilen + 1;
    }
    argv[n] = NULL;
    return n;
}

/* the memfd every replica of this run execs from. Made once per image and kept
 * open while the run lives, so replicas started in different passes still map
 * the same backing and share their .text. */
static int run_memfd(struct run *run, const unsigned char hash[SHA256_LEN],
                     const char *name)
{
    if (run->memfd >= 0 && memcmp(run->memfd_hash, hash, SHA256_LEN) == 0)
        return run->memfd;
    if (run->memfd >= 0) {
        close(run->memfd);      /* image changed: old replicas keep the old one */
        run->memfd = -1;
    }
    size_t imglen;
    const void *img = blob_get(hash, &imglen);
    if (img == NULL)
        return -1;
    int fd = exec_memfd(name, img, imglen);
    if (fd < 0)
        return -1;
    run->memfd = fd;
    memcpy(run->memfd_hash, hash, SHA256_LEN);
    return fd;
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
        ResSetInt(r, NOUN_STATUS, ST_PENDING);     /* no image to run */
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

    int memfd = run_memfd(run, hash, nm);
    if (memfd < 0) {
        ResSetInt(r, NOUN_STATUS, ST_UNHEALTHY);
        return;
    }

    char *argv[64];
    char scratch[4096];
    build_argv(r, argv, 64, scratch, sizeof scratch);

    for (int i = 0; i < count; i++) {
        int idx = run->nreps;      /* replica index for the environment */
        char e_name[192], e_node[160], e_rep[32];
        snprintf(e_name, sizeof e_name, "BUBE_NAME=%s", nm);
        snprintf(e_node, sizeof e_node, "BUBE_NODE=%s", g_self);
        snprintf(e_rep, sizeof e_rep, "BUBE_REPLICA=%d", idx);
        char *envp[] = { e_name, e_node, e_rep,
                         "PATH=/usr/bin:/bin", NULL };
        pid_t pid = exec_spawn(memfd, argv, envp);
        if (pid > 0) {
            run_add(run, pid);
            INFO("started '%s' replica %d pid %ld", nm, idx, (long)pid);
        }
    }
}

static void stop_some(struct run *run, int count, const char *why)
{
    if (run == NULL)
        return;
    /* stop running replicas from the end; ones already terminating don't count */
    for (int i = run->nreps - 1; i >= 0 && count > 0; i--) {
        if (run->reps[i].term_deadline == 0) {
            replica_terminate(run, i, why);
            count--;
        }
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
    if (id == NULL || idlen != RES_ID_LEN)
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

    if (want > MAX_PER_NODE) {
        WARN("capping replicas on this node at %d (wanted %d)", MAX_PER_NODE, want);
        want = MAX_PER_NODE;
    }

    struct run *run = run_find(id, idlen);
    if (run != NULL)
        escalate_expired(run);

    /* a replica that was asked to stop still exists until it is reaped, so it
     * counts: we never spawn a replacement for something that is still here */
    int have = run ? run->nreps : 0;
    int running = run ? run_running(run) : 0;

    size_t nlen;
    const char *name = ResGetBytes(r, NOUN_NAME, &nlen);
    char nm[160];
    snprintf(nm, sizeof nm, "%.*s", (int)(name ? nlen : 0), name ? name : "wl");

    if (want > have) {
        if (run == NULL)
            run = run_alloc(id, idlen);
        if (run != NULL)
            spawn_more(r, run, want - have);
    } else if (want < running) {
        stop_some(run, running - want, "scaled down");
    }

    /* publish runtime state (local nouns only) */
    running = run ? run_running(run) : 0;
    if (want > 0) {
        ResSetInt(r, NOUN_RUNNING, running);
        ResSetInt(r, NOUN_STATUS, running >= want ? ST_READY : ST_PENDING);
        if (running > 0)
            ResSetInt(r, NOUN_PID, run->reps[0].pid);
        else
            ResClear(r, NOUN_PID);
    } else {
        ResSetInt(r, NOUN_RUNNING, running);
        ResClear(r, NOUN_PID);
        if (running == 0)
            ResClear(r, NOUN_STATUS);   /* not ours to run: no status here */
    }

    /* nothing left and nothing wanted: drop the run and its memfd */
    if (run != NULL && run->nreps == 0 && want == 0)
        run_free(run);
}

/* stop replicas of resources that no longer exist or are no longer desired */
static void sweep_orphans(void)
{
    for (size_t i = 0; i < g_runlen; i++) {
        struct run *run = &g_runs[i];
        if (!run->used)
            continue;
        res_t *r = StoreGet(run->id, RES_ID_LEN);
        if (r != NULL && ResIsDesired(r))
            continue;
        if (run->nreps == 0) {
            run_free(run);
            continue;
        }
        escalate_expired(run);
        for (int j = 0; j < run->nreps; j++)
            replica_terminate(run, j, "resource gone");
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
        for (int j = 0; j < g_runs[i].nreps; j++) {
            if (g_runs[i].reps[j].pid == pid) {
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
        for (int j = 0; j < run->nreps; j++)
            kill(run->reps[j].pid, SIGTERM);
    }
}
