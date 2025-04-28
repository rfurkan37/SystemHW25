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

// --- Static Variables ---
static shm_region_t *region = NULL;         // Pointer to the mapped shared memory region.
static int shm_fd = -1;                     // File descriptor for the shared memory object.
static int server_fd = -1;                  // Read end of the main server FIFO.
static int dummy_fd = -1;                   // Write end of main server FIFO (kept open to prevent EOF on reader).
static const char *server_fifo_path = NULL; // Path to the main server FIFO.
static volatile sig_atomic_t running = 1;   // Flag to control the main server loop, set to 0 by signal handler.
static int teller_spawn_counter = 0;        // Counter for assigning sequential IDs to spawned Tellers for logging.

// --- Log Event Type Enum ---
// Used to categorize detailed log entries during runtime.
typedef enum
{
    LOG_CREATE,
    LOG_DEPOSIT,
    LOG_WITHDRAW,
    LOG_CLOSE, // Represents account state change to inactive (balance 0)
    LOG_ERROR  // Should not occur in normal operation, indicates potential issues.
} log_event_type_t;

// --- Log Summarization Structure ---
// Used only during cleanup to aggregate detailed log information for the final summary log format.
#define MAX_TRANSACTION_LOG_LEN 512
typedef struct
{
    bool active;                                   // True if this account ID was ever used.
    int bank_id;                                   // The account ID.
    char transaction_log[MAX_TRANSACTION_LOG_LEN]; // Aggregated string of " T Amount" transactions.
    long final_balance;                            // Final balance derived from log entries.
    bool created;                                  // Flag to track if the CREATE event was logged for this account.
} AccountSummary;

// --- Function Declarations ---
static void log_transaction(log_event_type_t type, int id, long amount, long balance);
static void load_state_from_log();
static int find_free_account_id();
static void sigint_handler(int signo);
static void cleanup();
static void process_deposit(request_t *req, int slot_idx);
static void process_withdraw(request_t *req, int slot_idx);
pid_t Teller(void *func, void *arg_func); // Provided API function.
int waitTeller(pid_t pid, int *status);   // Provided API function.
extern void *teller_main(void *);         // Entry point for teller process (defined in teller.c).

// --- Function Implementations ---

/**
 * @brief Appends a detailed transaction record to the runtime log file.
 *        This log captures every state change event. The summary format is
 *        generated separately during cleanup.
 * @param type The type of event (CREATE, DEPOSIT, WITHDRAW, CLOSE).
 * @param id The account ID involved.
 * @param amount The transaction amount (relevant for CREATE, DEPOSIT, WITHDRAW).
 * @param balance The account balance *after* the transaction (relevant for DEPOSIT, WITHDRAW).
 */
static void log_transaction(log_event_type_t type, int id, long amount, long balance)
{
    // <<< ADDED: Wait for log mutex
    if (region != NULL && region != MAP_FAILED)
    { // Check if region is mapped
        if (sem_wait(&region->logmutex) == -1)
        {
            perror("SERVER ERROR: sem_wait(logmutex)");
            // Decide how to handle: maybe return, maybe proceed without lock?
            // Proceeding without lock risks corruption but might be better than deadlock/exit
        }
    }
    else
    {
        fprintf(stderr, "SERVER WARNING: SHM region not available in log_transaction\n");
        // Cannot use semaphore if region is not mapped (e.g., during early init/late cleanup?)
    }

    FILE *log_fp = fopen(LOG_FILE_NAME, "a");
    if (!log_fp)
    {
        perror("SERVER ERROR: Log append failed");
        if (region != NULL && region != MAP_FAILED) sem_post(&region->logmutex);
        return;
    }

    // Log events in a detailed format for state reconstruction.
    switch (type)
    {
    case LOG_CREATE:
        fprintf(log_fp, "CREATE %d %ld\n", id, amount); // Log initial balance on creation.
        break;
    case LOG_DEPOSIT:
        fprintf(log_fp, "DEPOSIT %d %ld %ld\n", id, amount, balance); // Log amount and resulting balance.
        break;
    case LOG_WITHDRAW:
        fprintf(log_fp, "WITHDRAW %d %ld %ld\n", id, amount, balance); // Log amount and resulting balance.
        break;
    case LOG_CLOSE:
        fprintf(log_fp, "CLOSE %d\n", id); // Log explicit closure event (balance reached 0).
        break;
    default:
        fprintf(log_fp, "UNKNOWN_EVENT %d %ld %ld\n", id, amount, balance);
        break;
    }
    fflush(log_fp);
    fsync(fileno(log_fp)); // Ensure data is written to disk for recovery.
    fclose(log_fp);

    if (region != NULL && region != MAP_FAILED) sem_post(&region->logmutex);
}

/**
 * @brief Initializes the in-memory bank state (balances array in SHM) by
 *        reading and processing the detailed transaction log file.
 *        This allows the server to recover its state after a crash or restart.
 */
static void load_state_from_log()
{
    // Initialize balances to inactive state before loading.
    for (int i = 0; i < MAX_ACCOUNTS; ++i)
        region->balances[i] = ACCOUNT_INACTIVE;
    region->next_id = 0; // Reset ID hint.

    FILE *log_fp = fopen(LOG_FILE_NAME, "r");
    if (!log_fp)
    {
        if (errno == ENOENT)
        {
            printf("No previous logs.. Creating the bank database\n");
            FILE *create_log = fopen(LOG_FILE_NAME, "w"); // Create a new log file if none exists.
            if (create_log)
            {
                time_t now = time(NULL);
                struct tm *tm_info = localtime(&now);
                char time_buf[100];
                strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", tm_info);
                fprintf(create_log, "# Adabank Detailed Log created @ %s\n", time_buf);
                fclose(create_log);
            }
            else
                perror("SERVER WARNING: Cannot create log file");
        }
        else
            perror("SERVER ERROR: Cannot read log file");
        return; // No state to load.
    }

    char line[256];
    int max_id_found = -1;
    int line_num = 0;
    while (fgets(line, sizeof(line), log_fp))
    {
        line_num++;
        line[strcspn(line, "\n\r")] = 0;       // Strip newline/cr.
        if (line[0] == '#' || line[0] == '\0') // Skip comments and empty lines.
            continue;

        int id;
        long amount = 0, balance_from_log = 0;
        log_event_type_t current_type = LOG_ERROR;

        // Parse different detailed log entry formats.
        if (sscanf(line, "CREATE %d %ld", &id, &amount) == 2)
            current_type = LOG_CREATE;
        else if (sscanf(line, "DEPOSIT %d %ld %ld", &id, &amount, &balance_from_log) == 3)
            current_type = LOG_DEPOSIT;
        else if (sscanf(line, "WITHDRAW %d %ld %ld", &id, &amount, &balance_from_log) == 3)
            current_type = LOG_WITHDRAW;
        else if (sscanf(line, "CLOSE %d", &id) == 1)
            current_type = LOG_CLOSE;
        else
        {
            // Try to parse summary format (BankID_XX D 100 W 50 150)
            char bank_id_str[64];
            long final_balance = 0;

            // Extract bank ID from BankID_XX format
            if (sscanf(line, "%s", bank_id_str) == 1 &&
                strncmp(bank_id_str, "BankID_", 7) == 0)
            {

                char *id_part = bank_id_str + 7; // Skip "BankID_"
                id = atoi(id_part);

                // Extract the final balance (last number on the line)
                char *last_space = strrchr(line, ' ');
                if (last_space)
                {
                    final_balance = atol(last_space + 1);
                    balance_from_log = final_balance;
                    current_type = LOG_DEPOSIT; // Treat as deposit for simplicity
                }
            }
            else
            {
                fprintf(stderr, "SERVER WARNING: Unparseable log line %d: %s\n", line_num, line);
                continue;
            }
        }

        if (id < 0 || id >= MAX_ACCOUNTS)
        {
            fprintf(stderr, "SERVER WARNING: Invalid account ID %d in log line %d\n", id, line_num);
            continue;
        }

        // Update the in-memory state based on the parsed log event.
        // The balance recorded *in the log* is trusted as the correct state after that operation.
        switch (current_type)
        {
        case LOG_CREATE:
            region->balances[id] = amount; // Set initial balance.
            if (id > max_id_found)
                max_id_found = id;
            break;
        case LOG_DEPOSIT:
            region->balances[id] = balance_from_log; // Update balance from log entry.
            if (id > max_id_found)
                max_id_found = id;
            break;
        case LOG_WITHDRAW:
            region->balances[id] = balance_from_log; // Update balance from log entry.
            if (id > max_id_found)
                max_id_found = id;
            break;
        case LOG_CLOSE:
            region->balances[id] = ACCOUNT_INACTIVE; // Mark account as closed/inactive.
            break;
        case LOG_ERROR: // Should have been caught during parsing.
            break;
        }
    }
    fclose(log_fp);

    // Set the 'next_id' hint for finding free accounts based on the highest ID seen.
    region->next_id = (max_id_found == -1) ? 0 : (max_id_found + 1);
    if (region->next_id >= MAX_ACCOUNTS)
        region->next_id = 0; // Wrap around if max ID reached the limit.
}

/**
 * @brief Finds an available (inactive) account ID slot in the balances array.
 *        Uses linear probing starting from `region->next_id`. Protected by `dbmutex`.
 * @return The index of a free slot, or -1 if the bank is full.
 */
static int find_free_account_id()
{
    sem_wait(&region->dbmutex); // Protect access to balances and next_id.
    int start_idx = region->next_id;
    int current_idx = start_idx;
    do
    {
        if (region->balances[current_idx] == ACCOUNT_INACTIVE)
        {
            // Found a free slot.
            region->next_id = (current_idx + 1) % MAX_ACCOUNTS; // Update hint for next search.
            sem_post(&region->dbmutex);
            return current_idx;
        }
        current_idx = (current_idx + 1) % MAX_ACCOUNTS; // Move to the next slot.
    } while (current_idx != start_idx); // Loop until all slots are checked.

    sem_post(&region->dbmutex);
    return -1; // No free accounts found.
}

/**
 * @brief Signal handler for SIGINT and SIGTERM. Sets the global 'running'
 *        flag to initiate a graceful server shutdown. Uses write() for async-signal safety.
 */
static void sigint_handler(int signo)
{
    (void)signo; // Unused parameter.
    const char msg[] = "\nSignal received closing active Tellers\n";
    write(STDOUT_FILENO, msg, sizeof(msg) - 1); // Use write() for signal safety.
    running = 0;                                // Signal the main loop to terminate.
}

/**
 * @brief Performs server cleanup: closes FDs, summarizes the log file,
 *        destroys semaphores, unmaps and unlinks shared memory, and unlinks
 *        the server FIFO.
 */
static void cleanup()
{
    // Close server FIFO file descriptors.
    if (server_fd != -1)
        close(server_fd);
    if (dummy_fd != -1)
        close(dummy_fd);
    server_fd = dummy_fd = -1;

    // --- Log Summarization ---
    // Reads the detailed runtime log and overwrites it with a summarized format.
    printf("Updating log file... ");
    fflush(stdout);

    FILE *log_read_fp = fopen(LOG_FILE_NAME, "r");
    if (!log_read_fp)
    {
        perror("WARN: Cannot open detailed log for reading during cleanup");
        // Continue cleanup even if log reading fails.
    }
    else
    {
        AccountSummary summaries[MAX_ACCOUNTS];
        for (int i = 0; i < MAX_ACCOUNTS; ++i)
        {
            summaries[i].active = false;
            summaries[i].bank_id = i;
            summaries[i].final_balance = -1;        // Indicates initial state unknown.
            summaries[i].transaction_log[0] = '\0'; // Initialize as empty string.
            summaries[i].created = false;
        }

        // Read detailed log and build summaries in memory.
        char line[256];
        while (fgets(line, sizeof(line), log_read_fp))
        {
            line[strcspn(line, "\n\r")] = 0;
            if (line[0] == '#' || line[0] == '\0')
                continue;

            int id;
            long amount = 0, balance_from_log = 0;

            if (sscanf(line, "CREATE %d %ld", &id, &amount) == 2)
            {
                if (id >= 0 && id < MAX_ACCOUNTS)
                {
                    summaries[id].active = true;
                    summaries[id].created = true;
                    summaries[id].final_balance = amount; // Initial balance.
                    char temp_log[64];
                    snprintf(temp_log, sizeof(temp_log), " D %ld", amount); // Format: D amount
                    strncat(summaries[id].transaction_log, temp_log, MAX_TRANSACTION_LOG_LEN - strlen(summaries[id].transaction_log) - 1);
                }
            }
            else if (sscanf(line, "DEPOSIT %d %ld %ld", &id, &amount, &balance_from_log) == 3)
            {
                if (id >= 0 && id < MAX_ACCOUNTS && summaries[id].created)
                {
                    summaries[id].active = true;
                    summaries[id].final_balance = balance_from_log; // Update final balance.
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
                    summaries[id].final_balance = balance_from_log; // Update final balance.
                    char temp_log[64];
                    snprintf(temp_log, sizeof(temp_log), " W %ld", amount); // Format: W amount
                    strncat(summaries[id].transaction_log, temp_log, MAX_TRANSACTION_LOG_LEN - strlen(summaries[id].transaction_log) - 1);
                }
            }
            else if (sscanf(line, "CLOSE %d", &id) == 1)
            {
                if (id >= 0 && id < MAX_ACCOUNTS && summaries[id].created)
                {
                    summaries[id].active = true;
                    summaries[id].final_balance = 0; // Explicitly set balance to 0 on close.
                    // CLOSE event itself doesn't add to the transaction_log string.
                }
            }
        }
        fclose(log_read_fp);

        // Overwrite the log file with the generated summary.
        FILE *log_write_fp = fopen(LOG_FILE_NAME, "w");
        if (!log_write_fp)
        {
            perror("WARN: Cannot open log file for writing summary");
        }
        else
        {
            time_t now = time(NULL);
            struct tm *tm_info = localtime(&now);
            char time_buf[100];
            strftime(time_buf, sizeof(time_buf), "%H:%M %B %d %Y", tm_info);
            fprintf(log_write_fp, "# Adabank Log file updated @%s\n", time_buf); // Summary header.

            // Write summary lines for active and created accounts.
            for (int i = 0; i < MAX_ACCOUNTS; ++i)
            {
                if (summaries[i].active && summaries[i].created)
                {
                    bool closed = (summaries[i].final_balance == 0);
                    // Format: [# ]BankID_XX T Amt T Amt ... FinalBalance
                    fprintf(log_write_fp, "%sBankID_%02d%s %ld\n",
                            (closed ? "# " : ""), // Prefix with "# " if closed.
                            summaries[i].bank_id,
                            summaries[i].transaction_log, // The aggregated transaction string.
                            summaries[i].final_balance);  // The final calculated balance.
                }
            }

            fprintf(log_write_fp, "## end of log.\n");
            fclose(log_write_fp);
            printf("done.\n");
        }
    } // End log summarization block.

    // --- Destroy Semaphores and Cleanup SHM ---
    if (region != NULL && region != MAP_FAILED)
    {
        // Only destroy if semaphores were likely initialized.
        sem_destroy(&region->slots);
        sem_destroy(&region->items);
        sem_destroy(&region->qmutex);
        sem_destroy(&region->dbmutex);
        sem_destroy(&region->logmutex);
        for (int i = 0; i < REQ_QUEUE_LEN; ++i)
            sem_destroy(&region->resp_ready[i]);

        munmap(region, sizeof(shm_region_t)); // Unmap shared memory.
        region = NULL;
    }
    if (shm_fd != -1)
    {
        close(shm_fd); // Close SHM file descriptor.
        shm_fd = -1;
        shm_unlink(SHM_NAME); // Remove the shared memory object from the system.
    }

    // --- Unlink Server FIFO ---
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

    printf("Adabank says \"Bye\"...\n");
}

/**
 * @brief Processes a deposit request from the shared memory queue.
 *        Handles both creating new accounts (req->bank_id == -1) and
 *        depositing into existing accounts. Updates balance, logs transaction,
 *        and signals completion via resp_ready semaphore. Protected by dbmutex.
 * @param req Pointer to the request structure in the SHM queue.
 * @param slot_idx The index of the request in the SHM queue.
 */
static void process_deposit(request_t *req, int slot_idx)
{
    int op_status = 2; // Default to error status.
    long current_balance = 0;
    int account_id = req->bank_id; // Use a local variable for the target ID.

    // Handle account creation request.
    if (req->bank_id == -1)
    {
        int new_id = find_free_account_id(); // Atomically finds and reserves an ID slot.
        if (new_id != -1)
        {
            account_id = new_id;
            sem_wait(&region->dbmutex); // Lock for balance write.
            region->balances[account_id] = req->amount;
            current_balance = req->amount;
            sem_post(&region->dbmutex);                                  // Unlock.
            op_status = 0;                                               // OK.
            log_transaction(LOG_CREATE, account_id, current_balance, 0); // Log creation with initial balance.
            printf("Client%d deposited %ld credits... updating log\n", req->client_pid, req->amount);
        }
        else
        {
            // Bank is full.
            op_status = 2;       // Error.
            current_balance = 0; // Balance N/A.
            account_id = -1;     // Indicate creation failure.
            printf("Client%d create failed - bank full... operation not permitted.\n", req->client_pid);
            // No log entry for failed creation attempt.
        }
    }
    // Handle deposit to existing account.
    else if (req->bank_id >= 0 && req->bank_id < MAX_ACCOUNTS)
    {
        sem_wait(&region->dbmutex);                             // Lock for balance read/write.
        if (region->balances[req->bank_id] != ACCOUNT_INACTIVE) // Check if account exists and is active.
        {
            account_id = req->bank_id;
            long *bal_ptr = &region->balances[account_id];
            // Check for potential integer overflow before adding.
            if (__builtin_add_overflow(*bal_ptr, req->amount, bal_ptr))
            {
                // Overflow detected. Operation fails. Balance remains unchanged.
                op_status = 2;              // Error status.
                                            // Restore previous balance before overflow attempt (though bal_ptr might be corrupted depending on overflow behavior)
                                            // Re-reading is safer: current_balance = region->balances[account_id];
                sem_post(&region->dbmutex); // Unlock BEFORE potential re-read inside lock
                sem_wait(&region->dbmutex);
                current_balance = region->balances[account_id]; // Read the balance *before* failed attempt
                sem_post(&region->dbmutex);
                sem_wait(&region->dbmutex); // Re-acquire lock if needed later? No, logic ends here for error.
                printf("Client%d deposit %ld failed (OVERFLOW)... operation not permitted.\n", req->client_pid, req->amount);
                // No log entry for overflow error.
            }
            else
            {
                // Deposit successful.
                current_balance = *bal_ptr; // New balance after successful addition.
                op_status = 0;              // OK status.
                log_transaction(LOG_DEPOSIT, account_id, req->amount, current_balance);
                printf("Client%d deposited %ld credits... updating log\n", req->client_pid, req->amount);
            }
        }
        else
        {
            // Account is inactive/does not exist.
            op_status = 2;       // Error status.
            current_balance = 0; // Balance N/A.
            printf("Client%d deposit %ld failed (BankID_%d inactive)... operation not permitted.\n", req->client_pid, req->amount, account_id);
            // No log entry for attempting deposit to inactive account.
        }
        sem_post(&region->dbmutex); // Unlock.
    }
    // Handle invalid Bank ID provided in request.
    else
    {
        op_status = 2;       // Error status.
        current_balance = 0; // Balance N/A.
        printf("Client%d deposit %ld failed (Invalid BankID %d)... operation not permitted.\n", req->client_pid, req->amount, account_id);
        // No log entry for invalid ID.
    }

    // Update the response fields in the shared memory request slot.
    region->queue[slot_idx].bank_id = account_id;             // Return the actual account ID used (or -1 on creation failure).
    region->queue[slot_idx].result_balance = current_balance; // Return balance *after* operation (or relevant error value).
    region->queue[slot_idx].op_status = op_status;            // Return operation status code.

    // Signal the waiting Teller that the response is ready.
    sem_post(&region->resp_ready[slot_idx]);
}

/**
 * @brief Processes a withdraw request from the shared memory queue.
 *        Checks for valid account, sufficient funds, updates balance,
 *        logs transaction (including potential closure), and signals completion.
 *        Protected by dbmutex.
 * @param req Pointer to the request structure in the SHM queue.
 * @param slot_idx The index of the request in the SHM queue.
 */
static void process_withdraw(request_t *req, int slot_idx)
{
    int op_status = 2; // Default to error status.
    long current_balance = 0;
    int account_id = req->bank_id; // Should be a valid, active ID for withdrawal.

    if (account_id >= 0 && account_id < MAX_ACCOUNTS)
    {
        sem_wait(&region->dbmutex);                           // Lock for balance read/write.
        if (region->balances[account_id] != ACCOUNT_INACTIVE) // Check if account is active.
        {
            long *bal_ptr = &region->balances[account_id];
            if (*bal_ptr >= req->amount)
            {
                // Sufficient funds for withdrawal.
                *bal_ptr -= req->amount;
                current_balance = *bal_ptr;
                op_status = 0; // OK status.
                log_transaction(LOG_WITHDRAW, account_id, req->amount, current_balance);
                printf("Client%d withdraws %ld credits... updating log", req->client_pid, req->amount);

                // Check if the withdrawal emptied the account.
                if (current_balance == 0)
                {
                    *bal_ptr = ACCOUNT_INACTIVE;                   // Mark the account slot as inactive.
                    log_transaction(LOG_CLOSE, account_id, 0, 0);  // Log the closure event.
                    printf("... Bye Client%d\n", req->client_pid); // Indicate account closure.
                }
                else
                {
                    printf("\n"); // End the line if account remains open.
                }
            }
            else
            {
                // Insufficient funds.
                current_balance = *bal_ptr; // Report the current balance before failed withdrawal.
                op_status = 1;              // Insufficient funds status code.
                printf("Client%d withdraws %ld credit.. operation not permitted.\n", req->client_pid, req->amount);
                // No log entry for insufficient funds attempt.
            }
        }
        else
        {
            // Account is inactive.
            op_status = 2;       // Error status.
            current_balance = 0; // Balance N/A.
            printf("Client%d withdraws %ld failed (BankID_%d inactive)... operation not permitted.\n", req->client_pid, req->amount, account_id);
            // No log entry for inactive account withdrawal attempt.
        }
        sem_post(&region->dbmutex); // Unlock.
    }
    else
    {
        // Invalid Bank ID provided in request.
        op_status = 2;       // Error status.
        current_balance = 0; // Balance N/A.
        printf("Client%d withdraws %ld failed (Invalid BankID %d)... operation not permitted.\n", req->client_pid, req->amount, account_id);
        // No log entry for invalid ID.
    }

    // Update the response fields in the shared memory request slot.
    region->queue[slot_idx].bank_id = account_id;             // Account ID remains the same for withdraw.
    region->queue[slot_idx].result_balance = current_balance; // Balance after operation (or before if failed).
    region->queue[slot_idx].op_status = op_status;            // Result status code.

    // Signal the waiting Teller that the response is ready.
    sem_post(&region->resp_ready[slot_idx]);
}

/**
 * @brief Forks a new process to execute the specified teller function.
 * @param func Pointer to the teller entry function (e.g., teller_main).
 * @param arg_func Argument to be passed to the teller function (client PID).
 * @return PID of the created Teller process, or -1 on fork error.
 */
pid_t Teller(void *func, void *arg_func)
{
    pid_t pid = fork();
    if (pid == -1)
    {
        perror("Teller (fork)");
        return -1; // Fork failed.
    }
    else if (pid == 0)
    {
        // Child process (Teller).
        teller_main_func_t fn = (teller_main_func_t)func;
        fn(arg_func);        // Execute the teller main function.
        _exit(EXIT_SUCCESS); // Teller process exits cleanly.
    }
    else
    {
        // Parent process (Server).
        // The teller_spawn_counter is incremented in the main loop *before* calling Teller.
        return pid; // Return the new Teller's PID.
    }
}

/**
 * @brief Waits for a specific Teller process to terminate.
 * @param pid The PID of the Teller process to wait for.
 * @param status Pointer to an integer to store the exit status.
 * @return Result of the waitpid system call.
 */
int waitTeller(pid_t pid, int *status)
{
    // Use waitpid with 0 flags for a blocking wait on a specific child.
    return waitpid(pid, status, 0);
}

// --- Main Server Entry Point ---
int main(int argc, char *argv[])
{
    if (argc != 2)
    {
        fprintf(stderr, "Usage: %s <ServerFIFO_Name>\n", basename(argv[0]));
        exit(EXIT_FAILURE);
    }
    server_fifo_path = argv[1];
    printf("BankServer %s\n", server_fifo_path);

    // --- Signal Handling Setup ---
    struct sigaction sa_int_term;
    memset(&sa_int_term, 0, sizeof(sa_int_term));
    sa_int_term.sa_handler = sigint_handler; // Handle SIGINT and SIGTERM for graceful shutdown.
    sa_int_term.sa_flags = SA_RESTART;       // Restart interrupted syscalls if possible.
    sigaction(SIGINT, &sa_int_term, NULL);
    sigaction(SIGTERM, &sa_int_term, NULL);
    signal(SIGPIPE, SIG_IGN); // Ignore SIGPIPE (e.g., if a Teller writes to a closed client pipe).

    // --- Shared Memory Setup ---
    shm_unlink(SHM_NAME); // Attempt to remove any stale SHM segment first.
    shm_fd = shm_open(SHM_NAME, O_RDWR | O_CREAT | O_EXCL, 0600);
    int shm_existed = 0;
    if (shm_fd == -1)
    {
        if (errno == EEXIST) // SHM segment already exists (potential recovery scenario).
        {
            shm_existed = 1;
            fprintf(stderr, "WARN: Shared memory '%s' already exists. Attempting to reuse.\n", SHM_NAME);
            shm_fd = shm_open(SHM_NAME, O_RDWR, 0600); // Try to open the existing segment.
            if (shm_fd == -1)
            {
                perror("FATAL: shm_open (existing)");
                exit(EXIT_FAILURE);
            }
        }
        else
        {
            perror("FATAL: shm_open (create)");
            exit(EXIT_FAILURE);
        }
    }

    // Set the size of the SHM segment (only if newly created).
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

    // Map the shared memory segment into the server's address space.
    region = mmap(NULL, sizeof(shm_region_t), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (region == MAP_FAILED)
    {
        perror("FATAL: mmap");
        close(shm_fd);
        if (!shm_existed)
            shm_unlink(SHM_NAME); // Clean up SHM if we created it.
        exit(EXIT_FAILURE);
    }

    // --- Initialize Semaphores & Load State ---
    if (!shm_existed)
    {
        // First run: Initialize all semaphores and the region structure.
        int ok = 1;
        if (sem_init(&region->qmutex, 1, 1) == -1)
            ok = 0; // Mutex for queue head/tail
        if (sem_init(&region->slots, 1, REQ_QUEUE_LEN) == -1)
            ok = 0; // Available slots
        if (sem_init(&region->items, 1, 0) == -1)
            ok = 0; // Available items (requests)
        if (sem_init(&region->dbmutex, 1, 1) == -1)
            ok = 0; // Mutex for balance array & next_id
        if (sem_init(&region->logmutex, 1, 1) == -1)
            ok = 0; // Mutex for log file access
        for (int i = 0; i < REQ_QUEUE_LEN; ++i)
            if (sem_init(&region->resp_ready[i], 1, 0) == -1)
                ok = 0; // Response signals (initially 0)

        if (!ok)
        {
            fprintf(stderr, "FATAL: sem_init failed\n");
            cleanup(); // Attempt cleanup before exiting.
            exit(EXIT_FAILURE);
        }
        region->head = region->tail = 0; // Initialize queue indices.
        load_state_from_log();           // Load initial state from log file (or create it).
    }
    else
    {
        // SHM existed: Attempt recovery. Use timed wait on a mutex as a basic check
        // that another server instance isn't actively holding it.
        struct timespec t;
        clock_gettime(CLOCK_REALTIME, &t);
        t.tv_sec += 5; // Set a 5-second timeout.
        if (sem_timedwait(&region->dbmutex, &t) == -1)
        {
            perror("FATAL: timedwait on dbmutex during recovery");
            fprintf(stderr, "       Server might be running or SHM is corrupt.\n");
            cleanup();
            exit(EXIT_FAILURE);
        }
        // Acquired mutex, assume we can proceed with recovery.
        printf("Reloading state from log due to existing SHM...\n");
        load_state_from_log();      // Reload state from the detailed log.
        sem_post(&region->dbmutex); // Release the mutex.
    }

    // --- Server FIFO Setup ---
    unlink(server_fifo_path); // Remove any old server FIFO.
    if (mkfifo(server_fifo_path, 0600) == -1)
    {
        perror("FATAL: mkfifo server");
        cleanup();
        exit(EXIT_FAILURE);
    }
    // Open FIFO read-only and non-blocking for checking connections.
    server_fd = open(server_fifo_path, O_RDONLY | O_NONBLOCK);
    if (server_fd == -1)
    {
        perror("FATAL: open server read");
        cleanup();
        exit(EXIT_FAILURE);
    }
    // Open FIFO write-only to keep it existing even if no clients are writing.
    dummy_fd = open(server_fifo_path, O_WRONLY);
    if (dummy_fd == -1)
    {
        perror("FATAL: open server write");
        close(server_fd); // Close read end if write end failed.
        server_fd = -1;
        cleanup();
        exit(EXIT_FAILURE);
    }

    printf("Adabank is active….\n");
    printf("Waiting for clients @%s…\n", server_fifo_path);

// --- Main Server Loop ---
// Buffer for handling potentially multiple client PIDs arriving close together.
#define MAX_BATCH_CLIENTS 32
    pid_t batch_client_pids[MAX_BATCH_CLIENTS];
    int batch_teller_spawn_ids[MAX_BATCH_CLIENTS];

    while (running)
    {
        int clients_in_batch = 0;
        pid_t first_client_pid_in_batch = -1;

        // 1. Check for new client connection requests using poll (non-blocking).
        struct pollfd fds[1];
        fds[0].fd = server_fd;
        fds[0].events = POLLIN;           // Check for readability.
        int poll_res = poll(fds, 1, 250); // Timeout (milliseconds) to allow processing queue/reaping.

        if (poll_res > 0 && (fds[0].revents & POLLIN))
        {
            // Data available on the server FIFO (client PIDs).
            char buf[1024];
            ssize_t n = read(server_fd, buf, sizeof(buf) - 1);

            if (n > 0)
            {
                buf[n] = '\0'; // Null-terminate the read data.
                char *ptr = buf;
                char *next_pid_str;
                char *saveptr; // For strtok_r

                // Parse potentially multiple PIDs (newline-separated) and spawn Tellers.
                while (clients_in_batch < MAX_BATCH_CLIENTS &&
                       (next_pid_str = strtok_r(ptr, "\n", &saveptr)) != NULL)
                {
                    ptr = NULL; // For subsequent strtok_r calls.
                    if (strlen(next_pid_str) == 0)
                        continue; // Skip empty lines.

                    // Validate and parse PID.
                    char *endptr;
                    long client_pid_long = strtol(next_pid_str, &endptr, 10);
                    if (*endptr != '\0' || client_pid_long <= 0 || client_pid_long > INT_MAX)
                    {
                        fprintf(stderr, "Server WARN: Invalid PID received: '%s'\n", next_pid_str);
                        continue;
                    }
                    pid_t current_client_pid = (pid_t)client_pid_long;

                    if (first_client_pid_in_batch == -1)
                        first_client_pid_in_batch = current_client_pid;

                    // Spawn a Teller process for this client.
                    teller_spawn_counter++;                                  // Increment global teller ID counter.
                    void *teller_arg = (void *)(intptr_t)current_client_pid; // Pass client PID as argument.
                    pid_t current_teller_pid = Teller(teller_main, teller_arg);

                    if (current_teller_pid != -1)
                    {
                        // Store info for batched logging message.
                        batch_client_pids[clients_in_batch] = current_client_pid;
                        batch_teller_spawn_ids[clients_in_batch] = teller_spawn_counter;
                        clients_in_batch++;
                    }
                    else
                    {
                        teller_spawn_counter--; // Decrement counter if fork failed.
                        perror("  Server ERROR: Failed to spawn Teller process (fork failed)");
                    }
                } // End while(strtok_r)

                // Print a summary message for the batch of clients received.
                if (clients_in_batch > 0)
                {
                    printf("- Received %d clients from PIDClient%d..\n",
                           clients_in_batch, first_client_pid_in_batch);
                    for (int i = 0; i < clients_in_batch; ++i)
                    {
                        printf("-- Teller PID%02d is active serving Client%d…\n",
                               batch_teller_spawn_ids[i], batch_client_pids[i]);
                    }
                    printf("Waiting for clients @%s…\n", server_fifo_path);
                    fflush(stdout);
                }
            }
            else if (n == 0)
            {
                // Should not happen with the dummy writer, but handle defensively.
                fprintf(stderr, "Server WARN: EOF received on server FIFO.\n");
            }
            else
            { // n < 0
                // Error reading FIFO (ignore EAGAIN/EWOULDBLOCK from non-blocking read).
                if (errno != EAGAIN && errno != EWOULDBLOCK)
                {
                    perror("Server ERROR reading FIFO");
                    running = 0; // Trigger shutdown on persistent error.
                }
            }
        } // End handling poll result > 0
        else if (poll_res == -1 && errno != EINTR)
        {
            // Error during poll itself.
            perror("Server ERROR: poll");
            running = 0; // Trigger shutdown.
        }

        // 2. Process Pending Requests from the Shared Memory Queue (Consumer Role).
        bool processed_item = false; // Track if any work was done in this cycle.
        // Use sem_trywait for non-blocking check if items are available.
        while (sem_trywait(&region->items) == 0)
        {
            processed_item = true; // We got an item to process.
            // Check running flag again in case a signal arrived while waiting/polling.
            if (!running)
            {
                sem_post(&region->items); // Put the item back if shutting down.
                break;
            }

            // Get mutex to access queue head index.
            sem_wait(&region->qmutex);
            int idx = region->head;                            // Index of the request to process.
            request_t req = region->queue[idx];                // Copy request data locally.
            region->head = (region->head + 1) % REQ_QUEUE_LEN; // Advance head index.
            sem_post(&region->qmutex);                         // Release mutex.

            // Signal that a slot is now free in the queue.
            sem_post(&region->slots);

            // Dispatch request to appropriate processing function.
            if (req.type == REQ_DEPOSIT)
                process_deposit(&req, idx);
            else // REQ_WITHDRAW
                process_withdraw(&req, idx);

        } // End while sem_trywait (processing items)

        // 3a. Reap Zombie Teller Processes (opportunistic, after processing items).
        // This helps catch Tellers that finish right after their last request is processed.
        if (processed_item)
        { // Only reap if we might have freed up a Teller.
            int status_reap;
            pid_t ended_pid;
            while ((ended_pid = waitpid(-1, &status_reap, WNOHANG)) > 0)
            {
                // Optionally log reaped Teller PID here.
            }
            if (ended_pid == -1 && errno != ECHILD)
            { // Check for errors other than "no children".
                perror("Server waitpid error during post-processing reap");
            }
        }

        // 3b. Reap Zombie Teller Processes (general check in main loop).
        // This ensures Tellers that exit for other reasons (e.g., client disconnect) are eventually reaped.
        int status_reap_main;
        pid_t ended_pid_main;
        while ((ended_pid_main = waitpid(-1, &status_reap_main, WNOHANG)) > 0)
        {
            // Optionally log reaped Teller PID here.
        }
        if (ended_pid_main == -1 && errno != ECHILD)
        {
            perror("Server waitpid error during main loop reap");
        }

        // 4. Brief sleep if idle to prevent busy-waiting.
        // Sleep only if no new clients arrived AND no requests were processed in this iteration.
        if (running && poll_res <= 0 && !processed_item)
        {
            int item_count;
            // Double-check queue is empty before sleeping (sem_trywait might fail for other reasons).
            if (sem_getvalue(&region->items, &item_count) == 0 && item_count == 0)
            {
                struct timespec ts = {0, 5 * 1000 * 1000}; // 5 milliseconds sleep.
                nanosleep(&ts, NULL);
            }
        }

    } // End while(running) main loop.

    // --- Server Shutdown ---
    printf("Server shutting down...\n");

    // Final non-blocking reap of any remaining zombie Tellers before cleanup.
    int status_final;
    while (waitpid(-1, &status_final, WNOHANG) > 0)
        ;

    cleanup(); // Perform resource cleanup (log summary, SHM, FIFO, semaphores).
    return 0;
}