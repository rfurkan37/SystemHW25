#include "buffer.h"
#include <stdlib.h>
#include <stdio.h>

int init_buffer(Buffer *buffer, int size)
{
    buffer->data = malloc(size * sizeof(char *));
    if (buffer == NULL || size <= 0)
    {
        fprintf(stderr, "init_buffer: Invalid arguments (buffer is NULL or size <= 0).\n");
        return -1; // Indicate error
    }

    buffer->data = NULL; // Initialize pointers to NULL for safety in cleanup
    buffer->size = 0;    // Initialize fields to known state
    buffer->count = 0;
    buffer->head = 0;
    buffer->tail = 0;

    buffer->data = malloc(size * sizeof(char *));
    if (buffer->data == NULL)
    {
        perror("init_buffer: Failed to allocate buffer data array");
        return -1; // Error: Malloc failed
    }

    buffer->size = size;

    // Initialize mutex
    if (pthread_mutex_init(&buffer->mutex, NULL) != 0)
    {
        perror("init_buffer: pthread_mutex_init failed");
        free(buffer->data);  // Clean up allocated memory
        buffer->data = NULL; // Prevent double free in caller if they try cleanup
        return -1;           // Error: Mutex init failed
    }

    // Initialize 'not_full' condition variable
    if (pthread_cond_init(&buffer->not_full, NULL) != 0)
    {
        perror("init_buffer: pthread_cond_init (not_full) failed");
        pthread_mutex_destroy(&buffer->mutex); // Clean up successfully initialized mutex
        free(buffer->data);
        buffer->data = NULL;
        return -1; // Error: Cond init failed
    }

    // Initialize 'not_empty' condition variable
    if (pthread_cond_init(&buffer->not_empty, NULL) != 0)
    {
        perror("init_buffer: pthread_cond_init (not_empty) failed");
        pthread_cond_destroy(&buffer->not_full); // Clean up successfully initialized cond var
        pthread_mutex_destroy(&buffer->mutex);   // Clean up successfully initialized mutex
        free(buffer->data);
        buffer->data = NULL;
        return -1; // Error: Cond init failed
    }

    return 0;
}

void add_to_buffer(Buffer *buffer, char *line)
{
    pthread_mutex_lock(&buffer->mutex);
    while (buffer->count == buffer->size)
    {
        pthread_cond_wait(&buffer->not_full, &buffer->mutex);
    }
    buffer->data[buffer->head] = line;
    buffer->head = (buffer->head + 1) % buffer->size;
    buffer->count++;
    pthread_cond_signal(&buffer->not_empty);
    pthread_mutex_unlock(&buffer->mutex);
}

char *remove_from_buffer(Buffer *buffer)
{
    pthread_mutex_lock(&buffer->mutex);
    while (buffer->count == 0)
    {
        pthread_cond_wait(&buffer->not_empty, &buffer->mutex);
    }
    char *line = buffer->data[buffer->tail];
    buffer->tail = (buffer->tail + 1) % buffer->size;
    buffer->count--;
    pthread_cond_signal(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
    return line;
}

void free_buffer(Buffer *buffer)
{
    if (buffer->data == NULL)
        return; // Nothing to free

    if (buffer->data != NULL)
    {
        for (int i = 0; i < buffer->count; i++)
        {
            if (buffer->data[(buffer->tail + i) % buffer->size] != NULL)
            {
                free(buffer->data[(buffer->tail + i) % buffer->size]);
            }
        }
        free(buffer->data);
        buffer->data = NULL; // Mark as freed
    }

    pthread_mutex_destroy(&buffer->mutex);
    pthread_cond_destroy(&buffer->not_full);
    pthread_cond_destroy(&buffer->not_empty);
}