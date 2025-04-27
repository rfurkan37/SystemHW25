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
#include <stdint.h> // For intptr_t
#include <sys/types.h>
#include <limits.h>
#include <ctype.h>
#include <signal.h>
#include <stdbool.h> // For bool type

#include "common.h"

static shm_region_t *region = NULL;
static volatile sig_atomic_t teller_running = 1; // Flag controlled by signal handler

// --- Signal Handler for Teller ---
static void teller_sig_handler(int signo)
{
    (void)signo; // Unused parameter
    // Set flag to indicate graceful shutdown requested
    teller_running = 0;
    // Avoid printf in signal handlers
    // const char msg[] = "Teller exiting...\n";
    // write(STDOUT_FILENO, msg, sizeof(msg)-1); // Maybe too verbose
}

// --- SHM Management ---
static int attach_shm()
{
    // Attaches to the existing shared memory segment. UNCHANGED.
    int fd = shm_open(SHM_NAME, O_RDWR, 0); // Open existing SHM
    if (fd == -1)
    {
        // Use getpid() to identify which teller failed
        fprintf(stderr, "Teller(PID%d): Failed SHM open '%s': %s\n", getpid(), SHM_NAME, strerror(errno));
        return -1;
    }

    // Map the shared memory segment into the teller's address space
    region = mmap(NULL, sizeof(shm_region_t), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd); // File descriptor no longer needed after mmap

    if (region == MAP_FAILED)
    {
        perror("Teller mmap failed");
        region = NULL; // Ensure region is NULL on failure
        return -1;
    }
    return 0; // Success
}

static void detach_shm()
{
    // Detaches from the shared memory segment. UNCHANGED.
    if (region != NULL && region != MAP_FAILED)
    {
        munmap(region, sizeof(shm_region_t));
        region = NULL;
    }
}

// --- Request Queue Interaction ---
static int push_request(const request_t *src)
{
    // Pushes a request onto the shared memory queue. UNCHANGED logic, added running checks.
    if (!region)
        return -1; // SHM not attached

    int idx = -1; // Index where request was placed

    // 1. Wait for an empty slot (decrement 'slots' semaphore)
    // Use while loop to handle EINTR correctly if SA_RESTART is not set or fails
    while (sem_wait(&region->slots) == -1)
    {
        if (errno == EINTR && teller_running)
            continue; // Interrupted by signal, but still running? Retry.
        if (!teller_running)
            return -1; // Interrupted and should exit
        perror("Teller sem_wait(slots)");
        return -1; // Other semaphore error
    }

    // If we got a slot but are now shutting down, release the slot and exit
    if (!teller_running)
    {
        sem_post(&region->slots);
        return -1;
    }

    // 2. Wait for queue mutex (protect head/tail indices)
    while (sem_wait(&region->qmutex) == -1)
    {
        if (errno == EINTR && teller_running)
            continue; // Interrupted, retry
        if (!teller_running)
        {
            sem_post(&region->slots); // Release the slot we acquired
            return -1;
        }
        perror("Teller sem_wait(qmutex)");
        sem_post(&region->slots); // Release slot before returning error
        return -1;
    }

    // --- Critical Section (Queue Access) ---
    idx = region->tail;                                // Get the tail index where we will write
    region->queue[idx] = *src;                         // Copy the request data into the queue slot
    region->tail = (region->tail + 1) % REQ_QUEUE_LEN; // Move tail index forward (wrap around)
    // --- End Critical Section ---

    sem_post(&region->qmutex); // Release queue mutex

    // 3. Signal that an item is available (increment 'items' semaphore)
    sem_post(&region->items);

    return idx; // Return the index where the request was placed
}

// --- Utility ---
static int parse_bank_id(const char *bank_id_str)
{
    // Parses bank ID string ("N", "BankID_X", "X"). UNCHANGED.
    int bank_id = -2; // Default to invalid format indicator
    if (bank_id_str == NULL)
        return bank_id;

    // Check for "N" or "BankID_None" -> New account request
    if (strcmp(bank_id_str, "N") == 0 || strcmp(bank_id_str, "BankID_None") == 0)
    {
        bank_id = -1; // Special value for new account
    }
    // Check for "BankID_X" format
    else if (strncmp(bank_id_str, "BankID_", 7) == 0)
    {
        char *e;
        long v = strtol(bank_id_str + 7, &e, 10); // Parse number after "BankID_"
        // Check if parsing consumed the whole string and value is in valid range
        if (*e == '\0' && v >= 0 && v < MAX_ACCOUNTS)
            bank_id = (int)v;
        // else: remains -2 (invalid format)
    }
    // Check for plain numeric format "X"
    else
    {
        char *e;
        long v = strtol(bank_id_str, &e, 10);
        // Check if parsing consumed the whole string
        int ok = (*e == '\0');
        // Additional check: ensure all characters were digits (strtol allows leading whitespace)
        for (const char *c = bank_id_str; *c && ok; ++c)
        {
            if (!isdigit((unsigned char)*c) && !isspace((unsigned char)*c))
            {   // Allow spaces if strtol handles them
                // Check if it's just leading/trailing space handled by strtol?
                // Simpler: just rely on strtol's *e check for basic numeric case.
            }
        }
        // Check if numeric and in valid range
        if (ok && v >= 0 && v < MAX_ACCOUNTS)
            bank_id = (int)v;
        // else: remains -2 (invalid format)
    }
    return bank_id;
}

// --- Main Teller Logic ---
static void *teller_main_inner(void *arg)
{
    pid_t client_pid = (pid_t)(intptr_t)arg; // Client PID passed as argument
    pid_t teller_pid = getpid();             // This Teller's own PID
    char req_path[128], res_path[128];
    int req_fd = -1, res_fd = -1;
    FILE *req_fp = NULL;               // File stream for reading requests
    char first_line_buffer[128] = {0}; // Buffer to hold the first command line
    bool processed_first_line = false; // Flag to ensure first line is processed only once

    // Construct FIFO paths based on client PID
    snprintf(req_path, sizeof(req_path), "/tmp/bank_%d_req", client_pid);
    snprintf(res_path, sizeof(res_path), "/tmp/bank_%d_res", client_pid);

    // --- Setup: Signal Handling, SHM Attach ---
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = teller_sig_handler; // Use teller-specific handler
    sa.sa_flags = SA_RESTART;           // Restart syscalls if possible
    sigaction(SIGINT, &sa, NULL);       // Handle Ctrl+C from server's group
    sigaction(SIGTERM, &sa, NULL);      // Handle termination signal from server
    signal(SIGPIPE, SIG_IGN);           // Ignore SIGPIPE if client disconnects pipe

    if (attach_shm() != 0)
    {
        fprintf(stderr, "Teller(PID%d) for Client%d: Cannot attach SHM, exiting.\n", teller_pid, client_pid);
        return NULL; // Exit if SHM cannot be attached
    }

    // --- Open FIFOs (with retries) ---
    // The client creates the FIFOs, the teller opens them.
    // Retry opening in case the teller starts slightly before the client creates the FIFO.
    const int MAX_FIFO_ATTEMPTS = 15;
    const useconds_t RETRY_DELAY_US = 100 * 1000; // 100 milliseconds

    // Open Request FIFO (Read-Only)
    for (int i = 0; i < MAX_FIFO_ATTEMPTS && teller_running; ++i)
    {
        req_fd = open(req_path, O_RDONLY);
        if (req_fd != -1)
            break; // Success
        if (errno == ENOENT)
        {
            // FIFO doesn't exist yet, wait and retry
            usleep(RETRY_DELAY_US);
        }
        else
        {
            perror("Teller open req FIFO");
            break; // Other error, stop trying
        }
    }
    if (req_fd == -1)
    {
        fprintf(stderr, "Teller(PID%d) for Client%d: Failed to open request FIFO '%s' after retries, exiting.\n", teller_pid, client_pid, req_path);
        detach_shm();
        return NULL;
    }

    // Open Response FIFO (Write-Only)
    for (int i = 0; i < MAX_FIFO_ATTEMPTS && teller_running; ++i)
    {
        res_fd = open(res_path, O_WRONLY);
        if (res_fd != -1)
            break; // Success
        if (errno == ENOENT)
        {
            usleep(RETRY_DELAY_US);
        }
        else
        {
            perror("Teller open res FIFO");
            break; // Other error
        }
    }
    if (res_fd == -1)
    {
        fprintf(stderr, "Teller(PID%d) for Client%d: Failed to open response FIFO '%s' after retries, exiting.\n", teller_pid, client_pid, res_path);
        close(req_fd); // Close request FIFO if open
        detach_shm();
        return NULL;
    }

    // Associate file stream with request FIFO fd for easier line reading
    req_fp = fdopen(req_fd, "r");
    if (!req_fp)
    {
        perror("Teller fdopen req_fd");
        close(req_fd);
        close(res_fd);
        detach_shm();
        return NULL;
    }

    // --- Welcome Back Logic (Check first command) ---
    // Read the first line to check if it's an operation on an existing account
    if (teller_running && fgets(first_line_buffer, sizeof(first_line_buffer), req_fp) != NULL)
    {
        first_line_buffer[strcspn(first_line_buffer, "\n\r")] = 0; // Strip newline
        if (strlen(first_line_buffer) > 0 && first_line_buffer[0] != '#')
        {
            // Try to parse the first command to see if it uses an existing BankID
            char b_id_str[64], op_str[32], am_str[32];
            if (sscanf(first_line_buffer, "%63s %31s %31s", b_id_str, op_str, am_str) == 3)
            {
                int first_cmd_bank_id = parse_bank_id(b_id_str);
                // Check if the parsed ID is valid (>=0) and the account exists in SHM
                // Need to lock dbmutex to safely read balance state
                sem_wait(&region->dbmutex);
                if (first_cmd_bank_id >= 0 && region->balances[first_cmd_bank_id] != ACCOUNT_INACTIVE)
                {
                    // Account exists, print welcome back message
                    // *** MODIFIED: Use teller_pid here, as requested in example output ***
                    // The server prints the initial assignment, teller prints the welcome back.
                    printf("-- Teller PID%d is active serving Client%d… Welcome back Client%d\n",
                           teller_pid, client_pid, client_pid);
                    fflush(stdout);
                }
                sem_post(&region->dbmutex);
            }
            // Mark that we have buffered the first line (even if no welcome message was printed)
            processed_first_line = true;
        }
        else
        {
            processed_first_line = true; // Also mark if line was empty/comment
        }
    }
    else // Error reading first line or teller stopped
    {
        if (teller_running) // Only print error if we weren't explicitly stopped
            fprintf(stderr, "Teller(PID%d) for Client%d: Error reading first command or client disconnected.\n", teller_pid, client_pid);
        fclose(req_fp); // req_fd is closed by fclose
        close(res_fd);
        detach_shm();
        return NULL;
    }

    // --- Main Processing Loop ---
    char line[128];
    while (teller_running)
    {
        // Use the buffered first line on the first iteration
        if (processed_first_line)
        {
            strncpy(line, first_line_buffer, sizeof(line) - 1);
            line[sizeof(line) - 1] = '\0';
            processed_first_line = false; // Reset flag so we read normally next time
        }
        else // Read subsequent lines from the file stream
        {
            if (fgets(line, sizeof(line), req_fp) == NULL)
            {
                // End of file or read error
                if (ferror(req_fp))
                {
                    if (errno == EINTR && teller_running)
                        continue; // Interrupted, retry read
                    perror("Teller fgets");
                }
                else
                {
                    // Normal EOF from client
                    // printf("Teller(PID%d) for Client%d: Client disconnected.\n", teller_pid, client_pid);
                }
                break; // Exit loop on EOF or error
            }
            line[strcspn(line, "\n\r")] = 0; // Strip newline/cr
        }

        // Skip empty lines and comments
        if (strlen(line) == 0 || line[0] == '#')
            continue;

        // --- Parse Request ---
        request_t rq = {0};         // Initialize request struct
        rq.client_pid = client_pid; // Store client PID in request

        char bank_id_str[64], op_str[32], am_str[32];
        long parsed_amount;

        if (sscanf(line, "%63s %31s %31s", bank_id_str, op_str, am_str) != 3)
        {
            fprintf(stderr, "Teller(PID%d) WARN: Invalid command format: %s\n", teller_pid, line);
            // Send error response to client
            if (dprintf(res_fd, "Client%d something went WRONG\n", client_pid) < 0 && errno == EPIPE)
                teller_running = 0; // Client pipe closed, stop teller
            continue;               // Skip to next command
        }

        // Parse Bank ID ("N", "BankID_X", "X")
        rq.bank_id = parse_bank_id(bank_id_str);
        if (rq.bank_id == -2)
        { // Invalid BankID format
            fprintf(stderr, "Teller(PID%d) WARN: Invalid BankID format: %s\n", teller_pid, bank_id_str);
            if (dprintf(res_fd, "Client%d something went WRONG\n", client_pid) < 0 && errno == EPIPE)
                teller_running = 0;
            continue;
        }

        // Parse Operation Type ("deposit", "withdraw")
        if (strcmp(op_str, "deposit") == 0)
            rq.type = REQ_DEPOSIT;
        else if (strcmp(op_str, "withdraw") == 0)
            rq.type = REQ_WITHDRAW;
        else
        {
            fprintf(stderr, "Teller(PID%d) WARN: Invalid operation type: %s\n", teller_pid, op_str);
            if (dprintf(res_fd, "Client%d something went WRONG\n", client_pid) < 0 && errno == EPIPE)
                teller_running = 0;
            continue;
        }

        // Parse Amount (must be positive integer)
        char *e;
        parsed_amount = strtol(am_str, &e, 10);
        if (*e != '\0' || parsed_amount <= 0)
        {
            fprintf(stderr, "Teller(PID%d) WARN: Invalid amount: %s\n", teller_pid, am_str);
            if (dprintf(res_fd, "Client%d something went WRONG\n", client_pid) < 0 && errno == EPIPE)
                teller_running = 0;
            continue;
        }
        rq.amount = parsed_amount;

        // Semantic check: Cannot withdraw from "N" (new account)
        if (rq.bank_id == -1 && rq.type == REQ_WITHDRAW)
        {
            if (dprintf(res_fd, "Client%d something went WRONG\n", client_pid) < 0)
            {
                if (errno == EPIPE)
                    teller_running = 0; // Client pipe closed, stop teller
                else
                {
                    perror("Teller dprintf WRONG response");
                    teller_running = 0;
                }
            }
            continue;
        }

        // If teller received signal while parsing, exit before pushing
        if (!teller_running)
            break;

        // --- Submit Request to Server via SHM Queue ---
        int slot_idx = push_request(&rq);
        if (slot_idx == -1)
        {
            // Failed to push request (maybe server shutting down or queue error)
            if (!teller_running)
                break; // Exit if we were signaled to stop

            if (dprintf(res_fd, "Client%d something went WRONG\n", client_pid) < 0 && errno == EPIPE)
                teller_running = 0;
            // Decide whether to break or continue? Continue might retry. Break is safer.
            break;
        }

        // --- Wait for Response from Server ---
        // Wait on the specific semaphore corresponding to the request slot
        int wait_ret;
        while ((wait_ret = sem_wait(&region->resp_ready[slot_idx])) == -1)
        {
            if (errno == EINTR && teller_running)
                continue; // Interrupted, but still running, retry wait
            break;        // Exit wait loop if !teller_running or other error
        }

        // Check if wait succeeded or if we should exit
        if (wait_ret == -1)
        {
            if (!teller_running)
                break; // Exiting due to signal
            perror("Teller sem_wait(resp_ready)");
            if (dprintf(res_fd, "Client%d something went WRONG\n", client_pid) < 0 && errno == EPIPE)
                teller_running = 0;
            break; // Exit loop on semaphore error
        }

        // --- Read Results from SHM Queue Slot ---
        // Note: Accessing the queue slot outside mutex is safe here because:
        // 1. Server wrote the result *before* posting resp_ready.
        // 2. Only this teller is waiting on resp_ready[slot_idx].
        // 3. Server won't reuse the slot until the *next* request cycle posts 'slots'.
        long res_bal = region->queue[slot_idx].result_balance; // Resulting balance
        int res_id = region->queue[slot_idx].bank_id;          // Resulting bank ID (can change on CREATE)
        int status = region->queue[slot_idx].op_status;        // Operation status (0=OK, 1=Insuff, 2=Error)

        // --- Format and Send Response to Client ---
        int dprintf_ret = -1;
        switch (status)
        {
        case 0: // Success
            if (rq.type == REQ_DEPOSIT && rq.bank_id == -1)
            {
                // Successful CREATE (deposit with N) -> return new BankID
                dprintf_ret = dprintf(res_fd, "Client%d served.. BankID_%d\n", client_pid, res_id);
            }
            else if (rq.type == REQ_WITHDRAW && res_bal == 0)
            {
                // Successful WITHDRAW that resulted in account closure
                dprintf_ret = dprintf(res_fd, "Client%d served.. account closed\n", client_pid);
            }
            else
            {
                // Successful DEPOSIT to existing or regular WITHDRAW -> return existing BankID
                dprintf_ret = dprintf(res_fd, "Client%d served.. BankID_%d\n", client_pid, res_id);
            }
            break;
        case 1:  // Insufficient funds (specific error for withdraw)
        case 2:  // General error (invalid ID, bank full, overflow, etc.)
        default: // Unknown status code
            dprintf_ret = dprintf(res_fd, "Client%d something went WRONG\n", client_pid);
            break;
        }

        // Check write result to response FIFO
        if (dprintf_ret < 0)
        {
            if (errno == EPIPE)
            {
                // Client closed the reading end of the pipe
                // printf("Teller(PID%d): Client%d disconnected (EPIPE).\n", teller_pid, client_pid);
                teller_running = 0; // Stop the teller
            }
            else
            {
                perror("Teller write response");
                teller_running = 0; // Stop on other write errors
            }
        }

    } // End while(teller_running)

    // --- Cleanup ---
    // printf("Teller(PID%d) for Client%d cleaning up...\n", teller_pid, client_pid);
    if (req_fp)
        fclose(req_fp); // Closes underlying req_fd too
    else if (req_fd != -1)
        close(req_fd); // Close if fdopen failed but open succeeded
    if (res_fd != -1)
        close(res_fd);
    detach_shm(); // Detach from shared memory
    // Note: Teller does NOT unlink the FIFOs, client should handle that

    return NULL; // Teller thread/process exits
}

// --- Teller Entry Point ---
// This function is called by the server's Teller/fork wrapper.
void *teller_main(void *arg)
{
    // Call the main logic function
    teller_main_inner(arg);
    // Return NULL, indicating thread/process completion
    return NULL;
}