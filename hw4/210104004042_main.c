#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include "buffer.h"

#define EOF_MARKER "END"
#define LINE_BUFFER_SIZE 1024

volatile int running = 1;
int num_workers;
int *match_counts;
char *search_term;
Buffer buffer;
pthread_barrier_t barrier;

void *manager(void *arg)
{
    char *file_name = (char *)arg;

    int fd = open(file_name, O_RDONLY);

    if (fd == -1)
    {
        perror("Error opening file in manager");
        running = 0;
        // Crucial: Signal workers to wake up and terminate if manager fails here
        pthread_mutex_lock(&buffer.mutex);
        pthread_cond_broadcast(&buffer.not_empty);
        pthread_mutex_unlock(&buffer.mutex);
        return NULL;
    }

    char read_buf[LINE_BUFFER_SIZE];
    char current_line[LINE_BUFFER_SIZE];
    size_t current_line_idx = 0;
    ssize_t bytes_read;

    while (running && (bytes_read = read(fd, read_buf, sizeof(read_buf))) > 0)
    {
        for (int i = 0; i < bytes_read && running; ++i)
        {
            if (read_buf[i] == '\n')
            {
                current_line[current_line_idx] = '\0';
                if (current_line_idx > 0)
                {
                    char *line_copy = strdup(current_line);
                    if (!line_copy)
                    {
                        perror("strdup failed in manager");
                        running = 0;
                        break;
                    }
                    add_to_buffer(&buffer, line_copy);
                }
                current_line_idx = 0;
            }
            else
            {
                if (current_line_idx < sizeof(current_line) - 1)
                {
                    current_line[current_line_idx++] = read_buf[i];
                }
                else
                {
                    current_line[current_line_idx] = '\0';
                    fprintf(stderr, "Manager: Line too long, truncating.\n");
                    char *line_copy = strdup(current_line);
                    if (!line_copy)
                    {
                        perror("strdup failed for long line in manager");
                        running = 0;
                        break;
                    }
                    add_to_buffer(&buffer, line_copy);
                    current_line_idx = 0;
                }
            }
        }
        if (!running)
            break;
    }

    if (running && current_line_idx > 0 && bytes_read == 0)
    {
        current_line[current_line_idx] = '\0';
        char *line_copy = strdup(current_line);
        if (!line_copy)
        {
            perror("strdup for last line failed in manager");
            running = 0;
        }
        else
        {
            add_to_buffer(&buffer, line_copy);
        }
    }

    if (bytes_read == -1 && running)
    {
        perror("Error reading from file in manager");
        running = 0;
    }
    if (close(fd) == -1)
    {
        perror("Error closing file in manager");
    }

    for (int i = 0; i < num_workers && running; i++)
    {
        char *eof_marker = strdup(EOF_MARKER);
        if (!eof_marker)
        {
            perror("strdup for EOF marker failed");
            running = 0;
            pthread_mutex_lock(&buffer.mutex);
            pthread_cond_broadcast(&buffer.not_empty);
            pthread_mutex_unlock(&buffer.mutex);
            break;
        }
        add_to_buffer(&buffer, eof_marker);
    }
    return NULL;
}

void *worker(void *arg)
{
    int id = *(int *)arg;
    int count = 0;
    while (running)
    {
        char *line = remove_from_buffer(&buffer);
        if (!line)
            break;
        if (strcmp(line, EOF_MARKER) == 0)
        {
            free(line);
            break;
        }
        if (strstr(line, search_term))
            count++;
        free(line);
    }

    match_counts[id] = count;
    printf("Worker %d found %d matches\n", id, count);

    pthread_barrier_wait(&barrier);

    if (id == 0 && running)
    {
        int total = 0;
        printf("--------------------\n");
        for (int i = 0; i < num_workers; i++)
        {
            total += match_counts[i];
        }
        printf("Total matches found: %d\n", total);
    }
    return NULL;
}

void handle_signal()
{
    running = 0;
    pthread_cond_broadcast(&buffer.not_full);
    pthread_cond_broadcast(&buffer.not_empty);
}

int main(int argc, char *argv[])
{
    if (argc != 5)
    {
        fprintf(stderr, "Usage: %s <buffer_size> <num_workers> <log_file> <search_term>\n", argv[0]);
        return 1;
    }

    int buffer_size = atoi(argv[1]);
    num_workers = atoi(argv[2]);
    char *log_file = argv[3];
    search_term = argv[4];

    if (buffer_size <= 0 || num_workers <= 0)
    {
        fprintf(stderr, "Buffer size and number of workers must be positive\n");
        return 1;
    }

    init_buffer(&buffer, buffer_size);

    match_counts = calloc(num_workers, sizeof(int));
    if (match_counts == NULL)
    {
        perror("Failed to allocate memory for match_counts");
        return 1;
    }

    int barrier_ret = pthread_barrier_init(&barrier, NULL, num_workers);
    if (barrier_ret != 0)
    {
        fprintf(stderr, "Error initializing barrier: %s\n", strerror(barrier_ret));
        free(match_counts);
        return 1;
    }

    struct sigaction sa;
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGINT, &sa, NULL) == -1)
    {
        perror("Error setting up SIGINT handler");
        free(match_counts);
        pthread_barrier_destroy(&barrier);
        return 1;
    }

    pthread_t manager_thread;
    int pt_ret = pthread_create(&manager_thread, NULL, manager, log_file);
    if (pt_ret != 0)
    {
        fprintf(stderr, "Error creating manager thread: %s\n", strerror(pt_ret));
        free(match_counts);
        pthread_barrier_destroy(&barrier);
        return 1;
    }

    pthread_t *workers = malloc(num_workers * sizeof(pthread_t));
    if (workers == NULL)
    {
        perror("Failed to allocate memory for workers array");
        running = 0;
        pthread_join(manager_thread, NULL);
        free(match_counts);
        pthread_barrier_destroy(&barrier);
        return 1;
    }

    int *ids = malloc(num_workers * sizeof(int));
    if (ids == NULL)
    {
        perror("Failed to allocate memory for ids array");
        running = 0;
        free(workers);
        pthread_join(manager_thread, NULL);
        free(match_counts);
        pthread_barrier_destroy(&barrier);
        return 1;
    }

    for (int i = 0; i < num_workers; i++)
    {
        ids[i] = i;
        pt_ret = pthread_create(&workers[i], NULL, worker, &ids[i]);
        if (pt_ret != 0)
        {
            fprintf(stderr, "Error creating worker thread %d: %s\n", i, strerror(pt_ret));
            running = 0;
            pthread_cond_broadcast(&buffer.not_full);
            pthread_cond_broadcast(&buffer.not_empty);
            pthread_join(manager_thread, NULL);
            for (int j = 0; j < i; ++j)
            {
                pthread_join(workers[j], NULL);
            }
            free(ids);
            free(workers);
            free(match_counts);
            pthread_barrier_destroy(&barrier);
            return 1;
        }
    }

    pthread_join(manager_thread, NULL);
    for (int i = 0; i < num_workers; i++)
    {
        pthread_join(workers[i], NULL);
    }

    free_buffer(&buffer);
    free(match_counts);
    free(workers);
    free(ids);
    pthread_barrier_destroy(&barrier);

    return 0;
}