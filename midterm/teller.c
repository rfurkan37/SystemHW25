#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <time.h>
#include <sys/mman.h>
#include <poll.h>
// Remove duplicate includes if present

#include "common.h"

static shm_region_t *attach_region()
{
    int fd = shm_open(SHM_NAME, O_RDWR, 0);
    if (fd == -1)
    {
        fprintf(stderr, "Teller (%d): Failed to open shared memory (is server running?). Error: %s\n", getpid(), strerror(errno));
        exit(1); // Exit teller if SHM fails
    }
    shm_region_t *reg = mmap(NULL, sizeof(shm_region_t),
                             PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (reg == MAP_FAILED)
    {
        perror("Teller mmap");
        close(fd);
        exit(1); // Exit teller if mmap fails
    }
    close(fd);
    return reg;
}

/* push request and return pointer to queue slot */
static request_t *push_request(shm_region_t *reg, const request_t *src)
{
    // Consider adding a timeout to sem_wait if the server might get stuck
    sem_wait(&reg->slots);  /* wait for empty slot */
    sem_wait(&reg->qmutex); /* lock queue */

    int idx = reg->tail;
    reg->queue[idx] = *src; /* copy request data */
    // use atomic store to set status
    __atomic_store_n(&reg->queue[idx].status, -1, __ATOMIC_SEQ_CST); // Mark as pending

    reg->tail = (reg->tail + 1) % REQ_QUEUE_LEN;

    sem_post(&reg->qmutex);
    sem_post(&reg->items);
    return &reg->queue[idx];
}

// Helper function to wait for the request status to change from -1
static int wait_for_request_completion(request_t *slot)
{
    // Wait until status is no longer -1
    while (__atomic_load_n(&slot->status, __ATOMIC_SEQ_CST) == -1)
    {
        struct timespec ts = {0, 5000000}; /* 5 ms sleep */
        nanosleep(&ts, NULL);
        // Add a timeout mechanism here if needed
    }
    return __atomic_load_n(&slot->status, __ATOMIC_SEQ_CST);
}

static void *teller_main_inner(void *arg)
{
    int client_pid = (int)(intptr_t)arg;
    char req_path[64], res_path[64];
    snprintf(req_path, sizeof(req_path), "/tmp/bank_%d_req", client_pid);
    snprintf(res_path, sizeof(res_path), "/tmp/bank_%d_res", client_pid);

    printf("Teller (%d): Trying to open FIFOs for client %d...\n", getpid(), client_pid); // DEBUG

    // Use non-blocking open for request FIFO initially to check existence
    int req_fd = open(req_path, O_RDONLY | O_NONBLOCK);
    if (req_fd == -1)
    {
        fprintf(stderr, "Teller (%d): Error opening req FIFO %s: %s\n", getpid(), req_path, strerror(errno));
        return NULL; // Teller exits if request FIFO fails
    }
    printf("Teller (%d): Opened req FIFO %s (fd: %d)\n", getpid(), req_path, req_fd); // DEBUG


    int res_fd = open(res_path, O_WRONLY);
    if (res_fd == -1)
    {
        fprintf(stderr, "Teller (%d): Error opening res FIFO %s: %s\n", getpid(), res_path, strerror(errno));
        close(req_fd);
        return NULL; // Teller exits if response FIFO fails
    }
     printf("Teller (%d): Opened res FIFO %s (fd: %d)\n", getpid(), res_path, res_fd); // DEBUG

    // Set req_fd back to blocking mode for fgets
    int flags = fcntl(req_fd, F_GETFL, 0);
    if (flags == -1) {
        perror("Teller fcntl GETFL failed");
        close(req_fd); close(res_fd); return NULL;
    }
    if (fcntl(req_fd, F_SETFL, flags & ~O_NONBLOCK) == -1) {
        perror("Teller fcntl SETFL failed");
        close(req_fd); close(res_fd); return NULL;
    }
     printf("Teller (%d): Set req FIFO %d back to blocking mode.\n", getpid(), req_fd); // DEBUG


    FILE *req_fp = fdopen(req_fd, "r");
    if (!req_fp)
    {
        fprintf(stderr, "Teller (%d): fdopen failed for req_fd %d: %s\n", getpid(), req_fd, strerror(errno));
        close(req_fd); // fdopen does not close fd on failure
        close(res_fd);
        return NULL;
    }
    printf("Teller (%d): fdopen successful for req_fd %d.\n", getpid(), req_fd); // DEBUG

    // Attach to shared memory after FIFOs are confirmed open
    shm_region_t *reg = attach_region(); // reg is available here
    printf("Teller (%d): Attached to shared memory.\n", getpid()); // DEBUG


    printf("Teller (%d): Serving client %d. Entering command loop.\n", getpid(), client_pid);

    char line[128];
    int command_count = 0;
    // Main loop to process client commands
    while (1) // Changed loop condition for clarity with break/return
    {
        command_count++;
        printf("Teller (%d): Loop %d: Waiting for command from client %d via fgets...\n", getpid(), command_count, client_pid); // DEBUG

        if (fgets(line, sizeof(line), req_fp) == NULL) {
            // Check for EOF or error
            if (feof(req_fp)) {
                 printf("Teller (%d): Loop %d: fgets returned NULL (EOF reached). Client likely closed connection.\n", getpid(), command_count); // DEBUG
            } else {
                 fprintf(stderr, "Teller (%d): Loop %d: fgets returned NULL (Error reading from client FIFO): %s\n", getpid(), command_count, strerror(errno)); // DEBUG
            }
            break; // Exit loop on EOF or error
        }

        // Trim potential trailing newline BEFORE printing
        line[strcspn(line, "\n")] = 0;
        printf("Teller (%d): Loop %d: Received line: \"%s\"\n", getpid(), command_count, line); // DEBUG

        request_t rq = {0};
        rq.client_pid = client_pid;
        char bank_str[32], op[16];
        long amount;

        // --- Parsing Logic ---
        if (sscanf(line, "%31s %15s %ld", bank_str, op, &amount) != 3)
        {
             printf("Teller (%d): Loop %d: Bad format detected.\n", getpid(), command_count); // DEBUG
            dprintf(res_fd, "FAIL Protocol error: bad format '%s'\n", line);
            continue; // Skip to next command
        }
        printf("Teller (%d): Loop %d: Parsed: ID_str='%s', op='%s', amount=%ld\n", getpid(), command_count, bank_str, op, amount); // DEBUG

        // Bank ID Parsing
        if (strcmp(bank_str, "N") == 0 || strcmp(bank_str, "BankID_None") == 0) {
            rq.bank_id = -1;
        } else if (strncmp(bank_str, "BankID_", 7) == 0) {
            char *endptr;
            rq.bank_id = strtol(bank_str + 7, &endptr, 10);
            if (*endptr != '\0' || rq.bank_id < 0 || rq.bank_id >= MAX_ACCOUNTS) {
                 printf("Teller (%d): Loop %d: Invalid BankID format (prefixed).\n", getpid(), command_count); // DEBUG
                dprintf(res_fd, "FAIL Protocol error: invalid BankID format '%s'\n", bank_str);
                continue;
            }
        } else {
            char *endptr;
            rq.bank_id = strtol(bank_str, &endptr, 10);
            if (*endptr != '\0' || rq.bank_id < 0 || rq.bank_id >= MAX_ACCOUNTS) {
                 printf("Teller (%d): Loop %d: Invalid BankID format (raw).\n", getpid(), command_count); // DEBUG
                dprintf(res_fd, "FAIL Protocol error: invalid BankID format '%s'\n", bank_str);
                continue;
            }
        }

        // Operation Parsing
        if (strcmp(op, "deposit") == 0) {
            rq.type = REQ_DEPOSIT;
        } else if (strcmp(op, "withdraw") == 0) {
            rq.type = REQ_WITHDRAW;
        } else {
             printf("Teller (%d): Loop %d: Unknown operation '%s'.\n", getpid(), command_count, op); // DEBUG
            dprintf(res_fd, "FAIL Protocol error: unknown operation '%s'\n", op);
            continue;
        }

        // Amount Validation
        rq.amount = amount;
        if (rq.amount <= 0) {
             printf("Teller (%d): Loop %d: Invalid amount %ld.\n", getpid(), command_count, rq.amount); // DEBUG
            dprintf(res_fd, "FAIL Invalid amount: %ld\n", rq.amount);
            continue;
        }
        // --- End Parsing Logic ---

        printf("Teller (%d): Loop %d: Submitting request (ID: %d, Type: %d, Amount: %ld) to server...\n", getpid(), command_count, rq.bank_id, rq.type, rq.amount); // DEBUG
        request_t *slot = push_request(reg, &rq);

        printf("Teller (%d): Loop %d: Waiting for server processing...\n", getpid(), command_count); // DEBUG
        int final_status = wait_for_request_completion(slot);
        printf("Teller (%d): Loop %d: Server processing complete. Final Status: %d, Result Balance: %ld, Result ID: %d\n", getpid(), command_count, final_status, slot->result_balance, slot->bank_id); // DEBUG


        // --- Response Logic ---
        printf("Teller (%d): Loop %d: Preparing response for status %d...\n", getpid(), command_count, final_status); // DEBUG
        int dprintf_ret = -1; // Variable to store dprintf return value
        switch (final_status)
        {
        case 0: // OK
            // Check if it was a withdrawal that resulted in zero balance (implies closure)
            // We rely on the server's result_balance, not direct SHM access here.
            if (rq.type == REQ_WITHDRAW && slot->result_balance == 0) {
                dprintf_ret = dprintf(res_fd, "OK Account closed. Final Balance: 0 BankID_%d\n", slot->bank_id);
            } else if (rq.bank_id == -1) { // First deposit response uses slot->bank_id (correctly assigned by server)
                dprintf_ret = dprintf(res_fd, "OK Account created. BankID_%d balance=%ld\n", slot->bank_id, slot->result_balance);
            } else { // Standard successful deposit or withdrawal (not resulting in zero)
                dprintf_ret = dprintf(res_fd, "OK BankID_%d balance=%ld\n", slot->bank_id, slot->result_balance);
            }
            break;
        case 1: // Insufficient funds
             // Use slot->bank_id here as the server should have filled it even on failure for context
            dprintf_ret = dprintf(res_fd, "FAIL insufficient balance=%ld BankID_%d\n", slot->result_balance, slot->bank_id);
            break;
        case 2: // Error
        default: // Treat unexpected statuses as errors too
            // Use slot->bank_id if available and valid, otherwise fallback to rq.bank_id for error reporting
            int report_id = (slot->bank_id >= 0 && slot->bank_id < MAX_ACCOUNTS) ? slot->bank_id : rq.bank_id;
            dprintf_ret = dprintf(res_fd, "FAIL operation failed (invalid account or server error) BankID_%d\n", report_id);
            break;
        }

        printf("Teller (%d): Loop %d: Sent response (dprintf returned %d).\n", getpid(), command_count, dprintf_ret); // DEBUG
        if (dprintf_ret < 0) {
             fprintf(stderr, "Teller (%d): ERROR writing response to client %d: %s\n", getpid(), client_pid, strerror(errno));
             // Optionally break loop if writing fails consistently
             // break;
        }

        // REMOVED FSYNC CALL for FIFO
        printf("Teller (%d): Loop %d: Response write attempted.\n", getpid(), command_count); // DEBUG
        // --- End Response Logic ---

    } // End while loop

    printf("Teller (%d): Exited command loop. Cleaning up for client %d.\n", getpid(), client_pid); // DEBUG

    // Cleanup resources
    printf("Teller (%d): Unmapping shared memory.\n", getpid()); // DEBUG
    munmap(reg, sizeof(shm_region_t)); // Detach SHM
    printf("Teller (%d): Closing FILE* stream for req_fd.\n", getpid()); // DEBUG
    fclose(req_fp); // This also closes the underlying req_fd
    printf("Teller (%d): Closing res_fd.\n", getpid()); // DEBUG
    close(res_fd);
    // Do not unlink FIFO files here

    printf("Teller (%d): Finished serving client %d. Exiting.\n", getpid(), client_pid); // DEBUG
    return NULL; // Teller process terminates
}


/* Wrapper for Teller */
void *teller_main(void *arg)
{
    // Can add process-wide setup here if needed
    teller_main_inner(arg);
    // Can add process-wide cleanup here if needed
    return NULL;
}