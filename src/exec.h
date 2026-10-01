#ifndef EXEC_H
#define EXEC_H

#include <sys/types.h>
#include <stddef.h>

/*
 * The execution primitives. Three small calls; all policy (how many replicas,
 * where, when to restart) lives in the reconciler.
 *
 * The model:
 *   1. exec_memfd() turns an in-memory image into an anonymous, sealed,
 *      executable file that was never on disk.
 *   2. exec_spawn() fork()s and the child fexecve()s that memfd. Calling it
 *      several times on the SAME memfd gives several replicas whose read-only
 *      .text is backed by that one memfd inode - so the code exists once in
 *      physical memory and every replica maps it, while each keeps its own
 *      private data/stack. That is the cheap-replica goal, reached through a
 *      shared backing file rather than by asking the workload to fork itself.
 *   3. exec_reap() collects children that have exited, so the reconciler can
 *      update runtime state and decide whether to replace them.
 *
 * Only statically linked images work, by design: there is no filesystem for a
 * dynamic loader to find libraries in, and that is the point.
 */

/* build an anonymous in-memory executable from image bytes. Returns an fd to
 * fexecve, or -1. The caller spawns from it, then closes it. */
int exec_memfd(const char *name, const void *image, size_t len);

/* fork and fexecve the memfd with argv/envp. Returns the child pid, or -1.
 * The memfd fd stays owned by the caller. */
pid_t exec_spawn(int memfd, char *const argv[], char *const envp[]);

/* reap all children that have exited since last call; on_exit is called for
 * each with its pid and waitpid status. Returns how many were reaped. */
int exec_reap(void (*on_exit)(pid_t pid, int status, void *ctx), void *ctx);

#endif
