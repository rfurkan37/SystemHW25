#define _GNU_SOURCE
#include <sched.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include "common.h"

typedef void* (*teller_fn)(void*);

static int child_start(void *arg) {
    teller_fn fn = ((teller_fn*)arg)[0];
    void *farg = ((void**)arg)[1];
    free(arg);        /* allocated in Teller */
    fn(farg);
    _exit(0);
}

pid_t Teller(void *(*func)(void*), void *arg) {
    const size_t STACK_SZ = 1 << 20; /* 1 MB */
    void *stack = malloc(STACK_SZ);
    if (!stack) return -1;

    /* pack both func and arg so child can unpack */
    void **pack = malloc(2 * sizeof(void*));
    if (!pack) {
        free(stack);
        return -1;
    }
    pack[0] = (void*)func;
    pack[1] = arg;

    void *stack_top = (char*)stack + STACK_SZ;
    pid_t pid = clone(child_start, stack_top, SIGCHLD, pack);
    if (pid == -1) {
        free(stack);
        free(pack);
    }
    return pid;
}

int waitTeller(pid_t pid, int *status) {
    return waitpid(pid, status, 0);
}
