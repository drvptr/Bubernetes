#ifndef RECONCILER_H
#define RECONCILER_H

#include <sys/types.h>

/*
 * The reconciler closes the loop between desired state and reality, for this
 * node only. Each pass it asks, for every desired resource, "how many replicas
 * should I be running?" (the scheduler answers for DEPLOY; STATIC is pinned to
 * its origin) and compares that with how many it actually has. Too few and it
 * starts them - fetching the image from a peer first if it only holds the hash;
 * too many and it stops the surplus; none wanted and it stops them all. A
 * replica that exits on its own reappears next pass, so crashes self-heal.
 *
 * It never reaches onto another node. Every node reconciles itself, and because
 * they all compute placement the same way, the cluster as a whole converges.
 */

void reconcile_init(const char *self_name);
void reconcile_pass(void);
void reconcile_on_child_exit(pid_t pid, int status);
void reconcile_stop_all(void);

#endif
