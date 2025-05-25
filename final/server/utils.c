#include "common.h"

int is_valid_username(const char* username) {
    if (!username || strlen(username) == 0 || strlen(username) > MAX_USERNAME_LEN) {
        return 0;
    }
    
    for (int i = 0; username[i]; i++) {
        if (!isalnum(username[i])) {
            return 0;
        }
    }
    return 1;
}

int is_valid_room_name(const char* room_name) {
    if (!room_name || strlen(room_name) == 0 || strlen(room_name) > MAX_ROOM_NAME_LEN) {
        return 0;
    }
    
    for (int i = 0; room_name[i]; i++) {
        if (!isalnum(room_name[i])) {
            return 0;
        }
    }
    return 1;
}

int is_valid_file_type(const char* filename) {
    if (!filename) return 0;
    
    const char* ext = strrchr(filename, '.');
    if (!ext) return 0;
    
    return (strcmp(ext, ".txt") == 0 || strcmp(ext, ".pdf") == 0 ||
            strcmp(ext, ".jpg") == 0 || strcmp(ext, ".png") == 0);
}

int send_message(int socket_fd, message_t* msg) {
    ssize_t bytes_sent = send(socket_fd, msg, sizeof(message_t), 0);
    return (bytes_sent == sizeof(message_t)) ? 1 : 0;
}

int receive_message(int socket_fd, message_t* msg) {
    ssize_t bytes_received = recv(socket_fd, msg, sizeof(message_t), 0);
    return (bytes_received == sizeof(message_t)) ? 1 : 0;
}

void send_error_message(int socket_fd, const char* error) {
    message_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = MSG_ERROR;
    strcpy(msg.sender, "SERVER");
    strncpy(msg.content, error, MAX_MESSAGE_LEN - 1);
    msg.content[MAX_MESSAGE_LEN - 1] = '\0';
    
    send_message(socket_fd, &msg);
}

void send_success_message(int socket_fd, const char* message) {
    message_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = MSG_SUCCESS;
    strcpy(msg.sender, "SERVER");
    strncpy(msg.content, message, MAX_MESSAGE_LEN - 1);
    msg.content[MAX_MESSAGE_LEN - 1] = '\0';
    
    send_message(socket_fd, &msg);
}

client_t* find_client_by_username(const char* username) {
    pthread_mutex_lock(&g_server_state->clients_mutex);
    
    client_t* found_client = NULL;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (g_server_state->clients[i] && 
            strcmp(g_server_state->clients[i]->username, username) == 0) {
            found_client = g_server_state->clients[i];
            break;
        }
    }
    
    pthread_mutex_unlock(&g_server_state->clients_mutex);
    return found_client;
}

void broadcast_shutdown_message(void) {
    message_t shutdown_msg;
    memset(&shutdown_msg, 0, sizeof(shutdown_msg));
    shutdown_msg.type = MSG_ERROR;
    strcpy(shutdown_msg.sender, "SERVER");
    strcpy(shutdown_msg.content, "Server is shutting down");
    
    pthread_mutex_lock(&g_server_state->clients_mutex);
    
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (g_server_state->clients[i]) {
            send_message(g_server_state->clients[i]->socket_fd, &shutdown_msg);
        }
    }
    
    pthread_mutex_unlock(&g_server_state->clients_mutex);
} 