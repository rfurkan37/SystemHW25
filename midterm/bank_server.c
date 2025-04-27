#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/mman.h>
#include <errno.h>
#include <sys/wait.h>
#include <time.h>
#include <ctype.h>
#include <libgen.h> // For basename()
#include <poll.h>
#include <semaphore.h> // Needed for sem_getvalue
#include <sys/types.h> // pid_t
#include <limits.h>    // For INT_MAX

#include "common.h"

static shm_region_t *region = NULL;
static int shm_fd = -1;
static const char *server_fifo_path = NULL; // Global for cleanup
static volatile sig_atomic_t running = 1; // Use volatile sig_atomic_t for signal handlers

// --- Teller Tracking (for stack cleanup) ---
typedef struct {
    pid_t pid;
    void* stack_ptr;
} teller_info_t;

static teller_info_t *active_tellers = NULL; // Dynamically allocated array
static int teller_capacity = 0;      // Current capacity of the array
static int teller_count = 0;         // Number of active tellers
const size_t TELLER_STACK_SZ = 1 << 20; /* 1 MB */


// Enum for log transaction types
typedef enum {
    LOG_CREATE,
    LOG_DEPOSIT,
    LOG_WITHDRAW,
    LOG_CLOSE,
    LOG_ERROR // Represents potential parsing errors or other issues
} log_event_type_t;

// Logging function (Append transaction to log file)
// MUST be called with dbmutex HELD
static void log_transaction(log_event_type_t type, int id, long amount, long balance)
{
    // Open in append mode, create if not exists
    FILE *log_fp = fopen(LOG_FILE_NAME, "a");
    if (!log_fp)
    {
        perror("SERVER ERROR: Failed to open log file for appending");
        // This is serious, maybe server should exit?
        return;
    }

    // Get current time for log entries (optional, but nice)
    // time_t now = time(NULL);
    // struct tm *tm_info = localtime(&now);
    // char time_buf[26];
    // strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", tm_info);

    // fprintf(log_fp, "%s ", time_buf); // Optional timestamp per line

    switch (type) {
        case LOG_CREATE: fprintf(log_fp, "CREATE %d %ld\n", id, amount); break; // Amount is initial balance
        case LOG_DEPOSIT: fprintf(log_fp, "DEPOSIT %d %ld %ld\n", id, amount, balance); break; // Amount deposited, new balance
        case LOG_WITHDRAW: fprintf(log_fp, "WITHDRAW %d %ld %ld\n", id, amount, balance); break; // Amount withdrawn, new balance
        case LOG_CLOSE: fprintf(log_fp, "CLOSE %d\n", id); break; // Account closed
        default: fprintf(log_fp, "UNKNOWN_LOG_TYPE %d %ld %ld\n", id, amount, balance); break;
    }
    // fclose implicitly flushes. Explicit fsync is possible but often skipped.
    // fflush(log_fp); // Ensure buffer is written to OS
    // fsync(fileno(log_fp)); // Ensure OS writes to disk (Required by Req 4, but often omitted)
    fclose(log_fp);
}


// --- Persistence: Load State by Replaying Transaction Log ---
static void load_state_from_log()
{
    // Initialize balances before replay
    for (int i = 0; i < MAX_ACCOUNTS; ++i) {
        region->balances[i] = ACCOUNT_INACTIVE;
    }
    region->next_id = 0; // Start search for new IDs from 0

    FILE *log_fp = fopen(LOG_FILE_NAME, "r");
    if (!log_fp) {
        if (errno == ENOENT) {
            printf("No previous log file found (%s). Creating the bank database.\n", LOG_FILE_NAME);
            // Create the log file with a header
            FILE *create_log = fopen(LOG_FILE_NAME, "w");
            if(create_log) {
                fprintf(create_log, "# AdaBank Transaction Log - Started %ld\n", (long)time(NULL)); // Cast time_t for safety
                fclose(create_log);
            } else {
                perror("SERVER WARNING: Could not create initial log file");
            }
        } else {
            perror("SERVER ERROR: Failed to open log file for reading. Starting with empty state.");
            // Proceed with an empty state, but log the error
        }
        return; // Start fresh
    }

    printf("Loading state by replaying log file: %s\n", LOG_FILE_NAME);
    char line[256];
    int max_id_found = -1;
    int line_num = 0;

    while (fgets(line, sizeof(line), log_fp)) {
        line_num++;
        line[strcspn(line, "\n\r")] = 0; // Strip newline/CR
        // Skip comments and empty lines
        if (line[0] == '#' || line[0] == '\0') continue;

        int id;
        long amount = 0, balance_from_log = 0; // Use different name for balance from log line
        log_event_type_t current_type = LOG_ERROR; // Default to error

        // Try parsing different formats
        if (sscanf(line, "CREATE %d %ld", &id, &amount) == 2) current_type = LOG_CREATE;
        else if (sscanf(line, "DEPOSIT %d %ld %ld", &id, &amount, &balance_from_log) == 3) current_type = LOG_DEPOSIT;
        else if (sscanf(line, "WITHDRAW %d %ld %ld", &id, &amount, &balance_from_log) == 3) current_type = LOG_WITHDRAW;
        else if (sscanf(line, "CLOSE %d", &id) == 1) current_type = LOG_CLOSE;
        else {
            fprintf(stderr, "SERVER WARNING (Log Line %d): Could not parse log entry: '%s'. Skipping.\n", line_num, line);
            continue;
        }

        // Validate parsed ID
        if (id < 0 || id >= MAX_ACCOUNTS) {
            fprintf(stderr, "SERVER WARNING (Log Line %d): Invalid Account ID %d in log. Skipping entry: '%s'\n", line_num, id, line);
            continue;
        }

        // Apply the transaction to the in-memory state (region->balances)
        switch (current_type) {
            case LOG_CREATE:
                if (region->balances[id] != ACCOUNT_INACTIVE) {
                    fprintf(stderr, "SERVER WARNING (Log Line %d): Re-CREATE of active account BankID_%d detected. Overwriting balance. Line: '%s'\n", line_num, id, line);
                }
                region->balances[id] = amount; // Initial balance
                // printf("  Log Replay: CREATE BankID_%d, Balance: %ld\n", id, amount);
                if (id > max_id_found) max_id_found = id;
                break;
            case LOG_DEPOSIT:
                if (region->balances[id] == ACCOUNT_INACTIVE) {
                    fprintf(stderr, "SERVER WARNING (Log Line %d): DEPOSIT to inactive account BankID_%d. Applying anyway. Line: '%s'\n", line_num, id, line);
                    // Should we allow this? Let's apply the final balance from the log.
                }
                region->balances[id] = balance_from_log; // Set to the balance recorded *after* the deposit
                // printf("  Log Replay: DEPOSIT Applied -> BankID_%d, Balance: %ld\n", id, balance_from_log);
                if (id > max_id_found) max_id_found = id; // Track max ID even on deposit
                break;
            case LOG_WITHDRAW:
                if (region->balances[id] == ACCOUNT_INACTIVE) {
                     fprintf(stderr, "SERVER WARNING (Log Line %d): WITHDRAW from inactive account BankID_%d. Applying final balance anyway. Line: '%s'\n", line_num, id, line);
                }
                 region->balances[id] = balance_from_log; // Set to the balance recorded *after* the withdraw
                 // printf("  Log Replay: WITHDRAW Applied -> BankID_%d, Balance: %ld\n", id, balance_from_log);
                 if (id > max_id_found) max_id_found = id; // Track max ID
                 break;
            case LOG_CLOSE:
                 if (region->balances[id] == ACCOUNT_INACTIVE) {
                     // fprintf(stderr, "SERVER INFO (Log Line %d): Redundant CLOSE on inactive account BankID_%d. Ignoring. Line: '%s'\n", line_num, id, line);
                 } else {
                     region->balances[id] = ACCOUNT_INACTIVE;
                     // printf("  Log Replay: CLOSE BankID_%d\n", id);
                 }
                 // Keep max_id_found as is, closing doesn't affect next ID hint
                 break;
            case LOG_ERROR: // Should not happen if parsing worked
                 break;
        }
    }
    fclose(log_fp);

    // Set the next ID hint based on the highest ID encountered
    region->next_id = (max_id_found == -1) ? 0 : (max_id_found + 1);
    // Handle wrap-around or full case for next_id
    if (region->next_id >= MAX_ACCOUNTS) {
         // Search from 0 again if the next ID wraps around
         region->next_id = 0;
         printf("Max account ID reached, next ID hint wrapped to 0.\n");
    }

    printf("Log file replay complete. Next Account ID hint: %d\n", region->next_id);
}

// --- Find Available Account ID ---
// Must be called with dbmutex held!
static int find_free_account_id()
{
    int start_idx = region->next_id;
    int current_idx = start_idx;
    do {
        if (region->balances[current_idx] == ACCOUNT_INACTIVE) {
            // Found a free slot
            region->next_id = (current_idx + 1) % MAX_ACCOUNTS; // Update hint for next search
            return current_idx;
        }
        // Move to the next index, wrapping around
        current_idx = (current_idx + 1) % MAX_ACCOUNTS;
    } while (current_idx != start_idx); // Loop until we've checked all accounts

    // If we get here, no free slot was found
    return -1; // Indicate bank is full
}


// --- Signal Handler ---
static void sigint_handler(int signo)
{
    char msg[64];
    snprintf(msg, sizeof(msg), "\nServer: Signal %d received, initiating shutdown...\n", signo);
    // Use write for better signal safety than printf
    ssize_t written = write(STDOUT_FILENO, msg, strlen(msg));
    (void)written; // Avoid unused result warning
    running = 0; // Set flag to terminate main loop
}


// --- Teller Tracking & Cleanup ---
// Helper to add teller PID and stack pointer to tracked list
static void track_teller(pid_t pid, void* stack_ptr) {
    if (teller_count >= teller_capacity) {
        int new_capacity = (teller_capacity == 0) ? 16 : teller_capacity * 2;
        teller_info_t *new_array = realloc(active_tellers, new_capacity * sizeof(teller_info_t));
        if (!new_array) {
            perror("SERVER FATAL: Failed to resize teller tracking array");
            // Log the PID and the stack address *before* freeing it
            fprintf(stderr, "SERVER ERROR: Could not track Teller PID %d (stack @ %p), freeing stack.\n", pid, stack_ptr);
            // Now free the stack since it cannot be tracked
            free(stack_ptr);
            return; // Exit the function, don't attempt to track
        }
        // If realloc succeeded:
        active_tellers = new_array;
        teller_capacity = new_capacity;
        // printf("Server: Resized teller tracking array to %d\n", new_capacity); // Less verbose
    }
    active_tellers[teller_count].pid = pid;
    active_tellers[teller_count].stack_ptr = stack_ptr;
    teller_count++;
}

// Helper to find and free stack for a given PID, returns 1 if found, 0 otherwise
static int free_teller_stack(pid_t pid) {
    for (int i = 0; i < teller_count; ++i) {
        if (active_tellers[i].pid == pid) {
            printf("  Server: Freeing stack @ %p for terminated Teller PID %d\n", active_tellers[i].stack_ptr, pid);
            free(active_tellers[i].stack_ptr);
            // Remove entry by shifting the last element over it
            teller_count--; // Decrement count first
            if (i < teller_count) { // Avoid copy if it was the last element
                 active_tellers[i] = active_tellers[teller_count];
            }
            // Optional: Shrink array if count gets very low compared to capacity (e.g., if count < capacity / 4)
            return 1; // Found and freed
        }
    }
    fprintf(stderr, "  Server Warning: Tried to free stack for unknown/already freed Teller PID %d\n", pid);
    return 0; // Not found
}


// --- Server Cleanup Function ---
static void cleanup()
{
    printf("Server: Starting cleanup...\n");

    // 1. Destroy semaphores before unmapping SHM
    // Check if region was successfully mapped
    if (region != NULL && region != MAP_FAILED) {
        printf("Server: Destroying semaphores...\n");
        int sem_errors = 0;
        if (sem_destroy(&region->slots) == -1) { perror("sem_destroy(slots) warning"); sem_errors++; }
        if (sem_destroy(&region->items) == -1) { perror("sem_destroy(items) warning"); sem_errors++; }
        if (sem_destroy(&region->qmutex) == -1) { perror("sem_destroy(qmutex) warning"); sem_errors++; }
        if (sem_destroy(&region->dbmutex) == -1) { perror("sem_destroy(dbmutex) warning"); sem_errors++; }
        if (sem_errors > 0) fprintf(stderr, "Server Warning: %d semaphore(s) failed to destroy.\n", sem_errors);
    } else {
        printf("Server: Shared memory region not mapped, skipping semaphore destruction.\n");
    }

    // 2. Unmap shared memory
    if (region != NULL && region != MAP_FAILED) {
        if (munmap(region, sizeof(shm_region_t)) == -1) {
            perror("Server: munmap cleanup error");
        } else {
            printf("Server: Unmapped shared memory region.\n");
        }
        region = NULL; // Mark as unmapped
    }

    // 3. Close shared memory file descriptor (if open)
    if (shm_fd != -1) {
        close(shm_fd);
        shm_fd = -1;
        // 4. Unlink shared memory segment (name-based)
        printf("Server: Unlinking shared memory segment %s...\n", SHM_NAME);
        if (shm_unlink(SHM_NAME) == -1 && errno != ENOENT) {
            // ENOENT means it's already gone, which is fine. Other errors are warnings.
            perror("Server: shm_unlink warning");
        }
    }

    // 5. Remove the server FIFO
    if (server_fifo_path) {
        printf("Server: Removing server FIFO %s...\n", server_fifo_path);
        if (unlink(server_fifo_path) == -1 && errno != ENOENT) {
            perror("Server: unlink server FIFO warning");
        }
    }

    // 6. Free the teller tracking array itself
    // Stacks *should* have been freed during shutdown waitpid loop
    if (teller_count > 0) {
        fprintf(stderr, "Server WARNING: %d teller stack(s) might not have been freed if shutdown was unclean or waitpid failed.\n", teller_count);
        // As a fallback, iterate and free any remaining tracked stacks
        for(int i = 0; i < teller_count; ++i) {
            fprintf(stderr, "  Server: Force freeing potentially leaked stack @ %p for Teller PID %d.\n", active_tellers[i].stack_ptr, active_tellers[i].pid);
            free(active_tellers[i].stack_ptr);
        }
        teller_count = 0; // Reset count after forced free
    }
    free(active_tellers); // Free the array itself
    active_tellers = NULL;
    teller_capacity = 0;

    printf("AdaBank server cleanup complete. Bye!\n"); // Final message
}


// --- Request Processing Logic (Refactored) ---

// Process a deposit request. MUST be called with dbmutex held.
// Updates balance, logs transaction, sets status and result balance in SHM slot.
static void process_deposit(request_t *req, int slot_idx) {
    int status = 2; // Default: error
    long current_balance = 0; // Holds balance *after* operation
    int account_id = req->bank_id; // ID used for logging/reporting

    if (req->bank_id == -1) { // Request to create a new account
        int new_id = find_free_account_id();
        if (new_id != -1) {
            // Found a free slot
            account_id = new_id; // This is the ID we are creating
            region->balances[account_id] = req->amount; // Set initial balance
            current_balance = req->amount;
            status = 0; // OK
            log_transaction(LOG_CREATE, account_id, current_balance, 0); // Log CREATE with new ID and balance
            printf("Server: Processed Client %d: CREATE BankID_%d Deposit %ld Balance %ld. Updating log.\n",
                   req->client_pid, account_id, req->amount, current_balance);
        } else {
            // Bank is full
            status = 2; // Error status
            current_balance = 0; // No balance applicable
            account_id = -1; // Indicate no account was assigned
            printf("Server: Processed Client %d: CREATE FAILED - Bank Full.\n", req->client_pid);
            // Log ERROR? Maybe not necessary, status code is sufficient.
        }
    } else if (req->bank_id >= 0 && req->bank_id < MAX_ACCOUNTS && region->balances[req->bank_id] != ACCOUNT_INACTIVE) {
        // Existing, active account deposit
        account_id = req->bank_id;
        long *bal_ptr = &region->balances[account_id];
        // Check for potential overflow? (long max) - less critical for simulation
        *bal_ptr += req->amount;
        current_balance = *bal_ptr;
        status = 0; // OK
        log_transaction(LOG_DEPOSIT, account_id, req->amount, current_balance);
        printf("Server: Processed Client %d: DEPOSIT BankID_%d Amount %ld Balance %ld. Updating log.\n",
               req->client_pid, account_id, req->amount, current_balance);
    } else {
        // Invalid or inactive account ID provided by client
        status = 2; // Error status
        current_balance = 0; // No balance applicable
        account_id = req->bank_id; // Report the ID client tried to use
        printf("Server: Processed Client %d: DEPOSIT FAILED - Invalid/Inactive Account BankID_%d.\n",
               req->client_pid, account_id);
        // Log ERROR?
    }

    // Update SHM result slot AFTER releasing dbmutex? No, update before releasing.
    // Store results back into the SHM queue slot for the teller
    region->queue[slot_idx].bank_id = account_id; // Store the actual ID used (or -1 if create failed)
    region->queue[slot_idx].result_balance = current_balance;
    // Atomically update the status to signal completion to the waiting teller
    __atomic_store_n(&region->queue[slot_idx].status, status, __ATOMIC_SEQ_CST);
}

// Process a withdraw request. MUST be called with dbmutex held.
// Updates balance, logs transaction (including CLOSE), sets status/result balance.
static void process_withdraw(request_t *req, int slot_idx) {
    int status = 2; // Default: error
    long current_balance = 0; // Balance *after* operation (or before if failed)
    int account_id = req->bank_id; // ID used for logging/reporting

    if (account_id >= 0 && account_id < MAX_ACCOUNTS && region->balances[account_id] != ACCOUNT_INACTIVE) {
        // Existing, active account withdraw attempt
        long *bal_ptr = &region->balances[account_id];

        if (*bal_ptr >= req->amount) {
            // Sufficient funds
            *bal_ptr -= req->amount;
            current_balance = *bal_ptr;
            status = 0; // OK
            log_transaction(LOG_WITHDRAW, account_id, req->amount, current_balance);
             printf("Server: Processed Client %d: WITHDRAW BankID_%d Amount %ld Balance %ld. Updating log.\n",
                   req->client_pid, account_id, req->amount, current_balance);

            // Check if account should be closed
            if (current_balance == 0) {
                *bal_ptr = ACCOUNT_INACTIVE; // Mark account as inactive
                log_transaction(LOG_CLOSE, account_id, 0, 0); // Log closure event
                printf("  Server: Account BankID_%d closed due to zero balance. Updating log.\n", account_id);
                // Status remains 0 (OK), Teller formats response based on zero balance
            }
        } else {
            // Insufficient funds
            current_balance = *bal_ptr; // Report the balance they *do* have
            status = 1; // Insufficient funds status code
            printf("Server: Processed Client %d: WITHDRAW BankID_%d Amount %ld FAILED - Insufficient Funds (Balance %ld).\n",
                   req->client_pid, account_id, req->amount, current_balance);
            // Do not log WITHDRAW or CLOSE on insufficient funds
        }
    } else {
        // Invalid or inactive account ID
        status = 2; // Error status
        current_balance = 0; // No balance applicable
        printf("Server: Processed Client %d: WITHDRAW FAILED - Invalid/Inactive Account BankID_%d.\n",
               req->client_pid, account_id);
        // Log ERROR?
    }

    // Update SHM result slot
    region->queue[slot_idx].bank_id = account_id; // Store the ID used
    region->queue[slot_idx].result_balance = current_balance;
    // Atomically update the status
    __atomic_store_n(&region->queue[slot_idx].status, status, __ATOMIC_SEQ_CST);
}


// --- Main Server Function ---
int main(int argc, char *argv[])
{
    if (argc != 2) {
        // Use basename for cleaner program name in usage
        fprintf(stderr, "Usage: %s <ServerFIFO_Name>\n", basename(argv[0]));
        exit(EXIT_FAILURE);
    }
    server_fifo_path = argv[1]; // Use const char* directly

    printf("AdaBank server starting (PID: %d). Log: %s\n", getpid(), LOG_FILE_NAME);

    // Setup signal handlers
    struct sigaction sa_int_term;
    memset(&sa_int_term, 0, sizeof(sa_int_term));
    sa_int_term.sa_handler = sigint_handler;
    sigaction(SIGINT, &sa_int_term, NULL);
    sigaction(SIGTERM, &sa_int_term, NULL);

    // Handle SIGCHLD: Option 1: SIG_IGN (kernel reaps, simpler but less control)
    // Option 2: Custom handler (more complex, needed if tracking exits precisely before waitpid)
    // Sticking with SIG_IGN as the shutdown logic explicitly waits anyway.
    struct sigaction sa_chld;
    memset(&sa_chld, 0, sizeof(sa_chld));
    sa_chld.sa_handler = SIG_IGN; // Kernel handles reaping automatically
    sa_chld.sa_flags = SA_RESTART; // Restart syscalls if interrupted by this signal
    sigaction(SIGCHLD, &sa_chld, NULL);
    // printf("Server: Configured SIGCHLD handler to SIG_IGN (kernel reaps children).\n"); // Less verbose

    // Allocate initial teller tracking array
    teller_capacity = 16; // Start with capacity for 16 tellers
    active_tellers = malloc(teller_capacity * sizeof(teller_info_t));
    if (!active_tellers) {
        perror("SERVER FATAL: Failed to allocate teller tracking array");
        exit(EXIT_FAILURE);
    }
    teller_count = 0; // Initialize count


    // --- Set up Shared Memory ---
    shm_unlink(SHM_NAME); // Attempt cleanup first (ignore errors like ENOENT)
    shm_fd = shm_open(SHM_NAME, O_RDWR | O_CREAT | O_EXCL, 0600);
    int shm_existed = 0;
    if (shm_fd == -1) {
        if (errno == EEXIST) {
            // SHM already exists, likely from a previous crashed run
            printf("Server Warning: Shared memory %s already exists. Attempting to open...\n", SHM_NAME);
            shm_existed = 1;
            shm_fd = shm_open(SHM_NAME, O_RDWR, 0600); // Try opening existing
            if (shm_fd == -1) {
                perror("SERVER FATAL: Failed to open existing shared memory");
                free(active_tellers);
                exit(EXIT_FAILURE);
            }
        } else {
            // Other error during shm_open
            perror("SERVER FATAL: shm_open failed");
            free(active_tellers);
            exit(EXIT_FAILURE);
        }
    }

    // Set size if we created the SHM segment
    if (!shm_existed) {
        if (ftruncate(shm_fd, sizeof(shm_region_t)) == -1) {
            perror("SERVER FATAL: ftruncate failed");
            close(shm_fd);
            shm_unlink(SHM_NAME); // Clean up before exiting
            free(active_tellers);
            exit(EXIT_FAILURE);
        }
        printf("Server: Created and sized new shared memory segment %s.\n", SHM_NAME);
    }

    // Map the shared memory segment
    region = mmap(NULL, sizeof(shm_region_t), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (region == MAP_FAILED) {
        perror("SERVER FATAL: mmap failed");
        close(shm_fd);
        if (!shm_existed) shm_unlink(SHM_NAME); // Clean up if we created it
        free(active_tellers);
        exit(EXIT_FAILURE);
    }
    printf("Server: Shared memory mapped successfully.\n");

    // --- Initialize or Verify SHM State (Semaphores, Log Replay) ---
    if (!shm_existed) {
        // Initialize semaphores ONLY if we created the SHM
        printf("Server: Initializing new shared memory region (semaphores, queue)...\n");
        int sem_init_ok = 1;
        // Initialize unnamed semaphores within the mapped region
        // The second argument '1' means the semaphore is shared between processes
        if (sem_init(&region->qmutex, 1, 1) == -1) { perror("sem_init(qmutex)"); sem_init_ok = 0; }
        if (sem_init(&region->slots, 1, REQ_QUEUE_LEN) == -1) { perror("sem_init(slots)"); sem_init_ok = 0; }
        if (sem_init(&region->items, 1, 0) == -1) { perror("sem_init(items)"); sem_init_ok = 0; }
        if (sem_init(&region->dbmutex, 1, 1) == -1) { perror("sem_init(dbmutex)"); sem_init_ok = 0; }

        if (!sem_init_ok) {
            fprintf(stderr, "SERVER FATAL: Failed to initialize semaphores.\n");
            cleanup(); // Attempt cleanup before exiting
            exit(EXIT_FAILURE);
        }
        region->head = region->tail = 0; // Initialize queue pointers

        // Lock the database mutex before loading initial state (creates log if needed)
        sem_wait(&region->dbmutex);
        load_state_from_log(); // This will create the log file if it doesn't exist
        sem_post(&region->dbmutex);

    } else {
        // SHM existed, assume semaphores are initialized (potentially stale)
        // We MUST replay the log to ensure consistency, even if SHM has old data.
        printf("Server: Attaching to existing SHM. Verifying state and replaying log...\n");
        // Optional: Check semaphore usability (e.g., sem_getvalue)
        int sem_val;
        if (sem_getvalue(&region->dbmutex, &sem_val) == -1) {
             // If sem_getvalue fails, the SHM state is likely corrupt.
             perror("SERVER FATAL: dbmutex in existing SHM seems invalid (sem_getvalue failed)");
             fprintf(stderr, "       Consider manually removing SHM (%s) and potentially the log file (%s).\n", SHM_NAME, LOG_FILE_NAME);
             cleanup(); // Attempt cleanup
             exit(EXIT_FAILURE);
        }
        // printf("  Server: Existing dbmutex value check OK (value: %d - may not reflect lock state).\n", sem_val); // Debug info

        // Lock the database mutex (with timeout) and replay the log
        struct timespec wait_time;
        clock_gettime(CLOCK_REALTIME, &wait_time);
        wait_time.tv_sec += 5; // 5 second timeout to acquire lock

        if (sem_timedwait(&region->dbmutex, &wait_time) == -1) {
             if (errno == ETIMEDOUT) {
                 fprintf(stderr, "SERVER FATAL: Timed out waiting for dbmutex on existing SHM. Server might be running or SHM is stuck.\n");
             } else {
                 perror("SERVER FATAL: Failed to lock dbmutex on existing SHM (sem_timedwait error)");
             }
             cleanup(); // Attempt cleanup
             exit(EXIT_FAILURE);
        }
        // Successfully locked dbmutex
        load_state_from_log(); // Replay log onto potentially stale SHM balances
        sem_post(&region->dbmutex); // Release lock
    }

    // --- Create and open Server FIFO ---
    unlink(server_fifo_path); // Clean up old one first
    if (mkfifo(server_fifo_path, 0600) == -1) {
        perror("SERVER FATAL: mkfifo for server failed");
        cleanup(); exit(EXIT_FAILURE);
    }
    // Open read end non-blocking to check for client PIDs periodically
    int server_fd = open(server_fifo_path, O_RDONLY | O_NONBLOCK);
    if (server_fd == -1) {
        perror("SERVER FATAL: open server fifo for read failed");
        cleanup(); exit(EXIT_FAILURE);
    }
    // Open write end to keep FIFO alive even if no clients are writing
    int dummy_fd = open(server_fifo_path, O_WRONLY);
    if (dummy_fd == -1) {
        perror("SERVER FATAL: open server fifo for write failed");
        close(server_fd);
        cleanup(); exit(EXIT_FAILURE);
    }

    printf("Server: AdaBank is active...\n");
    printf("Server: Waiting for clients @ %s...\n", server_fifo_path);

    // --- Main Server Loop ---
    while (running)
    {
        // 1. Check for new client connections via Server FIFO
        struct pollfd fds[1];
        fds[0].fd = server_fd;
        fds[0].events = POLLIN; // Wait for data to read
        // Timeout allows checking 'running' flag and processing queue
        int poll_res = poll(fds, 1, 500); // 500 ms timeout

        if (poll_res > 0 && (fds[0].revents & POLLIN))
        {
            // Data available on server FIFO
            char buf[256]; // Buffer to read client PIDs
            ssize_t n = read(server_fd, buf, sizeof(buf) - 1);
            if (n > 0) {
                buf[n] = '\0'; // Null-terminate the buffer
                // Process potentially multiple PIDs if read quickly
                char *ptr = buf;
                char *next_pid_str;
                char *saveptr; // For strtok_r
                while ((next_pid_str = strtok_r(ptr, "\n", &saveptr))) {
                     if (strlen(next_pid_str) == 0) {
                         ptr = NULL; // Necessary for next strtok_r call if current token was last
                         continue; // Skip empty lines
                     }

                     char *endptr;
                     long client_pid_long = strtol(next_pid_str, &endptr, 10);
                     // Check for conversion errors or out-of-range PID
                     if (*endptr != '\0' || client_pid_long <= 0 || client_pid_long > INT_MAX) {
                         fprintf(stderr, "Server Warning: Received invalid PID format '%s' from server FIFO. Skipping.\n", next_pid_str);
                         ptr = NULL; // Move to next token in original buffer
                         continue;
                     }
                     pid_t client_pid = (pid_t)client_pid_long;

                     printf("- Server: Received connection request from Client PID %d.\n", client_pid);

                     extern void *teller_main(void *); // Ensure declaration is visible

                     // Allocate stack for the teller
                     void* teller_stack = malloc(TELLER_STACK_SZ);
                     if (!teller_stack) {
                          perror("Server ERROR: Failed to allocate stack for Teller");
                          fprintf(stderr, "  Server: Cannot serve client PID %d due to stack allocation failure.\n", client_pid);
                          // Maybe inform client? Difficult. Continue serving others.
                     } else {
                          // printf("  Server: Allocated stack @ %p for client PID %d\n", teller_stack, client_pid); // Debug
                          // Teller function expects (void *)(intptr_t)pid
                          // Cast through intptr_t for portability if pid_t size varies
                          void *teller_arg = (void *)(intptr_t)client_pid;

                          // Spawn Teller using clone wrapper
                          printf("  Server: Spawning Teller for Client PID %d...\n", client_pid);
                          pid_t teller_pid = Teller(teller_main, teller_arg, teller_stack, TELLER_STACK_SZ);

                          if (teller_pid == -1) {
                              perror("  Server ERROR: Failed to spawn Teller process (clone failed)");
                              free(teller_stack); // Free stack if clone failed
                          } else {
                              // Track PID and stack pointer for later cleanup
                              track_teller(teller_pid, teller_stack);
                              printf("  -- Teller PID %d is active serving Client PID %d...\n", teller_pid, client_pid);
                          }
                     }
                     ptr = NULL; // IMPORTANT: Tell strtok_r to continue searching from saveptr
                } // End while strtok_r
            } else if (n == 0) {
                // This case might happen if the write end (dummy_fd) is closed, but shouldn't normally.
                printf("Server Warning: Read 0 bytes from server FIFO (EOF?).\n");
                // Maybe reopen dummy_fd or handle as an error?
            } else if (n == -1 && errno != EAGAIN && errno != EWOULDBLOCK) {
                // Actual read error
                perror("Server ERROR reading server FIFO");
                running = 0; // Trigger shutdown on error
            }
            // If EAGAIN/EWOULDBLOCK, just means no data, loop continues
        } else if (poll_res == -1 && errno != EINTR) {
            // Error in poll itself
            perror("Server ERROR: poll error on server FIFO");
            running = 0; // Trigger shutdown
        }
        // If poll timed out (poll_res == 0) or was interrupted (EINTR), continue loop


        // 2. Process pending requests from SHM queue
        // Use sem_trywait to avoid blocking if queue is empty
        while (sem_trywait(&region->items) == 0)
        {
             // Got an item from the queue
             if (!running) { // Check running flag *after* getting item
                 sem_post(&region->items); // Put item back if shutting down
                 break; // Exit processing loop if shutting down
             }

             // Lock queue mutex to safely access head index and request data
             sem_wait(&region->qmutex);
             int idx = region->head;
             request_t req_copy = region->queue[idx]; // Copy request data out of queue
             region->head = (region->head + 1) % REQ_QUEUE_LEN; // Advance head index
             sem_post(&region->qmutex); // Unlock queue mutex
             sem_post(&region->slots);  // Signal that a slot is now free

             // Process the copied request (holds dbmutex internally)
             // printf("Server: Processing request from Client %d (Type: %d) in slot %d\n",
             //       req_copy.client_pid, req_copy.type, idx); // Debug

             // Lock database for processing this single request
             sem_wait(&region->dbmutex);
             if (req_copy.type == REQ_DEPOSIT) {
                 process_deposit(&req_copy, idx);
             } else { // REQ_WITHDRAW
                 process_withdraw(&req_copy, idx);
             }
             sem_post(&region->dbmutex); // Unlock database
             // Results (status, balance, id) are written to region->queue[idx] inside processing funcs

        } // end while sem_trywait items

        // 3. Brief sleep if idle to prevent busy-spinning (if poll timed out AND queue was empty)
        if (running && poll_res <= 0 && sem_trywait(&region->items) == -1) {
             // If sem_trywait failed (likely EAGAIN), queue is empty.
             struct timespec sleep_ts = {0, 10000000}; // 10 ms
             nanosleep(&sleep_ts, NULL);
        } else if (sem_trywait(&region->items) == 0) {
             // If we found an item just now, put it back for the next loop iteration
             sem_post(&region->items);
        }

    } // end while(running) main loop

    printf("Server: Main loop finished. Initiating shutdown sequence...\n");

    // --- Graceful Teller Shutdown ---
    printf("Server: Closing active Tellers (%d active)...\n", teller_count);
    if (teller_count > 0) {
        // Create a copy of the PIDs to iterate over, as free_teller_stack modifies the list
        pid_t *pids_to_terminate = malloc(teller_count * sizeof(pid_t));
        int count_to_terminate = 0;
        if (pids_to_terminate) {
            for(int i=0; i<teller_count; ++i) pids_to_terminate[i] = active_tellers[i].pid;
            count_to_terminate = teller_count;
        } else {
            fprintf(stderr, "Server Warning: Failed to allocate memory for PID list during shutdown. Termination may be incomplete.\n");
            // Fallback: directly iterate (risky if free_teller_stack reorders significantly)
            // Or just skip graceful termination? Let's proceed cautiously.
            count_to_terminate = 0; // Avoid using the list if alloc failed
        }

        // 1. Send SIGTERM to all tracked tellers
        printf("  Server: Sending SIGTERM to %d active Teller(s)...\n", count_to_terminate);
        for (int i = 0; i < count_to_terminate; ++i) {
             pid_t current_pid = pids_to_terminate[i];
             // printf("    Sending SIGTERM to Teller PID %d...\n", current_pid); // Verbose
             if (kill(current_pid, SIGTERM) == -1) {
                 if (errno == ESRCH) {
                     // Process already gone
                     printf("    Teller PID %d already exited (ESRCH).\n", current_pid);
                     // Try to free stack immediately if it wasn't already
                     free_teller_stack(current_pid);
                 } else {
                     perror("    kill(SIGTERM) error");
                 }
             }
        }

        // 2. Wait for Tellers to terminate with a timeout
        printf("  Server: Waiting up to 5 seconds for Tellers to terminate...\n");
        time_t start_wait = time(NULL);
        int remaining_tellers = teller_count; // Use the live count

        while(remaining_tellers > 0 && (time(NULL) - start_wait < 5)) {
            int status;
            // Use WNOHANG to check without blocking
            pid_t result = waitpid(-1, &status, WNOHANG); // Wait for any child

            if (result > 0) {
                // A child terminated
                // printf("  Server: Child PID %d terminated.\n", result); // Debug
                if (free_teller_stack(result)) { // Try to find and free its stack
                     remaining_tellers--; // Decrement count only if it was a tracked teller
                     // printf("    Freed stack for Teller PID %d. Remaining: %d\n", result, remaining_tellers); // Debug
                } else {
                     // waitpid caught a child not in our tracked list? Should not happen with SIG_IGN.
                     printf("  Server Warning: Waited for untracked child PID %d?\n", result);
                }
            } else if (result == 0) {
                // No child terminated, sleep briefly
                struct timespec wait_sleep = {0, 100000000}; // 100 ms
                nanosleep(&wait_sleep, NULL);
            } else if (result == -1) {
                if (errno == ECHILD) {
                    // No children left to wait for
                    // printf("  Server: No more child processes (ECHILD).\n"); // Debug
                    if (remaining_tellers > 0) {
                         fprintf(stderr, "  Server WARNING: ECHILD received from waitpid, but %d tellers still tracked!\n", remaining_tellers);
                         // This indicates a discrepancy, potentially leaked stacks/processes
                    }
                    remaining_tellers = 0; // Exit wait loop
                    break;
                } else if (errno == EINTR) {
                    // Interrupted by signal, continue waiting
                    continue;
                } else {
                    // Other waitpid error
                    perror("  Server ERROR during waitpid");
                    break; // Exit wait loop on error
                }
            }
        } // end while waiting

        // 3. Force kill any remaining tellers after timeout
        remaining_tellers = teller_count; // Check live count again
        if (remaining_tellers > 0) {
            printf("  Server: Timeout waiting for %d Teller(s). Sending SIGKILL...\n", remaining_tellers);
            // Iterate through the *original* copied list again for sending SIGKILL
            for (int i = 0; i < count_to_terminate; ++i) {
                 pid_t current_pid = pids_to_terminate[i];
                 // Check if it's still in the *live* active_tellers list before killing
                 int still_active = 0;
                 for(int j=0; j<teller_count; ++j) {
                     if(active_tellers[j].pid == current_pid) {
                         still_active = 1;
                         break;
                     }
                 }

                 if (still_active) {
                     printf("    Sending SIGKILL to Teller PID %d...\n", current_pid);
                     kill(current_pid, SIGKILL);
                     // Optionally wait briefly after SIGKILL
                     // sleep(1);
                     waitpid(current_pid, NULL, 0); // Blocking wait after SIGKILL
                     free_teller_stack(current_pid); // Attempt to free stack
                 }
            }
        } else {
            printf("  Server: All tracked Tellers terminated gracefully.\n");
        }
        if (pids_to_terminate) free(pids_to_terminate); // Free the copied list

    } else {
         printf("Server: No active Tellers found during shutdown.\n");
    }

    // Close server FIFO file descriptors
    close(server_fd);
    close(dummy_fd);

    // Final cleanup (SHM, semaphores, FIFO file, tracking array)
    cleanup();

    // No printf after cleanup as resources might be gone. The final "Bye" is in cleanup().
    return 0;
}