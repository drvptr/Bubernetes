/*
 * A sample Bubernetes workload. Compile it static:  cc -static -o workload workload.c
 * Then hand the binary to bubectl as a Deploy/Static image. It never touches the
 * filesystem again - bubelet runs it straight from memory.
 *
 * It reads a little context from the environment that bubelet sets per replica,
 * prints a heartbeat once a second, and exits cleanly on SIGTERM so the
 * reconciler can see it stop.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>

static volatile sig_atomic_t stop;

static void on_term(int sig)
{
    (void)sig;
    stop = 1;
}

int main(int argc, char **argv)
{
    signal(SIGTERM, on_term);
    signal(SIGINT, on_term);

    const char *name = getenv("BUBE_NAME");
    const char *node = getenv("BUBE_NODE");
    const char *rep = getenv("BUBE_REPLICA");
    if (name == NULL) name = (argc > 0 ? argv[0] : "workload");
    if (node == NULL) node = "?";
    if (rep == NULL) rep = "0";

    /* print the argv we were given, so manifests' argv is observable */
    char args[512];
    args[0] = '\0';
    for (int i = 1; i < argc; i++) {
        strncat(args, argv[i], sizeof args - strlen(args) - 2);
        strncat(args, " ", sizeof args - strlen(args) - 2);
    }

    printf("[%s] workload '%s' replica %s up on node %s pid %ld args=[%s]\n",
           node, name, rep, node, (long)getpid(), args);
    fflush(stdout);

    if (getenv("BUBE_ONCE") != NULL)
        return 0;

    long beat = 0;
    while (!stop) {
        sleep(1);
        beat++;
        printf("[%s] '%s' replica %s heartbeat %ld\n", node, name, rep, beat);
        fflush(stdout);
    }
    printf("[%s] '%s' replica %s stopping\n", node, name, rep);
    fflush(stdout);
    return 0;
}
