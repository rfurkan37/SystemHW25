#ifndef BANKSIM_COMMON_H
#define BANKSIM_COMMON_H

#include <semaphore.h>
#include <stdint.h>
#include <sys/types.h> // For pid_t

#define MAX_ACCOUNTS 1024
#define REQ_QUEUE_LEN 64
#define SHM_NAME "/adabank_shm"
#define DEFAULT_SERVER_FIFO_NAME "AdaBank"
#define LOG_FILE_NAME "AdaBank.bankLog"
#define ACCOUNT_INACTIVE -1

typedef enum
{
    REQ_DEPOSIT = 0,
    REQ_WITHDRAW = 1
} req_type_t;

typedef struct
{
    int client_pid;      /* originating client PID */
    int bank_id;         /* -1 for new account, or the target account ID */
    req_type_t type;     /* REQ_DEPOSIT or REQ_WITHDRAW */
    long amount;         /* deposit / withdraw amount */

    // --- Results filled by server ---
    long result_balance; /* balance after op */
    // bank_id is potentially updated by server (e.g., for CREATE)
    int op_status;       /* 0 = ok, 1 = insuff funds, 2 = err (filled by server) */

} request_t;

typedef struct
{
    request_t queue[REQ_QUEUE_LEN];
    int head;
    int tail;
    sem_t slots;                 /* empty slots in queue */
    sem_t items;                 /* filled slots in queue */
    sem_t qmutex;                /* protect head/tail */
    sem_t dbmutex;               /* protect account table */
    sem_t resp_ready[REQ_QUEUE_LEN]; /* Signal completion for each slot */
    long balances[MAX_ACCOUNTS]; // balance or -1 for inactive
    int next_id;                 // Hint for finding next free account ID
} shm_region_t;

// Teller function type
typedef void *(*teller_main_func_t)(void *);

// API specified in the PDF
pid_t Teller(void* func, void* arg_func);
int waitTeller(pid_t pid, int* status);

#endif /* BANKSIM_COMMON_H */