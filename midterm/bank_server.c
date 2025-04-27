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
    FILE *log_fp = fopen(LOG_FILE_NAME, "a"); // Open in append mode
    if (!log_fp)
    {
        perror("ERROR: Failed to open log file for appending");
        fprintf(stderr, "FATAL: Cannot write to log file. Server state may become inconsistent.\n");
        // Consider raising SIGTERM to initiate shutdown?
        // kill(getpid(), SIGTERM); // Maybe too drastic?
        return;
    }

    switch (type) {
        case LOG_CREATE: fprintf(log_fp, "CREATE %d %ld\n", id, amount); break;
        case LOG_DEPOSIT: fprintf(log_fp, "DEPOSIT %d %ld %ld\n", id, amount, balance); break;
        case LOG_WITHDRAW: fprintf(log_fp, "WITHDRAW %d %ld %ld\n", id, amount, balance); break;
        case LOG_CLOSE: fprintf(log_fp, "CLOSE %d\n", id); break;
        default: fprintf(log_fp, "UNKNOWN_LOG_TYPE %d %ld %ld\n", id, amount, balance); break;
    }
    fclose(log_fp);
}


// --- Persistence: Load State by Replaying Transaction Log ---
static void load_state_from_log()
{
    for (int i = 0; i < MAX_ACCOUNTS; ++i) region->balances[i] = ACCOUNT_INACTIVE;
    region->next_id = 0;

    FILE *log_fp = fopen(LOG_FILE_NAME, "r");
    if (!log_fp) {
        if (errno == ENOENT) {
            printf("No previous log file found (%s). Starting fresh.\n", LOG_FILE_NAME);
            FILE *create_log = fopen(LOG_FILE_NAME, "w");
            if(create_log) { fprintf(create_log, "# AdaBank Transaction Log - Started %ld\n", time(NULL)); fclose(create_log); }
            else { perror("Warning: Could not create initial log file"); }
        } else {
            perror("ERROR: Failed to open log file for reading. Starting fresh.");
        }
        return;
    }

    printf("Loading state by replaying log file: %s\n", LOG_FILE_NAME);
    char line[256];
    int max_id_found = -1;
    int line_num = 0;

    while (fgets(line, sizeof(line), log_fp)) {
        line_num++;
        line[strcspn(line, "\n\r")] = 0;
        if (line[0] == '#' || line[0] == '\0') continue;

        int id;
        long amount = 0, balance = 0;
        log_event_type_t current_type = LOG_ERROR;

        if (sscanf(line, "CREATE %d %ld", &id, &amount) == 2) current_type = LOG_CREATE;
        else if (sscanf(line, "DEPOSIT %d %ld %ld", &id, &amount, &balance) == 3) current_type = LOG_DEPOSIT;
        else if (sscanf(line, "WITHDRAW %d %ld %ld", &id, &amount, &balance) == 3) current_type = LOG_WITHDRAW;
        else if (sscanf(line, "CLOSE %d", &id) == 1) current_type = LOG_CLOSE;
        else { fprintf(stderr, "Warning (Line %d): Could not parse log line: '%s'. Skipping.\n", line_num, line); continue; }

        if (id < 0 || id >= MAX_ACCOUNTS) { fprintf(stderr, "Warning (Line %d): Invalid ID %d in log. Skipping: '%s'\n", line_num, id, line); continue; }

        switch (current_type) {
            case LOG_CREATE:
                if (region->balances[id] != ACCOUNT_INACTIVE) fprintf(stderr, "Warning (Line %d): Re-CREATE account %d. Overwriting. Line: '%s'\n", line_num, id, line);
                region->balances[id] = amount;
                // printf("  Log Replay: CREATE BankID_%d, Balance: %ld\n", id, amount); // Less verbose replay
                if (id > max_id_found) max_id_found = id;
                break;
            case LOG_DEPOSIT:
                if (region->balances[id] == ACCOUNT_INACTIVE) fprintf(stderr, "Warning (Line %d): DEPOSIT to inactive account %d. Ignoring. Line: '%s'\n", line_num, id, line);
                else region->balances[id] = balance;
                // printf("  Log Replay: DEPOSIT BankID_%d -> %ld\n", id, balance); // Less verbose replay
                break;
            case LOG_WITHDRAW:
                if (region->balances[id] == ACCOUNT_INACTIVE) fprintf(stderr, "Warning (Line %d): WITHDRAW from inactive account %d. Ignoring. Line: '%s'\n", line_num, id, line);
                else region->balances[id] = balance;
                // printf("  Log Replay: WITHDRAW BankID_%d -> %ld\n", id, balance); // Less verbose replay
                break;
            case LOG_CLOSE:
                 if (region->balances[id] == ACCOUNT_INACTIVE) fprintf(stderr, "Warning (Line %d): CLOSE on already inactive account %d. Ignoring. Line: '%s'\n", line_num, id, line);
                 else region->balances[id] = ACCOUNT_INACTIVE;
                 // printf("  Log Replay: CLOSE BankID_%d\n", id); // Less verbose replay
                 break;
            case LOG_ERROR: break;
        }
    }
    fclose(log_fp);

    region->next_id = (max_id_found == -1) ? 0 : max_id_found + 1;
    if (region->next_id >= MAX_ACCOUNTS) region->next_id = 0; // Wrap around

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
            region->next_id = (current_idx + 1) % MAX_ACCOUNTS; // Update hint
            return current_idx;
        }
        current_idx = (current_idx + 1) % MAX_ACCOUNTS;
    } while (current_idx != start_idx); // Searched all accounts

    return -1; // No free slot found
}


// --- Signal Handler ---
static void sigint_handler(int signo __attribute__((unused)))
{
    // Use write for signal safety if possible, though printf often works
    char msg[] = "\nShutdown signal received...\n";
    write(STDOUT_FILENO, msg, sizeof(msg) - 1);
    running = 0; // Set running to 0 to exit the loop
}


// --- Teller Tracking & Cleanup ---
// Helper to add teller PID and stack pointer to tracked list
static void track_teller(pid_t pid, void* stack_ptr) {
    if (teller_count >= teller_capacity) {
        int new_capacity = (teller_capacity == 0) ? 16 : teller_capacity * 2;
        teller_info_t *new_array = realloc(active_tellers, new_capacity * sizeof(teller_info_t));
        if (!new_array) {
            perror("Failed to resize teller tracking array");
            // Cannot track this teller, stack will leak!
            free(stack_ptr); // Free the stack we allocated but cannot track
            return;
        }
        active_tellers = new_array;
        teller_capacity = new_capacity;
        printf("Resized teller tracking array to %d\n", new_capacity);
    }
    active_tellers[teller_count].pid = pid;
    active_tellers[teller_count].stack_ptr = stack_ptr;
    teller_count++;
}

// Helper to find and free stack for a given PID, returns 1 if found, 0 otherwise
static int free_teller_stack(pid_t pid) {
    for (int i = 0; i < teller_count; ++i) {
        if (active_tellers[i].pid == pid) {
            printf("  Freeing stack @ %p for terminated Teller PID %d\n", active_tellers[i].stack_ptr, pid);
            free(active_tellers[i].stack_ptr);
            // Remove entry by shifting the last element over it
            active_tellers[i] = active_tellers[teller_count - 1];
            teller_count--;
            // Optional: Shrink array if count gets very low compared to capacity
            return 1; // Found and freed
        }
    }
    return 0; // Not found
}


// --- Server Cleanup Function ---
static void cleanup()
{
    printf("Starting server cleanup...\n");

    // Destroy semaphores before unmapping
    if (region) {
        printf("Destroying semaphores...\n");
        sem_destroy(&region->slots);
        sem_destroy(&region->items);
        sem_destroy(&region->qmutex);
        sem_destroy(&region->dbmutex);
    }

    // Unmap and close shared memory
    if (region != MAP_FAILED && region != NULL) {
        if (munmap(region, sizeof(shm_region_t)) == -1) perror("munmap cleanup");
        region = NULL;
    }
    if (shm_fd != -1) {
        close(shm_fd);
        shm_fd = -1;
        printf("Unlinking shared memory segment %s...\n", SHM_NAME);
        if (shm_unlink(SHM_NAME) == -1 && errno != ENOENT) {
            perror("shm_unlink warning");
        }
    }

    // Remove the server FIFO
    if (server_fifo_path) {
        printf("Removing server FIFO %s...\n", server_fifo_path);
        unlink(server_fifo_path);
    }

    // Free the teller tracking array itself (stacks should be freed already)
    if (teller_count > 0) {
        fprintf(stderr, "Warning: %d teller stacks might not have been freed if shutdown was unclean.\n", teller_count);
        // We could iterate and free here, but it implies waitpid didn't work correctly
        for(int i=0; i<teller_count; ++i) free(active_tellers[i].stack_ptr);
    }
    free(active_tellers);
    active_tellers = NULL;
    teller_count = 0;
    teller_capacity = 0;

    printf("AdaBank server cleanup complete. Bye!\n");
}


// --- Request Processing Logic (Refactored) ---

// Process a deposit request. MUST be called with dbmutex held.
// Updates balance, logs transaction, sets status and result balance in SHM slot.
static void process_deposit(request_t *req, int slot_idx) {
    int status = 2; // Default error
    long current_balance = 0;

    if (req->bank_id == -1) { // New account deposit
        int new_id = find_free_account_id();
        if (new_id != -1) {
            region->balances[new_id] = req->amount;
            req->bank_id = new_id; // Update ID in the request copy
            current_balance = req->amount;
            status = 0; // OK
            log_transaction(LOG_CREATE, new_id, current_balance, 0);
            printf("Processed: New Account BankID_%d Deposit %ld Balance %ld\n", new_id, req->amount, current_balance);
        } else {
            status = 2; // Error - bank full
            current_balance = 0;
            printf("Processed: New Account Failed - Bank Full\n");
        }
    } else if (req->bank_id >= 0 && req->bank_id < MAX_ACCOUNTS && region->balances[req->bank_id] != ACCOUNT_INACTIVE) {
        // Existing, active account deposit
        long *bal_ptr = &region->balances[req->bank_id];
        *bal_ptr += req->amount;
        current_balance = *bal_ptr;
        status = 0; // OK
        log_transaction(LOG_DEPOSIT, req->bank_id, req->amount, current_balance);
        printf("Processed: BankID_%d Deposit %ld Balance %ld\n", req->bank_id, req->amount, current_balance);
    } else {
        // Invalid or inactive account ID
        status = 2; // Error code
        current_balance = 0; // Indicate error appropriately
        printf("Processed: BankID_%d Deposit FAILED - Invalid/Inactive Account\n", req->bank_id);
    }

    // Update SHM slot
    region->queue[slot_idx].bank_id = req->bank_id;
    region->queue[slot_idx].result_balance = current_balance;
    __atomic_store_n(&region->queue[slot_idx].status, status, __ATOMIC_SEQ_CST);
}

// Process a withdraw request. MUST be called with dbmutex held.
// Updates balance, logs transaction (including CLOSE), sets status/result balance.
static void process_withdraw(request_t *req, int slot_idx) {
    int status = 2; // Default error
    long current_balance = 0;

    if (req->bank_id >= 0 && req->bank_id < MAX_ACCOUNTS && region->balances[req->bank_id] != ACCOUNT_INACTIVE) {
        // Existing, active account withdraw
        long *bal_ptr = &region->balances[req->bank_id];
        if (*bal_ptr >= req->amount) {
            *bal_ptr -= req->amount;
            current_balance = *bal_ptr;
            status = 0; // OK
            log_transaction(LOG_WITHDRAW, req->bank_id, req->amount, current_balance);
            printf("Processed: BankID_%d Withdraw %ld Balance %ld\n", req->bank_id, req->amount, current_balance);

            if (current_balance == 0) {
                *bal_ptr = ACCOUNT_INACTIVE; // Mark inactive
                log_transaction(LOG_CLOSE, req->bank_id, 0, 0); // Log closure
                printf("  Account BankID_%d closed due to zero balance.\n", req->bank_id);
            }
        } else {
            // Insufficient funds
            current_balance = *bal_ptr; // Show current balance
            status = 1; // Insufficient funds code
            printf("Processed: BankID_%d Withdraw %ld FAILED - Insufficient Funds (Balance %ld)\n", req->bank_id, req->amount, current_balance);
        }
    } else {
        // Invalid or inactive account ID
        status = 2; // Error code
        current_balance = 0; // Indicate error appropriately
        printf("Processed: BankID_%d Withdraw FAILED - Invalid/Inactive Account\n", req->bank_id);
    }

    // Update SHM slot
    // NOTE: req->bank_id was never modified for withdrawal, safe to use
    region->queue[slot_idx].bank_id = req->bank_id;
    region->queue[slot_idx].result_balance = current_balance;
    __atomic_store_n(&region->queue[slot_idx].status, status, __ATOMIC_SEQ_CST);
}


// --- Main Server Function ---
int main(int argc, char *argv[])
{
    if (argc != 2) {
        fprintf(stderr, "Usage: %s ServerFIFO_Name\n", basename(argv[0]));
        exit(EXIT_FAILURE);
    }
    server_fifo_path = argv[1];

    printf("AdaBank server starting (PID: %d). FIFO: %s, Log: %s\n", getpid(), server_fifo_path, LOG_FILE_NAME);

    // Setup signal handlers
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigint_handler;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    struct sigaction sa_chld;
    memset(&sa_chld, 0, sizeof(sa_chld));
    sa_chld.sa_handler = SIG_IGN; // Kernel reaps
    sa_chld.sa_flags = SA_RESTART;
    sigaction(SIGCHLD, &sa_chld, NULL);
    printf("Configured SIGCHLD to SIG_IGN (kernel reaps children).\n");

    // Allocate initial teller tracking array
    teller_capacity = 16;
    active_tellers = malloc(teller_capacity * sizeof(teller_info_t));
    if (!active_tellers) { perror("FATAL: Failed to allocate teller tracking array"); exit(EXIT_FAILURE); }

    // Set up shared memory (unlink first, create O_EXCL, fallback to open)
    shm_unlink(SHM_NAME); // Attempt cleanup first
    shm_fd = shm_open(SHM_NAME, O_RDWR | O_CREAT | O_EXCL, 0600);
    int shm_existed = 0;
    if (shm_fd == -1 && errno == EEXIST) {
        printf("Warning: Shared memory %s already exists. Attempting to open.\n", SHM_NAME);
        shm_existed = 1;
        shm_fd = shm_open(SHM_NAME, O_RDWR, 0600);
    }
    if (shm_fd == -1) { perror("FATAL: shm_open failed"); free(active_tellers); exit(EXIT_FAILURE); }

    if (!shm_existed) { // Set size only if we created it
        if (ftruncate(shm_fd, sizeof(shm_region_t)) == -1) {
            perror("FATAL: ftruncate failed"); close(shm_fd); shm_unlink(SHM_NAME); free(active_tellers); exit(EXIT_FAILURE);
        }
    }

    region = mmap(NULL, sizeof(shm_region_t), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (region == MAP_FAILED) {
        perror("FATAL: mmap failed"); close(shm_fd); if(!shm_existed) shm_unlink(SHM_NAME); free(active_tellers); exit(EXIT_FAILURE);
    }

    // Initialize or check Semaphores and load state from log
    if (!shm_existed) {
        printf("Initializing new shared memory region and semaphores...\n");
        int sem_init_ok = 1;
        if (sem_init(&region->qmutex, 1, 1) == -1) sem_init_ok = 0;
        if (sem_init(&region->slots, 1, REQ_QUEUE_LEN) == -1) sem_init_ok = 0;
        if (sem_init(&region->items, 1, 0) == -1) sem_init_ok = 0;
        if (sem_init(&region->dbmutex, 1, 1) == -1) sem_init_ok = 0;
        if (!sem_init_ok) { perror("FATAL: sem_init failed"); cleanup(); exit(EXIT_FAILURE); }
        region->head = region->tail = 0;
        sem_wait(&region->dbmutex); // Lock for initial load
        load_state_from_log();      // Creates log if needed
        sem_post(&region->dbmutex);
    } else {
        printf("Attaching to existing SHM. Checking semaphores and loading state...\n");
        // Check if a semaphore seems initialized (Req #8 improvement)
        int sem_val;
        if (sem_getvalue(&region->dbmutex, &sem_val) == -1) {
             perror("FATAL: dbmutex in existing SHM seems invalid (sem_getvalue failed)");
             fprintf(stderr, "       Consider manually removing %s and %s\n", SHM_NAME, LOG_FILE_NAME);
             cleanup(); exit(EXIT_FAILURE);
        }
        printf("  Existing dbmutex value: %d (Note: may not reflect lock state accurately)\n", sem_val);
        // Proceed to load state by replaying log
        struct timespec wait_time;
        clock_gettime(CLOCK_REALTIME, &wait_time);
        wait_time.tv_sec += 5; // 5 second timeout for lock
        if (sem_timedwait(&region->dbmutex, &wait_time) == -1) {
             perror("FATAL: Failed to lock dbmutex on existing SHM (timeout or error)");
             cleanup(); exit(EXIT_FAILURE);
        }
        load_state_from_log(); // Replay log onto potentially existing (stale) balances
        sem_post(&region->dbmutex);
    }

    // Create and open server FIFO
    unlink(server_fifo_path); // Clean up old one first
    if (mkfifo(server_fifo_path, 0600) == -1) { perror("FATAL: mkfifo server fifo failed"); cleanup(); exit(EXIT_FAILURE); }
    int server_fd = open(server_fifo_path, O_RDONLY | O_NONBLOCK);
    if (server_fd == -1) { perror("FATAL: open server fifo for read failed"); cleanup(); exit(EXIT_FAILURE); }
    int dummy_fd = open(server_fifo_path, O_WRONLY); // Keep FIFO open
    if (dummy_fd == -1) { perror("FATAL: open server fifo for write failed"); close(server_fd); cleanup(); exit(EXIT_FAILURE); }

    printf("Waiting for clients @ %s...\n", server_fifo_path);

    // --- Main Server Loop ---
    while (running)
    {
        // Check for new client connections
        struct pollfd fds[1];
        fds[0].fd = server_fd;
        fds[0].events = POLLIN;
        int poll_res = poll(fds, 1, 500); // 500ms timeout

        if (poll_res > 0 && (fds[0].revents & POLLIN))
        {
            char buf[128];
            ssize_t n = read(server_fd, buf, sizeof(buf) - 1);
            if (n > 0) {
                buf[n] = '\0';
                char *ptr = buf, *endptr; pid_t pid;
                printf("Received connection request(s): processing '%s' (trimmed)\n", buf); // DEBUG raw buffer
                while (*ptr != '\0') {
                    while (*ptr != '\0' && !isdigit((unsigned char)*ptr)) ptr++;
                    if (*ptr == '\0') break;
                    pid = strtol(ptr, &endptr, 10);
                    if (ptr == endptr) { fprintf(stderr, "Warning: Parse PID failed near '%s'\n", ptr); break; }
                    if (pid > 0) {
                        printf("Spawning Teller for client %d\n", pid);
                        extern void *teller_main(void *);

                        // Allocate stack for the teller
                        void* teller_stack = malloc(TELLER_STACK_SZ);
                        if (!teller_stack) {
                             perror("Failed to allocate stack for Teller");
                             // Maybe inform client? Difficult. Continue serving others.
                        } else {
                             printf("  Allocated stack @ %p for client PID %d\n", teller_stack, pid);
                             // Teller function expects (void *)(intptr_t)pid
                             void *teller_arg = (void *)(intptr_t)pid;
                             // Call Teller (clone wrapper)
                             pid_t teller_pid = Teller(teller_main, teller_arg, teller_stack, TELLER_STACK_SZ);

                             if (teller_pid == -1) {
                                 perror("Failed to spawn Teller process (clone failed)");
                                 free(teller_stack); // Free stack if clone failed
                             } else {
                                 // Track PID and stack pointer for later cleanup
                                 track_teller(teller_pid, teller_stack);
                                 printf("Teller process %d spawned for client %d\n", teller_pid, pid);
                             }
                        }
                    } else { fprintf(stderr, "Warning: Parsed invalid PID %d\n", pid); }
                    ptr = endptr;
                }
            } else if (n == 0) { printf("Warning: Read 0 bytes from server FIFO.\n"); }
            else if (n == -1 && errno != EAGAIN && errno != EWOULDBLOCK) { perror("Error reading server FIFO"); running = 0; }
        } else if (poll_res == -1 && errno != EINTR) { perror("poll error on server FIFO"); running = 0; }

        // Process pending requests from SHM queue
        while (sem_trywait(&region->items) == 0)
        {
             if (!running) { sem_post(&region->items); break; } // Exit if shutting down

             sem_wait(&region->qmutex);
             int idx = region->head;
             request_t req_copy = region->queue[idx]; // Copy request data
             region->head = (region->head + 1) % REQ_QUEUE_LEN;
             sem_post(&region->qmutex);
             sem_post(&region->slots);

             // Process the request using refactored functions
             sem_wait(&region->dbmutex); // Lock database
             if (req_copy.type == REQ_DEPOSIT) {
                 process_deposit(&req_copy, idx);
             } else { // REQ_WITHDRAW
                 process_withdraw(&req_copy, idx);
             }
             sem_post(&region->dbmutex); // Unlock database
        }

        // Prevent busy-spin if idle
        if (running && poll_res <= 0 && sem_trywait(&region->items) == -1) {
             usleep(10000);
        } else if (sem_trywait(&region->items) == 0) {
             sem_post(&region->items); // Put item back if found
        }
    } // end while(running)

    printf("Server main loop finished.\n");

    // Graceful Teller Shutdown
    if (teller_count > 0) {
        printf("Shutdown: Closing active Tellers (%d active)...\n", teller_count);
        // We need a copy because free_teller_stack modifies the array
        teller_info_t *tellers_to_wait = malloc(teller_count * sizeof(teller_info_t));
        int count_to_wait = teller_count;
         if (tellers_to_wait) {
             memcpy(tellers_to_wait, active_tellers, count_to_wait * sizeof(teller_info_t));
         } else {
             fprintf(stderr, "Warning: Failed to allocate memory for waiting list. Shutdown might be unclean.\n");
             // If allocation fails, we proceed without the copy, but free_teller_stack might behave oddly
             tellers_to_wait = active_tellers; // Risky fallback
             count_to_wait = teller_count;
         }

        // Send SIGTERM
        for (int i = 0; i < count_to_wait; ++i) {
             printf("  Sending SIGTERM to Teller PID %d...\n", tellers_to_wait[i].pid);
             if (kill(tellers_to_wait[i].pid, SIGTERM) == -1 && errno == ESRCH) {
                 printf("  Teller PID %d already gone (ESRCH).\n", tellers_to_wait[i].pid);
                 // If already gone, try to free stack immediately
                 free_teller_stack(tellers_to_wait[i].pid); // May do nothing if already freed by earlier wait
                 tellers_to_wait[i].pid = -1; // Mark as handled
             } else if (errno != ESRCH) {
                 perror("  kill(SIGTERM) error");
             }
        }

        // Wait with timeout
        printf("  Waiting up to 5 seconds for Tellers to terminate...\n");
        time_t start_wait = time(NULL);
        int remaining_tellers = 0;
        for(int i=0; i<count_to_wait; ++i) if(tellers_to_wait[i].pid > 0) remaining_tellers++;

        while(remaining_tellers > 0 && (time(NULL) - start_wait < 5)) {
            int status;
            pid_t result = waitpid(-1, &status, WNOHANG); // Check any child
            if (result > 0) {
                if (free_teller_stack(result)) { // Found in our list and freed stack
                     remaining_tellers--;
                     // Mark in the temporary list too if using copy
                     if (tellers_to_wait != active_tellers) {
                          for(int i=0; i<count_to_wait; ++i) if(tellers_to_wait[i].pid == result) tellers_to_wait[i].pid = -1;
                     }
                } else {
                     printf("  Waited for an unknown child PID %d?\n", result);
                }
            } else if (result == 0) { usleep(100000); } // No change, sleep
            else if (result == -1 && errno == ECHILD) { remaining_tellers = 0; break; } // No children
            else if (result == -1 && errno != EINTR) { perror("  waitpid error"); break; } // Error
        }

        // Force kill remaining
        if (remaining_tellers > 0) {
            printf("  Timeout waiting for Tellers. Sending SIGKILL...\n");
            for (int i = 0; i < count_to_wait; ++i) {
                 if (tellers_to_wait[i].pid > 0) { // Check if marked as handled
                    printf("    Sending SIGKILL to Teller PID %d...\n", tellers_to_wait[i].pid);
                    kill(tellers_to_wait[i].pid, SIGKILL);
                    waitpid(tellers_to_wait[i].pid, NULL, 0); // Wait after SIGKILL
                    free_teller_stack(tellers_to_wait[i].pid); // Attempt to free stack
                 }
            }
        }
         if (tellers_to_wait != active_tellers) free(tellers_to_wait); // Free copy list
    } else {
         printf("Shutdown: No active Tellers found.\n");
    }

    close(server_fd);
    close(dummy_fd);
    cleanup(); // Perform final cleanup
    return 0;
}