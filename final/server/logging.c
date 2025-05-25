#include "common.h"

pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
FILE* log_file = NULL;

void init_logging(void) {
    log_file = fopen("server.log", "a");
    if (!log_file) {
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
    
    if (log_file) {
        fprintf(log_file, "%s - [%s] %s\n", timestamp, type, message);
        fflush(log_file);
    }
    
    // Also print to console
    printf("%s - [%s] %s\n", timestamp, type, message);
    fflush(stdout);
    
    pthread_mutex_unlock(&log_mutex);
}

void log_connection(client_t* client) {
    char buffer[512];
    snprintf(buffer, sizeof(buffer), "User '%s' connected from %s:%d", 
             client->username, 
             inet_ntoa(client->address.sin_addr),
             ntohs(client->address.sin_port));
    log_message("CONNECT", buffer);
}

void log_disconnection(client_t* client) {
    char buffer[512];
    snprintf(buffer, sizeof(buffer), "User '%s' disconnected from %s:%d", 
             client->username, 
             inet_ntoa(client->address.sin_addr),
             ntohs(client->address.sin_port));
    log_message("DISCONNECT", buffer);
}

void log_room_join(client_t* client, const char* room_name) {
    char buffer[512];
    snprintf(buffer, sizeof(buffer), "User '%s' joined room '%s'", 
             client->username, room_name);
    log_message("ROOM", buffer);
}

void log_file_queued(const char* filename, const char* sender, int queue_size) {
    char buffer[512];
    snprintf(buffer, sizeof(buffer), "File '%s' from '%s' queued (queue size: %d)", 
             filename, sender, queue_size);
    log_message("FILE", buffer);
} 