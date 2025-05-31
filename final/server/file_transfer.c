#include "common.h"

static int validateFileTransferMeta(ClientInfo *sender_client, const Message *file_req_header, ClientInfo **out_receiver_client)
{
    logEventFileTransferInitiated(sender_client->username, file_req_header->receiver, file_req_header->filename);

    if (!isValidFileType(file_req_header->filename))
    {
        sendErrorToClient(sender_client->socket_fd, "Invalid file type. Supported: .txt, .pdf, .jpg, .png");
        return 0;
    }
    if (file_req_header->file_size == 0)
    {
        sendErrorToClient(sender_client->socket_fd, "Cannot transfer an empty file.");
        return 0;
    }
    if (file_req_header->file_size > MAX_FILE_SIZE)
    {
        logEventFileRejectedOversized(sender_client->username, file_req_header->filename, file_req_header->file_size);
        char err_msg[MESSAGE_BUF_SIZE];
        snprintf(err_msg, sizeof(err_msg), "File '%.50s' is too large (max %dMB).", file_req_header->filename, MAX_FILE_SIZE / (1024 * 1024));
        sendErrorToClient(sender_client->socket_fd, err_msg);
        return 0;
    }

    pthread_mutex_lock(&g_server_state->clients_list_mutex);
    *out_receiver_client = findClientByUsername(file_req_header->receiver);
    pthread_mutex_unlock(&g_server_state->clients_list_mutex);

    if (!*out_receiver_client || !(*out_receiver_client)->is_active)
    {
        sendErrorToClient(sender_client->socket_fd, "Recipient user not found or is offline.");
        return 0;
    }
    if (strcmp(sender_client->username, file_req_header->receiver) == 0)
    {
        sendErrorToClient(sender_client->socket_fd, "You cannot send a file to yourself.");
        return 0;
    }
    return 1;
}


void initializeFileTransferSystem()
{
    if (!g_server_state)
        return;
    FileUploadQueue *ftm = &g_server_state->file_transfer_manager;
    memset(ftm, 0, sizeof(FileUploadQueue));

    if (pthread_mutex_init(&ftm->queue_access_mutex, NULL) != 0 ||
        pthread_cond_init(&ftm->queue_not_empty_cond, NULL) != 0)
    {
        logServerEvent("CRITICAL", "Failed to initialize mutex/condvar for file transfer queue.");
        exit(EXIT_FAILURE); // Critical failure
    }
    if (sem_init(&ftm->available_upload_slots_sem, 0, MAX_UPLOAD_QUEUE_SIZE) != 0)
    {
        logServerEvent("CRITICAL", "Failed to initialize file upload semaphore: %s", strerror(errno));
        exit(EXIT_FAILURE); // Critical failure
    }

    for (int i = 0; i < MAX_UPLOAD_QUEUE_SIZE; ++i)
    {
        if (pthread_create(&g_server_state->file_worker_thread_ids[i], NULL, fileProcessingWorkerThread, NULL) != 0)
        {
            logServerEvent("CRITICAL", "Failed to create file worker thread %d: %s", i, strerror(errno));
            // Partial creation is problematic, simplest to exit.
            // A more robust server might try to run with fewer workers or cleanup and retry.
            exit(EXIT_FAILURE);
        }
        // PDF implies detach is okay, but joining on shutdown is cleaner for resource management.
        // Will join in cleanupFileTransferSystem.
    }
    logServerEvent("INFO", "File transfer system initialized with %d worker thread(s).", MAX_UPLOAD_QUEUE_SIZE);
}

void cleanupFileTransferSystem()
{
    if (!g_server_state)
        return;
    FileUploadQueue *ftm = &g_server_state->file_transfer_manager;
    logServerEvent("INFO", "Cleaning up file transfer system...");

    // server_is_running will be set to 0 by sigintShutdownHandler or main exit path
    // Signal worker threads to wake up and check server_is_running
    pthread_mutex_lock(&ftm->queue_access_mutex);
    pthread_cond_broadcast(&ftm->queue_not_empty_cond);
    pthread_mutex_unlock(&ftm->queue_access_mutex);
    for (int i = 0; i < MAX_UPLOAD_QUEUE_SIZE; ++i)
    { // Unblock any sem_timedwait
        sem_post(&ftm->available_upload_slots_sem);
    }

    for (int i = 0; i < MAX_UPLOAD_QUEUE_SIZE; ++i)
    {
        if (g_server_state->file_worker_thread_ids[i] != 0)
        { // Check if thread was created
            if (pthread_join(g_server_state->file_worker_thread_ids[i], NULL) != 0)
            {
                logServerEvent("ERROR", "Failed to join file worker thread %d: %s", i, strerror(errno));
            }
        }
    }
    logServerEvent("INFO", "All file worker threads joined.");

    pthread_mutex_lock(&ftm->queue_access_mutex);
    FileTransferTask *current_task = ftm->head;
    while (current_task != NULL)
    {
        FileTransferTask *next_task = current_task->next_task;
        logServerEvent("INFO", "Discarding queued file '%s' for %s due to shutdown.",
                       current_task->filename, current_task->receiver_username);
        if (current_task->file_data_buffer)
            free(current_task->file_data_buffer);
        free(current_task);
        current_task = next_task;
    }
    ftm->head = ftm->tail = NULL;
    ftm->current_queue_length = 0;
    pthread_mutex_unlock(&ftm->queue_access_mutex);

    pthread_mutex_destroy(&ftm->queue_access_mutex);
    pthread_cond_destroy(&ftm->queue_not_empty_cond);
    sem_destroy(&ftm->available_upload_slots_sem);
    logServerEvent("INFO", "File transfer system resources released.");
}

void handleFileTransferRequest(ClientInfo *sender_client, const Message *file_req_header)
{
    ClientInfo *receiver_client = NULL;
    if (!validateFileTransferMeta(sender_client, file_req_header, &receiver_client))
    {
        return; // Validation failed, error sent to client
    }

    // Additional check for overall queue capacity (not just processing slots)
    FileUploadQueue *ftm = &g_server_state->file_transfer_manager;
    pthread_mutex_lock(&ftm->queue_access_mutex);
    int queue_length = ftm->current_queue_length;
    pthread_mutex_unlock(&ftm->queue_access_mutex);

    const int MAX_TOTAL_QUEUED_FILES = 50; // Example absolute limit for the backlog
    if (queue_length >= MAX_TOTAL_QUEUED_FILES)
    {
        sendErrorToClient(sender_client->socket_fd, "Server file backlog is full. Try again later.");
        return;
    }

    Message accept_msg; // Prepare MSG_FILE_TRANSFER_ACCEPT
    memset(&accept_msg, 0, sizeof(accept_msg));
    accept_msg.type = MSG_FILE_TRANSFER_ACCEPT;
    // PDF Example implies "File added to the upload queue." for successful queuing
    // Or can mention wait time if queue is full but processing slots are just busy
    strncpy(accept_msg.content, "File transfer accepted by server. Preparing to receive data...", MESSAGE_BUF_SIZE - 1);
    strncpy(accept_msg.filename, file_req_header->filename, FILENAME_BUF_SIZE - 1); // Echo filename

    if (!sendMessage(sender_client->socket_fd, &accept_msg))
    {
        logServerEvent("ERROR", "Failed to send file transfer accept message to %s", sender_client->username);
        return;
    }

    // Now receive the actual file data
    char *actual_file_data = receiveFileDataFromServer(sender_client->socket_fd, file_req_header->file_size,
                                                       file_req_header->filename, sender_client->username);
    if (!actual_file_data)
    {
        // Error logged by helper. Client already got ACCEPT, but data transfer failed.
        // Server won't queue. Client might eventually timeout or error.
        return;
    }

    if (addFileToUploadQueue(file_req_header->filename, sender_client->username,
                             file_req_header->receiver, actual_file_data, file_req_header->file_size))
    {
        logServerEvent("INFO", "File '%s' (%zu bytes) from %s to %s received and queued.",
                       file_req_header->filename, file_req_header->file_size, sender_client->username, file_req_header->receiver);
        // The accept_msg content could be updated here to give more specific feedback if needed,
        // but for now, client gets one "accepted" message.
        // Example: Server could send another MSG_SUCCESS with "File added to upload queue."
        // For now, the initial accept is sufficient as per client's expectation.
    }
    else
    {
        logServerEvent("ERROR", "Server failed to enqueue file '%s' from %s internally after receiving data.",
                       file_req_header->filename, sender_client->username);
        free(actual_file_data); // Free data if not enqueued
        // Client thinks transfer is proceeding, server had an internal issue.
        // Could send an MSG_ERROR to client if possible, but tricky at this stage.
    }
}

int addFileToUploadQueue(const char *filename, const char *sender_user, const char *receiver_user, char *actual_file_data, size_t file_size_val)
{
    if (!g_server_state)
        return 0;

    FileTransferTask *new_task = malloc(sizeof(FileTransferTask));
    if (!new_task)
    {
        logServerEvent("ERROR", "Memory allocation failed for FileTransferTask.");
        return 0; // Caller must free actual_file_data
    }

    strncpy(new_task->filename, filename, FILENAME_BUF_SIZE - 1);
    strncpy(new_task->sender_username, sender_user, USERNAME_BUF_SIZE - 1);
    strncpy(new_task->receiver_username, receiver_user, USERNAME_BUF_SIZE - 1);
    new_task->file_size = file_size_val;
    new_task->file_data_buffer = actual_file_data; // Ownership transferred
    new_task->enqueue_timestamp = time(NULL);
    new_task->next_task = NULL;

    FileUploadQueue *ftm = &g_server_state->file_transfer_manager;
    pthread_mutex_lock(&ftm->queue_access_mutex);
    if (ftm->tail == NULL)
    {
        ftm->head = ftm->tail = new_task;
    }
    else
    {
        ftm->tail->next_task = new_task;
        ftm->tail = new_task;
    }
    ftm->current_queue_length++;
    int q_len = ftm->current_queue_length;

    pthread_cond_signal(&ftm->queue_not_empty_cond);
    pthread_mutex_unlock(&ftm->queue_access_mutex);

    logEventFileQueued(sender_user, filename, q_len);
    return 1;
}

void *fileProcessingWorkerThread(void *arg)
{
    (void)arg;
    unsigned long tid = (unsigned long)pthread_self(); // For logging
    logServerEvent("INFO", "File worker thread (ID %lu) started.", tid);

    if (!g_server_state)
    {
        logServerEvent("CRITICAL", "Worker %lu: g_server_state is NULL. Exiting.", tid);
        return NULL;
    }
    FileUploadQueue *ftm = &g_server_state->file_transfer_manager;

    while (g_server_state->server_is_running)
    {
        struct timespec timeout_sem;
        if (clock_gettime(CLOCK_REALTIME, &timeout_sem) == -1)
        {
            logServerEvent("ERROR", "Worker %lu: clock_gettime failed for sem wait. Retrying.", tid);
            sleep(1);
            continue;
        }
        timeout_sem.tv_sec += 1; // 1 second timeout

        int sem_ret = sem_timedwait(&ftm->available_upload_slots_sem, &timeout_sem);
        if (!g_server_state->server_is_running)
            break;

        if (sem_ret == -1)
        {
            if (errno == EINTR || errno == ETIMEDOUT)
                continue;
            logServerEvent("ERROR", "Worker %lu: sem_timedwait error: %s. Stopping.", tid, strerror(errno));
            break;
        }
        // Acquired a processing slot

        FileTransferTask *task_to_process = NULL;
        pthread_mutex_lock(&ftm->queue_access_mutex);
        while (ftm->head == NULL && g_server_state->server_is_running)
        {
            // Use timedwait on condvar to periodically check server_is_running
            struct timespec timeout_cond;
            if (clock_gettime(CLOCK_REALTIME, &timeout_cond) == -1)
            {
                logServerEvent("ERROR", "Worker %lu: clock_gettime failed for cond wait. Using untimed.", tid);
                pthread_cond_wait(&ftm->queue_not_empty_cond, &ftm->queue_access_mutex); // Fallback
                break;                                                                   // Break inner while to re-check conditions
            }
            timeout_cond.tv_sec += 1; // 1 second timeout
            pthread_cond_timedwait(&ftm->queue_not_empty_cond, &ftm->queue_access_mutex, &timeout_cond);
        }

        if (!g_server_state->server_is_running)
        { // Re-check after wake-up or timeout
            pthread_mutex_unlock(&ftm->queue_access_mutex);
            sem_post(&ftm->available_upload_slots_sem); // Release slot if shutting down
            break;
        }

        if (ftm->head != NULL)
        {
            task_to_process = ftm->head;
            ftm->head = ftm->head->next_task;
            if (ftm->head == NULL)
                ftm->tail = NULL;
            ftm->current_queue_length--;
        }
        pthread_mutex_unlock(&ftm->queue_access_mutex);

        if (task_to_process)
        {
            long wait_duration = (long)difftime(time(NULL), task_to_process->enqueue_timestamp);
            logEventFileTransferProcessingStart(task_to_process->sender_username, task_to_process->filename, wait_duration);
            executeFileTransferToRecipient(task_to_process);

            if (task_to_process->file_data_buffer)
                free(task_to_process->file_data_buffer);
            free(task_to_process);
            sem_post(&ftm->available_upload_slots_sem); // Release processing slot
        }
        else
        {
            // No task (spurious wakeup, or server shutdown and queue empty)
            sem_post(&ftm->available_upload_slots_sem); // Release slot
        }
    }
    logServerEvent("INFO", "File worker thread (ID %lu) stopping.", tid);
    return NULL;
}

void executeFileTransferToRecipient(FileTransferTask *task)
{
    if (!task || !g_server_state)
        return;

    int DUMMY_PROCESSING_SECONDS = 5; // Process each file for 5 seconds
    logServerEvent("DEBUG_DELAY", "Worker for '%s' (sender %s) starting %d-second artificial processing delay.",
                   task->filename, task->sender_username, DUMMY_PROCESSING_SECONDS);
    sleep(DUMMY_PROCESSING_SECONDS);
    logServerEvent("DEBUG_DELAY", "Worker for '%s' (sender %s) finished artificial delay.",
                   task->filename, task->sender_username);

    int recipient_socket_fd = -1;
    pthread_mutex_lock(&g_server_state->clients_list_mutex);
    ClientInfo *recipient = findClientByUsername(task->receiver_username);
    if (recipient && recipient->is_active)
    {
        recipient_socket_fd = recipient->socket_fd; // Copy fd while holding lock
    }
    pthread_mutex_unlock(&g_server_state->clients_list_mutex);

    if (recipient_socket_fd == -1)
    {
        logEventFileTransferFailed(task->sender_username, task->receiver_username, task->filename, "Recipient offline or not found during transfer execution.");
        return;
    }

    Message file_data_header;
    memset(&file_data_header, 0, sizeof(file_data_header));
    file_data_header.type = MSG_FILE_TRANSFER_DATA;
    strncpy(file_data_header.sender, task->sender_username, USERNAME_BUF_SIZE - 1);
    strncpy(file_data_header.receiver, task->receiver_username, USERNAME_BUF_SIZE - 1);
    strncpy(file_data_header.filename, task->filename, FILENAME_BUF_SIZE - 1);
    file_data_header.file_size = task->file_size;

    if (!sendMessage(recipient_socket_fd, &file_data_header))
    {
        logEventFileTransferFailed(task->sender_username, task->receiver_username, task->filename, "Failed to send file header to recipient.");
        return;
    }

    size_t total_bytes_sent = 0;
    char *current_data_ptr = task->file_data_buffer;
    while (total_bytes_sent < task->file_size)
    {
        // Adjust chunk size if needed, though for memory-to-socket, large sends are often fine.
        // For very large files, smaller chunks might be better for network buffers.
        // ssize_t bytes_to_send_this_chunk = (task->file_size - total_bytes_sent > 4096) ? 4096 : (task->file_size - total_bytes_sent);
        ssize_t bytes_to_send_this_chunk = task->file_size - total_bytes_sent;

        ssize_t bytes_sent_now = send(recipient_socket_fd, current_data_ptr, bytes_to_send_this_chunk, 0);
        if (bytes_sent_now <= 0)
        {
            logEventFileTransferFailed(task->sender_username, task->receiver_username, task->filename, "Socket error while sending file data to recipient.");
            return;
        }
        total_bytes_sent += bytes_sent_now;
        current_data_ptr += bytes_sent_now;
    }

    if (total_bytes_sent == task->file_size)
    {
        logEventFileTransferCompleted(task->sender_username, task->receiver_username, task->filename);
    }
    else
    {
        logEventFileTransferFailed(task->sender_username, task->receiver_username, task->filename, "Incomplete data sent to recipient.");
    }
}