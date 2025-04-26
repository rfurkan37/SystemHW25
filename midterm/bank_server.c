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

static shm_region_t *region = NULL;
static int shm_fd = -1;
static int running = 1;

static void sigint_handler(int signo __attribute__((unused)))
{
    running = 0;
}

static void cleanup()
{
    if (region)
    {
        munmap(region, sizeof(shm_region_t));
    }
    if (shm_fd != -1)
    {
        close(shm_fd);
    }
    unlink(SERVER_FIFO);
    /* leave SHM for later runs; can shm_unlink if desired */
}


int main(void)
{
    printf("AdaBank server starting…\n");

    /* set up shared memory */
    shm_fd = shm_open(SHM_NAME, O_RDWR | O_CREAT, 0600);
    if (shm_fd == -1)
    {
        perror("shm_open");
        exit(1);
    }
    ftruncate(shm_fd, sizeof(shm_region_t));
    region = mmap(NULL, sizeof(shm_region_t), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (region == MAP_FAILED)
    {
        perror("mmap");
        exit(1);
    }

    /* init semaphores only once (best-effort) */
    sem_init(&region->slots, 1, REQ_QUEUE_LEN);
    sem_init(&region->items, 1, 0);
    sem_init(&region->qmutex, 1, 1);
    sem_init(&region->dbmutex, 1, 1);

    /* init indices if first run */
    region->head = region->tail = 0;
    region->next_id = 0;

    /* make sure server fifo exists */
    mkfifo(SERVER_FIFO, 0600);
    int server_fd = open(SERVER_FIFO, O_RDONLY | O_NONBLOCK);
    int dummy_fd __attribute__((unused)) = open(SERVER_FIFO, O_WRONLY); /* keep fifo open */
    if (server_fd == -1)
    {
        perror("open fifo");
        exit(1);
    }

    signal(SIGINT, sigint_handler);
    signal(SIGTERM, sigint_handler);

    fd_set rset;
    int maxfd = server_fd;

    while (running)
    {
        /* Check for new clients via select with small timeout */
        FD_ZERO(&rset);
        FD_SET(server_fd, &rset);
        struct timeval tv = {0, 500000}; /* 0.5s */
        int sel = select(maxfd + 1, &rset, NULL, NULL, &tv);
        if (sel > 0 && FD_ISSET(server_fd, &rset))
        {
            char buf[64];
            int n = read(server_fd, buf, sizeof(buf));
            for (int i = 0; i < n;)
            {
                /* buf may contain multiple pids separated by newline */
                pid_t pid = 0;
                while (i < n && buf[i] >= '0' && buf[i] <= '9')
                {
                    pid = pid * 10 + (buf[i] - '0');
                    i++;
                }
                /* skip non-digit */
                while (i < n && (buf[i] < '0' || buf[i] > '9'))
                    i++;
                if (pid > 0)
                {
                    printf("Spawning Teller for client %d\n", pid);
                    extern pid_t Teller(void *(*)(void *), void *);
                    extern void *teller_main(void *);
                    Teller(teller_main, (void *)(intptr_t)pid);
                }
            }
        }

        /* process bank requests */
        while (sem_trywait(&region->items) == 0)
        {
            sem_wait(&region->qmutex);
            int idx = region->head;
            request_t *req = &region->queue[idx];
            region->head = (region->head + 1) % REQ_QUEUE_LEN;
            sem_post(&region->qmutex);
            sem_post(&region->slots);

            /* operate */
            sem_wait(&region->dbmutex);
            if (req->bank_id == -1 && req->type == REQ_DEPOSIT)
            {
                int new_id = region->next_id++;
                region->balances[new_id] = req->amount;
                req->bank_id = new_id;
                req->result_balance = req->amount;
                req->status = 0;
            }
            else if (req->bank_id >= 0 && req->bank_id < MAX_ACCOUNTS)
            {
                long *bal = &region->balances[req->bank_id];
                if (req->type == REQ_DEPOSIT)
                {
                    *bal += req->amount;
                    req->result_balance = *bal;
                    req->status = 0;
                }
                else
                { /* withdraw */
                    if (*bal >= req->amount)
                    {
                        *bal -= req->amount;
                        req->result_balance = *bal;
                        req->status = 0;
                    }
                    else
                    {
                        req->result_balance = *bal;
                        req->status = 1; /* insufficient */
                    }
                }
            }
            else
            {
                req->status = 2; /* invalid id */
            }
            sem_post(&region->dbmutex);
        }
    }

    printf("Shutdown requested… cleaning up.\n");
    cleanup();
    return 0;
}
