#include "common.h"
#include <stdbool.h> // For bool type
#include <stdint.h>  // For intptr_t for casting pointer to long for thread arg

// Validates file transfer metadata (type, size, recipient) before adding to queue.
// Also logs the initiation attempt.
// Returns 1 if valid, 0 otherwise. out_receiver_client is populated if recipient is valid.
static int validateFileTransferMeta(ClientInfo *sender_client, const Message *file_req_header, ClientInfo **out_receiver_client)
{
    if (!sender_client || !file_req_header || !out_receiver_client)
        return 0;
    *out_receiver_client = NULL; // Initialize output parameter

    // Log initiation attempt (moved here for earlier logging as per previous refinement)
    logEventFileTransferInitiated(sender_client->username, file_req_header->receiver, file_req_header->filename);

    if (!isValidFileType(file_req_header->filename))
    {
        char err_msg[MESSAGE_BUF_SIZE];
        snprintf(err_msg, sizeof(err_msg), "Invalid file type for '%s'. Supported: .txt, .pdf, .jpg, .png", file_req_header->filename);
        err_msg[sizeof(err_msg) - 1] = '\0';
        sendErrorToClient(sender_client->socket_fd, err_msg);
        return 0;
    }
    if (file_req_header->file_size == 0)
    {
        sendErrorToClient(sender_client->socket_fd, "Cannot transfer an empty file (0 bytes).");
        return 0;
    }
    if (file_req_header->file_size > MAX_FILE_SIZE)
    {
        logEventFileRejectedOversized(sender_client->username, file_req_header->filename, file_req_header->file_size);
        char err_msg[MESSAGE_BUF_SIZE];
        snprintf(err_msg, sizeof(err_msg), "File '%.50s' is too large (size %zu bytes, max %dMB).",
                 file_req_header->filename, file_req_header->file_size, MAX_FILE_SIZE / (1024 * 1024));
        err_msg[sizeof(err_msg) - 1] = '\0';
        sendErrorToClient(sender_client->socket_fd, err_msg);
        return 0;
    }

    // Check recipient
    pthread_mutex_lock(&g_server_state->clients_list_mutex);
    ClientInfo *temp_receiver_ref = findClientByUsername(file_req_header->receiver); // Assumes findClientByUsername needs lock
    if (temp_receiver_ref && temp_receiver_ref->is_active)
    {                                             // Must be active
        *out_receiver_client = temp_receiver_ref; // Store reference if valid
    }
    pthread_mutex_unlock(&g_server_state->clients_list_mutex);

    if (!(*out_receiver_client))
    { // Check using the populated out_receiver_client
        sendErrorToClient(sender_client->socket_fd, "Recipient user not found or is currently offline.");
        return 0;
    }
    if (strcmp(sender_client->username, file_req_header->receiver) == 0)
    {
        sendErrorToClient(sender_client->socket_fd, "You cannot send a file to yourself.");
        *out_receiver_client = NULL; // Invalidate, though already checked this user exists.
        return 0;
    }
    return 1; // All metadata checks passed
}

// Initializes the file transfer system: queue, mutex, condition variable, semaphore, and worker threads.
void initializeFileTransferSystem()
{
    if (!g_server_state)
    {
        fprintf(stderr, "CRITICAL: g_server_state is NULL in initializeFileTransferSystem.\n");
        exit(EXIT_FAILURE);
    }
    FileUploadQueue *ftm = &g_server_state->file_transfer_manager;
    memset(ftm, 0, sizeof(FileUploadQueue)); // Initialize all fields to zero/NULL

    if (pthread_mutex_init(&ftm->queue_access_mutex, NULL) != 0)
    {
        logServerEvent("CRITICAL", "Failed to initialize file queue mutex: %s", strerror(errno));
        exit(EXIT_FAILURE);
    }
    if (pthread_cond_init(&ftm->queue_not_empty_cond, NULL) != 0)
    {
        pthread_mutex_destroy(&ftm->queue_access_mutex);
        logServerEvent("CRITICAL", "Failed to initialize file queue condition variable: %s", strerror(errno));
        exit(EXIT_FAILURE);
    }
    // Initialize semaphore to control max concurrent file "uploads" (processing slots)
    if (sem_init(&ftm->available_upload_slots_sem, 0, MAX_UPLOAD_QUEUE_SIZE) != 0)
    {
        pthread_mutex_destroy(&ftm->queue_access_mutex);
        pthread_cond_destroy(&ftm->queue_not_empty_cond);
        logServerEvent("CRITICAL", "Failed to initialize file upload semaphore: %s", strerror(errno));
        exit(EXIT_FAILURE);
    }

    // Create worker threads
    for (int i = 0; i < MAX_UPLOAD_QUEUE_SIZE; ++i)
    {
        // Pass worker index as argument for logging/identification
        if (pthread_create(&g_server_state->file_worker_thread_ids[i], NULL, fileProcessingWorkerThread, (void *)(intptr_t)i) != 0)
        {
            logServerEvent("CRITICAL", "Failed to create file worker thread %d: %s", i, strerror(errno));
            // Partial creation is problematic. Simplest to attempt cleanup and exit.
            // Signal already created worker threads to stop (difficult without full shutdown mechanism yet)
            // For now, log and exit. A more robust server might try to run with fewer workers or retry.
            g_server_state->server_is_running = 0; // Signal shutdown
            // Wake up any threads that might be waiting
            pthread_cond_broadcast(&ftm->queue_not_empty_cond);
            for (int k = 0; k < i; ++k)
                sem_post(&ftm->available_upload_slots_sem);

            for (int j = 0; j < i; ++j)
            { // Attempt to join successfully created threads
                pthread_join(g_server_state->file_worker_thread_ids[j], NULL);
            }
            sem_destroy(&ftm->available_upload_slots_sem);
            pthread_cond_destroy(&ftm->queue_not_empty_cond);
            pthread_mutex_destroy(&ftm->queue_access_mutex);
            exit(EXIT_FAILURE);
        }
        // Worker threads are joinable, not detached, to ensure clean shutdown.
    }
    logServerEvent("INFO", "File transfer system initialized with %d worker thread(s).", MAX_UPLOAD_QUEUE_SIZE);
}

// Cleans up the file transfer system during server shutdown.
// Signals worker threads, joins them, clears queue, and destroys sync primitives.
void cleanupFileTransferSystem()
{
    if (!g_server_state)
        return;
    FileUploadQueue *ftm = &g_server_state->file_transfer_manager;
    logServerEvent("INFO", "Cleaning up file transfer system...");

    // Server_is_running flag should already be 0 if called during graceful shutdown.
    // 1. Signal worker threads to wake up and exit.
    // Broadcast on condition variable to wake up any workers waiting on queue_not_empty_cond.
    pthread_mutex_lock(&ftm->queue_access_mutex);
    pthread_cond_broadcast(&ftm->queue_not_empty_cond);
    pthread_mutex_unlock(&ftm->queue_access_mutex);

    // Post to semaphore for each worker to unblock any worker waiting on available_upload_slots_sem.
    for (int i = 0; i < MAX_UPLOAD_QUEUE_SIZE; ++i)
    {
        sem_post(&ftm->available_upload_slots_sem);
    }

    // 2. Join all worker threads.
    logServerEvent("INFO", "Waiting for file worker threads to terminate...");
    for (int i = 0; i < MAX_UPLOAD_QUEUE_SIZE; ++i)
    {
        if (g_server_state->file_worker_thread_ids[i] != 0)
        { // Check if thread was created
            if (pthread_join(g_server_state->file_worker_thread_ids[i], NULL) != 0)
            {
                logServerEvent("ERROR", "Failed to join file worker thread %d (ID: %lu): %s", i, (unsigned long)g_server_state->file_worker_thread_ids[i], strerror(errno));
            }
        }
    }
    logServerEvent("INFO", "All file worker threads have terminated.");

    // 3. Clear any remaining tasks in the queue (free allocated memory).
    pthread_mutex_lock(&ftm->queue_access_mutex);
    FileTransferTask *current_task = ftm->head;
    FileTransferTask *next_task;
    while (current_task != NULL)
    {
        next_task = current_task->next_task;
        logServerEvent("INFO", "Discarding queued file task '%s' for %s (from %s) due to server shutdown.",
                       current_task->filename, current_task->receiver_username, current_task->sender_username);
        // if (current_task->file_data_buffer) free(current_task->file_data_buffer); // Not used in simulation
        free(current_task);
        current_task = next_task;
    }
    ftm->head = ftm->tail = NULL;
    ftm->current_queue_length = 0;
    pthread_mutex_unlock(&ftm->queue_access_mutex);

    // 4. Destroy synchronization primitives.
    pthread_mutex_destroy(&ftm->queue_access_mutex);
    pthread_cond_destroy(&ftm->queue_not_empty_cond);
    sem_destroy(&ftm->available_upload_slots_sem);
    logServerEvent("INFO", "File transfer system resources released.");
}

// Handles a client's request to send a file.
// Validates the request, and if valid, adds it to the file upload queue.
void handleFileTransferRequest(ClientInfo *sender_client, const Message *file_req_header)
{
    if (!sender_client || !sender_client->is_active || !file_req_header || !g_server_state)
        return;

    ClientInfo *receiver_client_ref = NULL; // Will be populated by validateFileTransferMeta

    // Validate metadata (also logs initiation attempt).
    if (!validateFileTransferMeta(sender_client, file_req_header, &receiver_client_ref))
    {
        // Error already sent to client and/or logged by validateFileTransferMeta.
        // Send a generic MSG_FILE_TRANSFER_REJECT to ensure client gets a clear signal if specific msg wasn't sent.
        Message reject_msg;
        memset(&reject_msg, 0, sizeof(reject_msg));
        reject_msg.type = MSG_FILE_TRANSFER_REJECT;
        strncpy(reject_msg.content, "File request rejected due to validation error.", MESSAGE_BUF_SIZE - 1);
        reject_msg.content[MESSAGE_BUF_SIZE - 1] = '\0';
        strncpy(reject_msg.filename, file_req_header->filename, FILENAME_BUF_SIZE - 1);
        reject_msg.filename[FILENAME_BUF_SIZE - 1] = '\0';
        sendMessage(sender_client->socket_fd, &reject_msg);
        return;
    }
    // At this point, receiver_client_ref is valid if validation passed.

    // Check overall queue backlog (not just active processing slots).
    FileUploadQueue *ftm = &g_server_state->file_transfer_manager;
    pthread_mutex_lock(&ftm->queue_access_mutex);
    int current_total_queue_len = ftm->current_queue_length;
    pthread_mutex_unlock(&ftm->queue_access_mutex);

    const int MAX_BACKLOG_QUEUE_FILES = 50; // Arbitrary limit for total items waiting in queue.
    if (current_total_queue_len >= MAX_BACKLOG_QUEUE_FILES)
    {
        Message reject_msg;
        memset(&reject_msg, 0, sizeof(reject_msg));
        reject_msg.type = MSG_FILE_TRANSFER_REJECT;
        snprintf(reject_msg.content, MESSAGE_BUF_SIZE, "Server file backlog is full (max %d pending). Try again later.", MAX_BACKLOG_QUEUE_FILES);
        reject_msg.content[MESSAGE_BUF_SIZE - 1] = '\0';
        strncpy(reject_msg.filename, file_req_header->filename, FILENAME_BUF_SIZE - 1);
        reject_msg.filename[FILENAME_BUF_SIZE - 1] = '\0';
        sendMessage(sender_client->socket_fd, &reject_msg);
        logServerEvent("FILE-QUEUE", "File '%s' from %s rejected, server backlog full (%d items).",
                       file_req_header->filename, sender_client->username, current_total_queue_len);
        return;
    }

    // If all checks pass, the server "accepts" the file for queuing.
    // This is a simulation; no actual file data is received from client here.
    if (addFileToUploadQueue(file_req_header->filename, sender_client->username,
                             file_req_header->receiver, file_req_header->file_size))
    {
        Message accept_msg;
        memset(&accept_msg, 0, sizeof(accept_msg));
        accept_msg.type = MSG_FILE_TRANSFER_ACCEPT;
        // Client example output: "[Server]: File added to the upload queue."
        // This message confirms the server will process it.
        strncpy(accept_msg.content, "File request accepted and added to upload queue.", MESSAGE_BUF_SIZE - 1);
        accept_msg.content[MESSAGE_BUF_SIZE - 1] = '\0';
        strncpy(accept_msg.filename, file_req_header->filename, FILENAME_BUF_SIZE - 1); // Echo filename
        accept_msg.filename[FILENAME_BUF_SIZE - 1] = '\0';

        if (!sendMessage(sender_client->socket_fd, &accept_msg))
        {
            logServerEvent("ERROR", "Failed to send file transfer accept_msg to %s for '%s'. File is queued but client not notified of acceptance.",
                           sender_client->username, file_req_header->filename);
            // Task is queued, but client might be confused. Difficult to recover gracefully for this specific client.
        }
        else
        {
            // Log successful queueing after notifying client
            // logEventFileQueued is called inside addFileToUploadQueue
            // logServerEvent("INFO", "File '%s' (%zu bytes) from %s to %s. Request accepted and queued (SIMULATION).",
            //            file_req_header->filename, file_req_header->file_size, sender_client->username, file_req_header->receiver);
        }
    }
    else
    {
        // Failed to enqueue (e.g., malloc error inside addFileToUploadQueue)
        logServerEvent("ERROR", "Server internal error: failed to enqueue file '%s' from %s (SIMULATION).",
                       file_req_header->filename, sender_client->username);
        Message reject_internal_err_msg;
        memset(&reject_internal_err_msg, 0, sizeof(reject_internal_err_msg));
        reject_internal_err_msg.type = MSG_FILE_TRANSFER_REJECT;
        strncpy(reject_internal_err_msg.content, "Server internal error occurred while queueing your file.", MESSAGE_BUF_SIZE - 1);
        reject_internal_err_msg.content[MESSAGE_BUF_SIZE - 1] = '\0';
        strncpy(reject_internal_err_msg.filename, file_req_header->filename, FILENAME_BUF_SIZE - 1);
        reject_internal_err_msg.filename[FILENAME_BUF_SIZE - 1] = '\0';
        sendMessage(sender_client->socket_fd, &reject_internal_err_msg);
    }
}

// Adds a file transfer task to the upload queue.
// For simulation, actual_file_data is NULL.
// Returns 1 on success, 0 on failure (e.g., memory allocation).
int addFileToUploadQueue(const char *filename, const char *sender_user, const char *receiver_user,
                         size_t file_size_val)
{
    if (!g_server_state || !filename || !sender_user || !receiver_user)
        return 0;

    FileTransferTask *new_task = malloc(sizeof(FileTransferTask));
    if (!new_task)
    {
        logServerEvent("ERROR", "Memory allocation failed for new FileTransferTask.");
        return 0; // Memory allocation failure
    }

    memset(new_task, 0, sizeof(FileTransferTask)); // Initialize all fields
    strncpy(new_task->filename, filename, FILENAME_BUF_SIZE - 1);
    new_task->filename[FILENAME_BUF_SIZE - 1] = '\0';
    strncpy(new_task->sender_username, sender_user, USERNAME_BUF_SIZE - 1);
    new_task->sender_username[USERNAME_BUF_SIZE - 1] = '\0';
    strncpy(new_task->receiver_username, receiver_user, USERNAME_BUF_SIZE - 1);
    new_task->receiver_username[USERNAME_BUF_SIZE - 1] = '\0';
    new_task->file_size = file_size_val;
    // new_task->file_data_buffer = NULL; // For simulation, no data buffer
    new_task->enqueue_timestamp = time(NULL);
    new_task->next_task = NULL;

    FileUploadQueue *ftm = &g_server_state->file_transfer_manager;
    pthread_mutex_lock(&ftm->queue_access_mutex);
    if (ftm->tail == NULL)
    { // Queue is currently empty
        ftm->head = ftm->tail = new_task;
    }
    else
    { // Add to the end of the queue
        ftm->tail->next_task = new_task;
        ftm->tail = new_task;
    }
    ftm->current_queue_length++;
    int q_len_for_log = ftm->current_queue_length; // Capture for logging outside lock or signal

    // Signal one waiting worker thread that a new task is available
    pthread_cond_signal(&ftm->queue_not_empty_cond);
    pthread_mutex_unlock(&ftm->queue_access_mutex);

    logEventFileQueued(sender_user, filename, q_len_for_log); // Log after successfully adding
    return 1;
}

// Worker thread function for processing file transfers from the queue.
void *fileProcessingWorkerThread(void *arg)
{
    long worker_id = (long)(intptr_t)arg; // Get worker ID (index) for logging
    logServerEvent("INFO", "File worker thread (ID %ld) started.", worker_id);

    if (!g_server_state)
    {
        logServerEvent("CRITICAL", "Worker %ld: g_server_state is NULL. Exiting.", worker_id);
        return NULL;
    }
    FileUploadQueue *ftm = &g_server_state->file_transfer_manager;

    while (g_server_state->server_is_running)
    {
        // 1. Wait for an available processing slot (acquire semaphore)
        // Use sem_timedwait to periodically check server_is_running.
        struct timespec sem_timeout;
        if (clock_gettime(CLOCK_REALTIME, &sem_timeout) == -1)
        {
            logServerEvent("ERROR", "Worker %ld: clock_gettime failed for sem_timedwait. Retrying.", worker_id);
            sleep(1); // Brief pause before retrying loop
            continue;
        }
        sem_timeout.tv_sec += 1; // 1-second timeout

        int sem_ret = sem_timedwait(&ftm->available_upload_slots_sem, &sem_timeout);

        if (!g_server_state->server_is_running)
            break; // Check flag immediately after sem_timedwait returns

        if (sem_ret == -1)
        { // sem_timedwait failed
            if (errno == EINTR)
                continue; // Interrupted, loop to re-check server_is_running
            if (errno == ETIMEDOUT)
                continue; // Timeout (no slot available yet), loop to re-check
            logServerEvent("ERROR", "Worker %ld: sem_timedwait error: %s. Stopping worker.", worker_id, strerror(errno));
            break; // Critical semaphore error, stop this worker
        }
        // Successfully acquired a processing slot (semaphore decremented)

        // 2. Dequeue a task
        FileTransferTask *task_to_process = NULL;
        pthread_mutex_lock(&ftm->queue_access_mutex);
        // Wait for a task if queue is empty, but periodically check server_is_running
        while (ftm->head == NULL && g_server_state->server_is_running)
        {
            struct timespec cond_timeout;
            if (clock_gettime(CLOCK_REALTIME, &cond_timeout) == -1)
            {
                // Fallback to untimed wait if clock_gettime fails
                logServerEvent("ERROR", "Worker %ld: clock_gettime for cond_timedwait failed. Using untimed wait.", worker_id);
                pthread_cond_wait(&ftm->queue_not_empty_cond, &ftm->queue_access_mutex);
                continue; // Re-check conditions after untimed wait
            }
            cond_timeout.tv_sec += 1; // 1-second timeout for cond_timedwait

            pthread_cond_timedwait(&ftm->queue_not_empty_cond, &ftm->queue_access_mutex, &cond_timeout);
            // After timed wait (or signal), loop condition (ftm->head == NULL && g_server_state->server_is_running) re-evaluates.
        }

        if (!g_server_state->server_is_running)
        { // Re-check after wake-up or timeout
            pthread_mutex_unlock(&ftm->queue_access_mutex);
            sem_post(&ftm->available_upload_slots_sem); // Must release the acquired slot if shutting down
            break;                                      // Exit worker loop
        }

        if (ftm->head != NULL)
        { // Task is available
            task_to_process = ftm->head;
            ftm->head = ftm->head->next_task;
            if (ftm->head == NULL)
            { // Queue became empty after dequeuing
                ftm->tail = NULL;
            }
            ftm->current_queue_length--;
        }
        pthread_mutex_unlock(&ftm->queue_access_mutex);

        // 3. Process the task (if one was dequeued)
        if (task_to_process)
        {
            long wait_duration_secs = (long)difftime(time(NULL), task_to_process->enqueue_timestamp);
            logEventFileTransferProcessingStart(task_to_process->sender_username, task_to_process->filename, wait_duration_secs);

            executeFileTransferToRecipient(task_to_process); // Simulate the transfer logic

            // Free the processed task structure
            // if (task_to_process->file_data_buffer) free(task_to_process->file_data_buffer); // Not used
            free(task_to_process);
            task_to_process = NULL;

            sem_post(&ftm->available_upload_slots_sem); // Release processing slot
        }
        else
        {
            // No task found (e.g., spurious wakeup, or server shutting down and queue was empty)
            // Still need to release the semaphore slot acquired earlier.
            sem_post(&ftm->available_upload_slots_sem);
        }
    } // End of main worker loop

    logServerEvent("INFO", "File worker thread (ID %ld) stopping.", worker_id);
    return NULL;
}

// Simulates the actual transfer of a file to the recipient.
// For this project, it means notifying the recipient client.
// Handles filename collision for the recipient.
void executeFileTransferToRecipient(FileTransferTask *task)
{
    if (!task || !g_server_state)
        return;

    // Simulate processing time for the "upload"
    // As per PDF: "Design a queue-based file upload manager to simulate limited system resources"
    // This sleep simulates the "processing" of one file by a worker.
    int SIMULATED_PROCESSING_SECONDS = 2; // Keep it short for testing; PDF doesn't specify duration.
    sleep(SIMULATED_PROCESSING_SECONDS);

    ClientInfo *recipient_client_struct = NULL;
    int recipient_socket_fd = -1;                         // Local variable for socket FD
    char final_filename_for_recipient[FILENAME_BUF_SIZE]; // May be altered due to collision
    strncpy(final_filename_for_recipient, task->filename, FILENAME_BUF_SIZE - 1);
    final_filename_for_recipient[FILENAME_BUF_SIZE - 1] = '\0';

    // Find recipient and handle filename collision
    pthread_mutex_lock(&g_server_state->clients_list_mutex);                 // Lock for finding client
    recipient_client_struct = findClientByUsername(task->receiver_username); // Assumes findClient... needs lock

    if (recipient_client_struct && recipient_client_struct->is_active)
    {
        recipient_socket_fd = recipient_client_struct->socket_fd; // Get socket if recipient is active

        // Filename collision handling (Test Scenario 9)
        pthread_mutex_lock(&recipient_client_struct->received_files_lock); // Lock specific client's file list
        int collision_suffix_num = 0;
        bool name_is_unique = false;
        char temp_filename_check[FILENAME_BUF_SIZE];
        strncpy(temp_filename_check, final_filename_for_recipient, FILENAME_BUF_SIZE - 1);
        temp_filename_check[FILENAME_BUF_SIZE - 1] = '\0';

        while (!name_is_unique)
        {
            name_is_unique = true; // Assume unique for this iteration
            for (int i = 0; i < recipient_client_struct->num_received_files; ++i)
            {
                if (strcmp(recipient_client_struct->received_filenames[i], temp_filename_check) == 0)
                {
                    name_is_unique = false;
                    collision_suffix_num++;
                    // Generate new name: original_basename_suffix.ext
                    generate_collided_filename(task->filename, collision_suffix_num, temp_filename_check, sizeof(temp_filename_check));
                    break; // Break from inner loop to re-check new temp_filename_check
                }
            }
        }

        if (collision_suffix_num > 0)
        { // A new name was generated
            logEventFileCollision(task->filename, temp_filename_check, recipient_client_struct->username, task->sender_username);
            strncpy(final_filename_for_recipient, temp_filename_check, FILENAME_BUF_SIZE - 1);
            final_filename_for_recipient[FILENAME_BUF_SIZE - 1] = '\0';
        }

        // Add the final (potentially new) filename to the recipient's tracking list
        if (recipient_client_struct->num_received_files < MAX_RECEIVED_FILES_TRACKED)
        {
            strncpy(recipient_client_struct->received_filenames[recipient_client_struct->num_received_files],
                    final_filename_for_recipient, FILENAME_BUF_SIZE - 1);
            recipient_client_struct->received_filenames[recipient_client_struct->num_received_files][FILENAME_BUF_SIZE - 1] = '\0';
            recipient_client_struct->num_received_files++;
        }
        else
        {
            logServerEvent("WARNING", "Recipient %s's tracked received file list is full. Cannot track '%s' for future collision checks.",
                           recipient_client_struct->username, final_filename_for_recipient);
        }
        pthread_mutex_unlock(&recipient_client_struct->received_files_lock);
    }
    pthread_mutex_unlock(&g_server_state->clients_list_mutex); // Unlock client list

    if (recipient_socket_fd == -1)
    { // Recipient became inactive or was not found
        logEventFileTransferFailed(task->sender_username, task->receiver_username, task->filename,
                                   "Recipient offline or not found during transfer execution.");
        return;
    }

    // Prepare notification message (MSG_FILE_TRANSFER_DATA type) for the recipient
    Message file_data_notification_msg;
    memset(&file_data_notification_msg, 0, sizeof(file_data_notification_msg));
    file_data_notification_msg.type = MSG_FILE_TRANSFER_DATA; // Client handles this as file arrival
    strncpy(file_data_notification_msg.sender, task->sender_username, USERNAME_BUF_SIZE - 1);
    file_data_notification_msg.sender[USERNAME_BUF_SIZE - 1] = '\0';
    strncpy(file_data_notification_msg.receiver, task->receiver_username, USERNAME_BUF_SIZE - 1); // For recipient's context
    file_data_notification_msg.receiver[USERNAME_BUF_SIZE - 1] = '\0';
    strncpy(file_data_notification_msg.filename, final_filename_for_recipient, FILENAME_BUF_SIZE - 1); // Use final (potentially renamed) name
    file_data_notification_msg.filename[FILENAME_BUF_SIZE - 1] = '\0';
    file_data_notification_msg.file_size = task->file_size;

    // SIMULATION: No actual file data chunks are sent. Just this notification message.
    if (!sendMessage(recipient_socket_fd, &file_data_notification_msg))
    {
        logEventFileTransferFailed(task->sender_username, task->receiver_username, final_filename_for_recipient,
                                   "Failed to send file arrival notification to recipient's socket.");
        // Note: Recipient might have disconnected just as we tried to send.
        // Their client_handler thread should manage their cleanup.
    }
    else
    {
        // If send was successful, log completion.
        logEventFileTransferCompleted(task->sender_username, task->receiver_username, final_filename_for_recipient);
    }
}