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

// --- Static Variables ---
static shm_region_t *region = NULL;              // Pointer to the mapped shared memory region.
static volatile sig_atomic_t teller_running = 1; // Flag to control the main teller loop, set by signal handler.

// --- Signal Handler for Teller ---
/**
 * @brief Signal handler for SIGINT/SIGTERM received by the Teller.
 *        Sets the 'teller_running' flag to initiate graceful shutdown.
 */
static void teller_sig_handler(int signo)
{
    (void)signo;        // Unused parameter.
    teller_running = 0; // Signal the main loop to terminate.
}

// --- SHM Management ---
/**
 * @brief Attaches the Teller process to the existing shared memory segment
 *        created by the Bank Server.
 * @return 0 on success, -1 on failure.
 */
static int attach_shm()
{
    // Open the existing shared memory object created by the server.
    int fd = shm_open(SHM_NAME, O_RDWR, 0);
    if (fd == -1)
    {
        fprintf(stderr, "Teller(PID%d): Failed SHM open '%s': %s\n", getpid(), SHM_NAME, strerror(errno));
        return -1;
    }

    // Map the shared memory segment into this Teller's address space.
    region = mmap(NULL, sizeof(shm_region_t), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd); // File descriptor is no longer needed after successful mmap.

    if (region == MAP_FAILED)
    {
        perror("Teller mmap failed");
        region = NULL; // Ensure region is NULL on failure.
        return -1;
    }
    return 0; // Success.
}

/**
 * @brief Detaches the Teller process from the shared memory segment.
 */
static void detach_shm()
{
    if (region != NULL && region != MAP_FAILED)
    {
        munmap(region, sizeof(shm_region_t));
        region = NULL;
    }
}

// --- Request Queue Interaction ---
/**
 * @brief Pushes a client request onto the shared memory request queue for the server.
 *        Handles synchronization using semaphores (slots, items, qmutex).
 *        Checks the 'teller_running' flag to allow graceful exit if signaled.
 * @param src Pointer to the request_t structure to be pushed.
 * @return The index (slot_idx) in the queue where the request was placed, or -1 on failure or shutdown.
 */
static int push_request(const request_t *src)
{
    if (!region)
        return -1; // SHM must be attached.

    int idx = -1; // Index where request is placed.

    // 1. Wait for an empty slot in the queue.
    // Loop handles EINTR in case sem_wait is interrupted by a signal.
    while (sem_wait(&region->slots) == -1)
    {
        if (errno == EINTR && teller_running)
            continue; // Interrupted, but still running: retry.
        if (!teller_running)
            return -1; // Interrupted and should exit.
        perror("Teller sem_wait(slots)");
        return -1; // Other semaphore error.
    }

    // If signaled *after* acquiring a slot, release the slot and exit.
    if (!teller_running)
    {
        sem_post(&region->slots);
        return -1;
    }

    // 2. Acquire mutex to protect queue indices (head/tail).
    while (sem_wait(&region->qmutex) == -1)
    {
        if (errno == EINTR && teller_running)
            continue; // Interrupted, retry.
        if (!teller_running)
        {
            sem_post(&region->slots); // Release the slot we acquired earlier.
            return -1;
        }
        perror("Teller sem_wait(qmutex)");
        sem_post(&region->slots); // Release slot before returning error.
        return -1;
    }

    // --- Critical Section (Queue Index Access) ---
    idx = region->tail;                                // Get index to write to.
    region->queue[idx] = *src;                         // Copy request data into the queue slot.
    region->tail = (region->tail + 1) % REQ_QUEUE_LEN; // Advance tail index (circular).
    // --- End Critical Section ---

    sem_post(&region->qmutex); // Release queue mutex.

    // 3. Signal the server that a new item is available in the queue.
    sem_post(&region->items);

    return idx; // Return the slot index used.
}

// --- Utility ---
/**
 * @brief Parses a bank ID string provided by the client.
 *        Accepts "N" (or "BankID_None") for new account, "BankID_X" format, or plain numeric "X".
 * @param bank_id_str The string to parse.
 * @return Parsed bank ID (>= 0), -1 for new account request, or -2 for invalid format/value.
 */
static int parse_bank_id(const char *bank_id_str)
{
    int bank_id = -2; // Default to invalid format indicator.
    if (bank_id_str == NULL)
        return bank_id;

    // Check for "N" -> New account request.
    if (strcmp(bank_id_str, "N") == 0 || strcmp(bank_id_str, "BankID_None") == 0)
    {
        bank_id = -1; // Special value indicates new account request.
    }
    // Check for "BankID_X" format.
    else if (strncmp(bank_id_str, "BankID_", 7) == 0)
    {
        char *e;
        errno = 0;                                // Reset errno before strtol
        long v = strtol(bank_id_str + 7, &e, 10); // Parse number after "BankID_".
        // Check for successful parse, valid range, and no trailing characters.
        if (errno == 0 && *e == '\0' && v >= 0 && v < MAX_ACCOUNTS)
            bank_id = (int)v;
    }
    // Check for plain numeric format "X".
    else
    {
        char *e;
        errno = 0;
        long v = strtol(bank_id_str, &e, 10);
        // Check for successful parse, valid range, and no trailing characters.
        if (errno == 0 && *e == '\0' && v >= 0 && v < MAX_ACCOUNTS)
            bank_id = (int)v;
    }
    return bank_id;
}

// --- Main Teller Logic (Internal) ---
/**
 * @brief Core logic executed by the Teller process. Handles communication
 *        with a single client via dedicated FIFOs, parses commands, interacts
 *        with the server via the SHM queue, and sends responses back to the client.
 * @param arg The client's PID, passed as a void pointer.
 * @return Always returns NULL.
 */
static void *teller_main_inner(void *arg)
{
    pid_t client_pid = (pid_t)(intptr_t)arg; // Extract client PID from argument.
    pid_t teller_pid = getpid();             // This Teller's own PID.
    char req_path[128], res_path[128];       // Paths for client-specific FIFOs.
    int req_fd = -1, res_fd = -1;            // File descriptors for FIFOs.
    FILE *req_fp = NULL;                     // File stream for easier reading from request FIFO.
    char first_line_buffer[128] = {0};       // Buffer for the first command line (for "Welcome back").
    bool processed_first_line = false;       // Flag to track if the first line has been read/processed.

    // Construct FIFO paths using the client's PID.
    snprintf(req_path, sizeof(req_path), "/tmp/bank_%d_req", client_pid); // Client -> Teller
    snprintf(res_path, sizeof(res_path), "/tmp/bank_%d_res", client_pid); // Teller -> Client

    // --- Setup: Signal Handling & SHM ---
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = teller_sig_handler; // Use Teller-specific handler.
    sa.sa_flags = SA_RESTART;           // Restart syscalls if possible after signal.
    sigaction(SIGINT, &sa, NULL);       // Handle SIGINT (e.g., Ctrl+C on server).
    sigaction(SIGTERM, &sa, NULL);      // Handle SIGTERM from server shutdown.
    signal(SIGPIPE, SIG_IGN);           // Ignore SIGPIPE if client closes read end of response FIFO.

    // Attach to shared memory; exit if unsuccessful.
    if (attach_shm() != 0)
    {
        fprintf(stderr, "Teller(PID%d) for Client%d: Cannot attach SHM, exiting.\n", teller_pid, client_pid);
        return NULL;
    }

    // --- Open FIFOs (with retries) ---
    // The client creates the FIFOs; the Teller opens them. Retries handle the
    // race condition where the Teller might start slightly before the client finishes FIFO creation.
    const int MAX_FIFO_ATTEMPTS = 15;
    const useconds_t RETRY_DELAY_US = 100 * 1000; // 100ms delay between attempts.

    // Open Request FIFO (Read-Only by Teller).
    for (int i = 0; i < MAX_FIFO_ATTEMPTS && teller_running; ++i)
    {
        req_fd = open(req_path, O_RDONLY);
        if (req_fd != -1)
            break; // Success.
        if (errno == ENOENT)
        {
            usleep(RETRY_DELAY_US); // FIFO not created yet, wait and retry.
        }
        else
        {
            perror("Teller open req FIFO");
            break; // Other error, stop trying.
        }
    }
    if (req_fd == -1)
    {
        fprintf(stderr, "Teller(PID%d) for Client%d: Failed to open request FIFO '%s' after retries, exiting.\n", teller_pid, client_pid, req_path);
        detach_shm();
        return NULL;
    }

    // Open Response FIFO (Write-Only by Teller).
    // This might block until the client opens the read end.
    for (int i = 0; i < MAX_FIFO_ATTEMPTS && teller_running; ++i)
    {
        res_fd = open(res_path, O_WRONLY);
        if (res_fd != -1)
            break; // Success.
        if (errno == ENOENT)
        { // Should be less common here if req_fd opened.
            usleep(RETRY_DELAY_US);
        }
        else
        {
            perror("Teller open res FIFO");
            break; // Other error.
        }
    }
    if (res_fd == -1)
    {
        fprintf(stderr, "Teller(PID%d) for Client%d: Failed to open response FIFO '%s' after retries, exiting.\n", teller_pid, client_pid, res_path);
        close(req_fd); // Close request FIFO if it was opened.
        detach_shm();
        return NULL;
    }

    // Associate a file stream with the request FIFO for convenient line-based reading (fgets).
    req_fp = fdopen(req_fd, "r");
    if (!req_fp)
    {
        perror("Teller fdopen req_fd");
        close(req_fd);
        close(res_fd);
        detach_shm();
        return NULL;
    }

    // --- "Welcome Back" Logic ---
    // Read the *first* command line from the client to check if it references
    // an existing account, allowing a "Welcome back" message.
    if (teller_running && fgets(first_line_buffer, sizeof(first_line_buffer), req_fp) != NULL)
    {
        first_line_buffer[strcspn(first_line_buffer, "\n\r")] = 0; // Strip newline.
        if (strlen(first_line_buffer) > 0 && first_line_buffer[0] != '#')
        {
            // Parse the first command to extract the BankID.
            char b_id_str[64] = "", op_str[32] = "", am_str[32] = "";
            if (sscanf(first_line_buffer, "%63s %31s %31s", b_id_str, op_str, am_str) == 3)
            {
                int first_cmd_bank_id = parse_bank_id(b_id_str);
                // Check if it's a valid existing account ID (>=0).
                if (first_cmd_bank_id >= 0)
                {
                    // Safely check the balance array in SHM.
                    sem_wait(&region->dbmutex);
                    if (region->balances[first_cmd_bank_id] != ACCOUNT_INACTIVE)
                    {
                        // Account exists, print the welcome message using the Teller's PID.
                        printf("-- Teller PID%d is active serving Client%d… Welcome back Client%d\n",
                               teller_pid, client_pid, client_pid);
                        fflush(stdout);
                    }
                    sem_post(&region->dbmutex);
                }
            }
            // Mark the first line as buffered, regardless of welcome message print.
            processed_first_line = true;
        }
        else
        {
            processed_first_line = true; // Also mark if first line was empty/comment.
        }
    }
    else
    { // Error reading first line or Teller was signaled to stop.
        if (teller_running && ferror(req_fp))
        { // Only print error if not stopped and actual error occurred.
            fprintf(stderr, "Teller(PID%d) for Client%d: Error reading first command: %s\n", teller_pid, client_pid, strerror(errno));
        }
        else if (teller_running)
        { // EOF received immediately
            fprintf(stderr, "Teller(PID%d) for Client%d: Client disconnected before sending commands.\n", teller_pid, client_pid);
        }
        // Cleanup and exit if the first interaction fails.
        fclose(req_fp); // Closes req_fd too.
        close(res_fd);
        detach_shm();
        return NULL;
    }

    // --- Main Command Processing Loop ---
    char line[128];
    while (teller_running)
    {
        // Use the buffered first line on the first iteration.
        if (processed_first_line)
        {
            strncpy(line, first_line_buffer, sizeof(line) - 1);
            line[sizeof(line) - 1] = '\0'; // Ensure null termination.
            processed_first_line = false;  // Reset flag for subsequent iterations.
        }
        else
        {
            // Read subsequent command lines from the client.
            if (fgets(line, sizeof(line), req_fp) == NULL)
            {
                // End of file (client disconnected) or read error.
                if (ferror(req_fp))
                {
                    if (errno == EINTR && teller_running)
                        continue; // Interrupted, retry read.
                    perror("Teller fgets");
                }
                // Normal EOF is expected when client finishes sending commands.
                break; // Exit loop on EOF or error.
            }
            line[strcspn(line, "\n\r")] = 0; // Strip newline/cr.
        }

        // Skip empty lines and comments.
        if (strlen(line) == 0 || line[0] == '#')
            continue;

        // --- Parse Client Request ---
        request_t rq = {0};         // Initialize request struct for SHM.
        rq.client_pid = client_pid; // Store client PID for server logging.

        char bank_id_str[64] = "", op_str[32] = "", am_str[32] = "";
        long parsed_amount;

        // Basic parsing of the command line.
        if (sscanf(line, "%63s %31s %31s", bank_id_str, op_str, am_str) != 3)
        {
            fprintf(stderr, "Teller(PID%d) WARN: Invalid command format: %s\n", teller_pid, line);
            // Send error response back to client. Check for EPIPE.
            if (dprintf(res_fd, "Client%d something went WRONG\n", client_pid) < 0 && errno == EPIPE)
                teller_running = 0;
            continue; // Skip to next command.
        }

        // Parse Bank ID string ("N", "BankID_X", "X").
        rq.bank_id = parse_bank_id(bank_id_str);
        if (rq.bank_id == -2)
        { // Handle invalid BankID format.
            fprintf(stderr, "Teller(PID%d) WARN: Invalid BankID format: %s\n", teller_pid, bank_id_str);
            if (dprintf(res_fd, "Client%d something went WRONG\n", client_pid) < 0 && errno == EPIPE)
                teller_running = 0;
            continue;
        }

        // Parse Operation Type ("deposit", "withdraw").
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

        // Parse Amount (must be a positive integer).
        char *e;
        errno = 0;
        parsed_amount = strtol(am_str, &e, 10);
        if (errno != 0 || *e != '\0' || parsed_amount <= 0)
        {
            fprintf(stderr, "Teller(PID%d) WARN: Invalid or non-positive amount: %s\n", teller_pid, am_str);
            if (dprintf(res_fd, "Client%d something went WRONG\n", client_pid) < 0 && errno == EPIPE)
                teller_running = 0;
            continue;
        }
        rq.amount = parsed_amount;

        // Semantic check: Cannot withdraw from a non-existent account ("N").
        if (rq.bank_id == -1 && rq.type == REQ_WITHDRAW)
        {
            fprintf(stderr, "Teller(PID%d) WARN: Cannot withdraw from new account request ('N')\n", teller_pid);
            if (dprintf(res_fd, "Client%d something went WRONG\n", client_pid) < 0 && errno == EPIPE)
                teller_running = 0;
            continue;
        }

        // If signaled to stop during parsing, exit before pushing request.
        if (!teller_running)
            break;

        // --- Submit Request to Server via SHM Queue ---
        int slot_idx = push_request(&rq);
        if (slot_idx == -1)
        {
            // Failed to push request (queue full, server issue, or shutdown signal).
            if (!teller_running)
                break; // Exit if signaled.

            // Otherwise, assume transient error or server issue. Inform client.
            fprintf(stderr, "Teller(PID%d) ERROR: Failed to push request to server queue.\n", teller_pid);
            if (dprintf(res_fd, "Client%d something went WRONG\n", client_pid) < 0 && errno == EPIPE)
                teller_running = 0;
            break; // Stop processing client commands on queue failure.
        }

        // --- Wait for Response from Server ---
        // Wait on the specific semaphore corresponding to the queue slot used.
        int wait_ret;
        while ((wait_ret = sem_wait(&region->resp_ready[slot_idx])) == -1)
        {
            if (errno == EINTR && teller_running)
                continue; // Interrupted, but still running: retry wait.
            break;        // Exit wait loop if not running or other semaphore error.
        }

        // Check if wait succeeded or if we should exit.
        if (wait_ret == -1)
        {
            if (!teller_running)
                break; // Exiting due to signal.
            // An error here likely indicates a server-side problem or semaphore issue.
            perror("Teller sem_wait(resp_ready)");
            fprintf(stderr, "Teller(PID%d) ERROR: Failed waiting for server response.\n", teller_pid);
            if (dprintf(res_fd, "Client%d something went WRONG\n", client_pid) < 0 && errno == EPIPE)
                teller_running = 0;
            break; // Stop processing on response wait failure.
        }

        // --- Process Server Results (Read from SHM Queue Slot) ---
        // Accessing the queue slot here is safe without qmutex because:
        // 1. Server wrote results *before* posting resp_ready[slot_idx].
        // 2. Only this Teller was waiting on this specific semaphore.
        // 3. Server won't reuse this slot until the Teller pushes another request
        //    (which first requires sem_wait(&region->slots)).
        long res_bal = region->queue[slot_idx].result_balance; // Resulting balance from server.
        int res_id = region->queue[slot_idx].bank_id;          // Resulting bank ID (may differ on CREATE).
        int status = region->queue[slot_idx].op_status;        // Operation status code from server.

        // --- Format and Send Response Back to Client ---
        int dprintf_ret = -1;
        switch (status)
        {
        case 0: // OK - Success
            if (rq.type == REQ_DEPOSIT && rq.bank_id == -1)
            {
                // Successful CREATE (deposit with 'N') -> return the new BankID.
                dprintf_ret = dprintf(res_fd, "Client%d served.. BankID_%d\n", client_pid, res_id);
            }
            else if (rq.type == REQ_WITHDRAW && res_bal == 0)
            {
                // Successful WITHDRAW that closed the account (balance is now 0).
                dprintf_ret = dprintf(res_fd, "Client%d served.. account closed\n", client_pid);
            }
            else
            {
                // Successful DEPOSIT to existing account or regular WITHDRAW (account still open).
                dprintf_ret = dprintf(res_fd, "Client%d served.. BankID_%d\n", client_pid, res_id);
            }
            break;
        case 1:  // Insufficient funds (specific error code from server).
        case 2:  // General error (invalid ID, bank full, overflow, etc.).
        default: // Handle unknown/unexpected status codes as errors.
            dprintf_ret = dprintf(res_fd, "Client%d something went WRONG\n", client_pid);
            break;
        }

        // Check the result of writing to the response FIFO.
        if (dprintf_ret < 0)
        {
            if (errno == EPIPE)
            {
                // Client closed the read end of the response pipe. Treat as disconnection.
                teller_running = 0; // Signal Teller loop to stop.
            }
            else
            {
                perror("Teller write response");
                teller_running = 0; // Stop on other unexpected write errors.
            }
        }

    } // End while(teller_running) - Main Command Processing Loop

    // --- Teller Cleanup ---
    // This section is reached on loop exit (EOF, error, or signal).
    // printf("Teller(PID%d) for Client%d cleaning up...\n", teller_pid, client_pid); // Optional debug msg
    if (req_fp)
        fclose(req_fp); // Closes underlying req_fd as well.
    else if (req_fd != -1)
        close(req_fd); // Close fd if fdopen failed but open succeeded.
    if (res_fd != -1)
        close(res_fd); // Close response FIFO fd.
    detach_shm();      // Detach from shared memory.

    // Note: Teller does NOT unlink the FIFOs; the client is responsible for that.
    return NULL; // Teller process/thread exits.
}

// --- Teller Entry Point ---
/**
 * @brief The entry function called by the Server's `Teller` wrapper (fork).
 *        Simply calls the internal logic function.
 * @param arg Argument passed from the server (client PID).
 * @return Always returns NULL.
 */
void *teller_main(void *arg)
{
    teller_main_inner(arg);
    return NULL;
}