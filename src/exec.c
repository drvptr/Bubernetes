#define _GNU_SOURCE
#include "exec.h"
#include "log.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <sys/syscall.h>

int exec_memfd(const char *name, const void *image, size_t len)
{
    int fd = memfd_create(name, MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0) {
        PERR("memfd_create");
        return -1;
    }
    if (write_full(fd, image, len) != 0) {
        PERR("write image to memfd");
        close(fd);
        return -1;
    }
    /* seal it: the image is immutable from here on, and since every replica
     * shares this one backing, no one can rewrite another's code */
    if (fcntl(fd, F_ADD_SEALS,
              F_SEAL_WRITE | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL) != 0) {
        /* not fatal: sealing is defence in depth, exec still works without it */
        WARN("could not seal memfd (%s), continuing", strerror(errno));
    }
    return fd;
}

pid_t exec_spawn(int memfd, char *const argv[], char *const envp[])
{
    pid_t pid = fork();
    if (pid < 0) {
        PERR("fork");
        return -1;
    }
    if (pid == 0) {
        /* child: become the workload. fexecve maps the memfd as the new image;
         * nothing of the workload ever touched the filesystem. */
        /* if our bubelet dies, we should not outlive it as an orphan */
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        fexecve(memfd, argv, envp);
        /* only reached if exec failed */
        bube_log("ERR ", "exec_spawn/child", errno, "fexecve");
        _exit(127);
    }
    return pid;
}

int exec_reap(void (*on_exit)(pid_t pid, int status, void *ctx), void *ctx)
{
    int reaped = 0;
    for (;;) {
        int status;
        pid_t pid = waitpid(-1, &status, WNOHANG);
        if (pid < 0) {
            if (errno == EINTR)
                continue;
            break;      /* ECHILD: nothing left */
        }
        if (pid == 0)
            break;      /* children exist but none have exited */
        if (on_exit != NULL)
            on_exit(pid, status, ctx);
        reaped++;
    }
    return reaped;
}
