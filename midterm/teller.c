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
#include "common.h"


static shm_region_t *attach_region() {
    int fd = shm_open(SHM_NAME, O_RDWR, 0);
    if (fd == -1) {
        perror("shm_open in teller"); exit(1);
    }
    shm_region_t *reg = mmap(NULL, sizeof(shm_region_t),
                             PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (reg == MAP_FAILED) {
        perror("mmap teller"); exit(1);
    }
    close(fd);
    return reg;
}

/* push request and return pointer to queue slot */
static request_t *push_request(shm_region_t *reg, const request_t *src) {
    sem_wait(&reg->slots);
    sem_wait(&reg->qmutex);
    int idx = reg->tail;
    reg->queue[idx] = *src;
    reg->queue[idx].status = -1;
    reg->tail = (reg->tail + 1) % REQ_QUEUE_LEN;
    sem_post(&reg->qmutex);
    sem_post(&reg->items);
    return &reg->queue[idx];
}

static void *teller_main_inner(void *arg) {
    int client_pid = (int)(intptr_t)arg;
    char req_path[64], res_path[64];
    snprintf(req_path, sizeof(req_path), "/tmp/bank_%d_req", client_pid);
    snprintf(res_path, sizeof(res_path), "/tmp/bank_%d_res", client_pid);

    int req_fd = open(req_path, O_RDONLY);
    if (req_fd == -1) { perror("teller open req"); return NULL; }
    int res_fd = open(res_path, O_WRONLY);
    if (res_fd == -1) { perror("teller open res"); close(req_fd); return NULL; }

    shm_region_t *reg = attach_region();

    char line[128];
    while (fgets(line, sizeof(line), fdopen(req_fd, "r"))) {
        request_t rq = {0};
        rq.client_pid = client_pid;
        char bank_str[16], op[16];
        long amount;
        if (sscanf(line, "%15s %15s %ld", bank_str, op, &amount) != 3) {
            dprintf(res_fd, "ERR bad format\n");
            continue;
        }
        if (strcmp(bank_str, "N") == 0 || strcmp(bank_str, "BankID_None") == 0) {
            rq.bank_id = -1;
        } else if (strncmp(bank_str, "BankID_", 7) == 0) {
            rq.bank_id = atoi(bank_str + 7);
        } else {
            rq.bank_id = atoi(bank_str);
        }
        rq.type = (strcmp(op, "deposit") == 0) ? REQ_DEPOSIT : REQ_WITHDRAW;
        rq.amount = amount;

        request_t *slot = push_request(reg, &rq);

        /* busy wait for completion */
        while (__atomic_load_n(&slot->status, __ATOMIC_SEQ_CST) == -1) {
            struct timespec ts = {0, 1000000}; /* 1 ms */
            nanosleep(&ts, NULL);
        }

        if (slot->status == 0) {
            dprintf(res_fd, "OK BankID_%d balance=%ld\n",
                    slot->bank_id, slot->result_balance);
        } else if (slot->status == 1) {
            dprintf(res_fd, "FAIL insufficient balance=%ld\n",
                    slot->result_balance);
        } else {
            dprintf(res_fd, "FAIL invalid account\n");
        }
    }

    close(req_fd); close(res_fd);
    return NULL;
}

/* Wrapper for Teller */
void *teller_main(void *arg) {
    teller_main_inner(arg);
    return NULL;
}
