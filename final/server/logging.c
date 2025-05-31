#include "common.h"
#include <stdarg.h>  // For va_list, va_start, va_end, vsnprintf
#include <fcntl.h>   // For open flags (O_WRONLY, O_CREAT, O_APPEND)
#include <unistd.h>  // For write, close, fsync
#include <pthread.h> // For pthread_mutex_t

static int log_file_fd = -1;                                         // File descriptor for the log file
static pthread_mutex_t server_log_mutex = PTHREAD_MUTEX_INITIALIZER; // Mutex for thread-safe logging

// Initializes the server logging system.
// Opens the specified log file in append mode.
// Returns 1 on success, 0 on failure.
int initializeServerLogging(const char *log_filename)
{
    // Open in append mode, create if it doesn't exist.
    // Permissions: Read/Write for owner, Read for group, Read for others.
    log_file_fd = open(log_filename, O_WRONLY | O_CREAT | O_APPEND, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
    if (log_file_fd < 0)
    {
        perror("CRITICAL: Failed to open server log file");
        // Logging is not possible if this fails.
        return 0; // Failure
    }

    // Log an initial message to confirm the logging system is operational.
    // This message is written directly as logServerEvent might not be fully safe yet (though mutex is static).
    char init_msg_buf[300];
    time_t now_init_time = time(NULL);
    struct tm *local_time_struct_init = localtime(&now_init_time);

    int len = strftime(init_msg_buf, sizeof(init_msg_buf), "%Y-%m-%d %H:%M:%S - [INFO] Server logging system initialized. Log file: ", local_time_struct_init);
    if (len > 0 && len + strlen(log_filename) + 2 < sizeof(init_msg_buf))
    { // +2 for newline and null
        strcat(init_msg_buf, log_filename);
        strcat(init_msg_buf, "\n");
    }
    else
    {
        // Fallback if buffer too small (should not happen with 300 bytes)
        snprintf(init_msg_buf, sizeof(init_msg_buf), "%s - [INFO] Server logging system initialized.\n", SERVER_LOG_FILENAME);
    }

    // Write initial message to both stdout and the log file
    if (write(STDOUT_FILENO, init_msg_buf, strlen(init_msg_buf)) < 0)
    { /* Ignore error */
    };
    if (log_file_fd >= 0)
    { // Check again, just in case
        if (write(log_file_fd, init_msg_buf, strlen(init_msg_buf)) < 0)
        { /* Ignore error */
        };
    }
    return 1; // Success
}

// Finalizes the server logging system.
// Flushes any buffered data and closes the log file.
void finalizeServerLogging()
{
    logServerEvent("INFO", "Server logging system shutting down."); // Log shutdown attempt

    pthread_mutex_lock(&server_log_mutex); // Ensure exclusive access for final operations
    if (log_file_fd >= 0)
    {
        if (fsync(log_file_fd) == -1)
        { // Ensure all data is physically written to disk
            perror("Warning: Failed to fsync log file during finalization");
        }
        if (close(log_file_fd) == -1)
        {
            perror("Warning: Failed to close log file during finalization");
        }
        log_file_fd = -1; // Mark as closed
    }
    pthread_mutex_unlock(&server_log_mutex);

    // Destroy the mutex. It's generally safe to destroy an unlocked mutex.
    // Ensure no threads are trying to log after this point.
    if (pthread_mutex_destroy(&server_log_mutex) != 0)
    {
        // perror("Warning: Failed to destroy server log mutex");
        // This can happen if the mutex is still locked, indicating a problem elsewhere.
    }
}

// Generic thread-safe logging function.
// Prepends timestamp and tag, appends newline.
void logServerEvent(const char *tag, const char *details_format, ...)
{
    if (log_file_fd == -1 && g_server_state && !g_server_state->server_is_running)
    {
        // If log_file_fd is already closed (e.g. during finalization) and server is shutting down,
        // try to print to stderr as a last resort for critical shutdown messages.
        // This check is a bit heuristic.
        char temp_stderr_buf[1024];
        va_list args_stderr;
        va_start(args_stderr, details_format);
        vsnprintf(temp_stderr_buf, sizeof(temp_stderr_buf), details_format, args_stderr);
        va_end(args_stderr);
        fprintf(stderr, "FALLBACK LOG (File Closed) - [%s] %s\n", tag, temp_stderr_buf);
        return;
    }

    if (log_file_fd < 0)
    {
        // Log file not initialized or already closed, and not in the specific shutdown fallback case above.
        // Avoid fprintf to stderr here to prevent recursive errors if stderr itself is problematic.
        // Could write to a string and then use 'write' to stderr for more safety.
        // For now, if log_file_fd is invalid, we silently drop the log unless it's the specific shutdown case.
        return;
    }

    char log_entry_buffer[1536]; // Buffer for the complete log entry
    char timestamp_buffer[64];   // Buffer for the formatted timestamp
    time_t now_time = time(NULL);
    struct tm *local_time_info = localtime(&now_time);

    // Format timestamp: YYYY-MM-DD HH:MM:SS
    strftime(timestamp_buffer, sizeof(timestamp_buffer), "%Y-%m-%d %H:%M:%S", local_time_info);

    // Construct the log entry: "TIMESTAMP - [TAG] "
    int current_len = snprintf(log_entry_buffer, sizeof(log_entry_buffer), "%s - [%s] ", timestamp_buffer, tag);

    // Append the variable arguments part using vsnprintf
    if (current_len > 0 && current_len < (int)sizeof(log_entry_buffer))
    {
        va_list args;
        va_start(args, details_format);
        vsnprintf(log_entry_buffer + current_len, sizeof(log_entry_buffer) - current_len, details_format, args);
        va_end(args);
    }

    // Ensure the log entry ends with a newline and is null-terminated
    current_len = strlen(log_entry_buffer); // Recalculate length
    if (current_len < (int)sizeof(log_entry_buffer) - 1)
    {
        log_entry_buffer[current_len] = '\n';
        log_entry_buffer[current_len + 1] = '\0';
    }
    else
    {
        // Buffer was too small, or exactly full. Ensure newline and null termination at the end.
        log_entry_buffer[sizeof(log_entry_buffer) - 2] = '\n';
        log_entry_buffer[sizeof(log_entry_buffer) - 1] = '\0';
    }

    // Lock mutex for thread-safe writing to console and file
    pthread_mutex_lock(&server_log_mutex);

    // Write to console (STDOUT_FILENO for potentially unbuffered terminal output)
    // We use write() directly to avoid stdio buffering issues, especially with threads.
    if (write(STDOUT_FILENO, log_entry_buffer, strlen(log_entry_buffer)) < 0)
    {
        // Error writing to stdout; non-critical, proceed to file logging.
    }

    // Write to log file if it's open
    if (log_file_fd >= 0)
    {
        if (write(log_file_fd, log_entry_buffer, strlen(log_entry_buffer)) < 0)
        {
            char error_msg[256];
            // Print only the start of log_entry_buffer to avoid truncation in error_msg
            snprintf(error_msg, sizeof(error_msg),
                     "CRITICAL: Error writing to server log file (fd %d): %s. Failed entry (first 100 chars): %.100s\n",
                     log_file_fd, strerror(errno), log_entry_buffer);
            error_msg[sizeof(error_msg) - 1] = '\0'; // Ensure null termination
            if (write(STDERR_FILENO, error_msg, strlen(error_msg)) < 0)
            { /* Last resort failed */
            }
        }
    }
    pthread_mutex_unlock(&server_log_mutex);
}

// --- Specific Event Logging Functions ---
// These functions call logServerEvent with predefined tags and formats
// to match the PDF examples and ensure consistency.

void logEventServerStart(int port)
{
    logServerEvent("INFO", "Server listening on port %d...", port);
}

// Example: [CONNECT] user 'ali34' connected (from client test) -> [CONNECT] user 'ali34' connected from 192.168.1.104
void logEventClientConnected(const char *username, const char *ip_address)
{
    logServerEvent("CONNECT", "New client connected: %s from %s", username, ip_address); // Matches example more closely
}

// Example: [DISCONNECT] Client emre2025 disconnected. OR [DISCONNECT] user 'mehmet1' lost connection. Cleaned up resources.
void logEventClientDisconnected(const char *username, int is_unexpected)
{
    if (is_unexpected)
    {
        logServerEvent("DISCONNECT", "user '%s' lost connection. Cleaned up resources.", username);
    }
    else
    {
        // Example: [DISCONNECT] Client emre2025 disconnected.
        logServerEvent("DISCONNECT", "Client %s disconnected.", username);
    }
}

// Example: [REJECTED] Duplicate username attempted: ali34
void logEventClientLoginFailed(const char *username_attempted, const char *ip_addr, const char *reason)
{
    if (strstr(reason, "Duplicate username") || strstr(reason, "already taken"))
    {
        logServerEvent("REJECTED", "Duplicate username attempted: %s", username_attempted);
    }
    else if (strstr(reason, "Invalid username"))
    { // E.g. invalid format
        logServerEvent("REJECTED", "Invalid username format attempt: %s (from %s)", username_attempted, ip_addr);
    }
    else
    { // Generic login failure
        logServerEvent("LOGIN_FAIL", "Login failed for user '%s' from %s. Reason: %s", username_attempted, ip_addr, reason);
    }
}

void logEventRoomCreated(const char *room_name)
{
    // No direct PDF example for "Room Created" tag, using "INFO" or custom like "ROOM_MGMT"
    logServerEvent("INFO", "Room '%s' was created.", room_name);
}

// Example: [COMMAND] emre2025 joined room 'teamchat'
// Example: [INFO] ali34 joined room 'project1' (from client test)
void logEventClientJoinedRoom(const char *username, const char *room_name)
{
    // Using COMMAND tag to match one of the examples explicitly.
    logServerEvent("COMMAND", "%s joined room '%s'", username, room_name);
}

void logEventClientLeftRoom(const char *username, const char *room_name)
{
    // No direct "left room" example tag, but follows "joined room" pattern.
    logServerEvent("COMMAND", "%s left room '%s'", username, room_name);
}

// Example: [ROOM] user 'irem56' left room 'groupA', joined 'groupB'
void logEventClientSwitchedRoom(const char *username, const char *old_room, const char *new_room)
{
    logServerEvent("ROOM", "user '%s' left room '%s', joined '%s'", username, old_room, new_room);
}

// Example: [COMMAND] emre2025 broadcasted to 'teamchat'
// Example: [BROADCAST] ali34: Hello all (from client test)
void logEventBroadcast(const char *sender_username, const char *room_name, const char *message_content)
{
    (void)message_content; // The example logs "broadcasted to 'room_name'", not the content itself for this tag.
    logServerEvent("COMMAND", "%s broadcasted to '%s'", sender_username, room_name);
    // For the other example type "[BROADCAST] user: content", a different log call or tag might be used
    // if server needs to log the content of broadcasts directly.
    // For now, sticking to one of the PDF examples.
}

// Example: [COMMAND] emre2025 sent whisper to john42
void logEventWhisper(const char *sender_username, const char *receiver_username, const char *message_preview)
{
    (void)message_preview; // Message content/preview is not in the PDF log example for this tag.
    logServerEvent("COMMAND", "%s sent whisper to %s", sender_username, receiver_username);
}

// Example: [COMMAND] emre2025 initiated file transfer to john42 (filename not explicitly shown in this example line)
void logEventFileTransferInitiated(const char *sender_username, const char *receiver_username, const char *filename)
{
    // Adding filename for more clarity, though PDF example for "COMMAND" tag omits it for file transfer.
    logServerEvent("COMMAND", "%s initiated file transfer of '%s' to %s", sender_username, filename, receiver_username);
}

// Example: [FILE-QUEUE] Upload 'project.pdf' from emre02 added to queue. Queue size: 5
void logEventFileQueued(const char *sender_username, const char *filename, int current_q_size)
{
    logServerEvent("FILE-QUEUE", "Upload '%s' from %s added to queue. Queue size: %d", filename, sender_username, current_q_size);
}

// Example: [ERROR] File 'huge_data.zip' from user 'melis22' exceeds size limit.
void logEventFileRejectedOversized(const char *sender_username, const char *filename, size_t attempted_size)
{
    (void)attempted_size; // The PDF log example does not include the actual size, only that it exceeds limit.
    logServerEvent("ERROR", "File '%s' from user '%s' exceeds size limit.", filename, sender_username);
}

// Example: [FILE] 'code.zip' from user 'berkay98' started upload after 14 seconds in queue.
void logEventFileTransferProcessingStart(const char *sender_username, const char *filename, long wait_time_seconds)
{
    logServerEvent("FILE", "'%s' from user '%s' started upload after %ld seconds in queue.",
                   filename, sender_username, wait_time_seconds);
}

// PDF Example page 3: 2025-05-18 14:03:22 - [SEND FILE] 'project.pdf' sent from john45 to alice99 (success)
// Server console example page 4: [FILE] alice12 sent file 'project.pdf' to john
// Let's pick one for consistency, the page 4 one seems more aligned with other [TAG] formats.
void logEventFileTransferCompleted(const char *sender_username, const char *receiver_username, const char *filename)
{
    logServerEvent("FILE", "%s sent file '%s' to %s", sender_username, filename, receiver_username);
}

void logEventFileTransferFailed(const char *sender_username, const char *receiver_username, const char *filename, const char *reason)
{
    // No direct PDF example for this, creating a reasonable one.
    logServerEvent("FILE_ERROR", "File transfer of '%s' from %s to %s failed. Reason: %s",
                   filename, sender_username, receiver_username, reason);
}

// Example: [FILE] Conflict: 'project.pdf' received twice -> renamed 'project_1.pdf' (recipient context is implied/not in log)
// Modifying to include recipient for clarity if needed, but sticking to example.
void logEventFileCollision(const char *original_name, const char *new_name, const char *recipient_user, const char *sender_user)
{
    (void)recipient_user; // PDF example does not explicitly state recipient in this log line.
    (void)sender_user;    // Nor the sender.
    logServerEvent("FILE", "Conflict: '%s' received twice -> renamed '%s'", original_name, new_name);
}

// Example: [SHUTDOWN] SIGINT received. Disconnecting 12 clients, saving logs.
void logEventSigintShutdown(int num_clients_at_shutdown)
{
    logServerEvent("SHUTDOWN", "SIGINT received. Disconnecting %d clients, saving logs.", num_clients_at_shutdown);
}