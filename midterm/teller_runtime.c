#define _GNU_SOURCE
#include <sched.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <stdio.h>
#include <errno.h>
#include "common.h" // Includes the correct Teller signature prototype

// Child entry point for clone
static int child_start(void *arg) {
    // Unpack arguments
    teller_main_func_t fn = ((teller_main_func_t*)arg)[0];
    void *farg = ((void**)arg)[1];

    // Free the argument package allocated by Teller()
    free(arg);

    // Execute the actual teller main function
    fn(farg);

    // Child must exit using _exit after clone
    _exit(0);
}

/**
 * @brief Creates a Teller process using clone.
 *        THIS IS THE FUNCTION DEFINITION - Signature MUST match common.h
 *
 * @param func The teller_main function pointer.
 * @param arg The argument to pass to teller_main.
 * @param stack Pointer to the stack allocated by the caller (server).
 * @param stack_size Size of the allocated stack.
 * @return pid_t The PID of the created teller process, or -1 on failure.
 */
pid_t Teller(teller_main_func_t func, void *arg, void *stack, size_t stack_size) { // <--- CORRECTED SIGNATURE
    if (!stack) {
        fprintf(stderr, "Teller Runtime: Invalid stack provided.\n");
        errno = EINVAL;
        return -1;
    }

    // Pack the function pointer and its argument for child_start
    void **pack = malloc(2 * sizeof(void*));
    if (!pack) {
        perror("Teller Runtime: malloc pack failed");
        // Stack belongs to caller, do not free here
        errno = ENOMEM;
        return -1;
    }
    pack[0] = (void*)func;
    pack[1] = arg;

    // Calculate the top of the stack (stack grows downwards)
    void *stack_top = (char*)stack + stack_size;

    // clone flags: SIGCHLD ensures parent gets signal on termination.
    pid_t pid = clone(child_start, stack_top, SIGCHLD, pack);

    if (pid == -1) {
        perror("Teller Runtime: clone failed");
        // Keep errno from clone
        free(pack); // Free pack if clone failed
        // Stack belongs to the caller, do not free here. The caller (server)
        // is responsible for freeing the stack if Teller() returns -1.
    }
    // If clone succeeds:
    // - 'pack' will be freed by child_start in the child process.
    // - 'stack' will be freed by the caller (server) after waitpid in the parent process.

    return pid;
}

// waitpid wrapper (remains unchanged, could be removed if unused)
int waitTeller(pid_t pid, int *status) {
    return waitpid(pid, status, 0);
}