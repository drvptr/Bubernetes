#ifndef EVENT_H
#define EVENT_H

#include <stdint.h>

/*
 * Periodic tasks.
 *
 * Everything the daemon does on a timer - a SWIM tick, a gossip round, a
 * reconcile - is just a function registered here and run from the one event
 * loop. This is also where the "a sidecar is a periodic task, not a process"
 * idea lands: a sidecar that polls a socket or rotates a log is another entry in
 * this list, sharing the loop, not a second process to supervise. Two hundred of
 * them cost two hundred small callbacks, not two hundred pids.
 */

#define MAX_PERIODIC 64

void    periodic_add(int64_t interval_ms, void (*fn)(void *ctx), void *ctx,
                     const char *name);
void    periodic_run_due(void);          /* run every task whose time has come */
int64_t periodic_next_delay(void);       /* ms until the next task is due */

#endif
