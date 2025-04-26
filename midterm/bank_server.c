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
#include "common.h"
#include <sys/wait.h>
#include <time.h>
#include <ctype.h>
#include <poll.h>

static shm_region_t *region = NULL;
static int shm_fd = -1;
static volatile sig_atomic_t running = 1; // Use volatile sig_atomic_t for signal handlers

// Logging function
static void update_log_file()
{
    if (!region)
        return; // Should not happen if called correctly

    FILE *log_fp = fopen(LOG_FILE_NAME, "w"); // Overwrite/create log file
    if (!log_fp)
    {
        perror("ERROR: Failed to open log file for writing");
        // Non-fatal, but log will be outdated
        return;
    }

    // Get current time
    time_t now = time(NULL);
    char time_buf[100];
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", localtime(&now));

    fprintf(log_fp, "# Adabank Log file updated @ %s\n\n", time_buf);

    // Lock database while reading balances for logging
    sem_wait(&region->dbmutex);
    int active_accounts = 0;
    for (int i = 0; i < MAX_ACCOUNTS; ++i)
    {
        if (region->balances[i] != ACCOUNT_INACTIVE)
        {
            // Log format matching the example's apparent final state: ID Balance
            fprintf(log_fp, "BankID_%02d %ld\n", i, region->balances[i]);
            active_accounts++;
        }
    }
    sem_post(&region->dbmutex);

    if (active_accounts == 0)
    {
        fprintf(log_fp, "# No active accounts.\n");
    }

    fprintf(log_fp, "\n## end of log.\n");
    fclose(log_fp);
    printf("Log file updated.\n");
}

// --- Persistence: Load State from Log ---
static void load_state_from_log()
{
    // Initialize balances to inactive first
    for (int i = 0; i < MAX_ACCOUNTS; ++i)
    {
        region->balances[i] = ACCOUNT_INACTIVE;
    }
    region->next_id = 0; // Start searching for IDs from 0

    FILE *log_fp = fopen(LOG_FILE_NAME, "r");
    if (!log_fp)
    {
        if (errno == ENOENT)
        {
            printf("No previous log file found. Starting fresh.\n");
            // Initialize shared memory (already done above) and create initial log
            update_log_file();
        }
        else
        {
            perror("ERROR: Failed to open log file for reading. Starting fresh.");
            // Proceed with empty state
        }
        return; // Exit function, state is already initialized to empty/inactive
    }

    printf("Loading state from log file: %s\n", LOG_FILE_NAME);
    char line[256];
    int max_id_found = -1;

    while (fgets(line, sizeof(line), log_fp))
    {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r')
        {
            continue; // Skip comments and blank lines
        }

        // Try to parse the format: BankID_XX Balance
        int id;
        long balance;
        if (sscanf(line, "BankID_%d %ld", &id, &balance) == 2)
        {
            if (id >= 0 && id < MAX_ACCOUNTS)
            {
                region->balances[id] = balance;
                printf("  Loaded Account BankID_%d, Balance: %ld\n", id, balance);
                if (id > max_id_found)
                {
                    max_id_found = id;
                }
            }
            else
            {
                fprintf(stderr, "Warning: Invalid ID %d found in log file. Ignoring line: %s", id, line);
            }
        }
        else
        {
            fprintf(stderr, "Warning: Could not parse log line: %s", line);
        }
    }
    fclose(log_fp);

    // Set the next_id hint to one greater than the highest ID found
    region->next_id = (max_id_found == -1) ? 0 : max_id_found + 1;
    // Ensure next_id doesn't exceed bounds (though unlikely with MAX_ACCOUNTS)
    if (region->next_id >= MAX_ACCOUNTS)
    {
        region->next_id = MAX_ACCOUNTS; // Indicate potentially full
        printf("Warning: Max accounts potentially reached based on log file.\n");
    }

    printf("Log file loading complete. Next Account ID hint: %d\n", region->next_id);
}

// --- Find Available Account ID ---
// Must be called with dbmutex held!
static int find_free_account_id()
{
    // Start search from next_id hint
    for (int i = region->next_id; i < MAX_ACCOUNTS; ++i)
    {
        if (region->balances[i] == ACCOUNT_INACTIVE)
        {
            region->next_id = i + 1; // Update hint for next time
            return i;
        }
    }
    // If not found after hint, search from the beginning
    for (int i = 0; i < region->next_id; ++i)
    {
        if (region->balances[i] == ACCOUNT_INACTIVE)
        {
            region->next_id = i + 1; // Update hint
            return i;
        }
    }
    return -1; // No free slot found
}

static void sigint_handler(int signo __attribute__((unused)))
{
    printf("Shutdown signal received...\n");
    running = 0; // Set running to 0 to exit the loop
}

static void cleanup()
{
    printf("Starting server cleanup...\n");

    // Update log one last time before closing
    if (region)
    {
        printf("Updating final log state...\n");
        update_log_file();
    }
    else
    {
        printf("Region not available for final log update.\n");
    }

    // Destroy semaphores (optional but good practice)
    if (region)
    {
        sem_destroy(&region->slots);
        sem_destroy(&region->items);
        sem_destroy(&region->qmutex);
        sem_destroy(&region->dbmutex);
    }

    // Unmap and close shared memory
    if (region != MAP_FAILED && region != NULL) // Check if mapping was successful
    {
        if (munmap(region, sizeof(shm_region_t)) == -1)
        {
            perror("munmap cleanup");
        }
        region = NULL; // Mark as unmapped
    }
    if (shm_fd != -1)
    {
        close(shm_fd);
        shm_fd = -1;
        // Optionally unlink shared memory if it should not persist after server stops
        // If you want it to persist for the next run, comment this out.
        // If state is fully recoverable from log, unlinking is safer.
        printf("Unlinking shared memory segment %s...\n", SHM_NAME);
        if (shm_unlink(SHM_NAME) == -1)
        {
            perror("shm_unlink warning");
        }
    }

    // Remove the server FIFO
    printf("Removing server FIFO %s...\n", SERVER_FIFO);
    unlink(SERVER_FIFO);

    printf("AdaBank server cleanup complete. Bye!\n");
}

int main(void)
{
    printf("AdaBank server starting...\n");

    // Set up signal handlers early
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigint_handler;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    // *** Handle Zombie Processes ***
    // Ignore SIGCHLD - tells the kernel to automatically reap terminated children
    // This is the simplest way to prevent zombies.
    sa.sa_handler = SIG_IGN;
    sigaction(SIGCHLD, &sa, NULL);
    printf("Configured to automatically reap child (Teller) processes.\n");

    /* set up shared memory */
    shm_fd = shm_open(SHM_NAME, O_RDWR | O_CREAT, 0600);
    if (shm_fd == -1)
    {
        perror("FATAL: shm_open failed");
        exit(EXIT_FAILURE);
    }
    // Ensure the segment is the correct size (important if it already existed)
    if (ftruncate(shm_fd, sizeof(shm_region_t)) == -1)
    {
        perror("FATAL: ftruncate failed");
        close(shm_fd);
        shm_unlink(SHM_NAME); // Clean up potentially bad SHM segment
        exit(EXIT_FAILURE);
    }

    region = mmap(NULL, sizeof(shm_region_t), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (region == MAP_FAILED)
    {
        perror("FATAL: mmap failed");
        close(shm_fd);
        shm_unlink(SHM_NAME); // Clean up
        exit(EXIT_FAILURE);
    }

    // *** Initialize Shared Memory State ***
    // Check if region seems initialized (e.g. based on a magic number or semaphore state)
    // Simple approach: Try to initialize semaphores. If they already exist, init might fail harmlessly or reset them.
    // Using sem_init here assumes this server process is the *first* one to initialize them.
    // If multiple servers could potentially run, use named semaphores (sem_open) or file-based locking
    // to ensure only one initializes. For this project, sem_init is likely sufficient.

    int is_first_init = 0; // Flag to track if we are the ones initializing
    // Attempt to initialize semaphores. If they already exist, this might reset them.
    if (sem_init(&region->qmutex, 1, 1) == -1 && errno == EBUSY)
    {
        printf("Semaphores appear to be already initialized.\n");
    }
    else
    {
        printf("Initializing semaphores...\n");
        is_first_init = 1;
        sem_init(&region->slots, 1, REQ_QUEUE_LEN);
        sem_init(&region->items, 1, 0);
        sem_init(&region->dbmutex, 1, 1);
        // Initialize queue indices only if we are the first initializer
        region->head = region->tail = 0;
    }

    // *** Load state from log OR initialize if first run ***
    if (is_first_init)
    {
        printf("First time initialization detected, initializing balances...\n");
        sem_wait(&region->dbmutex); // Lock DB for init
        for (int i = 0; i < MAX_ACCOUNTS; ++i)
            region->balances[i] = ACCOUNT_INACTIVE;
        region->next_id = 0;
        sem_post(&region->dbmutex);
        update_log_file(); // Create initial empty log file
    }
    else
    {
        // Load state from log, assumes balances/next_id might be valid in SHM
        // Lock DB while loading state into SHM
        sem_wait(&region->dbmutex);
        load_state_from_log();
        sem_post(&region->dbmutex);
    }

    /* Create and open server fifo */
    if (mkfifo(SERVER_FIFO, 0600) == -1 && errno != EEXIST)
    {
        perror("FATAL: mkfifo server fifo failed");
        cleanup(); // Attempt cleanup
        exit(EXIT_FAILURE);
    }
    // Use NONBLOCK for reading to allow checking 'running' flag periodically
    int server_fd = open(SERVER_FIFO, O_RDONLY | O_NONBLOCK);
    if (server_fd == -1)
    {
        perror("FATAL: open server fifo for read failed");
        cleanup();
        exit(EXIT_FAILURE);
    }
    // Keep a write FD open to prevent EOF when last client closes its write FD
    int dummy_fd = open(SERVER_FIFO, O_WRONLY);
    if (dummy_fd == -1)
    {
        perror("FATAL: open server fifo for write failed");
        close(server_fd);
        cleanup();
        exit(EXIT_FAILURE);
    }

    // Register cleanup function to be called on exit
    atexit(cleanup);

    printf("Waiting for clients @ %s...\n", SERVER_FIFO);

    while (running)
    {
        /* Check for new clients using select or poll */
        struct pollfd fds[1];
        fds[0].fd = server_fd;
        fds[0].events = POLLIN;
        int poll_res = poll(fds, 1, 500); // 500ms timeout

        if (poll_res > 0 && (fds[0].revents & POLLIN))
        {
            char buf[128]; // Larger buffer
            // Read might return less than requested, or multiple PIDs
            ssize_t n = read(server_fd, buf, sizeof(buf) - 1);
            if (n > 0)
            {
                buf[n] = '\0'; // Null-terminate the buffer
                char *ptr = buf;
                char *endptr;
                pid_t pid;

                printf("Received connection request(s)...\n");
                while (*ptr != '\0')
                {
                    // Skip leading non-digit characters (like newlines)
                    while (*ptr != '\0' && !isdigit((unsigned char)*ptr))
                    {
                        ptr++;
                    }
                    if (*ptr == '\0')
                        break; // End of buffer

                    pid = strtol(ptr, &endptr, 10);

                    if (ptr == endptr)
                    { // No digits found, break
                        break;
                    }

                    if (pid > 0)
                    {
                        printf("Spawning Teller for client %d\n", pid);
                        extern pid_t Teller(void *(*)(void *), void *);
                        extern void *teller_main(void *);
                        // Pass PID as integer type directly cast to void*
                        pid_t teller_pid = Teller(teller_main, (void *)(intptr_t)pid);
                        if (teller_pid == -1)
                        {
                            perror("Failed to spawn Teller process");
                            // Log error, maybe inform client? Difficult here.
                        }
                        else
                        {
                            printf("Teller process %d spawned for client %d\n", teller_pid, pid);
                        }
                    }
                    ptr = endptr; // Move pointer past the processed number
                }
            }
            else if (n == 0)
            {
                // EOF on FIFO? Shouldn't happen with dummy_fd open. Ignore.
            }
            else if (n == -1 && errno != EAGAIN && errno != EWOULDBLOCK)
            {
                perror("Error reading from server FIFO");
                // Potentially serious error, maybe break loop?
                // running = 0;
            }
        }
        else if (poll_res == -1 && errno != EINTR)
        {
            perror("poll error on server FIFO");
            // Consider stopping the server on poll errors
            // running = 0;
        }

        /* process bank requests from shared memory queue */
        // Use sem_trywait to avoid blocking the main loop indefinitely
        while (sem_trywait(&region->items) == 0)
        {
            // Got an item
            sem_wait(&region->qmutex); // Lock queue access
            int idx = region->head;
            // Make a local copy to process outside the queue lock
            request_t req_copy = region->queue[idx];
            region->head = (region->head + 1) % REQ_QUEUE_LEN;
            sem_post(&region->qmutex); // Unlock queue
            sem_post(&region->slots);  // Signal one more slot is free

            int log_needed = 0; // Flag to check if state changed

            // Process the request (using req_copy)
            sem_wait(&region->dbmutex); // Lock database access *****

            long current_balance = ACCOUNT_INACTIVE; // Default for error cases
            int status = 2;                          // Default to error

            if (req_copy.bank_id == -1 && req_copy.type == REQ_DEPOSIT)
            {
                // New account request
                int new_id = find_free_account_id(); // Find first available slot
                if (new_id != -1)
                {
                    region->balances[new_id] = req_copy.amount;
                    req_copy.bank_id = new_id; // Update the ID in the request copy
                    current_balance = req_copy.amount;
                    status = 0; // OK
                    log_needed = 1;
                    printf("Processed: New Account BankID_%d Deposit %ld Balance %ld\n",
                           new_id, req_copy.amount, current_balance);
                }
                else
                {
                    status = 2;          // Error - bank full
                    current_balance = 0; // Or some other indicator
                    printf("Processed: New Account Failed - Bank Full\n");
                }
            }
            else if (req_copy.bank_id >= 0 && req_copy.bank_id < MAX_ACCOUNTS &&
                     region->balances[req_copy.bank_id] != ACCOUNT_INACTIVE) // Check ID valid AND account active
            {
                // Existing, active account
                long *bal_ptr = &region->balances[req_copy.bank_id];

                if (req_copy.type == REQ_DEPOSIT)
                {
                    *bal_ptr += req_copy.amount;
                    current_balance = *bal_ptr;
                    status = 0; // OK
                    log_needed = 1;
                    printf("Processed: BankID_%d Deposit %ld Balance %ld\n",
                           req_copy.bank_id, req_copy.amount, current_balance);
                }
                else // REQ_WITHDRAW
                {
                    if (*bal_ptr >= req_copy.amount)
                    {
                        *bal_ptr -= req_copy.amount;
                        current_balance = *bal_ptr;
                        status = 0; // OK
                        log_needed = 1;
                        printf("Processed: BankID_%d Withdraw %ld Balance %ld\n",
                               req_copy.bank_id, req_copy.amount, current_balance);

                        // *** Account Removal Logic ***
                        if (current_balance == 0)
                        {
                            *bal_ptr = ACCOUNT_INACTIVE; // Mark account as inactive/removed
                                                         // status remains 0 (OK), teller can inform client specifically
                            printf("  Account BankID_%d closed due to zero balance.\n", req_copy.bank_id);
                        }
                    }
                    else
                    {
                        // Insufficient funds
                        current_balance = *bal_ptr; // Show current balance
                        status = 1;                 // Insufficient funds code
                                                    // No state change, no log needed
                        printf("Processed: BankID_%d Withdraw %ld FAILED - Insufficient Funds (Balance %ld)\n",
                               req_copy.bank_id, req_copy.amount, current_balance);
                    }
                }
            }
            else
            {
                // Invalid or inactive account ID
                status = 2;          // Error code
                current_balance = 0; // Or indicate error appropriately
                printf("Processed: BankID_%d Request FAILED - Invalid or Inactive Account\n", req_copy.bank_id);
            }

            // Update the request slot in shared memory *AFTER* releasing DB lock is safer
            // But we need the updated ID if it was a new account.
            // Compromise: Update the necessary fields while lock is held.
            // Store results back into the SHM slot for the teller
            // Use atomic store for status, other fields can be regular writes
            // as status is the trigger teller waits on.
            region->queue[idx].bank_id = req_copy.bank_id; // Update ID if it was new
            region->queue[idx].result_balance = current_balance;
            // Crucially, update status LAST using atomic store
            __atomic_store_n(&region->queue[idx].status, status, __ATOMIC_SEQ_CST);

            sem_post(&region->dbmutex); // Unlock database access *****

            // Update log file if the state changed
            if (log_needed)
            {
                update_log_file();
            }
        } // end while(sem_trywait)

        // Short sleep if no work done, prevents busy-spinning on an empty queue
        usleep(10000); // Sleep for 10ms

    } // end while(running)

    printf("Server main loop finished.\n");
    // cleanup() is called automatically via atexit()

    close(server_fd); // Close FIFO read end
    close(dummy_fd);  // Close FIFO write end

    return 0;
}