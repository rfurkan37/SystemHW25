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
#include <stdint.h> // For intptr_t
#include <sys/types.h>
#include <limits.h>
#include <ctype.h>
#include <signal.h>

#include "common.h"

static shm_region_t *region = NULL;
static volatile sig_atomic_t teller_running = 1;

// --- (Keep sig_handler, attach_shm, detach_shm, push_request, parse_bank_id ) ---
static void teller_sig_handler(int signo)
{
    (void)signo;
    teller_running = 0;
}
static int attach_shm()
{ /* ... Keep implementation ... */
    int fd = shm_open(SHM_NAME, O_RDWR, 0);
    if (fd == -1)
    {
        fprintf(stderr, "Teller(PID%d): Failed SHM open '%s': %s\n", getpid(), SHM_NAME, strerror(errno));
        return -1;
    }
    region = mmap(NULL, sizeof(shm_region_t), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (region == MAP_FAILED)
    {
        perror("Teller mmap failed");
        region = NULL;
        return -1;
    }
    return 0;
}
static void detach_shm()
{
    if (region != NULL && region != MAP_FAILED)
    {
        munmap(region, sizeof(shm_region_t));
        region = NULL;
    }
}
static int push_request(const request_t *src)
{ /* ... Keep implementation ... */
    if (!region)
        return -1;
    int idx = -1;
    while (sem_wait(&region->slots) == -1)
    {
        if (errno == EINTR && teller_running)
            continue;
        if (!teller_running)
            return -1;
        perror("Teller sem_wait(slots)");
        return -1;
    }
    while (sem_wait(&region->qmutex) == -1)
    {
        if (errno == EINTR && teller_running)
            continue;
        if (!teller_running)
        {
            sem_post(&region->slots);
            return -1;
        }
        perror("Teller sem_wait(qmutex)");
        sem_post(&region->slots);
        return -1;
    }
    idx = region->tail;
    region->queue[idx] = *src;
    region->tail = (region->tail + 1) % REQ_QUEUE_LEN;
    sem_post(&region->qmutex);
    sem_post(&region->items);
    return idx;
}
static int parse_bank_id(const char *bank_id_str)
{ /* ... Keep implementation ... */
    int bank_id = -2;
    if (strcmp(bank_id_str, "N") == 0 || strcmp(bank_id_str, "BankID_None") == 0)
        bank_id = -1;
    else if (strncmp(bank_id_str, "BankID_", 7) == 0)
    {
        char *e;
        long v = strtol(bank_id_str + 7, &e, 10);
        if (*e == '\0' && v >= 0 && v < MAX_ACCOUNTS)
            bank_id = (int)v;
    }
    else
    {
        char *e;
        long v = strtol(bank_id_str, &e, 10);
        int ok = (*e == '\0');
        for (const char *c = bank_id_str; *c && ok; ++c)
            if (!isdigit((unsigned char)*c))
                ok = 0;
        if (ok && v >= 0 && v < MAX_ACCOUNTS)
            bank_id = (int)v;
    }
    return bank_id;
}

// --- Main Teller Logic ---
static void *teller_main_inner(void *arg)
{
    pid_t client_pid = (pid_t)(intptr_t)arg;
    pid_t teller_pid = getpid();
    char req_path[128], res_path[128];
    int req_fd = -1, res_fd = -1;
    FILE *req_fp = NULL;
    char first_line_buffer[128] = {0};
    int processed_first_line = 0;
    // REMOVED: int welcome_message_printed = 0;

    snprintf(req_path, sizeof(req_path), "/tmp/bank_%d_req", client_pid);
    snprintf(res_path, sizeof(res_path), "/tmp/bank_%d_res", client_pid);

    // --- (Signal setup, SHM attach, FIFO open - Keep as before) ---
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = teller_sig_handler;
    sa.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
    if (attach_shm() != 0)
        return NULL;
    const int MAX_ATTEMPTS = 15;
    const useconds_t RETRY_DELAY_US = 100000;
    for (int i = 0; i < MAX_ATTEMPTS && teller_running; ++i)
    {
        req_fd = open(req_path, O_RDONLY);
        if (req_fd != -1)
            break;
        if (errno == ENOENT)
            usleep(RETRY_DELAY_US);
        else
        {
            perror("Teller open req FIFO");
            break;
        }
    }
    if (req_fd == -1)
    {
        detach_shm();
        return NULL;
    }
    for (int i = 0; i < MAX_ATTEMPTS && teller_running; ++i)
    {
        res_fd = open(res_path, O_WRONLY);
        if (res_fd != -1)
            break;
        if (errno == ENOENT)
            usleep(RETRY_DELAY_US);
        else
        {
            perror("Teller open res FIFO");
            break;
        }
    }
    if (res_fd == -1)
    {
        close(req_fd);
        detach_shm();
        return NULL;
    }
    req_fp = fdopen(req_fd, "r");
    if (!req_fp)
    {
        perror("Teller fdopen");
        close(req_fd);
        close(res_fd);
        detach_shm();
        return NULL;
    }

    // --- Welcome Back Logic ---
    if (teller_running && fgets(first_line_buffer, sizeof(first_line_buffer), req_fp) != NULL)
    {
        first_line_buffer[strcspn(first_line_buffer, "\n\r")] = 0;
        if (strlen(first_line_buffer) > 0 && first_line_buffer[0] != '#')
        {
            char b_id_str[64], op_str[32], am_str[32];
            if (sscanf(first_line_buffer, "%63s %31s %31s", b_id_str, op_str, am_str) == 3)
            {
                int first_cmd_bank_id = parse_bank_id(b_id_str);
                if (first_cmd_bank_id >= 0 && region->balances[first_cmd_bank_id] != ACCOUNT_INACTIVE)
                {
                    // Print welcome message from teller process (using actual teller PID)
                    printf("-- Teller PID%d is active serving Client%d…Welcome back Client%d\n",
                           teller_pid, client_pid, client_pid);
                    // welcome_message_printed = 1; // No longer needed
                }
            }
        }
    }
    else
    {
        if (teller_running)
            fprintf(stderr, "Teller(PID%d): Error reading first cmd\n", teller_pid);
        fclose(req_fp);
        close(res_fd);
        detach_shm();
        return NULL;
    }

    // --- Main Processing Loop ---
    char line[128];
    while (teller_running)
    {
        if (!processed_first_line)
        {
            strncpy(line, first_line_buffer, sizeof(line) - 1);
            line[sizeof(line) - 1] = '\0';
            processed_first_line = 1;
        }
        else
        {
            if (fgets(line, sizeof(line), req_fp) == NULL)
                break;
            line[strcspn(line, "\n\r")] = 0;
        }

        if (strlen(line) == 0 || line[0] == '#')
            continue;

        // --- (Parse Request: Keep as before) ---
        request_t rq = {0};
        rq.client_pid = client_pid;
        char bank_id_str[64], op_str[32], am_str[32];
        long parsed_amount;
        if (sscanf(line, "%63s %31s %31s", bank_id_str, op_str, am_str) != 3)
        {
            if (dprintf(res_fd, "Client%d something went WRONG\n", client_pid) < 0 && errno == EPIPE)
                teller_running = 0;
            continue;
        }
        rq.bank_id = parse_bank_id(bank_id_str);
        if (rq.bank_id == -2)
        {
            if (dprintf(res_fd, "Client%d something went WRONG\n", client_pid) < 0 && errno == EPIPE)
                teller_running = 0;
            continue;
        }
        if (strcmp(op_str, "deposit") == 0)
            rq.type = REQ_DEPOSIT;
        else if (strcmp(op_str, "withdraw") == 0)
            rq.type = REQ_WITHDRAW;
        else
        {
            if (dprintf(res_fd, "Client%d something went WRONG\n", client_pid) < 0 && errno == EPIPE)
                teller_running = 0;
            continue;
        }
        char *e;
        parsed_amount = strtol(am_str, &e, 10);
        if (*e != '\0' || parsed_amount <= 0)
        {
            if (dprintf(res_fd, "Client%d something went WRONG\n", client_pid) < 0 && errno == EPIPE)
                teller_running = 0;
            continue;
        }
        rq.amount = parsed_amount;
        if (rq.bank_id == -1 && rq.type == REQ_WITHDRAW)
        {
            if (dprintf(res_fd, "Client%d something went WRONG\n", client_pid) < 0 && errno == EPIPE)
                teller_running = 0;
            continue;
        }
        if (!teller_running)
            break;

        // --- (Submit Request: Keep as before) ---
        int slot_idx = push_request(&rq);
        if (slot_idx == -1)
        {
            if (!teller_running)
                break;
            if (dprintf(res_fd, "Client%d something went WRONG\n", client_pid) < 0 && errno == EPIPE)
                teller_running = 0;
            continue;
        }

        // --- (Wait for Response: Keep as before) ---
        int wait_ret = sem_wait(&region->resp_ready[slot_idx]);
        if (wait_ret == -1)
        {
            if (errno == EINTR && !teller_running)
                break;
            else
            {
                perror("Teller sem_wait");
                if (dprintf(res_fd, "Client%d something went WRONG\n", client_pid) < 0 && errno == EPIPE)
                    teller_running = 0;
                break;
            }
        }

        // --- (Read Results: Keep as before) ---
        long res_bal = region->queue[slot_idx].result_balance;
        int res_id = region->queue[slot_idx].bank_id;
        int status = region->queue[slot_idx].op_status;

        // --- (Format Response: Keep as before) ---
        int dprintf_ret = -1;
        switch (status)
        {
        case 0:
            if (rq.bank_id == -1)
                dprintf_ret = dprintf(res_fd, "Client%d served.. BankID_%d\n", client_pid, res_id);
            else if (rq.type == REQ_WITHDRAW && res_bal == 0)
                dprintf_ret = dprintf(res_fd, "Client%d served.. account closed\n", client_pid);
            else
                dprintf_ret = dprintf(res_fd, "Client%d served.. BankID_%d\n", client_pid, res_id);
            break;
        case 1:
        case 2:
        default:
            dprintf_ret = dprintf(res_fd, "Client%d something went WRONG\n", client_pid);
            break;
        }
        if (dprintf_ret < 0 && errno == EPIPE)
            teller_running = 0;
        else if (dprintf_ret < 0)
        {
            perror("Teller write response");
            teller_running = 0;
        }

    } // End while

    // --- (Cleanup: Keep as before) ---
    if (req_fp)
        fclose(req_fp);
    else if (req_fd != -1)
        close(req_fd);
    if (res_fd != -1)
        close(res_fd);
    detach_shm();
    return NULL;
}

void *teller_main(void *arg)
{
    teller_main_inner(arg);
    return NULL;
}