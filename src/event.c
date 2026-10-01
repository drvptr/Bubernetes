#include "event.h"
#include "util.h"

struct task {
    int64_t interval;
    int64_t last;
    void (*fn)(void *ctx);
    void *ctx;
    const char *name;
    int used;
};

static struct task g_tasks[MAX_PERIODIC];
static int g_ntasks;

void periodic_add(int64_t interval_ms, void (*fn)(void *ctx), void *ctx,
                  const char *name)
{
    if (g_ntasks >= MAX_PERIODIC)
        return;
    struct task *t = &g_tasks[g_ntasks++];
    t->interval = interval_ms;
    t->last = monotime_ms();
    t->fn = fn;
    t->ctx = ctx;
    t->name = name;
    t->used = 1;
}

void periodic_run_due(void)
{
    int64_t now = monotime_ms();
    for (int i = 0; i < g_ntasks; i++) {
        struct task *t = &g_tasks[i];
        if (!t->used)
            continue;
        if (now - t->last >= t->interval) {
            t->last = now;
            t->fn(t->ctx);
        }
    }
}

int64_t periodic_next_delay(void)
{
    int64_t now = monotime_ms();
    int64_t soonest = 1000;     /* never sleep longer than this */
    for (int i = 0; i < g_ntasks; i++) {
        struct task *t = &g_tasks[i];
        if (!t->used)
            continue;
        int64_t due = t->last + t->interval - now;
        if (due < 0)
            due = 0;
        if (due < soonest)
            soonest = due;
    }
    return soonest;
}
