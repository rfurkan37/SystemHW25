#include "common.h"
#include <stdarg.h> // For va_list, va_start, va_end
#include <fcntl.h>  // For open flags
#include <sys/stat.h> // For open modes

static int log_file_fd = -1;
static pthread_mutex_t server_log_mutex = PTHREAD_MUTEX_INITIALIZER;

int initialize_server_logging(const char* log_filename) {
    // Open with O_APPEND to add to existing log, O_CREAT to create if not exists
    // O_WRONLY for write-only access.
    // Mode 0644: user can read/write, group/others can read.
    log_file_fd = open(log_filename, O_WRONLY | O_CREAT | O_APPEND, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
    if (log_file_fd < 0) {
        perror("CRITICAL: Failed to open server log file");
        // Depending on policy, server might exit or try to run without file logging.
        // For this project, failing to open log is critical.
        return 0; // Failure
    }
    log_server_event("INFO", "Server logging system initialized. Log file: %s", log_filename);
    return 1; // Success
}

void finalize_server_logging(void) {
    log_server_event("INFO", "Server logging system shutting down.");
    if (log_file_fd >= 0) {
        // fsync(log_file_fd); // Ensure all buffered data is written (optional, close usually handles)
        close(log_file_fd);
        log_file_fd = -1;
    }
    pthread_mutex_destroy(&server_log_mutex); // Clean up mutex
}

// Generic logging function, thread-safe
void log_server_event(const char* tag, const char* details_format, ...) {
    char log_entry_buffer[1500]; // Reasonably sized buffer for log entries
    char timestamp_buffer[64];
    time_t now;
    struct tm *local_time_info;

    time(&now);
    local_time_info = localtime(&now);

    // Format timestamp: YYYY-MM-DD HH:MM:SS
    strftime(timestamp_buffer, sizeof(timestamp_buffer), "%Y-%m-%d %H:%M:%S", local_time_info);

    // Start building the log entry: "TIMESTAMP - [TAG] "
    int current_len = snprintf(log_entry_buffer, sizeof(log_entry_buffer), "%s - [%s] ", timestamp_buffer, tag);

    // Append the variable part of the message
    va_list args;
    va_start(args, details_format);
    // Check if there's enough space before vsnprintf
    if (current_len < (int)sizeof(log_entry_buffer)) {
         vsnprintf(log_entry_buffer + current_len, sizeof(log_entry_buffer) - current_len, details_format, args);
    }
    va_end(args);

    // Ensure newline at the end
    // Find end of string, append newline if not present and space allows
    current_len = strlen(log_entry_buffer);
    if (current_len > 0 && current_len < (int)sizeof(log_entry_buffer) -1 && log_entry_buffer[current_len-1] != '\n') {
        log_entry_buffer[current_len] = '\n';
        log_entry_buffer[current_len+1] = '\0';
    }


    pthread_mutex_lock(&server_log_mutex);
    // Write to console
    printf("%s", log_entry_buffer); 
    fflush(stdout); // Ensure console output is immediate

    // Write to log file if open
    if (log_file_fd >= 0) {
        ssize_t bytes_written = write(log_file_fd, log_entry_buffer, strlen(log_entry_buffer));
        if (bytes_written < 0) {
            // perror("Error writing to server log file"); // Avoid recursive logging calls
            fprintf(stderr, "Error writing to server log file: %s\n", strerror(errno));
        } else {
             fsync(log_file_fd); // Ensure critical logs are written to disk
        }
    }
    pthread_mutex_unlock(&server_log_mutex);
}


// --- Specific Log Event Helpers ---
// These functions format messages according to PDF examples and call log_server_event.

void log_event_server_start(int port) {
    // PDF example: [INFO] Server listening on port 5000...
    log_server_event("INFO", "Server listening on port %d...", port);
}

void log_event_client_connected(const char* username, const char* ip_address) {
    // PDF examples:
    // [CONNECT] New client connected: emre2025 from 192.168.1.104 (initial connection, username might not be known yet)
    // [LOGIN] user 'john45' connected from 192.168.1.44 (after successful login)
    // This function is called *after* successful login, so we use the [LOGIN] format.
    log_server_event("LOGIN", "user '%s' connected from %s", username, ip_address);
}

void log_event_client_disconnected(const char* username, int is_unexpected) {
    if (is_unexpected) {
        // PDF: [DISCONNECT] user 'mehmet1' lost connection. Cleaned up resources.
        log_server_event("DISCONNECT", "user '%s' lost connection. Cleaned up resources.", username);
    } else {
        // PDF: [DISCONNECT] Client emre2025 disconnected.
        log_server_event("DISCONNECT", "Client %s disconnected.", username);
    }
}

void log_event_client_login_failed(const char* username_attempted, const char* ip_addr, const char* reason) {
    if (strstr(reason, "Duplicate") || strstr(reason, "already taken")) {
        // PDF: [REJECTED] Duplicate username attempted: ali34
        log_server_event("REJECTED", "Duplicate username attempted: %s (from %s)", username_attempted, ip_addr);
    } else {
        // Generic login failure
        log_server_event("LOGIN_FAIL", "Login failed for '%s' (from %s): %s", username_attempted, ip_addr, reason);
    }
}

void log_event_room_created(const char* room_name) {
    log_server_event("ROOM_MGMT", "Room '%s' created.", room_name);
}

void log_event_client_joined_room(const char* username, const char* room_name) {
    // PDF (page 4): [COMMAND] emre2025 joined room 'teamchat'
    // PDF (page 3): [JOIN] user 'john45' joined room 'team1'
    // Using [COMMAND] format as it's more generic for user actions.
    log_server_event("COMMAND", "%s joined room '%s'", username, room_name);
}

void log_event_client_left_room(const char* username, const char* room_name) {
    // Implied by room switching log, or just a generic command.
    log_server_event("COMMAND", "%s left room '%s'", username, room_name);
}

void log_event_client_switched_room(const char* username, const char* old_room, const char* new_room) {
    // PDF: [ROOM] user 'irem56' left room 'groupA', joined 'groupB'
    log_server_event("ROOM", "user '%s' left room '%s', joined '%s'", username, old_room, new_room);
}

void log_event_broadcast(const char* sender_username, const char* room_name, const char* message_preview) {
    // PDF: [COMMAND] emre2025 broadcasted to 'teamchat'
    // PDF: [BROADCAST] ali34: Hello all (message content included, also a test scenario example)
    // We'll use the COMMAND format for action, content can be in preview.
    // A truncated preview to avoid overly long log lines.
    char preview[51]; // 50 chars + null
    strncpy(preview, message_preview, 50);
    preview[50] = '\0';
    if (strlen(message_preview) > 50) strcat(preview, "...");
    log_server_event("COMMAND", "%s broadcasted to '%s' (msg: \"%s\")", sender_username, room_name, preview);
}

void log_event_whisper(const char* sender_username, const char* receiver_username) {
    // PDF: [COMMAND] emre2025 sent whisper to john42 (message content not in this specific log example)
    log_server_event("COMMAND", "%s sent whisper to %s", sender_username, receiver_username);
}

void log_event_file_transfer_initiated(const char* sender_username, const char* receiver_username, const char* filename) {
    // PDF: [COMMAND] emre2025 initiated file transfer to john42 (filename not in this particular log example)
    // PDF: [SEND FILE] 'project.pdf' sent from john45 to alice99 (success) (This one is more specific and seems client-side or success log)
    // The COMMAND log seems appropriate for the server action of *receiving* the initiation.
    log_server_event("COMMAND", "%s initiated file transfer of '%s' to %s", sender_username, filename, receiver_username);
}

void log_event_file_queued(const char* sender_username, const char* filename, int current_q_size) {
    // PDF: [FILE-QUEUE] Upload 'project.pdf' from emre02 added to queue. Queue size: 5
    log_server_event("FILE-QUEUE", "Upload '%s' from %s added to queue. Queue size: %d", filename, sender_username, current_q_size);
}

void log_event_file_rejected_oversized(const char* sender_username, const char* filename, size_t attempted_size) {
    // PDF: [ERROR] File 'huge_data.zip' from user 'melis22' exceeds size limit.
    log_server_event("ERROR", "File '%s' from user '%s' exceeds size limit (attempted: %zu bytes, max: %d bytes).", 
                     filename, sender_username, attempted_size, MAX_FILE_SIZE);
}

void log_event_file_transfer_processing_start(const char* sender_username, const char* filename, long wait_time_seconds) {
    // PDF: [FILE] 'code.zip' from user 'berkay98' started upload after 14 seconds in queue.
    log_server_event("FILE", "'%s' from user '%s' started processing after %ld seconds in queue.", 
                     filename, sender_username, wait_time_seconds);
}

void log_event_file_transfer_completed(const char* sender_username, const char* receiver_username, const char* filename) {
     // This is a server-side log indicating successful transmission to the recipient client.
     // The client-side /sendfile success log: [SEND FILE] 'project.pdf' sent from john45 to alice99 (success)
     // This log can be similar but from server perspective.
    log_server_event("FILE", "Successfully transferred '%s' from %s to %s.",
                     filename, sender_username, receiver_username);
}

void log_event_file_transfer_failed(const char* sender_username, const char* receiver_username, const char* filename, const char* reason) {
    log_server_event("FILE_ERROR", "Failed to transfer '%s' from %s to %s. Reason: %s",
                     filename, sender_username, receiver_username, reason);
}


void log_event_sigint_shutdown(int num_clients_at_shutdown) {
    // PDF: [SHUTDOWN] SIGINT received. Disconnecting 12 clients, saving logs.
    log_server_event("SHUTDOWN", "SIGINT received. Disconnecting %d client(s), saving logs.", num_clients_at_shutdown);
}