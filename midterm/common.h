#ifndef BANKSIM_COMMON_H
#define BANKSIM_COMMON_H

#include <semaphore.h>
#include <stdint.h>

#define MAX_ACCOUNTS 1024
#define REQ_QUEUE_LEN 64
#define SHM_NAME "/adabank_shm"
// #define SERVER_FIFO "/tmp/adabank_fifo" // Removed: Name/path is now dynamic (CLI arg for server)
#define DEFAULT_SERVER_FIFO_NAME "AdaBank" // Default name if server arg omitted, used by client.
#define LOG_FILE_NAME "AdaBank.bankLog"
#define ACCOUNT_INACTIVE -1

typedef enum
{
    REQ_DEPOSIT = 0,
    REQ_WITHDRAW = 1
} req_type_t;

typedef struct
{
    int client_pid; /* originating client PID */
    int bank_id;    /* -1 for new account */
    req_type_t type;
    long amount;         /* deposit / withdraw amount */
    long result_balance; /* balance after op (filled by server) */
    int status;          /* -1 = pending, 0 = ok, 1 = insuff funds, 2 = err */
} request_t;

typedef struct
{
    request_t queue[REQ_QUEUE_LEN];
    int head;
    int tail;
    sem_t slots;                 /* empty slots */
    sem_t items;                 /* filled slots */
    sem_t qmutex;                /* protect head/tail */
    sem_t dbmutex;               /* protect account table */
    long balances[MAX_ACCOUNTS]; // balance or -1 for inactive
    int next_id;
} shm_region_t;

// Forward declaration for Teller function signature
typedef void *(*teller_main_func_t)(void *);
pid_t Teller(teller_main_func_t func, void *arg, void *stack, size_t stack_size);


#endif /* BANKSIM_COMMON_H */