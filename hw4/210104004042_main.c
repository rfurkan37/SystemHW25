#define _POSIX_C_SOURCE 200809L // Required for getline, strdup
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>  // Not strictly needed now, but good practice
#include <stdbool.h> // Include for bool if not implicitly included
#include <limits.h>  // For INT_MAX

#include "buffer.h" // Include our buffer definition

// Global shared buffer pointer - accessible by signal handler for potential cleanup trigger
Buffer *shared_buffer = NULL;
// Global barrier for worker synchronization
pthread_barrier_t barrier;
// Global flag to signal termination (set by signal handler)
extern volatile sig_atomic_t running; // Defined in buffer.c

// Structure to pass arguments to worker threads
typedef struct
{
    int id;
    const char *search_term;
    long match_count; // Store result here or return it
} WorkerArgs;

// Signal handler for SIGINT (Ctrl+C)
void handle_sigint(int sig)
{
    // Signal received, tell threads to stop gracefully
    printf("\nSIGINT received (Signal %d), shutting down...\n", sig);
    running = 0; // Set the global flag

    // It's generally unsafe to do complex operations (locks, free, etc.)
    // directly in a signal handler. Setting a flag is the safest approach.
    // We can try to nudge the producer if it's waiting.
    if (shared_buffer)
    {
        pthread_mutex_lock(&shared_buffer->mutex);
        // Wake up producer if it's waiting on buffer full condition
        pthread_cond_signal(&shared_buffer->cond_full);
        // Also wake up consumers in case they are waiting
        pthread_cond_broadcast(&shared_buffer->cond_empty);
        pthread_mutex_unlock(&shared_buffer->mutex);
    }
    // The main loops in producer and consumer must check the 'running' flag.
}

// Worker thread function
void *worker_thread(void *arg)
{
    WorkerArgs *args = (WorkerArgs *)arg;
    int worker_id = args->id;
    const char *search_term = args->search_term;
    long local_match_count = 0;
    char *line;

    printf("Worker %d started.\n", worker_id);

    while (running)
    {
        line = buffer_remove(shared_buffer);

        if (line == NULL)
        {
            // NULL means either EOF reached and buffer empty, or SIGINT received
            break; // Exit the loop
        }

        // Search for the keyword in the line
        if (strstr(line, search_term) != NULL)
        {
            local_match_count++;
            // Optionally print matching lines (can be verbose for large files)
            // printf("Worker %d found match: %s", worker_id, line);
        }

        // CRITICAL: Worker consumes the line, so it must free the memory
        free(line);
    }

    args->match_count = local_match_count; // Store result back in args struct
    printf("Worker %d finished. Found %ld matches.\n", worker_id, local_match_count);

    // Wait for all other worker threads to finish processing before proceeding
    // The barrier ensures all workers reach this point before the summary logic
    int barrier_ret = pthread_barrier_wait(&barrier);

    if (barrier_ret == PTHREAD_BARRIER_SERIAL_THREAD)
    {
        // This block is executed by only ONE arbitrary thread after all workers
        // have called pthread_barrier_wait.
        // The summary printing is moved to the main thread after join for simplicity.
        printf("All workers reached the barrier.\n");
    }
    else if (barrier_ret != 0)
    {
        fprintf(stderr, "Worker %d: Error waiting on barrier: %d\n", worker_id, barrier_ret);
    }

    // Exit thread, returning the count (alternatively, use the args struct)
    // Using pthread_exit is cleaner than return for threads
    pthread_exit((void *)local_match_count);
    // return (void*)local_match_count; // Alternative return
}

// Function to print usage instructions
void print_usage(const char *prog_name)
{
    fprintf(stderr, "Usage: %s <buffer_size> <num_workers> <log_file> <search_term>\n", prog_name);
    fprintf(stderr, "Example: %s 20 4 /var/log/syslog \"ERROR\"\n", prog_name);
}

int main(int argc, char *argv[])
{
    // --- Argument Parsing and Validation ---
    if (argc != 5)
    {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    char *endptr;
    errno = 0; // To distinguish success/failure after strtol
    long buffer_size_long = strtol(argv[1], &endptr, 10);
    if (errno != 0 || *endptr != '\0' || buffer_size_long <= 0 || buffer_size_long > INT_MAX)
    {
        fprintf(stderr, "Error: Invalid buffer size '%s'. Must be a positive integer.\n", argv[1]);
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }
    int buffer_size = (int)buffer_size_long;

    errno = 0;
    long num_workers_long = strtol(argv[2], &endptr, 10);
    if (errno != 0 || *endptr != '\0' || num_workers_long <= 0 || num_workers_long > INT_MAX)
    {
        fprintf(stderr, "Error: Invalid number of workers '%s'. Must be a positive integer.\n", argv[2]);
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }
    int num_workers = (int)num_workers_long;

    const char *log_file = argv[3];
    const char *search_term = argv[4];

    printf("Starting Log Analyzer with:\n");
    printf("  Buffer Size: %d\n", buffer_size);
    printf("  Num Workers: %d\n", num_workers);
    printf("  Log File: %s\n", log_file);
    printf("  Search Term: %s\n", search_term);
    printf("------------------------------------\n");

    // --- Initialization ---
    shared_buffer = buffer_init(buffer_size);
    if (!shared_buffer)
    {
        // Error message already printed in buffer_init
        return EXIT_FAILURE;
    }

    // Initialize barrier for num_workers threads
    if (pthread_barrier_init(&barrier, NULL, num_workers) != 0)
    {
        perror("Barrier initialization failed");
        buffer_destroy(shared_buffer);
        return EXIT_FAILURE;
    }

    // Setup signal handler for SIGINT
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_sigint;
    sigemptyset(&sa.sa_mask); // Don't block other signals during handler
    sa.sa_flags = 0;          // No special flags needed
    if (sigaction(SIGINT, &sa, NULL) == -1)
    {
        perror("Failed to register SIGINT handler");
        pthread_barrier_destroy(&barrier);
        buffer_destroy(shared_buffer);
        return EXIT_FAILURE;
    }

    pthread_t *threads = malloc(num_workers * sizeof(pthread_t));
    WorkerArgs *args = malloc(num_workers * sizeof(WorkerArgs));
    long *results = malloc(num_workers * sizeof(long)); // To store results from join

    if (!threads || !args || !results)
    {
        perror("Failed to allocate memory for thread structures");
        if (threads)
            free(threads);
        if (args)
            free(args);
        if (results)
            free(results);
        pthread_barrier_destroy(&barrier);
        buffer_destroy(shared_buffer);
        return EXIT_FAILURE;
    }

    // --- Thread Creation ---
    for (int i = 0; i < num_workers; ++i)
    {
        args[i].id = i + 1; // Worker IDs start from 1
        args[i].search_term = search_term;
        args[i].match_count = 0; // Initialize
        if (pthread_create(&threads[i], NULL, worker_thread, &args[i]) != 0)
        {
            perror("Failed to create worker thread");
            // Need to clean up already created threads and resources
            running = 0; // Signal other threads to stop
            // Attempt to join already created threads
            for (int j = 0; j < i; ++j)
            {
                pthread_join(threads[j], NULL); // Ignore return value during cleanup
            }
            free(threads);
            free(args);
            free(results);
            pthread_barrier_destroy(&barrier);
            buffer_destroy(shared_buffer); // Buffer destroy handles remaining lines
            return EXIT_FAILURE;
        }
    }

    // --- Producer Logic (Main Thread) ---
    FILE *fp = fopen(log_file, "r");
    if (!fp)
    {
        perror("Failed to open log file");
        fprintf(stderr, "Error opening file: %s\n", log_file);
        running = 0;                      // Signal workers to stop
        buffer_signal_eof(shared_buffer); // Ensure workers unblock if waiting
                                          // Join threads before exiting
        for (int i = 0; i < num_workers; ++i)
        {
            pthread_join(threads[i], NULL);
        }
        free(threads);
        free(args);
        free(results);
        pthread_barrier_destroy(&barrier);
        buffer_destroy(shared_buffer);
        return EXIT_FAILURE;
    }

    char *line_buf = NULL; // Buffer for getline
    size_t line_cap = 0;   // Capacity of buffer
    ssize_t line_len;      // Length of line read

    printf("Manager thread starting to read file '%s'...\n", log_file);
    while (running && (line_len = getline(&line_buf, &line_cap, fp)) != -1)
    {
        // Need to copy the line because getline reuses the buffer
        char *line_copy = strdup(line_buf);
        if (!line_copy)
        {
            perror("Failed to duplicate line (strdup)");
            running = 0; // Stop processing
            break;       // Exit loop, proceed to cleanup
        }
        // buffer_add takes ownership of line_copy's memory
        buffer_add(shared_buffer, line_copy);

        // Check running flag again in case SIGINT occurred during buffer_add wait
        if (!running)
        {
            break;
        }
    }
    printf("Manager thread finished reading file.\n");

    // Cleanup getline buffer
    free(line_buf);
    fclose(fp);

    // Signal EOF to workers AFTER reading is complete or interrupted
    if (running)
    {
        printf("Manager signaling EOF.\n");
    }
    else
    {
        printf("Manager shutting down due to signal.\n");
    }
    buffer_signal_eof(shared_buffer);

    // --- Thread Joining and Result Collection ---
    long total_matches = 0;
    printf("Manager waiting for workers to finish...\n");
    for (int i = 0; i < num_workers; ++i)
    {
        void *status;
        if (pthread_join(threads[i], &status) == 0)
        {
            results[i] = (long)status; // Retrieve count returned by worker
                                       // Alternatively, access args[i].match_count if stored there
                                       // results[i] = args[i].match_count;
            total_matches += results[i];
        }
        else
        {
            fprintf(stderr, "Warning: Failed to join worker thread %d\n", i + 1);
            // Handle potential error - maybe the thread crashed?
        }
    }

    // --- Final Summary Report ---
    // This happens after all workers have finished and been joined.
    printf("\n--- Summary Report ---\n");
    for (int i = 0; i < num_workers; ++i)
    {
        printf("Worker %d reported %ld matches.\n", i + 1, results[i]);
    }
    printf("----------------------\n");
    printf("Total matches found: %ld\n", total_matches);
    printf("----------------------\n");

    // --- Final Cleanup ---
    printf("Cleaning up resources...\n");
    free(threads);
    free(args);
    free(results);
    pthread_barrier_destroy(&barrier); // Destroy barrier
    buffer_destroy(shared_buffer);     // Destroy buffer (frees remaining lines if any)

    printf("Log Analyzer finished successfully.\n");
    return EXIT_SUCCESS;
}