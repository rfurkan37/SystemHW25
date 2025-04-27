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
#include <stdint.h>   // For intptr_t
#include <sys/types.h> // pid_t
#include <limits.h>   // For INT_MAX etc if needed
#include <ctype.h>    // For isdigit

#include "common.h"

// Global pointer to SHM region for this Teller instance
static shm_region_t *region = NULL;

// Attach to existing shared memory
static int attach_shm()
{
    int fd = shm_open(SHM_NAME, O_RDWR, 0);
    if (fd == -1)
    {
        // Log error specific to this Teller instance
        fprintf(stderr, "Teller (PID %d): Failed to open shared memory '%s'. Is server running? Error: %s\n",
                getpid(), SHM_NAME, strerror(errno));
        return -1; // Indicate failure
    }

    // Map the region
    region = mmap(NULL, sizeof(shm_region_t),
                  PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

    // Close the file descriptor after mapping, it's no longer needed
    close(fd);

    if (region == MAP_FAILED)
    {
        perror("Teller mmap failed");
        region = NULL; // Ensure region is NULL on failure
        return -1;     // Indicate failure
    }
    // printf("Teller (PID %d): Attached to shared memory region.\n", getpid()); // Debug
    return 0; // Success
}

// Detach from shared memory
static void detach_shm() {
    if (region != NULL && region != MAP_FAILED) {
        if (munmap(region, sizeof(shm_region_t)) == -1) {
            perror("Teller munmap detach failed");
        }
        region = NULL;
    }
}

/* Push request into the shared memory queue and return pointer to queue slot */
static request_t *push_request(const request_t *src)
{
    if (!region) return NULL; // Safety check

    // Wait for an empty slot to become available in the queue
    // Consider adding a timeout (sem_timedwait) if server might be unresponsive
    if (sem_wait(&region->slots) == -1) {
        perror("Teller sem_wait(slots) failed");
        return NULL;
    }

    // Lock the queue mutex to safely access the tail index and write data
    if (sem_wait(&region->qmutex) == -1) {
        perror("Teller sem_wait(qmutex) failed");
        sem_post(&region->slots); // Release the slot we acquired
        return NULL;
    }

    // --- Critical Section for Queue Access ---
    int idx = region->tail;
    region->queue[idx] = *src; // Copy request data into the slot
    // Mark status as pending using an atomic store
    __atomic_store_n(&region->queue[idx].status, -1, __ATOMIC_SEQ_CST);

    // Advance the tail index, wrapping around if necessary
    region->tail = (region->tail + 1) % REQ_QUEUE_LEN;
    // --- End Critical Section ---

    sem_post(&region->qmutex); // Unlock the queue mutex
    sem_post(&region->items);  // Signal that a new item is available for the server

    return &region->queue[idx]; // Return pointer to the slot (for waiting on status)
}

// Helper function to wait for the request status to change from -1 (pending)
// Returns the final status code from the server.
static int wait_for_request_completion(request_t *slot)
{
    if (!slot) return 2; // Error if slot is invalid

    // Poll the status field atomically until it's no longer -1
    // Use nanosleep to avoid pure busy-waiting
    int current_status = __atomic_load_n(&slot->status, __ATOMIC_SEQ_CST);
    int wait_iterations = 0;
    const int MAX_WAIT_ITERATIONS = 2000; // Approx 10 seconds (2000 * 5ms)

    while (current_status == -1)
    {
        if (++wait_iterations > MAX_WAIT_ITERATIONS) {
            fprintf(stderr, "Teller (PID %d): Timeout waiting for server response for slot.\n", getpid());
            // Attempt to recover the status one last time, maybe it changed just now
             current_status = __atomic_load_n(&slot->status, __ATOMIC_SEQ_CST);
             if(current_status == -1) return 2; // Return error status on timeout
             else return current_status; // Return the status if it finally changed
        }

        struct timespec ts = {0, 5000000}; // 5 milliseconds sleep
        nanosleep(&ts, NULL);

        current_status = __atomic_load_n(&slot->status, __ATOMIC_SEQ_CST);
    }
    return current_status; // Return the final status set by the server
}

// --- Main Teller Logic ---
// Argument 'arg' is the client PID passed as (void *)(intptr_t)
static void *teller_main_inner(void *arg)
{
    pid_t client_pid = (pid_t)(intptr_t)arg;
    pid_t teller_pid = getpid();
    char req_path[128], res_path[128];

    snprintf(req_path, sizeof(req_path), "/tmp/bank_%d_req", client_pid);
    snprintf(res_path, sizeof(res_path), "/tmp/bank_%d_res", client_pid);

    // printf("Teller (PID %d): Initializing for Client PID %d.\n", teller_pid, client_pid); // Debug

    // 1. Attach to Shared Memory
    if (attach_shm() != 0) {
        fprintf(stderr, "Teller (PID %d): Critical Error - Could not attach to shared memory. Exiting.\n", teller_pid);
        // Cannot communicate failure back to client easily here.
        return NULL; // Teller exits
    }

    // 2. Open FIFOs with Retry Logic
    // ***** REMOVED DUPLICATE DECLARATION HERE *****
    int req_fd = -1, res_fd = -1;
    int attempts = 0;
    const int MAX_ATTEMPTS = 15; // Try for ~1.5 seconds total
    const useconds_t RETRY_DELAY_US = 100000; // 100ms delay between retries

    printf("Teller (PID %d): Attempting to open FIFOs for Client PID %d...\n", teller_pid, client_pid);

    // --- Open Request FIFO (Reader) ---
    attempts = 0; // Reset attempts
    while (attempts < MAX_ATTEMPTS) {
        req_fd = open(req_path, O_RDONLY);
        if (req_fd != -1) {
            printf("Teller (PID %d): Opened request FIFO '%s' successfully.\n", teller_pid, req_path);
            break; // Success
        }
        if (errno == ENOENT) {
            usleep(RETRY_DELAY_US);
            attempts++;
        } else {
            fprintf(stderr, "Teller (PID %d): Error opening request FIFO '%s': %s.\n",
                    teller_pid, req_path, strerror(errno));
            break; // Fatal error other than not existing yet
        }
    }
    if (req_fd == -1) {
        fprintf(stderr, "Teller (PID %d): Failed to open request FIFO '%s' after %d attempts. Last Error: %s\n",
                teller_pid, req_path, MAX_ATTEMPTS, strerror(errno));
        detach_shm();
        return NULL; // Exit Teller
    }

    // --- Open Response FIFO (Writer) ---
    attempts = 0; // Reset attempts
    while (attempts < MAX_ATTEMPTS) {
        res_fd = open(res_path, O_WRONLY);
        if (res_fd != -1) {
            printf("Teller (PID %d): Opened response FIFO '%s' successfully.\n", teller_pid, res_path);
            break; // Success
        }
        if (errno == ENOENT) {
            usleep(RETRY_DELAY_US);
            attempts++;
        } else {
            fprintf(stderr, "Teller (PID %d): Error opening response FIFO '%s': %s.\n",
                    teller_pid, res_path, strerror(errno));
            break; // Fatal error
        }
    }
    if (res_fd == -1) {
        fprintf(stderr, "Teller (PID %d): Failed to open response FIFO '%s' after %d attempts. Last Error: %s\n",
                teller_pid, res_path, MAX_ATTEMPTS, strerror(errno));
        close(req_fd); // Close the request FIFO that was successfully opened
        detach_shm();
        return NULL; // Exit Teller
    }
    // --- End Revised Teller FIFO Opening ---


    // Use FILE stream for easier reading of lines from request FIFO
    FILE *req_fp = fdopen(req_fd, "r");
    if (!req_fp) {
        fprintf(stderr, "Teller (PID %d): fdopen failed for request FIFO: %s\n", teller_pid, strerror(errno));
        close(req_fd); // fdopen doesn't close fd on failure
        close(res_fd);
        detach_shm();
        return NULL;
    }
    // printf("Teller (PID %d): fdopen successful for request FIFO.\n", teller_pid); // Debug

    printf("Teller (PID %d): Active, serving Client PID %d.\n", teller_pid, client_pid);


    // 3. Command Processing Loop
    char line[128];
    int command_count = 0;
    while (fgets(line, sizeof(line), req_fp) != NULL)
    {
        command_count++;
        // Strip trailing newline/CR BEFORE processing
        line[strcspn(line, "\n\r")] = 0;

        // printf("Teller (PID %d): Received line #%d: \"%s\"\n", teller_pid, command_count, line); // Debug

        // Ignore empty lines or comments (client should filter, but double check)
        if (strlen(line) == 0 || line[0] == '#') {
             continue;
        }

        request_t rq = {0}; // Initialize request struct
        rq.client_pid = client_pid;
        char bank_id_str[64]; // Allow longer BankID string potentially
        char operation_str[32];
        char amount_str[32]; // Read amount as string first for better validation
        long parsed_amount;

        // --- Command Parsing ---
        // Expecting format: <BankID> <Operation> <Amount>
        int items_scanned = sscanf(line, "%63s %31s %31s", bank_id_str, operation_str, amount_str);

        if (items_scanned != 3) {
            fprintf(stderr, "Teller (PID %d): Protocol error (items scanned %d != 3) on line: '%s'\n", teller_pid, items_scanned, line);
            dprintf(res_fd, "FAIL Protocol error: bad format '%s'\n", line);
            // fflush is not usually needed for pipes/fifos, kernel handles it.
            // fflush(stdout); // Ensure response sent - Not needed for dprintf to fd
            continue; // Get next command
        }
        // printf("Teller (PID %d): Parsed: ID_str='%s', Op='%s', Amount_str='%s'\n", teller_pid, bank_id_str, operation_str, amount_str); // Debug

        // Parse Bank ID ("N", "BankID_None", "BankID_<num>", "<num>")
        if (strcmp(bank_id_str, "N") == 0 || strcmp(bank_id_str, "BankID_None") == 0) {
            rq.bank_id = -1; // Signal new account request
        } else if (strncmp(bank_id_str, "BankID_", 7) == 0) {
            char *endptr;
            rq.bank_id = strtol(bank_id_str + 7, &endptr, 10);
            if (*endptr != '\0' || rq.bank_id < 0 || rq.bank_id >= MAX_ACCOUNTS) {
                fprintf(stderr, "Teller (PID %d): Invalid BankID format: '%s'\n", teller_pid, bank_id_str);
                dprintf(res_fd, "FAIL Protocol error: invalid BankID format '%s'\n", bank_id_str);
                // fflush(stdout);
                continue;
            }
        } else { // Try parsing as a raw number
             char *endptr;
             rq.bank_id = strtol(bank_id_str, &endptr, 10);
             // Allow only digits in raw number format
             int valid_raw = 1;
             if (*endptr != '\0') valid_raw = 0; // Check if strtol consumed everything
             for(char *c = bank_id_str; *c != '\0'; c++) { // Check if all chars were digits
                 if (!isdigit((unsigned char)*c)) { valid_raw = 0; break; }
             }

             if (!valid_raw || rq.bank_id < 0 || rq.bank_id >= MAX_ACCOUNTS) {
                 fprintf(stderr, "Teller (PID %d): Invalid BankID format: '%s'\n", teller_pid, bank_id_str);
                 dprintf(res_fd, "FAIL Protocol error: invalid BankID format '%s'\n", bank_id_str);
                 // fflush(stdout);
                 continue;
             }
        }

        // Parse Operation ("deposit" or "withdraw")
        if (strcmp(operation_str, "deposit") == 0) {
            rq.type = REQ_DEPOSIT;
        } else if (strcmp(operation_str, "withdraw") == 0) {
            rq.type = REQ_WITHDRAW;
        } else {
            fprintf(stderr, "Teller (PID %d): Unknown operation: '%s'\n", teller_pid, operation_str);
            dprintf(res_fd, "FAIL Protocol error: unknown operation '%s'\n", operation_str);
            // fflush(stdout);
            continue;
        }

        // Parse and Validate Amount (must be positive long)
        char *endptr_amt;
        parsed_amount = strtol(amount_str, &endptr_amt, 10);
        if (*endptr_amt != '\0' || parsed_amount <= 0) {
             fprintf(stderr, "Teller (PID %d): Invalid amount: '%s'\n", teller_pid, amount_str);
             dprintf(res_fd, "FAIL Invalid amount: %ld\n", parsed_amount); // Use parsed_amount even if invalid string
             // fflush(stdout);
             continue;
        }
        rq.amount = parsed_amount;

        // Special check: Cannot withdraw from BankID N/-1
        if (rq.bank_id == -1 && rq.type == REQ_WITHDRAW) {
             fprintf(stderr, "Teller (PID %d): Cannot withdraw from non-existent account (ID: N)\n", teller_pid);
             dprintf(res_fd, "FAIL Cannot withdraw from new account 'N'\n");
             // fflush(stdout);
             continue;
        }
        // --- End Parsing ---


        // Submit request to server via SHM queue
        // printf("Teller (PID %d): Submitting Request (ClientPID %d, BankID %d, Type %d, Amount %ld)\n",
        //       teller_pid, rq.client_pid, rq.bank_id, rq.type, rq.amount); // Debug
        request_t *shm_slot = push_request(&rq);
        if (!shm_slot) {
            // Failed to push request (sem wait failed?)
            fprintf(stderr, "Teller (PID %d): CRITICAL ERROR pushing request to server queue. Aborting request.\n", teller_pid);
            // Send generic failure back to client?
            dprintf(res_fd, "FAIL Internal server communication error\n");
            // fflush(stdout);
            // Maybe break the loop or exit teller? Let's continue for now.
            continue;
        }

        // Wait for server to process the request (updates status in shm_slot)
        // printf("Teller (PID %d): Waiting for server response...\n", teller_pid); // Debug
        int final_status = wait_for_request_completion(shm_slot);
        // Retrieve results from the SHM slot (server should have updated them)
        long result_balance = shm_slot->result_balance;
        int result_bank_id = shm_slot->bank_id; // Get ID assigned by server (esp. for CREATE)

        // printf("Teller (PID %d): Server processing complete. Status: %d, ResBalance: %ld, ResID: %d\n",
        //       teller_pid, final_status, result_balance, result_bank_id); // Debug

        // --- Format and Send Response to Client ---
        int dprintf_ret = -1;
        switch (final_status)
        {
        case 0: // OK
            // Check if this was a CREATE request (original rq.bank_id was -1)
            if (rq.bank_id == -1) {
                // Server should have assigned a valid ID in result_bank_id
                dprintf_ret = dprintf(res_fd, "OK Account created. BankID_%d balance=%ld\n",
                                      result_bank_id, result_balance);
            }
            // Check if this was a WITHDRAW that resulted in closure (balance is 0)
            else if (rq.type == REQ_WITHDRAW && result_balance == 0) {
                 // Server should have used the original ID (result_bank_id == rq.bank_id)
                 dprintf_ret = dprintf(res_fd, "OK Account closed. Final Balance: 0 BankID_%d\n",
                                       result_bank_id);
            }
             // Standard successful deposit or withdraw (non-closing)
            else {
                 // Server should have used the original ID
                 dprintf_ret = dprintf(res_fd, "OK BankID_%d balance=%ld\n",
                                       result_bank_id, result_balance);
            }
            break;

        case 1: // Insufficient funds (only possible for withdraw)
            // Server provides current balance in result_balance and original ID
            dprintf_ret = dprintf(res_fd, "FAIL insufficient balance=%ld BankID_%d\n",
                                  result_balance, result_bank_id);
            break;

        case 2: // Generic Error (Invalid account, bank full, server internal error, timeout etc.)
        default: // Treat any unexpected status as error
            // Report the ID the client *tried* to use (rq.bank_id) or the result_id if available?
            // Let's use the ID the server reported back (result_bank_id) for consistency,
            // even if it's -1 (e.g., for Bank Full on CREATE).
             if (rq.bank_id == -1 && result_bank_id == -1) { // Specifically Bank Full on CREATE
                 dprintf_ret = dprintf(res_fd, "FAIL operation failed (bank full)\n");
             } else if (result_bank_id == -1) { // Some other error where server didn't assign/use an ID
                 dprintf_ret = dprintf(res_fd, "FAIL operation failed (server error or invalid request)\n");
             } else { // Error related to a specific account ID
                 dprintf_ret = dprintf(res_fd, "FAIL operation failed (invalid/closed account?) BankID_%d\n", result_bank_id);
             }
            break;
        } // End switch(final_status)

        // Check if dprintf failed (e.g., client closed response pipe)
        if (dprintf_ret < 0) {
            if (errno == EPIPE) {
                 fprintf(stderr, "Teller (PID %d): Client PID %d closed response pipe (EPIPE). Stopping.\n", teller_pid, client_pid);
            } else {
                 perror("Teller: ERROR writing response to client");
            }
            // Break loop if we can't write to client
            break;
        }
        // Ensure response is sent out (dprintf might buffer, though less likely for pipes)
        // fflush(fdopen(res_fd, "w")); // Risky to fdopen again, direct fsync if needed
        // fsync(res_fd); // Generally not needed for pipes

        // printf("Teller (PID %d): Response sent.\n", teller_pid); // Debug

    } // End while(fgets)

    // Check if loop exited due to error or EOF
    if (ferror(req_fp)) {
        fprintf(stderr, "Teller (PID %d): Error reading from client request FIFO: %s\n", teller_pid, strerror(errno));
    } else {
        // printf("Teller (PID %d): EOF reached on client request FIFO. Client closed connection.\n", teller_pid); // Debug
    }

    // 4. Cleanup
    printf("Teller (PID %d): Client PID %d session finished. Cleaning up.\n", teller_pid, client_pid);

    // Close FILE stream (this also closes req_fd)
    if (req_fp) fclose(req_fp);
    else if (req_fd != -1) close(req_fd); // Close fd if fclose wasn't called

    // Close response FIFO fd explicitly (if not closed by fclose)
    if (res_fd != -1) close(res_fd);

    // Detach from shared memory
    detach_shm();

    // Do NOT unlink the FIFOs here - the client should handle that.

    printf("Teller (PID %d): Exiting.\n", teller_pid);
    return NULL; // Teller process terminates
}


/* Wrapper for Teller's entry point, required by clone */
void *teller_main(void *arg)
{
    // Can add process-wide setup here if needed (e.g., signal masks)
    // printf("Teller process %d started with arg %p\n", getpid(), arg); // Debug

    teller_main_inner(arg);

    // Can add process-wide cleanup here if needed
    // printf("Teller process %d finished.\n", getpid()); // Debug

    // Return value is ignored by child_start, which calls _exit(0)
    return NULL;
}