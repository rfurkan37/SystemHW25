/**
 * File: buffer.c
 * Assignment: HW4 - Producer-Consumer Problem with pthread
 * Description: Implementation of the thread-safe circular buffer for the
 *              producer-consumer pattern. Provides functions for initialization,
 *              adding/removing items, and cleanup.
 * Author: Recep Furkan Akın
 * Student ID: 210104004042
 */

#include <stdlib.h>
#include <stdio.h>
#include "buffer.h"

/**
 * Initializes the buffer with the given size
 * Allocates memory for the buffer and initializes synchronization objects
 *
 * @param buffer - Pointer to the buffer structure to initialize
 * @param size - Maximum number of items the buffer can hold
 * @return 0 on success, -1 on failure
 */
int init_buffer(Buffer *buffer, int size)
{
    // Basic validation of input parameters
    if (buffer == NULL || size <= 0)
    {
        fprintf(stderr, "init_buffer: Invalid arguments (buffer is NULL or size <= 0).\n");
        return -1; // Indicate error
    }

    // Initialize all buffer fields to a known state for safety
    buffer->data = NULL; // Initialize pointers to NULL for safety in cleanup
    buffer->size = 0;    // Initialize fields to known state
    buffer->count = 0;
    buffer->head = 0;
    buffer->tail = 0;

    // Allocate memory for the buffer data array
    buffer->data = malloc(size * sizeof(char *));
    if (buffer->data == NULL)
    {
        perror("init_buffer: Failed to allocate buffer data array");
        return -1; // Error: Malloc failed
    }

    // Initialize buffer metadata *after* successful allocation
    buffer->size = size;
    // count, head, tail are already set to 0

    // Initialize mutex for thread synchronization
    if (pthread_mutex_init(&buffer->mutex, NULL) != 0)
    {
        perror("init_buffer: pthread_mutex_init failed");
        free(buffer->data);  // Clean up allocated memory
        buffer->data = NULL; // Prevent double free in caller if they try cleanup
        return -1;           // Error: Mutex init failed
    }

    // Initialize 'not_full' condition variable
    // This is signaled when an item is removed, making the buffer not full
    if (pthread_cond_init(&buffer->not_full, NULL) != 0)
    {
        perror("init_buffer: pthread_cond_init (not_full) failed");
        pthread_mutex_destroy(&buffer->mutex); // Clean up successfully initialized mutex
        free(buffer->data);
        buffer->data = NULL;
        return -1; // Error: Cond init failed
    }

    // Initialize 'not_empty' condition variable
    // This is signaled when an item is added, making the buffer not empty
    if (pthread_cond_init(&buffer->not_empty, NULL) != 0)
    {
        perror("init_buffer: pthread_cond_init (not_empty) failed");
        pthread_cond_destroy(&buffer->not_full); // Clean up successfully initialized cond var
        pthread_mutex_destroy(&buffer->mutex);   // Clean up successfully initialized mutex
        free(buffer->data);
        buffer->data = NULL;
        return -1; // Error: Cond init failed
    }

    // All initializations successful
    return 0;
}

/**
 * Adds a line to the buffer (producer function)
 * If the buffer is full, this function will block until space is available
 *
 * @param buffer - Pointer to the buffer structure
 * @param line - The string to add to the buffer (dynamically allocated)
 */
void add_to_buffer(Buffer *buffer, char *line)
{
    // Lock the mutex to ensure exclusive access to the buffer
    pthread_mutex_lock(&buffer->mutex);

    // If buffer is full, wait until there's space available
    // This condition may be signaled by remove_from_buffer
    while (buffer->count == buffer->size)
    {
        pthread_cond_wait(&buffer->not_full, &buffer->mutex);
    }

    // Add the line to the buffer at the head position
    buffer->data[buffer->head] = line;

    // Update head using modulo arithmetic for circular buffer
    buffer->head = (buffer->head + 1) % buffer->size;

    // Increment count to track number of items in buffer
    buffer->count++;

    // Signal any waiting consumers that the buffer is not empty
    pthread_cond_signal(&buffer->not_empty);

    // Release the mutex
    pthread_mutex_unlock(&buffer->mutex);
}

/**
 * Removes and returns a line from the buffer (consumer function)
 * If the buffer is empty, this function will block until an item is available
 *
 * @param buffer - Pointer to the buffer structure
 * @return A dynamically allocated string that must be freed by the caller
 */
char *remove_from_buffer(Buffer *buffer)
{
    // Lock the mutex to ensure exclusive access to the buffer
    pthread_mutex_lock(&buffer->mutex);

    // If buffer is empty, wait until there's at least one item
    // This condition may be signaled by add_to_buffer
    while (buffer->count == 0)
    {
        pthread_cond_wait(&buffer->not_empty, &buffer->mutex);
    }

    // Get the line from the buffer at the tail position
    char *line = buffer->data[buffer->tail];

    // Update tail using modulo arithmetic for circular buffer
    buffer->tail = (buffer->tail + 1) % buffer->size;

    // Decrement count to track number of items in buffer
    buffer->count--;

    // Signal any waiting producers that the buffer is not full
    pthread_cond_signal(&buffer->not_full);

    // Release the mutex
    pthread_mutex_unlock(&buffer->mutex);

    // Return the line (caller is responsible for freeing it)
    return line;
}

/**
 * Frees all resources associated with the buffer
 * Frees any remaining strings in the buffer and destroys synchronization objects
 *
 * @param buffer - Pointer to the buffer structure to free
 */
void free_buffer(Buffer *buffer)
{
    // Check if buffer data is already freed
    if (buffer->data == NULL)
        return; // Nothing to free

    // Free any remaining strings in the buffer
    if (buffer->data != NULL)
    {
        // Iterate through all items currently in the buffer
        for (int i = 0; i < buffer->count; i++)
        {
            // Calculate the actual index using modulo arithmetic
            if (buffer->data[(buffer->tail + i) % buffer->size] != NULL)
            {
                free(buffer->data[(buffer->tail + i) % buffer->size]);
            }
        }
        // Free the buffer's data array
        free(buffer->data);
        buffer->data = NULL; // Mark as freed
    }

    // Clean up synchronization objects
    pthread_mutex_destroy(&buffer->mutex);
    pthread_cond_destroy(&buffer->not_full);
    pthread_cond_destroy(&buffer->not_empty);
}