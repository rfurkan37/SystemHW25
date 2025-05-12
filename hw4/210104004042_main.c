/**
 * File: 210104004042_main.c
 * Assignment: HW4 - Producer-Consumer Problem with pthread
 * Description: This program implements a multi-threaded log file analyzer
 *              using the producer-consumer pattern. One manager thread reads
 *              lines from a log file and multiple worker threads search for
 *              a specific term in these lines.
 * Author: Recep Furkan Akın
 * Student ID: 210104004042
 * 
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include "buffer.h"

// Marker to signal end of file processing
#define EOF_MARKER "END"
// Maximum size of a line read from the log file
#define LINE_BUFFER_SIZE 1024

// Flag to control thread execution, volatile because it's modified in signal handler
volatile int running = 1;
// Number of worker threads to create
int num_workers;
// Array to store match counts for each worker thread
int *match_counts;
// The term to search for in the log file
char *search_term;
// Shared buffer for producer-consumer pattern
Buffer buffer;
// Barrier for synchronizing worker threads before displaying results
pthread_barrier_t barrier;

/**
 * Manager thread function (producer)
 * Reads lines from the log file and adds them to the shared buffer
 *
 * @param arg - Pointer to the log file name
 * @return NULL
 */
void *manager(void *arg)
{
    char *file_name = (char *)arg;

    // Open the log file in read-only mode
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

    // Buffers for reading and line processing
    char read_buf[LINE_BUFFER_SIZE];
    char current_line[LINE_BUFFER_SIZE];
    size_t current_line_idx = 0;
    ssize_t bytes_read;

    // Read chunks from file and process them line by line
    while (running && (bytes_read = read(fd, read_buf, sizeof(read_buf))) > 0)
    {
        for (int i = 0; i < bytes_read && running; ++i)
        {
            if (read_buf[i] == '\n')
            {
                // End of line found, process it
                current_line[current_line_idx] = '\0';
                if (current_line_idx > 0)
                {
                    // Create a copy of the line to add to buffer
                    char *line_copy = strdup(current_line);
                    if (!line_copy)
                    {
                        perror("strdup failed in manager");
                        running = 0;
                        break;
                    }
                    // Add the line to the shared buffer for workers
                    add_to_buffer(&buffer, line_copy);
                }
                current_line_idx = 0;
            }
            else
            {
                // Process each character in the line
                if (current_line_idx < sizeof(current_line) - 1)
                {
                    current_line[current_line_idx++] = read_buf[i];
                }
                else
                {
                    // Line too long, truncate and process what we have
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

    // Handle last line if file doesn't end with newline
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

    // Check for read errors
    if (bytes_read == -1 && running)
    {
        perror("Error reading from file in manager");
        running = 0;
    }
    if (close(fd) == -1)
    {
        perror("Error closing file in manager");
    }

    // Send EOF markers to all workers to signal them to terminate
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

/**
 * Worker thread function (consumer)
 * Removes lines from the shared buffer and searches for the specified term
 *
 * @param arg - Pointer to the worker's ID
 * @return NULL
 */
void *worker(void *arg)
{
    int id = *(int *)arg;
    int count = 0;

    // Process lines until EOF marker or program termination
    while (running)
    {
        // Get a line from the buffer
        char *line = remove_from_buffer(&buffer);
        if (!line)
            break;

        // Check if this is the EOF marker
        if (strcmp(line, EOF_MARKER) == 0)
        {
            free(line);
            break;
        }

        // Check if line contains the search term
        if (strstr(line, search_term))
            count++;
        free(line);
    }

    // Save the count and report results
    match_counts[id] = count;
    printf("Worker %d found %d matches\n", id, count);

    // Wait for all workers to finish before showing total
    pthread_barrier_wait(&barrier);

    // Worker 0 is responsible for calculating and showing the total
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

/**
 * Signal handler for SIGINT (Ctrl+C)
 * Sets the running flag to 0 and signals all threads to terminate
 */
void handle_signal()
{
    running = 0;
    pthread_cond_broadcast(&buffer.not_full);
    pthread_cond_broadcast(&buffer.not_empty);
}

/**
 * Main function
 * Parses command-line arguments, initializes resources, creates threads,
 * waits for them to finish, and cleans up resources
 *
 * @param argc - Number of command-line arguments
 * @param argv - Array of command-line argument strings
 * @return 0 on success, non-zero on failure
 */
int main(int argc, char *argv[])
{
    // Validate command-line arguments
    if (argc != 5)
    {
        fprintf(stderr, "Usage: %s <buffer_size> <num_workers> <log_file> <search_term>\n", argv[0]);
        return 1;
    }

    // Parse command-line arguments
    int buffer_size = atoi(argv[1]);
    num_workers = atoi(argv[2]);
    char *log_file = argv[3];
    search_term = argv[4];

    // Validate numeric arguments
    if (buffer_size <= 0 || num_workers <= 0)
    {
        fprintf(stderr, "Buffer size and number of workers must be positive\n");
        return 1;
    }

    // Initialize the shared buffer
    init_buffer(&buffer, buffer_size);

    // Allocate memory for worker match counts
    match_counts = calloc(num_workers, sizeof(int));
    if (match_counts == NULL)
    {
        perror("Failed to allocate memory for match_counts");
        return 1;
    }

    // Initialize barrier for worker synchronization
    int barrier_ret = pthread_barrier_init(&barrier, NULL, num_workers);
    if (barrier_ret != 0)
    {
        fprintf(stderr, "Error initializing barrier: %s\n", strerror(barrier_ret));
        free(match_counts);
        return 1;
    }

    // Set up signal handler for graceful termination
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

    // Create manager thread (producer)
    pthread_t manager_thread;
    int pt_ret = pthread_create(&manager_thread, NULL, manager, log_file);
    if (pt_ret != 0)
    {
        fprintf(stderr, "Error creating manager thread: %s\n", strerror(pt_ret));
        free(match_counts);
        pthread_barrier_destroy(&barrier);
        return 1;
    }

    // Allocate memory for worker thread handles
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

    // Allocate memory for worker thread IDs
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

    // Create worker threads (consumers)
    for (int i = 0; i < num_workers; i++)
    {
        ids[i] = i;
        int pt_ret = pthread_create(&workers[i], NULL, worker, &ids[i]);
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

    // Wait for all threads to finish
    pthread_join(manager_thread, NULL);
    for (int i = 0; i < num_workers; i++)
    {
        pthread_join(workers[i], NULL);
    }

    // Clean up resources
    free_buffer(&buffer);
    free(match_counts);
    free(workers);
    free(ids);
    pthread_barrier_destroy(&barrier);

    return 0;
}