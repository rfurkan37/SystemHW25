#ifndef BUFFER_H
#define BUFFER_H

#include <pthread.h>
#include <stdbool.h> // For bool type

// Structure for the shared bounded buffer
typedef struct
{
    char **lines;              // Array of character pointers (strings)
    int capacity;              // Maximum number of items
    int count;                 // Current number of items
    int head;                  // Index to remove from (consumer)
    int tail;                  // Index to add to (producer)
    bool eof_reached;          // Flag to signal end of file
    pthread_mutex_t mutex;     // Mutex for protecting buffer access
    pthread_cond_t cond_full;  // Condition variable for when buffer is full
    pthread_cond_t cond_empty; // Condition variable for when buffer is empty
} Buffer;

/**
 * @brief Initializes the shared buffer.
 * @param capacity The maximum number of lines the buffer can hold.
 * @return Pointer to the initialized Buffer, or NULL on failure.
 */
Buffer *buffer_init(int capacity);

/**
 * @brief Destroys the shared buffer and frees associated resources.
 * @param buffer Pointer to the Buffer to destroy.
 */
void buffer_destroy(Buffer *buffer);

/**
 * @brief Adds a line to the buffer (producer operation).
 *        Blocks if the buffer is full.
 * @param buffer Pointer to the Buffer.
 * @param line The line (string) to add. Assumes ownership of the line's memory.
 */
void buffer_add(Buffer *buffer, char *line);

/**
 * @brief Removes a line from the buffer (consumer operation).
 *        Blocks if the buffer is empty and EOF has not been reached.
 * @param buffer Pointer to the Buffer.
 * @return Pointer to the removed line (string), or NULL if EOF reached and buffer is empty.
 *         The caller is responsible for freeing the returned line's memory.
 */
char *buffer_remove(Buffer *buffer);

/**
 * @brief Signals that the end of the file has been reached by the producer.
 *        Wakes up any waiting consumers.
 * @param buffer Pointer to the Buffer.
 */
void buffer_signal_eof(Buffer *buffer);

#endif // BUFFER_H