#include "common.h"

// Finds a client by username. Used for whispers, file transfers.
// NOTE: The caller must handle locking/unlocking g_server_state->clients_list_mutex
// if this function is called in a context where the list might change.
// However, for read-only operations like find, if the pointer is copied quickly,
// it might be acceptable for some use cases. For safety, lock before calling.
client_info_t* find_client_by_username(const char *username_to_find) {
    if (!username_to_find) return NULL;

    // No lock here, assuming caller handles or it's safe enough for a quick read.
    // For higher concurrency safety, a lock would be needed around the loop.
    // pthread_mutex_lock(&g_server_state->clients_list_mutex);
    for (int i = 0; i < MAX_SERVER_CLIENTS; i++) {
        if (g_server_state->connected_clients[i] != NULL &&
            g_server_state->connected_clients[i]->is_active &&
            strcmp(g_server_state->connected_clients[i]->username, username_to_find) == 0) {
            // pthread_mutex_unlock(&g_server_state->clients_list_mutex);
            return g_server_state->connected_clients[i];
        }
    }
    // pthread_mutex_unlock(&g_server_state->clients_list_mutex);
    return NULL; // Not found
}

void send_error_to_client(int client_socket_fd, const char *error_message) {
    message_t err_msg;
    memset(&err_msg, 0, sizeof(err_msg));
    err_msg.type = MSG_ERROR; // Generic error
    strncpy(err_msg.sender, "SERVER", USERNAME_BUF_SIZE -1);
    strncpy(err_msg.content, error_message, MESSAGE_BUF_SIZE - 1);
    
    send_message(client_socket_fd, &err_msg); // Using shared send_message
}

void send_success_to_client(int client_socket_fd, const char *success_message) {
    message_t suc_msg;
    memset(&suc_msg, 0, sizeof(suc_msg));
    suc_msg.type = MSG_SUCCESS; // Generic success
    strncpy(suc_msg.sender, "SERVER", USERNAME_BUF_SIZE -1);
    strncpy(suc_msg.content, success_message, MESSAGE_BUF_SIZE - 1);

    send_message(client_socket_fd, &suc_msg);
}

void send_success_with_room_to_client(int client_socket_fd, const char* message, const char* room_name) {
    message_t suc_msg;
    memset(&suc_msg, 0, sizeof(suc_msg));
    suc_msg.type = MSG_SUCCESS;
    strncpy(suc_msg.sender, "SERVER", USERNAME_BUF_SIZE -1);
    strncpy(suc_msg.content, message, MESSAGE_BUF_SIZE - 1);
    if (room_name) { // Include room name if provided (e.g. for join success)
        strncpy(suc_msg.room, room_name, ROOM_NAME_BUF_SIZE - 1);
    }
    send_message(client_socket_fd, &suc_msg);
}

// Helper for server to send notifications to clients (e.g. user X joined room Y)
void send_server_notification_to_client(int client_socket_fd, const char* notification_message, const char* room_context) {
    message_t notif_msg; 
    memset(&notif_msg, 0, sizeof(notif_msg));
    notif_msg.type = MSG_SERVER_NOTIFICATION;
    strncpy(notif_msg.sender, "SERVER", USERNAME_BUF_SIZE -1);
    strncpy(notif_msg.content, notification_message, MESSAGE_BUF_SIZE - 1);
    if (room_context) {
        strncpy(notif_msg.room, room_context, ROOM_NAME_BUF_SIZE - 1);
    }
    send_message(client_socket_fd, &notif_msg);
}