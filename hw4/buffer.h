/**
 * File: buffer.h
 * Assignment: HW4 - Producer-Consumer Problem with pthread
 * Description: Header file for the circular buffer implementation used in
 *              the producer-consumer pattern. This buffer is thread-safe and
 *              supports blocking operations when full or empty.
 * Author: Recep Furkan Akın
 * Student ID: 210104004042
 */

#ifndef BUFFER_H
#define BUFFER_H

#include <pthread.h>

/**
 * Buffer structure - implements a thread-safe circular buffer
 * Used for communication between manager (producer) and worker (consumer) threads
 */
typedef struct
{
    char **data;              // Array of strings (lines from the log file)
    int size;                 // Maximum capacity of the buffer
    int count;                // Current number of items in the buffer
    int head;                 // Index where the next item will be added
    int tail;                 // Index where the next item will be removed
    pthread_mutex_t mutex;    // Mutex for thread-safe access
    pthread_cond_t not_full;  // Condition variable to signal when buffer is not full
    pthread_cond_t not_empty; // Condition variable to signal when buffer is not empty
} Buffer;

/**
 * Initializes the buffer with the given size
 *
 * @param buffer - Pointer to the buffer structure to initialize
 * @param size - Maximum number of items the buffer can hold
 * @return 0 on success, -1 on failure
 */
int init_buffer(Buffer *buffer, int size);

/**
 * Adds a line to the buffer
 * If the buffer is full, this function will block until space is available
 *
 * @param buffer - Pointer to the buffer structure
 * @param line - The string to add to the buffer (must be dynamically allocated)
 */
void add_to_buffer(Buffer *buffer, char *line);

/**
 * Removes and returns a line from the buffer
 * If the buffer is empty, this function will block until an item is available
 *
 * @param buffer - Pointer to the buffer structure
 * @return A dynamically allocated string that must be freed by the caller
 */
char *remove_from_buffer(Buffer *buffer);

/**
 * Frees all resources associated with the buffer
 * Frees any remaining strings in the buffer and destroys synchronization objects
 *
 * @param buffer - Pointer to the buffer structure to free
 */
void free_buffer(Buffer *buffer);

#endif // BUFFER_H