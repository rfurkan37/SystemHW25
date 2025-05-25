#include "common.h"

pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
int log_fd = -1;

void init_logging(void) {
    log_fd = open("server.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (log_fd < 0) {
        perror("Failed to open log file");
        exit(1);
    }
    
    // Log startup message
    log_message("INIT", "Logging system initialized");
}

void log_message(const char* type, const char* message) {
    pthread_mutex_lock(&log_mutex);
    
    time_t now = time(NULL);
    struct tm* local_time = localtime(&now);
    char timestamp[64];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", local_time);
    
    // Format log message
    char log_buffer[1024];
    int log_len = snprintf(log_buffer, sizeof(log_buffer), "%s - [%s] %s\n", timestamp, type, message);
    
    if (log_fd >= 0 && log_len > 0) {
        ssize_t written = write(log_fd, log_buffer, log_len);
        if (written > 0) {
            fsync(log_fd);  // Force write to disk (equivalent to fflush)
        }
    }
    
    // Also print to console
    printf("%s - [%s] %s\n", timestamp, type, message);
    fflush(stdout);
    
    pthread_mutex_unlock(&log_mutex);
}

void log_connection(client_t* client) {
    char buffer[512];
    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &(client->address.sin_addr), ip_str, INET_ADDRSTRLEN);
    snprintf(buffer, sizeof(buffer), "User '%s' connected from %s:%d", 
             client->username, 
             ip_str,
             ntohs(client->address.sin_port));
    log_message("CONNECT", buffer);
}

void log_disconnection(client_t* client) {
    char buffer[512];
    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &(client->address.sin_addr), ip_str, INET_ADDRSTRLEN);
    snprintf(buffer, sizeof(buffer), "User '%s' disconnected from %s:%d", 
             client->username, 
             ip_str,
             ntohs(client->address.sin_port));
    log_message("DISCONNECT", buffer);
}

void log_room_join(client_t* client, const char* room_name) {
    char buffer[512];
    snprintf(buffer, sizeof(buffer), "User '%s' joined room '%s'", 
             client->username, room_name);
    log_message("ROOM", buffer);
}

void log_room_leave(client_t* client, const char* room_name) {
    char buffer[512];
    snprintf(buffer, sizeof(buffer), "User '%s' left room '%s'", 
             client->username, room_name);
    log_message("ROOM", buffer);
}

void log_failed_login(const char* username, const char* reason) {
    char buffer[512];
    snprintf(buffer, sizeof(buffer), "Failed login attempt: username '%s' - %s", 
             username, reason);
    log_message("LOGIN", buffer);
}

void log_whisper(const char* sender, const char* receiver, const char* message) {
    char buffer[512];
    snprintf(buffer, sizeof(buffer), "Whisper from '%s' to '%s': %s", 
             sender, receiver, message);
    log_message("WHISPER", buffer);
}

void log_broadcast(const char* sender, const char* room, const char* message) {
    char buffer[512];
    snprintf(buffer, sizeof(buffer), "Broadcast from '%s' in room '%s': %s", 
             sender, room, message);
    log_message("BROADCAST", buffer);
}

void log_file_queued(const char* filename, const char* sender, int queue_size) {
    char buffer[512];
    snprintf(buffer, sizeof(buffer), "File '%s' from '%s' queued (queue size: %d)", 
             filename, sender, queue_size);
    log_message("FILE", buffer);
}

void cleanup_logging(void) {
    if (log_fd >= 0) {
        close(log_fd);
        log_fd = -1;
    }
    
    // Destroy the log mutex for complete cleanup
    pthread_mutex_destroy(&log_mutex);
}

