#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include "buffer.h"

// Flag to indicate if SIGINT was received
volatile sig_atomic_t running = 1; // Accessed by signal handler and threads

Buffer *buffer_init(int capacity)
{
    if (capacity <= 0)
    {
        fprintf(stderr, "Error: Buffer capacity must be positive.\n");
        return NULL;
    }

    Buffer *buffer = (Buffer *)malloc(sizeof(Buffer));
    if (!buffer)
    {
        perror("Failed to allocate memory for buffer structure");
        return NULL;
    }

    buffer->lines = (char **)malloc(capacity * sizeof(char *));
    if (!buffer->lines)
    {
        perror("Failed to allocate memory for buffer lines array");
        free(buffer);
        return NULL;
    }

    buffer->capacity = capacity;
    buffer->count = 0;
    buffer->head = 0;
    buffer->tail = 0;
    buffer->eof_reached = false;

    if (pthread_mutex_init(&buffer->mutex, NULL) != 0)
    {
        perror("Mutex initialization failed");
        free(buffer->lines);
        free(buffer);
        return NULL;
    }
    if (pthread_cond_init(&buffer->cond_full, NULL) != 0)
    {
        perror("Condition variable 'full' initialization failed");
        pthread_mutex_destroy(&buffer->mutex);
        free(buffer->lines);
        free(buffer);
        return NULL;
    }
    if (pthread_cond_init(&buffer->cond_empty, NULL) != 0)
    {
        perror("Condition variable 'empty' initialization failed");
        pthread_cond_destroy(&buffer->cond_full);
        pthread_mutex_destroy(&buffer->mutex);
        free(buffer->lines);
        free(buffer);
        return NULL;
    }

    return buffer;
}

void buffer_destroy(Buffer *buffer)
{
    if (!buffer)
        return;

    // Free any remaining lines in the buffer
    // This is important for cleanup, especially if terminated early
    if (buffer->lines)
    {
        while (buffer->count > 0)
        {
            free(buffer->lines[buffer->head]);
            buffer->head = (buffer->head + 1) % buffer->capacity;
            buffer->count--;
        }
        free(buffer->lines);
    }

    pthread_mutex_destroy(&buffer->mutex);
    pthread_cond_destroy(&buffer->cond_full);
    pthread_cond_destroy(&buffer->cond_empty);
    free(buffer);
}

void buffer_add(Buffer *buffer, char *line)
{
    if (!buffer)
        return;

    pthread_mutex_lock(&buffer->mutex);

    // Wait while the buffer is full AND running flag is set
    while (buffer->count == buffer->capacity && running)
    {
        // If producer needs to wait, check running flag before waiting
        pthread_cond_wait(&buffer->cond_full, &buffer->mutex);
    }

    // If SIGINT occurred while waiting or before adding, don't add
    if (!running)
    {
        // We might own the line memory here, free it before returning
        free(line);
        pthread_mutex_unlock(&buffer->mutex);
        // Signal potentially waiting consumers just in case they can now exit
        pthread_cond_broadcast(&buffer->cond_empty);
        return;
    }

    // Add the line to the buffer
    buffer->lines[buffer->tail] = line; // Assumes line is malloc'd
    buffer->tail = (buffer->tail + 1) % buffer->capacity;
    buffer->count++;

    // Signal that the buffer is no longer empty
    pthread_cond_signal(&buffer->cond_empty);

    pthread_mutex_unlock(&buffer->mutex);
}

char *buffer_remove(Buffer *buffer)
{
    if (!buffer)
        return NULL;

    pthread_mutex_lock(&buffer->mutex);

    // Wait while the buffer is empty AND EOF has not been signaled AND running flag is set
    while (buffer->count == 0 && !buffer->eof_reached && running)
    {
        pthread_cond_wait(&buffer->cond_empty, &buffer->mutex);
    }

    // Check conditions for returning NULL:
    // 1. Buffer is empty AND EOF has been reached
    // 2. SIGINT was received (running flag is false)
    if ((buffer->count == 0 && buffer->eof_reached) || !running)
    {
        pthread_mutex_unlock(&buffer->mutex);
        // If we are exiting due to !running, signal others who might be waiting
        if (!running)
        {
            pthread_cond_broadcast(&buffer->cond_empty);
        }
        return NULL; // Signal EOF or shutdown
    }

    // Remove the line from the buffer
    char *line = buffer->lines[buffer->head];
    buffer->head = (buffer->head + 1) % buffer->capacity;
    buffer->count--;

    // Signal that the buffer is no longer full
    pthread_cond_signal(&buffer->cond_full);

    pthread_mutex_unlock(&buffer->mutex);

    return line; // Caller must free this memory
}

void buffer_signal_eof(Buffer *buffer)
{
    if (!buffer)
        return;

    pthread_mutex_lock(&buffer->mutex);
    buffer->eof_reached = true;
    // Broadcast to wake up ALL waiting consumers so they can check the eof_reached flag
    pthread_cond_broadcast(&buffer->cond_empty);
    pthread_mutex_unlock(&buffer->mutex);
}