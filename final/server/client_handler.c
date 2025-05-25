#include "common.h"

client_t* create_client(int socket_fd, struct sockaddr_in address) {
    client_t* client = malloc(sizeof(client_t));
    if (!client) {
        return NULL;
    }
    
    client->socket_fd = socket_fd;
    client->address = address;
    client->is_active = 1;
    memset(client->username, 0, sizeof(client->username));
    memset(client->current_room, 0, sizeof(client->current_room));
    
    return client;
}

void cleanup_client(client_t* client) {
    if (client) {
        close(client->socket_fd);
        free(client);
    }
}

int handle_login(client_t* client) {
    message_t msg;
    
    // Receive login message
    if (receive_message(client->socket_fd, &msg) <= 0) {
        return 0;
    }
    
    if (msg.type != MSG_LOGIN) {
        send_error_message(client->socket_fd, "Expected login message");
        return 0;
    }
    
    // Validate username
    if (!is_valid_username(msg.sender)) {
        send_error_message(client->socket_fd, "Invalid username format");
        return 0;
    }
    
    // Check if username is already taken
    pthread_mutex_lock(&g_server_state->clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (g_server_state->clients[i] && 
            strcmp(g_server_state->clients[i]->username, msg.sender) == 0) {
            pthread_mutex_unlock(&g_server_state->clients_mutex);
            send_error_message(client->socket_fd, "Username already taken");
            return 0;
        }
    }
    
    // Add client to server
    int slot = -1;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!g_server_state->clients[i]) {
            slot = i;
            break;
        }
    }
    
    if (slot == -1) {
        pthread_mutex_unlock(&g_server_state->clients_mutex);
        send_error_message(client->socket_fd, "Server is full");
        return 0;
    }
    
    strcpy(client->username, msg.sender);
    g_server_state->clients[slot] = client;
    g_server_state->client_count++;
    
    pthread_mutex_unlock(&g_server_state->clients_mutex);
    
    send_success_message(client->socket_fd, "Login successful");
    return 1;
}

void process_client_message(client_t* client, message_t* msg) {
    switch (msg->type) {
        case MSG_JOIN_ROOM:
            handle_join_room(client, msg->room);
            break;
            
        case MSG_LEAVE_ROOM:
            handle_leave_room(client);
            break;
            
        case MSG_BROADCAST:
            handle_broadcast(client, msg->content);
            break;
            
        case MSG_WHISPER:
            handle_whisper(client, msg->receiver, msg->content);
            break;
            
        case MSG_FILE_TRANSFER:
            handle_file_transfer(client, msg);
            break;
            
        case MSG_DISCONNECT:
            client->is_active = 0;
            break;
            
        default:
            send_error_message(client->socket_fd, "Unknown command");
            break;
    }
}

void remove_client_from_server(client_t* client) {
    pthread_mutex_lock(&g_server_state->clients_mutex);
    
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (g_server_state->clients[i] == client) {
            g_server_state->clients[i] = NULL;
            g_server_state->client_count--;
            break;
        }
    }
    
    pthread_mutex_unlock(&g_server_state->clients_mutex);
}

void* client_handler(void* arg) {
    client_t* client = (client_t*)arg;
    message_t message;
    
    // Handle username registration
    if (!handle_login(client)) {
        cleanup_client(client);
        return NULL;
    }
    
    log_connection(client);
    
    // Main message loop
    while (g_server_state->running && client->is_active) {
        if (receive_message(client->socket_fd, &message) <= 0) {
            break; // Client disconnected
        }
        
        process_client_message(client, &message);
    }
    
    // Cleanup
    log_disconnection(client);
    remove_client_from_room(client);
    remove_client_from_server(client);
    cleanup_client(client);
    
    return NULL;
} 