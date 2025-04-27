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
#include <limits.h> // For INT_MAX
#include <stdint.h> // For intptr_t

#include "common.h"

// --- (Keep existing static vars, enums, functions - load_state, log_transaction etc.) ---
static shm_region_t *region = NULL;
static int shm_fd = -1;
static int server_fd = -1;
static int dummy_fd = -1;
static const char *server_fifo_path = NULL;
static volatile sig_atomic_t running = 1;
static int teller_spawn_counter = 0;

typedef enum
{
    LOG_CREATE,
    LOG_DEPOSIT,
    LOG_WITHDRAW,
    LOG_CLOSE,
    LOG_ERROR
} log_event_type_t;

static void log_transaction(log_event_type_t type, int id, long amount, long balance)
{ /* ... Keep implementation ... */
    FILE *log_fp = fopen(LOG_FILE_NAME, "a");
    if (!log_fp)
    {
        perror("SERVER ERROR: Log append failed");
        return;
    }
    switch (type)
    {
    case LOG_CREATE:
        fprintf(log_fp, "CREATE %d %ld\n", id, amount);
        break;
    case LOG_DEPOSIT:
        fprintf(log_fp, "DEPOSIT %d %ld %ld\n", id, amount, balance);
        break;
    case LOG_WITHDRAW:
        fprintf(log_fp, "WITHDRAW %d %ld %ld\n", id, amount, balance);
        break;
    case LOG_CLOSE:
        fprintf(log_fp, "CLOSE %d\n", id);
        break;
    default:
        fprintf(log_fp, "UNKNOWN %d %ld %ld\n", id, amount, balance);
        break;
    }
    fflush(log_fp);
    fsync(fileno(log_fp));
    fclose(log_fp);
}
static void load_state_from_log()
{ /* ... Keep implementation ... */
    for (int i = 0; i < MAX_ACCOUNTS; ++i)
        region->balances[i] = ACCOUNT_INACTIVE;
    region->next_id = 0;
    FILE *log_fp = fopen(LOG_FILE_NAME, "r");
    if (!log_fp)
    {
        if (errno == ENOENT)
        {
            printf("No previous logs.. Creating the bank database\n");
            FILE *create_log = fopen(LOG_FILE_NAME, "w");
            if (create_log)
            {
                time_t now = time(NULL);
                struct tm *tm_info = localtime(&now);
                char time_buf[100];
                strftime(time_buf, sizeof(time_buf), "%B %d %Y %H:%M", tm_info);
                fprintf(create_log, "# Adabank Log file updated @%s\n", time_buf);
                fclose(create_log);
            }
            else
                perror("SERVER WARNING: Cannot create log file");
        }
        else
            perror("SERVER ERROR: Cannot read log file");
        return;
    }
    char line[256];
    int max_id_found = -1;
    int line_num = 0;
    while (fgets(line, sizeof(line), log_fp))
    {
        line_num++;
        line[strcspn(line, "\n\r")] = 0;
        if (line[0] == '#' || line[0] == '\0')
            continue;
        int id;
        long amount = 0, balance_from_log = 0;
        log_event_type_t current_type = LOG_ERROR;
        if (sscanf(line, "CREATE %d %ld", &id, &amount) == 2)
            current_type = LOG_CREATE;
        else if (sscanf(line, "DEPOSIT %d %ld %ld", &id, &amount, &balance_from_log) == 3)
            current_type = LOG_DEPOSIT;
        else if (sscanf(line, "WITHDRAW %d %ld %ld", &id, &amount, &balance_from_log) == 3)
            current_type = LOG_WITHDRAW;
        else if (sscanf(line, "CLOSE %d", &id) == 1)
            current_type = LOG_CLOSE;
        else
            continue;
        if (id < 0 || id >= MAX_ACCOUNTS)
            continue;
        switch (current_type)
        {
        case LOG_CREATE:
            region->balances[id] = amount;
            if (id > max_id_found)
                max_id_found = id;
            break;
        case LOG_DEPOSIT:
            region->balances[id] = balance_from_log;
            if (id > max_id_found)
                max_id_found = id;
            break;
        case LOG_WITHDRAW:
            region->balances[id] = balance_from_log;
            if (id > max_id_found)
                max_id_found = id;
            break;
        case LOG_CLOSE:
            region->balances[id] = ACCOUNT_INACTIVE;
            break;
        case LOG_ERROR:
            break;
        }
    }
    fclose(log_fp);
    region->next_id = (max_id_found == -1) ? 0 : (max_id_found + 1);
    if (region->next_id >= MAX_ACCOUNTS)
        region->next_id = 0;
}
static int find_free_account_id()
{ /* ... Keep implementation ... */
    int start_idx = region->next_id, current_idx = start_idx;
    do
    {
        if (region->balances[current_idx] == ACCOUNT_INACTIVE)
        {
            region->next_id = (current_idx + 1) % MAX_ACCOUNTS;
            return current_idx;
        }
        current_idx = (current_idx + 1) % MAX_ACCOUNTS;
    } while (current_idx != start_idx);
    return -1;
}
static void sigint_handler(int signo)
{
    (void)signo;
    const char msg[] = "\nSignal received closing active Tellers\n";
    write(STDOUT_FILENO, msg, sizeof(msg) - 1);
    running = 0;
}
static void cleanup()
{ /* ... Keep implementation ... */
    if (server_fd != -1)
        close(server_fd);
    if (dummy_fd != -1)
        close(dummy_fd);
    server_fd = dummy_fd = -1;
    if (region != NULL && region != MAP_FAILED)
    {
        sem_destroy(&region->slots);
        sem_destroy(&region->items);
        sem_destroy(&region->qmutex);
        sem_destroy(&region->dbmutex);
        for (int i = 0; i < REQ_QUEUE_LEN; ++i)
            sem_destroy(&region->resp_ready[i]);
    }
    if (region != NULL && region != MAP_FAILED)
    {
        munmap(region, sizeof(shm_region_t));
        region = NULL;
    }
    if (shm_fd != -1)
    {
        close(shm_fd);
        shm_fd = -1;
        shm_unlink(SHM_NAME);
    }
    if (server_fifo_path)
    {
        printf("Removing ServerFIFO... ");
        fflush(stdout);
        unlink(server_fifo_path);
    }
    printf("Updating log file... ");
    fflush(stdout);
    FILE *log_fp = fopen(LOG_FILE_NAME, "a");
    if (log_fp)
    {
        fprintf(log_fp, "## end of log.\n");
        fclose(log_fp);
    }
    else
        perror("WARN: log footer");
    printf("\n");
    printf("Adabank says \"Bye\"...\n");
}
static void process_deposit(request_t *req, int slot_idx)
{ /* ... Keep implementation ... */
    int op_status = 2;
    long current_balance = 0;
    int account_id = req->bank_id;
    if (req->bank_id == -1)
    {
        int new_id = find_free_account_id();
        if (new_id != -1)
        {
            account_id = new_id;
            region->balances[account_id] = req->amount;
            current_balance = req->amount;
            op_status = 0;
            log_transaction(LOG_CREATE, account_id, current_balance, 0);
            printf("Client%d deposited %ld credits... updating log\n", req->client_pid, req->amount);
        }
        else
        {
            op_status = 2;
            current_balance = 0;
            account_id = -1;
            printf("Client%d create failed - bank full... operation not permitted.\n", req->client_pid);
        }
    }
    else if (req->bank_id >= 0 && req->bank_id < MAX_ACCOUNTS && region->balances[req->bank_id] != ACCOUNT_INACTIVE)
    {
        account_id = req->bank_id;
        long *bal_ptr = &region->balances[account_id];
        if (__builtin_add_overflow(*bal_ptr, req->amount, bal_ptr))
        {
            op_status = 2;
            current_balance = region->balances[account_id];
            printf("Client%d deposit %ld failed... operation not permitted.\n", req->client_pid, req->amount);
        }
        else
        {
            current_balance = *bal_ptr;
            op_status = 0;
            log_transaction(LOG_DEPOSIT, account_id, req->amount, current_balance);
            printf("Client%d deposited %ld credits... updating log\n", req->client_pid, req->amount);
        }
    }
    else
    {
        op_status = 2;
        current_balance = 0;
        printf("Client%d deposit %ld failed (BankID_%d)... operation not permitted.\n", req->client_pid, req->amount, account_id);
    }
    region->queue[slot_idx].bank_id = account_id;
    region->queue[slot_idx].result_balance = current_balance;
    region->queue[slot_idx].op_status = op_status;
    sem_post(&region->resp_ready[slot_idx]);
}
static void process_withdraw(request_t *req, int slot_idx)
{ /* ... Keep implementation ... */
    int op_status = 2;
    long current_balance = 0;
    int account_id = req->bank_id;
    if (account_id >= 0 && account_id < MAX_ACCOUNTS && region->balances[account_id] != ACCOUNT_INACTIVE)
    {
        long *bal_ptr = &region->balances[account_id];
        if (*bal_ptr >= req->amount)
        {
            *bal_ptr -= req->amount;
            current_balance = *bal_ptr;
            op_status = 0;
            log_transaction(LOG_WITHDRAW, account_id, req->amount, current_balance);
            printf("Client%d withdraws %ld credits... updating log", req->client_pid, req->amount);
            if (current_balance == 0)
            {
                *bal_ptr = ACCOUNT_INACTIVE;
                log_transaction(LOG_CLOSE, account_id, 0, 0);
                printf("... Bye Client%d\n", req->client_pid);
            }
            else
                printf("\n");
        }
        else
        {
            current_balance = *bal_ptr;
            op_status = 1;
            printf("Client%d withdraws %ld credit.. operation not permitted.\n", req->client_pid, req->amount);
        }
    }
    else
    {
        op_status = 2;
        current_balance = 0;
        printf("Client%d withdraws %ld failed (BankID_%d)... operation not permitted.\n", req->client_pid, req->amount, account_id);
    }
    region->queue[slot_idx].bank_id = account_id;
    region->queue[slot_idx].result_balance = current_balance;
    region->queue[slot_idx].op_status = op_status;
    sem_post(&region->resp_ready[slot_idx]);
}
pid_t Teller(void *func, void *arg_func)
{ /* ... Keep implementation ... */
    pid_t pid = fork();
    if (pid == -1)
    {
        perror("Teller (fork)");
        return -1;
    }
    else if (pid == 0)
    {
        teller_main_func_t fn = (teller_main_func_t)func;
        fn(arg_func);
        _exit(EXIT_SUCCESS);
    }
    else
    {
        teller_spawn_counter++;
        return pid;
    }
}
int waitTeller(pid_t pid, int *status) { return waitpid(pid, status, 0); }

// --- Main Server Function ---
int main(int argc, char *argv[])
{
    if (argc != 2)
    {
        fprintf(stderr, "Usage: %s <ServerFIFO_Name>\n", basename(argv[0]));
        exit(EXIT_FAILURE);
    }
    server_fifo_path = argv[1];
    printf("BankServer %s\n", server_fifo_path);

    // --- (Signal handling, SHM setup, Sem init, Log replay - Keep as before) ---
    struct sigaction sa_int_term;
    memset(&sa_int_term, 0, sizeof(sa_int_term));
    sa_int_term.sa_handler = sigint_handler;
    sa_int_term.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sa_int_term, NULL);
    sigaction(SIGTERM, &sa_int_term, NULL);
    signal(SIGPIPE, SIG_IGN);
    shm_unlink(SHM_NAME);
    shm_fd = shm_open(SHM_NAME, O_RDWR | O_CREAT | O_EXCL, 0600);
    int shm_existed = 0;
    if (shm_fd == -1)
    {
        if (errno == EEXIST)
        {
            shm_existed = 1;
            shm_fd = shm_open(SHM_NAME, O_RDWR, 0600);
            if (shm_fd == -1)
            {
                perror("FATAL: open existing SHM");
                exit(EXIT_FAILURE);
            }
        }
        else
        {
            perror("FATAL: shm_open");
            exit(EXIT_FAILURE);
        }
    }
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
            shm_unlink(SHM_NAME);
        exit(EXIT_FAILURE);
    }
    if (!shm_existed)
    {
        int ok = 1;
        if (sem_init(&region->qmutex, 1, 1) == -1)
            ok = 0;
        if (sem_init(&region->slots, 1, REQ_QUEUE_LEN) == -1)
            ok = 0;
        if (sem_init(&region->items, 1, 0) == -1)
            ok = 0;
        if (sem_init(&region->dbmutex, 1, 1) == -1)
            ok = 0;
        for (int i = 0; i < REQ_QUEUE_LEN; ++i)
            if (sem_init(&region->resp_ready[i], 1, 0) == -1)
                ok = 0;
        if (!ok)
        {
            fprintf(stderr, "FATAL: sem_init\n");
            cleanup();
            exit(EXIT_FAILURE);
        }
        region->head = region->tail = 0;
        sem_wait(&region->dbmutex);
        load_state_from_log();
        sem_post(&region->dbmutex);
    }
    else
    {
        int v;
        if (sem_getvalue(&region->dbmutex, &v) == -1 && errno == EINVAL)
        {
            fprintf(stderr, "FATAL: invalid sem\n");
            cleanup();
            exit(EXIT_FAILURE);
        }
        struct timespec t;
        clock_gettime(CLOCK_REALTIME, &t);
        t.tv_sec += 5;
        if (sem_timedwait(&region->dbmutex, &t) == -1)
        {
            perror("FATAL: timedwait dbmutex");
            cleanup();
            exit(EXIT_FAILURE);
        }
        load_state_from_log();
        sem_post(&region->dbmutex);
    }
    unlink(server_fifo_path);
    if (mkfifo(server_fifo_path, 0600) == -1)
    {
        perror("FATAL: mkfifo server");
        cleanup();
        exit(EXIT_FAILURE);
    }
    server_fd = open(server_fifo_path, O_RDONLY | O_NONBLOCK);
    if (server_fd == -1)
    {
        perror("FATAL: open server read");
        cleanup();
        exit(EXIT_FAILURE);
    }
    dummy_fd = open(server_fifo_path, O_WRONLY);
    if (dummy_fd == -1)
    {
        perror("FATAL: open server write");
        close(server_fd);
        server_fd = -1;
        cleanup();
        exit(EXIT_FAILURE);
    }

    printf("Adabank is active….\n");
    printf("Waiting for clients @%s…\n", server_fifo_path);

// Temporary storage for batch connection messages
#define MAX_BATCH 32
    // REMOVED: pid_t batch_teller_pids[MAX_BATCH]; // This was unused
    pid_t batch_client_pids[MAX_BATCH];
    int batch_teller_spawn_counters[MAX_BATCH];

    while (running)
    {
        int clients_in_batch = 0;
        pid_t first_client_pid_in_batch = -1;

        // 1. Check for new client connections
        struct pollfd fds[1];
        fds[0].fd = server_fd;
        fds[0].events = POLLIN;
        int poll_res = poll(fds, 1, 500);

        if (poll_res > 0 && (fds[0].revents & POLLIN))
        {
            char buf[1024];
            ssize_t n = read(server_fd, buf, sizeof(buf) - 1);
            if (n > 0)
            {
                buf[n] = '\0';
                char *ptr = buf;
                char *next_pid_str;
                char *saveptr;
                while (clients_in_batch < MAX_BATCH && (next_pid_str = strtok_r(ptr, "\n", &saveptr)))
                {
                    if (strlen(next_pid_str) == 0)
                    {
                        ptr = NULL;
                        continue;
                    }
                    char *endptr;
                    long client_pid_long = strtol(next_pid_str, &endptr, 10);
                    if (*endptr != '\0' || client_pid_long <= 0 || client_pid_long > INT_MAX)
                    {
                        ptr = NULL;
                        continue;
                    }
                    pid_t current_client_pid = (pid_t)client_pid_long;

                    if (first_client_pid_in_batch == -1)
                        first_client_pid_in_batch = current_client_pid;

                    extern void *teller_main(void *);
                    void *teller_arg = (void *)(intptr_t)current_client_pid;
                    pid_t current_teller_pid = Teller(teller_main, teller_arg);

                    if (current_teller_pid != -1)
                    {
                        // Store info needed for printing
                        batch_client_pids[clients_in_batch] = current_client_pid;
                        batch_teller_spawn_counters[clients_in_batch] = teller_spawn_counter;
                        clients_in_batch++;
                    }
                    else
                    {
                        perror("  Server ERROR: Failed to spawn Teller process (fork failed)");
                    }
                    ptr = NULL;
                }

                // Print connection messages AFTER loop
                if (clients_in_batch > 0)
                {
                    printf("- Received %d clients from PIDClient%d..\n",
                           clients_in_batch, first_client_pid_in_batch);
                    for (int i = 0; i < clients_in_batch; ++i)
                    {
                        printf("-- Teller PID%02d is active serving Client%d…\n",
                               batch_teller_spawn_counters[i], batch_client_pids[i]);
                    }
                    printf("Waiting for clients @%s…\n", server_fifo_path);
                }
            }
            else if (n == 0)
            { /* Handle EOF */
            }
            else if (errno != EAGAIN && errno != EWOULDBLOCK)
            {
                perror("Server ERROR reading FIFO");
                running = 0;
            }
        }
        else if (poll_res == -1 && errno != EINTR)
        {
            perror("Server ERROR: poll");
            running = 0;
        }

        // 2. Process pending requests (Keep as before)
        while (sem_trywait(&region->items) == 0)
        {
            if (!running)
            {
                sem_post(&region->items);
                break;
            }
            sem_wait(&region->qmutex);
            int idx = region->head;
            request_t req = region->queue[idx];
            region->head = (region->head + 1) % REQ_QUEUE_LEN;
            sem_post(&region->qmutex);
            sem_post(&region->slots);
            sem_wait(&region->dbmutex);
            if (req.type == REQ_DEPOSIT)
                process_deposit(&req, idx);
            else
                process_withdraw(&req, idx);
            sem_post(&region->dbmutex);
        }

        // 3. Brief sleep if idle (Keep as before)
        if (running && poll_res <= 0)
        {
            int v;
            if (sem_getvalue(&region->items, &v) == 0 && v == 0)
            {
                struct timespec ts = {0, 10000000};
                nanosleep(&ts, NULL);
            }
        }

    } // end while(running)

    // --- Shutdown ---
    int status;
    while (waitpid(-1, &status, WNOHANG) > 0)
        ;
    cleanup();
    return 0;
}