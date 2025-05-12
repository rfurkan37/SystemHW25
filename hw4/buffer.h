#ifndef BUFFER_H
#define BUFFER_H

#include <pthread.h>

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

int init_buffer(Buffer *buffer, int size);
void add_to_buffer(Buffer *buffer, char *line);
char *remove_from_buffer(Buffer *buffer);
void free_buffer(Buffer *buffer);

#endif // BUFFER_H