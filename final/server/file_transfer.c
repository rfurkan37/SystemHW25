#include "common.h"

void init_file_queue(void) {
    g_server_state->file_queue = malloc(sizeof(file_queue_t));
    g_server_state->file_queue->head = NULL;
    g_server_state->file_queue->tail = NULL;
    g_server_state->file_queue->count = 0;
    g_server_state->file_queue->active_transfers = 0;
    
    pthread_mutex_init(&g_server_state->file_queue->queue_mutex, NULL);
    pthread_cond_init(&g_server_state->file_queue->queue_cond, NULL);
    sem_init(&g_server_state->file_queue->queue_semaphore, 0, MAX_UPLOAD_QUEUE);
    
    log_message("INIT", "File transfer queue initialized");
}

int enqueue_file_request(const char* filename, const char* sender, 
                        const char* receiver, char* file_data, size_t file_size) {
    
    file_request_t* request = malloc(sizeof(file_request_t));
    if (!request) {
        return 0;
    }
    
    strcpy(request->filename, filename);
    strcpy(request->sender, sender);
    strcpy(request->receiver, receiver);
    request->file_size = file_size;
    request->file_data = file_data;
    request->next = NULL;
    
    pthread_mutex_lock(&g_server_state->file_queue->queue_mutex);
    
    if (g_server_state->file_queue->tail) {
        g_server_state->file_queue->tail->next = request;
    } else {
        g_server_state->file_queue->head = request;
    }
    g_server_state->file_queue->tail = request;
    g_server_state->file_queue->count++;
    
    pthread_cond_signal(&g_server_state->file_queue->queue_cond);
    pthread_mutex_unlock(&g_server_state->file_queue->queue_mutex);
    
    log_file_queued(filename, sender, g_server_state->file_queue->count);
    return 1;
}

file_request_t* dequeue_file_request(void) {
    pthread_mutex_lock(&g_server_state->file_queue->queue_mutex);
    
    // Wait for file requests
    while (g_server_state->file_queue->count == 0 && g_server_state->running) {
        pthread_cond_wait(&g_server_state->file_queue->queue_cond, 
                         &g_server_state->file_queue->queue_mutex);
    }
    
    if (!g_server_state->running) {
        pthread_mutex_unlock(&g_server_state->file_queue->queue_mutex);
        return NULL;
    }
    
    // Dequeue request
    file_request_t* request = g_server_state->file_queue->head;
    g_server_state->file_queue->head = request->next;
    if (!g_server_state->file_queue->head) {
        g_server_state->file_queue->tail = NULL;
    }
    g_server_state->file_queue->count--;
    g_server_state->file_queue->active_transfers++;
    
    pthread_mutex_unlock(&g_server_state->file_queue->queue_mutex);
    
    return request;
}

void process_file_transfer(file_request_t* request) {
    char log_msg[512];
    snprintf(log_msg, sizeof(log_msg), 
             "Processing file transfer: %s from %s to %s (%zu bytes)", 
             request->filename, request->sender, request->receiver, request->file_size);
    log_message("FILE", log_msg);
    
    // Find receiver
    client_t* receiver = find_client_by_username(request->receiver);
    if (!receiver) {
        snprintf(log_msg, sizeof(log_msg), 
                "File transfer failed: receiver %s not found", request->receiver);
        log_message("FILE", log_msg);
        return;
    }
    
    // Send file data message
    message_t file_msg;
    memset(&file_msg, 0, sizeof(file_msg));
    file_msg.type = MSG_FILE_DATA;
    strcpy(file_msg.sender, request->sender);
    strcpy(file_msg.receiver, request->receiver);
    strcpy(file_msg.filename, request->filename);
    file_msg.file_size = request->file_size;
    
    if (send_message(receiver->socket_fd, &file_msg) > 0) {
        // Send file data in chunks
        size_t bytes_sent = 0;
        while (bytes_sent < request->file_size) {
            size_t chunk_size = (request->file_size - bytes_sent > BUFFER_SIZE) ? 
                               BUFFER_SIZE : (request->file_size - bytes_sent);
            
            ssize_t sent = send(receiver->socket_fd, 
                               request->file_data + bytes_sent, chunk_size, 0);
            if (sent <= 0) {
                snprintf(log_msg, sizeof(log_msg), 
                        "File transfer failed: connection error to %s", request->receiver);
                log_message("FILE", log_msg);
                break;
            }
            bytes_sent += sent;
        }
        
        if (bytes_sent == request->file_size) {
            snprintf(log_msg, sizeof(log_msg), 
                    "File transfer completed: %s to %s", 
                    request->filename, request->receiver);
            log_message("FILE", log_msg);
        }
    } else {
        snprintf(log_msg, sizeof(log_msg), 
                "File transfer failed: could not send to %s", request->receiver);
        log_message("FILE", log_msg);
    }
}

void* file_transfer_worker(void* arg) {
    (void)arg; // Suppress unused parameter warning
    
    log_message("FILE", "File transfer worker thread started");
    
    while (g_server_state->running) {
        // Use sem_trywait with a timeout to allow checking shutdown status
        struct timespec timeout;
        clock_gettime(CLOCK_REALTIME, &timeout);
        timeout.tv_sec += 1; // 1 second timeout
        
        int sem_result = sem_timedwait(&g_server_state->file_queue->queue_semaphore, &timeout);
        
        if (sem_result != 0) {
            if (errno == ETIMEDOUT) {
                // Timeout occurred, check if we should continue
                continue;
            } else if (!g_server_state->running) {
                // Server is shutting down
                break;
            }
            continue;
        }
        
        if (!g_server_state->running) {
            sem_post(&g_server_state->file_queue->queue_semaphore);
            break;
        }
        
        file_request_t* request = dequeue_file_request();
        if (!request) {
            sem_post(&g_server_state->file_queue->queue_semaphore);
            break;
        }
        
        // Process file transfer
        process_file_transfer(request);
        
        // Mark transfer complete
        pthread_mutex_lock(&g_server_state->file_queue->queue_mutex);
        g_server_state->file_queue->active_transfers--;
        pthread_mutex_unlock(&g_server_state->file_queue->queue_mutex);
        
        sem_post(&g_server_state->file_queue->queue_semaphore);
        
        // Cleanup
        free(request->file_data);
        free(request);
    }
    
    log_message("FILE", "File transfer worker thread stopped");
    return NULL;
}

void handle_file_transfer(client_t* client, message_t* msg) {
    // Validate file type
    if (!is_valid_file_type(msg->filename)) {
        send_error_message(client->socket_fd, "Invalid file type");
        return;
    }
    
    // Validate file size
    if (msg->file_size > MAX_FILE_SIZE) {
        send_error_message(client->socket_fd, "File too large");
        return;
    }
    
    // Check if receiver exists
    client_t* receiver = find_client_by_username(msg->receiver);
    if (!receiver) {
        send_error_message(client->socket_fd, "Receiver not found");
        return;
    }
    
    // Check queue capacity
    if (g_server_state->file_queue->count >= MAX_UPLOAD_QUEUE) {
        send_error_message(client->socket_fd, "File queue is full");
        return;
    }
    
    // Allocate memory for file data
    char* file_data = malloc(msg->file_size);
    if (!file_data) {
        send_error_message(client->socket_fd, "Memory allocation failed");
        return;
    }
    
    // Receive file data
    size_t bytes_received = 0;
    while (bytes_received < msg->file_size) {
        ssize_t received = recv(client->socket_fd, 
                               file_data + bytes_received, 
                               msg->file_size - bytes_received, 0);
        if (received <= 0) {
            free(file_data);
            send_error_message(client->socket_fd, "File transfer failed");
            return;
        }
        bytes_received += received;
    }
    
    // Enqueue file transfer request
    if (enqueue_file_request(msg->filename, client->username, 
                            msg->receiver, file_data, msg->file_size)) {
        send_success_message(client->socket_fd, "File queued for transfer");
    } else {
        free(file_data);
        send_error_message(client->socket_fd, "Failed to queue file");
    }
} 