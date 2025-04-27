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
#include <semaphore.h>
#include <sys/types.h>
#include <limits.h>  // For INT_MAX
#include <stdint.h>  // For intptr_t
#include <stdbool.h> // For bool type

#include "common.h"

// --- Global/Static Variables ---
static shm_region_t *region = NULL;
static int shm_fd = -1;
static int server_fd = -1; // Read end of main server FIFO
static int dummy_fd = -1;  // Write end of main server FIFO (to keep it open)
static const char *server_fifo_path = NULL;
static volatile sig_atomic_t running = 1;
static int teller_spawn_counter = 0; // Global counter for assigning Teller PIDs sequentially

// --- Log Event Type ---
typedef enum
{
    LOG_CREATE,
    LOG_DEPOSIT,
    LOG_WITHDRAW,
    LOG_CLOSE, // Represents the state change, not necessarily a direct command
    LOG_ERROR  // Should not happen in normal operation
} log_event_type_t;

// --- Structure for Log Summarization (used in cleanup) ---
#define MAX_TRANSACTION_LOG_LEN 512 // Adjust if needed
typedef struct
{
    bool active;                                   // Is this account ID ever used?
    int bank_id;                                   // The account ID
    char transaction_log[MAX_TRANSACTION_LOG_LEN]; // String to store " T Amount Balance" parts
    long final_balance;                            // Final balance after all transactions
    bool created;                                  // Flag to track if CREATE event was processed
} AccountSummary;

// --- Function Declarations ---
static void log_transaction(log_event_type_t type, int id, long amount, long balance);
static void load_state_from_log();
static int find_free_account_id();
static void sigint_handler(int signo);
static void cleanup();
static void process_deposit(request_t *req, int slot_idx);
static void process_withdraw(request_t *req, int slot_idx);
pid_t Teller(void *func, void *arg_func); // Assumed definition from common.h/pdf
int waitTeller(pid_t pid, int *status);   // Assumed definition from common.h/pdf
extern void *teller_main(void *);         // Actual entry point for teller process (defined in teller.c)

// --- Function Implementations ---

static void log_transaction(log_event_type_t type, int id, long amount, long balance)
{
    // This function now ONLY appends to the *detailed* log during runtime.
    // The summary format is generated during cleanup.
    FILE *log_fp = fopen(LOG_FILE_NAME, "a");
    if (!log_fp)
    {
        perror("SERVER ERROR: Log append failed");
        return;
    }

    // Use the original detailed format for runtime logging
    switch (type)
    {
    case LOG_CREATE:
        // Log the creation event and the initial balance
        fprintf(log_fp, "CREATE %d %ld\n", id, amount);
        break;
    case LOG_DEPOSIT:
        fprintf(log_fp, "DEPOSIT %d %ld %ld\n", id, amount, balance);
        break;
    case LOG_WITHDRAW:
        fprintf(log_fp, "WITHDRAW %d %ld %ld\n", id, amount, balance);
        break;
    case LOG_CLOSE:
        // Log the explicit close event (balance becomes 0)
        fprintf(log_fp, "CLOSE %d\n", id);
        break;
    default: // LOG_ERROR or unknown
        fprintf(log_fp, "UNKNOWN_EVENT %d %ld %ld\n", id, amount, balance);
        break;
    }
    fflush(log_fp);
    fsync(fileno(log_fp)); // Ensure data is written to disk
    fclose(log_fp);
}

static void load_state_from_log()
{
    // This function reads the *detailed* log to reconstruct the state. UNCHANGED.
    for (int i = 0; i < MAX_ACCOUNTS; ++i)
        region->balances[i] = ACCOUNT_INACTIVE;
    region->next_id = 0;

    FILE *log_fp = fopen(LOG_FILE_NAME, "r");
    if (!log_fp)
    {
        if (errno == ENOENT)
        {
            printf("No previous logs.. Creating the bank database\n");
            FILE *create_log = fopen(LOG_FILE_NAME, "w"); // Create new log file
            if (create_log)
            {
                // Write initial header for the detailed log
                time_t now = time(NULL);
                struct tm *tm_info = localtime(&now);
                char time_buf[100];
                // Format for detailed log header (can be simpler)
                strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", tm_info);
                fprintf(create_log, "# Adabank Detailed Log created @ %s\n", time_buf);
                fclose(create_log);
            }
            else
                perror("SERVER WARNING: Cannot create log file");
        }
        else
            perror("SERVER ERROR: Cannot read log file");
        return; // No state to load if file didn't exist or couldn't be read
    }

    char line[256];
    int max_id_found = -1;
    int line_num = 0;
    while (fgets(line, sizeof(line), log_fp))
    {
        line_num++;
        line[strcspn(line, "\n\r")] = 0;       // Strip newline/cr
        if (line[0] == '#' || line[0] == '\0') // Skip comments and empty lines
            continue;

        int id;
        long amount = 0, balance_from_log = 0;
        log_event_type_t current_type = LOG_ERROR; // Default to error

        // Parse different log entry types
        if (sscanf(line, "CREATE %d %ld", &id, &amount) == 2)
            current_type = LOG_CREATE;
        else if (sscanf(line, "DEPOSIT %d %ld %ld", &id, &amount, &balance_from_log) == 3)
            current_type = LOG_DEPOSIT;
        else if (sscanf(line, "WITHDRAW %d %ld %ld", &id, &amount, &balance_from_log) == 3)
            current_type = LOG_WITHDRAW;
        else if (sscanf(line, "CLOSE %d", &id) == 1) // Check for CLOSE event
            current_type = LOG_CLOSE;
        else
        {
            fprintf(stderr, "SERVER WARNING: Unparseable log line %d: %s\n", line_num, line);
            continue; // Skip invalid lines
        }

        // Basic validation of account ID
        if (id < 0 || id >= MAX_ACCOUNTS)
        {
            fprintf(stderr, "SERVER WARNING: Invalid account ID %d in log line %d\n", id, line_num);
            continue;
        }

        // Update in-memory state based on log event
        switch (current_type)
        {
        case LOG_CREATE:
            region->balances[id] = amount; // Set initial balance
            if (id > max_id_found)
                max_id_found = id;
            break;
        case LOG_DEPOSIT:
            // Trust the balance recorded in the log after the deposit
            region->balances[id] = balance_from_log;
            if (id > max_id_found)
                max_id_found = id;
            break;
        case LOG_WITHDRAW:
            // Trust the balance recorded in the log after the withdrawal
            region->balances[id] = balance_from_log;
            if (id > max_id_found) // Keep track even if balance is non-zero
                max_id_found = id;
            break;
        case LOG_CLOSE:
            region->balances[id] = ACCOUNT_INACTIVE; // Mark account as inactive
            // Do not update max_id_found here, as the ID might be reused later
            break;
        case LOG_ERROR: // Should have been caught by parsing checks
            break;
        }
    }
    fclose(log_fp);

    // Determine the next available ID based on the highest ID seen
    region->next_id = (max_id_found == -1) ? 0 : (max_id_found + 1);
    // Simple wrap-around if MAX_ACCOUNTS is reached (could be smarter)
    if (region->next_id >= MAX_ACCOUNTS)
        region->next_id = 0; // Or potentially search from 0 again
}

static int find_free_account_id()
{
    // This function finds the next inactive slot. UNCHANGED.
    sem_wait(&region->dbmutex); // Protect access to balances and next_id
    int start_idx = region->next_id;
    int current_idx = start_idx;
    do
    {
        if (region->balances[current_idx] == ACCOUNT_INACTIVE)
        {
            // Found a free slot
            region->next_id = (current_idx + 1) % MAX_ACCOUNTS; // Update hint for next search
            sem_post(&region->dbmutex);
            return current_idx;
        }
        current_idx = (current_idx + 1) % MAX_ACCOUNTS; // Move to next slot
    } while (current_idx != start_idx); // Loop until we've checked all slots

    sem_post(&region->dbmutex);
    return -1; // No free accounts found
}

static void sigint_handler(int signo)
{
    (void)signo; // Unused parameter
    // Use write for signal safety
    const char msg[] = "\nSignal received closing active Tellers\n";
    write(STDOUT_FILENO, msg, sizeof(msg) - 1);
    running = 0; // Set flag to terminate main loop
}

static void cleanup()
{
    // --- Original Cleanup Steps ---
    if (server_fd != -1)
        close(server_fd);
    if (dummy_fd != -1)
        close(dummy_fd);
    server_fd = dummy_fd = -1;

    // --- Log Summarization ---
    printf("Updating log file... ");
    fflush(stdout);

    FILE *log_read_fp = fopen(LOG_FILE_NAME, "r");
    if (!log_read_fp)
    {
        perror("WARN: Cannot open detailed log for reading");
        // Proceed to cleanup other resources even if log reading fails
    }
    else
    {
        AccountSummary summaries[MAX_ACCOUNTS];
        // Initialize summaries
        for (int i = 0; i < MAX_ACCOUNTS; ++i)
        {
            summaries[i].active = false;
            summaries[i].bank_id = i;
            summaries[i].final_balance = -1;        // Indicate not created or state unknown yet
            summaries[i].transaction_log[0] = '\0'; // *** Initialize log string as empty ***
            summaries[i].created = false;
        }

        char line[256];
        while (fgets(line, sizeof(line), log_read_fp))
        {
            line[strcspn(line, "\n\r")] = 0;
            if (line[0] == '#' || line[0] == '\0')
                continue;

            int id;
            long amount = 0, balance_from_log = 0; // balance_from_log is used to update final_balance

            // --- Process detailed log entries ---
            if (sscanf(line, "CREATE %d %ld", &id, &amount) == 2)
            {
                if (id >= 0 && id < MAX_ACCOUNTS)
                {
                    summaries[id].active = true;
                    summaries[id].created = true;
                    summaries[id].final_balance = amount; // Initial balance is the first final_balance
                    // *** Append only " D Amount" ***
                    char temp_log[64];
                    snprintf(temp_log, sizeof(temp_log), " D %ld", amount);
                    // Use strncat to append safely
                    strncat(summaries[id].transaction_log, temp_log, MAX_TRANSACTION_LOG_LEN - strlen(summaries[id].transaction_log) - 1);
                }
            }
            else if (sscanf(line, "DEPOSIT %d %ld %ld", &id, &amount, &balance_from_log) == 3)
            {
                if (id >= 0 && id < MAX_ACCOUNTS && summaries[id].created)
                {
                    summaries[id].active = true;
                    summaries[id].final_balance = balance_from_log; // Update final balance
                    // *** Append only " D Amount" ***
                    char temp_log[64];
                    snprintf(temp_log, sizeof(temp_log), " D %ld", amount);
                    strncat(summaries[id].transaction_log, temp_log, MAX_TRANSACTION_LOG_LEN - strlen(summaries[id].transaction_log) - 1);
                }
            }
            else if (sscanf(line, "WITHDRAW %d %ld %ld", &id, &amount, &balance_from_log) == 3)
            {
                if (id >= 0 && id < MAX_ACCOUNTS && summaries[id].created)
                {
                    summaries[id].active = true;
                    summaries[id].final_balance = balance_from_log; // Update final balance
                    // *** Append only " W Amount" ***
                    char temp_log[64];
                    snprintf(temp_log, sizeof(temp_log), " W %ld", amount);
                    strncat(summaries[id].transaction_log, temp_log, MAX_TRANSACTION_LOG_LEN - strlen(summaries[id].transaction_log) - 1);
                }
            }
            else if (sscanf(line, "CLOSE %d", &id) == 1)
            {
                if (id >= 0 && id < MAX_ACCOUNTS && summaries[id].created)
                {
                    summaries[id].active = true;     // Keep it active for summary line
                    summaries[id].final_balance = 0; // Explicitly set final balance to 0
                    // Note: CLOSE event itself doesn't add to transaction_log string
                }
            }
        }
        fclose(log_read_fp);

        // --- Overwrite Log File with Summary Format ---
        FILE *log_write_fp = fopen(LOG_FILE_NAME, "w");
        if (!log_write_fp)
        {
            perror("WARN: Cannot open log file for writing summary");
        }
        else
        {
            // Write new header for summary log
            time_t now = time(NULL);
            struct tm *tm_info = localtime(&now);
            char time_buf[100];
            strftime(time_buf, sizeof(time_buf), "%H:%M %B %d %Y", tm_info);
            fprintf(log_write_fp, "# Adabank Log file updated @%s\n", time_buf);

            // Write summary lines
            for (int i = 0; i < MAX_ACCOUNTS; ++i)
            {
                if (summaries[i].active && summaries[i].created)
                { // Only write if account was created
                    // Determine prefix based on final balance being zero
                    bool closed = (summaries[i].final_balance == 0);
                    // *** Print BankID, Transaction Log String, and Final Balance ***
                    fprintf(log_write_fp, "%sBankID_%02d%s %ld\n",
                            (closed ? "# " : ""), // Add "# " prefix if closed
                            summaries[i].bank_id,
                            summaries[i].transaction_log, // The string of " T Amount"
                            summaries[i].final_balance);  // The final balance number
                }
            }

            fprintf(log_write_fp, "## end of log.\n");
            fclose(log_write_fp);
            printf("done.\n"); // Confirmation for "Updating log file..."
        }
    } // End of else block for successful log_read_fp open

    // --- Remaining Original Cleanup ---
    // Destroy semaphores only if they were initialized (check region pointer)
    if (region != NULL && region != MAP_FAILED)
    {
        sem_destroy(&region->slots);
        sem_destroy(&region->items);
        sem_destroy(&region->qmutex);
        sem_destroy(&region->dbmutex);
        for (int i = 0; i < REQ_QUEUE_LEN; ++i)
            sem_destroy(&region->resp_ready[i]);
    }

    // Unmap and close shared memory
    if (region != NULL && region != MAP_FAILED)
    {
        munmap(region, sizeof(shm_region_t));
        region = NULL;
    }
    if (shm_fd != -1)
    {
        close(shm_fd);
        shm_fd = -1;
        // Unlink shared memory segment
        shm_unlink(SHM_NAME);
    }

    // Unlink server FIFO
    if (server_fifo_path)
    {
        printf("Removing ServerFIFO... ");
        fflush(stdout);
        if (unlink(server_fifo_path) == 0)
        {
            printf("done.\n");
        }
        else
        {
            perror("WARN: unlink server FIFO");
        }
    }

    fflush(stdout); // Ensure previous messages are printed
    printf("Adabank says \"Bye\"...\n");
}

static void process_deposit(request_t *req, int slot_idx)
{
    // This function processes deposits. Logging balance is important here. UNCHANGED Logic.
    int op_status = 2; // Default to error
    long current_balance = 0;
    int account_id = req->bank_id;

    // Case 1: Create new account (bank_id == -1)
    if (req->bank_id == -1)
    {
        int new_id = find_free_account_id(); // find_free_account_id already handles mutex
        if (new_id != -1)
        {
            account_id = new_id;
            sem_wait(&region->dbmutex); // Lock for writing balance
            region->balances[account_id] = req->amount;
            current_balance = req->amount;
            sem_post(&region->dbmutex);                                  // Unlock
            op_status = 0;                                               // OK
            log_transaction(LOG_CREATE, account_id, current_balance, 0); // Log CREATE event
            printf("Client%d deposited %ld credits... updating log\n", req->client_pid, req->amount);
        }
        else
        {
            // Bank full
            op_status = 2;       // Error
            current_balance = 0; // N/A
            account_id = -1;     // Indicate failure to create
            printf("Client%d create failed - bank full... operation not permitted.\n", req->client_pid);
            // No log entry for failed creation
        }
    }
    // Case 2: Deposit to existing account
    else if (req->bank_id >= 0 && req->bank_id < MAX_ACCOUNTS)
    {
        sem_wait(&region->dbmutex);                             // Lock for read/write balance
        if (region->balances[req->bank_id] != ACCOUNT_INACTIVE) // Check if account is active
        {
            account_id = req->bank_id;
            long *bal_ptr = &region->balances[account_id];
            // Check for overflow before adding
            if (__builtin_add_overflow(*bal_ptr, req->amount, bal_ptr))
            {
                // Overflow occurred, restore original balance (although it might be corrupted)
                // In a real system, more robust handling is needed. Here, we just report error.
                op_status = 2;                                  // Error
                                                                // Don't update current_balance if overflow occurred, report old balance maybe?
                                                                // Let's report the balance *before* the attempted operation failed
                current_balance = region->balances[account_id]; // Re-read potentially unchanged balance
                printf("Client%d deposit %ld failed (OVERFLOW)... operation not permitted.\n", req->client_pid, req->amount);
                // No log entry for overflow error
            }
            else
            {
                // Deposit successful
                current_balance = *bal_ptr; // New balance
                op_status = 0;              // OK
                log_transaction(LOG_DEPOSIT, account_id, req->amount, current_balance);
                printf("Client%d deposited %ld credits... updating log\n", req->client_pid, req->amount);
            }
        }
        else
        {
            // Account is inactive/invalid
            op_status = 2;       // Error
            current_balance = 0; // N/A
            printf("Client%d deposit %ld failed (BankID_%d inactive)... operation not permitted.\n", req->client_pid, req->amount, account_id);
            // No log entry for inactive account
        }
        sem_post(&region->dbmutex); // Unlock
    }
    // Case 3: Invalid Bank ID (should ideally be caught earlier)
    else
    {
        op_status = 2;       // Error
        current_balance = 0; // N/A
        printf("Client%d deposit %ld failed (Invalid BankID %d)... operation not permitted.\n", req->client_pid, req->amount, account_id);
        // No log entry for invalid ID
    }

    // Update the request slot with results
    region->queue[slot_idx].bank_id = account_id;             // Return the (potentially new) account ID
    region->queue[slot_idx].result_balance = current_balance; // Return balance *after* operation (or relevant value on error)
    region->queue[slot_idx].op_status = op_status;            // Return status code

    // Signal the waiting Teller
    sem_post(&region->resp_ready[slot_idx]);
}

static void process_withdraw(request_t *req, int slot_idx)
{
    // Processes withdrawals. UNCHANGED Logic.
    int op_status = 2; // Default error
    long current_balance = 0;
    int account_id = req->bank_id; // Should be a valid, active ID for withdraw

    if (account_id >= 0 && account_id < MAX_ACCOUNTS)
    {
        sem_wait(&region->dbmutex);                           // Lock for read/write balance
        if (region->balances[account_id] != ACCOUNT_INACTIVE) // Check if active
        {
            long *bal_ptr = &region->balances[account_id];
            if (*bal_ptr >= req->amount)
            {
                // Sufficient funds
                *bal_ptr -= req->amount;
                current_balance = *bal_ptr;
                op_status = 0; // OK
                log_transaction(LOG_WITHDRAW, account_id, req->amount, current_balance);
                printf("Client%d withdraws %ld credits... updating log", req->client_pid, req->amount);

                // Check if account is now empty and should be closed
                if (current_balance == 0)
                {
                    *bal_ptr = ACCOUNT_INACTIVE;                   // Mark as inactive
                    log_transaction(LOG_CLOSE, account_id, 0, 0);  // Log the closure
                    printf("... Bye Client%d\n", req->client_pid); // Account closed message
                }
                else
                {
                    printf("\n"); // End line if account not closed
                }
            }
            else
            {
                // Insufficient funds
                current_balance = *bal_ptr; // Report current balance
                op_status = 1;              // Insufficient funds status
                printf("Client%d withdraws %ld credit.. operation not permitted.\n", req->client_pid, req->amount);
                // No log entry for insufficient funds
            }
        }
        else
        {
            // Account inactive
            op_status = 2;       // Error
            current_balance = 0; // N/A
            printf("Client%d withdraws %ld failed (BankID_%d inactive)... operation not permitted.\n", req->client_pid, req->amount, account_id);
            // No log entry for inactive account
        }
        sem_post(&region->dbmutex); // Unlock
    }
    else
    {
        // Invalid Bank ID (e.g., -1 or out of range)
        op_status = 2;       // Error
        current_balance = 0; // N/A
        printf("Client%d withdraws %ld failed (Invalid BankID %d)... operation not permitted.\n", req->client_pid, req->amount, account_id);
        // No log entry for invalid ID
    }

    // Update request slot with results
    region->queue[slot_idx].bank_id = account_id;             // Account ID remains the same for withdraw
    region->queue[slot_idx].result_balance = current_balance; // Balance after operation (or before if failed)
    region->queue[slot_idx].op_status = op_status;            // Result status

    // Signal the waiting Teller
    sem_post(&region->resp_ready[slot_idx]);
}

// --- Teller Process Creation ---
pid_t Teller(void *func, void *arg_func)
{
    // Forks a new process to run the teller function. UNCHANGED.
    pid_t pid = fork();
    if (pid == -1)
    {
        perror("Teller (fork)");
        return -1; // Fork failed
    }
    else if (pid == 0)
    {
        // Child process (Teller)
        teller_main_func_t fn = (teller_main_func_t)func;
        fn(arg_func);        // Execute the teller main function with its argument
        _exit(EXIT_SUCCESS); // Teller exits cleanly
    }
    else
    {
        // Parent process (Server)
        // teller_spawn_counter is incremented in the main loop *before* calling Teller now
        return pid; // Return the PID of the newly created Teller process
    }
}

int waitTeller(pid_t pid, int *status)
{
    // Waits for a specific Teller process to terminate. UNCHANGED.
    return waitpid(pid, status, 0); // Use waitpid with 0 flags for blocking wait
}

// --- Main Server Function ---
int main(int argc, char *argv[])
{
    if (argc != 2)
    {
        fprintf(stderr, "Usage: %s <ServerFIFO_Name>\n", basename(argv[0]));
        exit(EXIT_FAILURE);
    }
    server_fifo_path = argv[1];
    printf("BankServer %s\n", server_fifo_path); // Print server name

    // --- Signal Handling Setup ---
    struct sigaction sa_int_term;
    memset(&sa_int_term, 0, sizeof(sa_int_term));
    sa_int_term.sa_handler = sigint_handler; // Use our handler for INT and TERM
    sa_int_term.sa_flags = SA_RESTART;       // Restart interrupted syscalls if possible
    sigaction(SIGINT, &sa_int_term, NULL);
    sigaction(SIGTERM, &sa_int_term, NULL);
    signal(SIGPIPE, SIG_IGN); // Ignore SIGPIPE (e.g., client disconnects)

    // --- Shared Memory Setup ---
    shm_unlink(SHM_NAME); // Attempt to remove old SHM segment first
    shm_fd = shm_open(SHM_NAME, O_RDWR | O_CREAT | O_EXCL, 0600);
    int shm_existed = 0;
    if (shm_fd == -1)
    {
        if (errno == EEXIST) // SHM already exists (maybe server crashed?)
        {
            shm_existed = 1;
            fprintf(stderr, "WARN: Shared memory '%s' already exists. Attempting to reuse.\n", SHM_NAME);
            shm_fd = shm_open(SHM_NAME, O_RDWR, 0600); // Try to open existing
            if (shm_fd == -1)
            {
                perror("FATAL: shm_open (existing)");
                exit(EXIT_FAILURE);
            }
            // Consider adding recovery logic here if SHM is in an inconsistent state
        }
        else
        {
            perror("FATAL: shm_open (create)");
            exit(EXIT_FAILURE);
        }
    }

    // --- Truncate & Map SHM (only if newly created) ---
    if (!shm_existed)
    {
        if (ftruncate(shm_fd, sizeof(shm_region_t)) == -1)
        {
            perror("FATAL: ftruncate");
            close(shm_fd);
            shm_unlink(SHM_NAME);
            exit(EXIT_FAILURE);
        }
    }

    region = mmap(NULL, sizeof(shm_region_t), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (region == MAP_FAILED)
    {
        perror("FATAL: mmap");
        close(shm_fd);
        if (!shm_existed)
            shm_unlink(SHM_NAME); // Clean up if we created it
        exit(EXIT_FAILURE);
    }

    // --- Initialize Semaphores & Load State (if new or potentially recovering) ---
    if (!shm_existed)
    {
        // Initialize all semaphores and region structure for the first time
        int ok = 1;
        if (sem_init(&region->qmutex, 1, 1) == -1)
            ok = 0; // Mutex for queue head/tail
        if (sem_init(&region->slots, 1, REQ_QUEUE_LEN) == -1)
            ok = 0; // Counting sem for empty slots
        if (sem_init(&region->items, 1, 0) == -1)
            ok = 0; // Counting sem for filled slots
        if (sem_init(&region->dbmutex, 1, 1) == -1)
            ok = 0; // Mutex for account balances/next_id
        for (int i = 0; i < REQ_QUEUE_LEN; ++i)
            if (sem_init(&region->resp_ready[i], 1, 0) == -1)
                ok = 0; // Sem for each response slot

        if (!ok)
        {
            fprintf(stderr, "FATAL: sem_init failed\n");
            cleanup(); // Attempt cleanup
            exit(EXIT_FAILURE);
        }
        region->head = region->tail = 0; // Initialize queue indices
        // Load state from log for the first time
        load_state_from_log(); // dbmutex is not needed here as no one else accessing yet
    }
    else
    {
        // SHM existed, potentially recovering. Try to acquire mutexes and reload state.
        // Use timed wait to detect potential deadlock if previous server crashed holding mutex.
        struct timespec t;
        clock_gettime(CLOCK_REALTIME, &t);
        t.tv_sec += 5; // Wait up to 5 seconds for the database mutex

        if (sem_timedwait(&region->dbmutex, &t) == -1)
        {
            perror("FATAL: timedwait on dbmutex during recovery");
            fprintf(stderr, "       Server might be running or SHM is corrupt.\n");
            cleanup(); // Attempt cleanup
            exit(EXIT_FAILURE);
        }
        // Acquired dbmutex, reload state from log to ensure consistency
        printf("Reloading state from log due to existing SHM...\n");
        load_state_from_log();
        sem_post(&region->dbmutex); // Release mutex
        // Note: We don't re-initialize other semaphores here, assuming they are valid.
        // More robust recovery might involve checking/resetting semaphore values.
    }

    // --- Server FIFO Setup ---
    unlink(server_fifo_path); // Remove old FIFO if it exists
    if (mkfifo(server_fifo_path, 0600) == -1)
    {
        perror("FATAL: mkfifo server");
        cleanup();
        exit(EXIT_FAILURE);
    }

    // Open FIFO for reading (non-blocking)
    server_fd = open(server_fifo_path, O_RDONLY | O_NONBLOCK);
    if (server_fd == -1)
    {
        perror("FATAL: open server read");
        cleanup();
        exit(EXIT_FAILURE);
    }

    // Open FIFO for writing (blocking) - This keeps the read end open even if no clients are writing
    dummy_fd = open(server_fifo_path, O_WRONLY);
    if (dummy_fd == -1)
    {
        perror("FATAL: open server write");
        close(server_fd); // Close read end before cleanup
        server_fd = -1;
        cleanup();
        exit(EXIT_FAILURE);
    }

    printf("Adabank is active….\n");
    printf("Waiting for clients @%s…\n", server_fifo_path);

// --- Main Server Loop ---
#define MAX_BATCH_CLIENTS 32 // Max clients to process from one read()
    pid_t batch_client_pids[MAX_BATCH_CLIENTS];
    int batch_teller_spawn_ids[MAX_BATCH_CLIENTS]; // Store the spawn ID (counter)

    while (running)
    {
        int clients_in_batch = 0;
        pid_t first_client_pid_in_batch = -1;

        // 1. Check for new client connection requests using poll
        struct pollfd fds[1];
        fds[0].fd = server_fd;
        fds[0].events = POLLIN; // Wait for data to read

        // Poll with a timeout (e.g., 500ms) to periodically check 'running' flag
        // and process queued items even if no new clients connect.
        int poll_res = poll(fds, 1, 500);

        if (poll_res > 0 && (fds[0].revents & POLLIN))
        {
            // Data is available on the server FIFO
            char buf[1024]; // Buffer to read client PIDs
            ssize_t n = read(server_fd, buf, sizeof(buf) - 1);

            if (n > 0)
            {
                buf[n] = '\0'; // Null-terminate the buffer
                char *ptr = buf;
                char *next_pid_str;
                char *saveptr; // For strtok_r

                // Parse all PIDs received in this read operation
                while (clients_in_batch < MAX_BATCH_CLIENTS &&
                       (next_pid_str = strtok_r(ptr, "\n", &saveptr)) != NULL)
                {
                    ptr = NULL; // After first call, strtok_r uses saveptr

                    if (strlen(next_pid_str) == 0)
                        continue; // Skip empty lines

                    // Validate and convert PID string
                    char *endptr;
                    long client_pid_long = strtol(next_pid_str, &endptr, 10);

                    if (*endptr != '\0' || client_pid_long <= 0 || client_pid_long > INT_MAX)
                    {
                        fprintf(stderr, "Server WARN: Invalid PID received: '%s'\n", next_pid_str);
                        continue; // Skip invalid PID
                    }
                    pid_t current_client_pid = (pid_t)client_pid_long;

                    // Record the first client PID of this batch
                    if (first_client_pid_in_batch == -1)
                        first_client_pid_in_batch = current_client_pid;

                    // Increment spawn counter *before* forking, so the first is PID01
                    teller_spawn_counter++;

                    // Spawn a Teller process for this client
                    // Pass the client PID as the argument to the teller_main function
                    void *teller_arg = (void *)(intptr_t)current_client_pid;
                    pid_t current_teller_pid = Teller(teller_main, teller_arg);

                    if (current_teller_pid != -1)
                    {
                        // Store info for batched printing *after* processing all PIDs in buffer
                        batch_client_pids[clients_in_batch] = current_client_pid;
                        batch_teller_spawn_ids[clients_in_batch] = teller_spawn_counter; // Store the ID used
                        clients_in_batch++;
                    }
                    else
                    {
                        // Fork failed, decrement counter
                        teller_spawn_counter--;
                        perror("  Server ERROR: Failed to spawn Teller process (fork failed)");
                        // Optionally notify client? Difficult without a dedicated channel yet.
                    }
                } // End while strtok_r

                // --- Print Batched Connection Messages ---
                if (clients_in_batch > 0)
                {
                    // Print the summary line for the batch
                    printf("- Received %d clients from PIDClient%d..\n",
                           clients_in_batch, first_client_pid_in_batch);

                    // Print the details for each Teller spawned in this batch
                    for (int i = 0; i < clients_in_batch; ++i)
                    {
                        // Use the stored spawn ID and client PID
                        printf("-- Teller PID%02d is active serving Client%d…\n",
                               batch_teller_spawn_ids[i], batch_client_pids[i]);
                    }
                    // Print waiting message again after handling the batch
                    printf("Waiting for clients @%s…\n", server_fifo_path);
                    fflush(stdout); // Ensure output is visible
                }

            } // End if (n > 0)
            else if (n == 0)
            {
                // EOF on the server FIFO? Should not happen with the dummy writer.
                fprintf(stderr, "Server WARN: EOF received on server FIFO.\n");
                // Consider closing and reopening? For now, just continue.
            }
            else // n < 0
            {
                // Read error (ignore EAGAIN/EWOULDBLOCK as we use poll)
                if (errno != EAGAIN && errno != EWOULDBLOCK)
                {
                    perror("Server ERROR reading FIFO");
                    running = 0; // Stop server on critical read error
                }
            }
        } // End if (poll_res > 0)
        else if (poll_res == -1 && errno != EINTR)
        {
            // Poll error (ignore EINTR)
            perror("Server ERROR: poll");
            running = 0; // Stop server on poll error
        }

        // 2. Process Pending Requests from the Queue (Non-blocking check)
        // Use sem_trywait to avoid blocking if the queue is empty
        while (sem_trywait(&region->items) == 0)
        {
            // Successfully decremented 'items', meaning a request is available
            if (!running)
            {
                // If server is stopping, put the item back and break
                sem_post(&region->items);
                break;
            }

            // Lock the queue mutex to safely access head index
            sem_wait(&region->qmutex);
            int idx = region->head;                            // Get index of the request
            request_t req = region->queue[idx];                // Copy the request data
            region->head = (region->head + 1) % REQ_QUEUE_LEN; // Move head index
            sem_post(&region->qmutex);                         // Unlock queue mutex

            // Increment 'slots' semaphore (signal that a slot is now free)
            sem_post(&region->slots);

            // Process the request (this involves DB access, so use dbmutex)
            // Note: process_deposit/withdraw now handle dbmutex internally
            if (req.type == REQ_DEPOSIT)
                process_deposit(&req, idx);
            else // REQ_WITHDRAW
                process_withdraw(&req, idx);

            // The process function will signal region->resp_ready[idx] when done.

        } // End while sem_trywait

        // 3. Reap Zombie Teller Processes (Non-blocking check)
        int status;
        pid_t ended_pid;
        while ((ended_pid = waitpid(-1, &status, WNOHANG)) > 0)
        {
            // Optionally log teller exit status
            // printf("DEBUG: Teller PID %d terminated.\n", ended_pid);
        }

        // 4. Brief sleep if idle to prevent busy-waiting (optional)
        // This check is less critical now due to poll timeout, but can still be useful
        if (running && poll_res <= 0) // No client connections in this cycle
        {
            int item_count;
            if (sem_getvalue(&region->items, &item_count) == 0 && item_count == 0) // And queue is empty
            {
                // Sleep for a very short duration
                struct timespec ts = {0, 10 * 1000 * 1000}; // 10 milliseconds
                nanosleep(&ts, NULL);
            }
        }

    } // End while(running)

    // --- Shutdown ---
    printf("Server shutting down...\n");

    // Wait for any remaining Teller processes to finish (optional, with timeout?)
    // A more robust shutdown might signal tellers to exit gracefully first.
    // For now, rely on the SIGINT/SIGTERM handler setting 'running' and
    // hoping Tellers check their 'teller_running' flag.

    // Final cleanup of zombies
    int status;
    while (waitpid(-1, &status, WNOHANG) > 0)
        ; // Reap any remaining children

    cleanup(); // Perform resource cleanup (SHM, FIFO, Semaphores, Log summarization)

    return 0;
}