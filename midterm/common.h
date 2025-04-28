#ifndef BANKSIM_COMMON_H
#define BANKSIM_COMMON_H

#include <semaphore.h>
#include <stdint.h>
#include <sys/types.h> // For pid_t

// --- Constants ---
#define MAX_ACCOUNTS 1024                  // Maximum number of bank accounts supported.
#define REQ_QUEUE_LEN 64                   // Size of the shared request queue.
#define SHM_NAME "/adabank_shm"            // Name for the POSIX shared memory segment.
#define DEFAULT_SERVER_FIFO_NAME "AdaBank" // Default name for the main server FIFO.
#define LOG_FILE_NAME "AdaBank.bankLog"    // Name of the transaction log file.
#define ACCOUNT_INACTIVE -1                // Indicates an account slot is not currently in use.

// --- Request Type Enum ---
typedef enum
{
    REQ_DEPOSIT = 0,
    REQ_WITHDRAW = 1
} req_type_t;

// --- Request Structure (used in SHM Queue) ---
typedef struct
{
    // --- Client Request Fields ---
    pid_t client_pid; // Originating client PID (for Teller identification).
    int bank_id;      // Target account ID; -1 signifies a request to create a new account.
    req_type_t type;  // Type of operation: REQ_DEPOSIT or REQ_WITHDRAW.
    long amount;      // Amount to deposit or withdraw (must be positive).

    // --- Server Response Fields (updated in place) ---
    long result_balance; // Balance after the operation completes (or relevant value on error).
    // bank_id is updated by the server if a new account was created (on deposit with bank_id = -1).
    int op_status; // Result code: 0 = OK, 1 = insufficient funds, 2 = other error (e.g., invalid ID, overflow, bank full).

} request_t;

// --- Shared Memory Region Layout ---
typedef struct
{
    // Request Queue (Circular Buffer)
    request_t queue[REQ_QUEUE_LEN];
    int head; // Index of the next request to be processed by the server.
    int tail; // Index where the next request will be placed by a Teller.

    // Queue Semaphores
    sem_t slots;  // Counts available empty slots in the queue. Tellers wait on this before pushing.
    sem_t items;  // Counts available requests in the queue. Server waits on this before processing.
    sem_t qmutex; // Mutex protecting access to queue 'head' and 'tail' indices.

    // Database Semaphores & Data
    sem_t logmutex;               // Mutex protecting access to the log file.
    sem_t dbmutex;               // Mutex protecting access to the 'balances' array and 'next_id'.
    long balances[MAX_ACCOUNTS]; // Array storing account balances; ACCOUNT_INACTIVE indicates unused slot.
    int next_id;                 // Hint for the server when searching for the next free account ID.

    // Response Semaphores
    sem_t resp_ready[REQ_QUEUE_LEN]; // Array of semaphores; server posts resp_ready[i] when request in queue[i] is processed. Teller waits on the corresponding semaphore.

} shm_region_t;

// --- Teller Function Type ---
// Signature for the function executed by a Teller process.
typedef void *(*teller_main_func_t)(void *);

// --- API Functions (Provided) ---
// Function to fork a new Teller process.
pid_t Teller(void *func, void *arg_func);
// Function to wait for a specific Teller process to terminate.
int waitTeller(pid_t pid, int *status);

#endif // BANKSIM_COMMON_H