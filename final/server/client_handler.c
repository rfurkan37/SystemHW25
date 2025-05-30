#include "common.h"
#include <sys/select.h>

// Function to allocate and initialize a new client_info_t structure
// This is called when a new connection is accepted by the server main loop.
client_info_t* register_new_client_on_server(int client_socket_fd, struct sockaddr_in client_address_info) {
    client_info_t *new_client_info = malloc(sizeof(client_info_t));
    if (!new_client_info) {
        log_server_event("ERROR", "Memory allocation failed for new client_info_t.");
        close(client_socket_fd); // Close the socket if we can't handle the client
        return NULL;
    }

    memset(new_client_info, 0, sizeof(client_info_t)); // Zero out the structure
    new_client_info->socket_fd = client_socket_fd;
    new_client_info->client_address = client_address_info;
    new_client_info->is_active = 1; // Mark as active, pending login
    new_client_info->connection_time = time(NULL);
    // Username and current_room_name are empty until login/join

    // Add to global clients list (Needs protection with clients_list_mutex)
    pthread_mutex_lock(&g_server_state->clients_list_mutex);
    int client_slot = -1;
    for (int i = 0; i < MAX_SERVER_CLIENTS; ++i) {
        if (g_server_state->connected_clients[i] == NULL) {
            g_server_state->connected_clients[i] = new_client_info;
            client_slot = i;
            break;
        }
    }

    if (client_slot == -1) { // Should have been checked by accept loop, but as a safeguard
        pthread_mutex_unlock(&g_server_state->clients_list_mutex);
        log_server_event("ERROR", "No slot for new client_info_t, though accept loop should prevent this.");
        send_error_to_client(client_socket_fd, "Server is critically overloaded. Try later.");
        close(client_socket_fd);
        free(new_client_info);
        return NULL;
    }
    // g_server_state->active_client_count is incremented *after* successful login.
    pthread_mutex_unlock(&g_server_state->clients_list_mutex);
    
    return new_client_info;
}


// Handles the MSG_LOGIN message from a newly connected client.
// Returns 1 on successful login, 0 on failure.
int process_client_login(client_info_t *client_info, const message_t *login_message) {
    if (!client_info || !login_message || login_message->type != MSG_LOGIN) {
        send_error_to_client(client_info->socket_fd, "Invalid login sequence.");
        return 0;
    }

    char client_ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client_info->client_address.sin_addr, client_ip_str, INET_ADDRSTRLEN);

    if (!is_valid_username(login_message->sender)) {
        log_event_client_login_failed(login_message->sender, client_ip_str, "Invalid username format.");
        // Use MSG_LOGIN_FAILURE for client to distinguish
        message_t fail_msg;
        memset(&fail_msg, 0, sizeof(fail_msg));
        fail_msg.type = MSG_LOGIN_FAILURE;
        strncpy(fail_msg.content, "Invalid username: Alphanumeric, 1-16 chars, no spaces.", MESSAGE_BUF_SIZE -1);
        send_message(client_info->socket_fd, &fail_msg);
        return 0;
    }

    pthread_mutex_lock(&g_server_state->clients_list_mutex);
    // Check for duplicate username
    for (int i = 0; i < MAX_SERVER_CLIENTS; ++i) {
        if (g_server_state->connected_clients[i] != NULL &&
            g_server_state->connected_clients[i]->is_active && // Check if the slot is active
            strcmp(g_server_state->connected_clients[i]->username, login_message->sender) == 0) {
            
            pthread_mutex_unlock(&g_server_state->clients_list_mutex);
            log_event_client_login_failed(login_message->sender, client_ip_str, "Username already taken.");
            message_t fail_msg;
            memset(&fail_msg, 0, sizeof(fail_msg));
            fail_msg.type = MSG_LOGIN_FAILURE;
            strncpy(fail_msg.content, "Username already taken. Choose another.", MESSAGE_BUF_SIZE -1);
            send_message(client_info->socket_fd, &fail_msg);
            return 0;
        }
    }

    // Username is valid and unique, complete login
    strncpy(client_info->username, login_message->sender, USERNAME_BUF_SIZE - 1);
    g_server_state->active_client_count++; // Increment active count now
    pthread_mutex_unlock(&g_server_state->clients_list_mutex);

    log_event_client_connected(client_info->username, client_ip_str);
    
    message_t success_msg;
    memset(&success_msg, 0, sizeof(success_msg));
    success_msg.type = MSG_LOGIN_SUCCESS;
    strncpy(success_msg.content, "Login successful. Welcome!", MESSAGE_BUF_SIZE -1);
    send_message(client_info->socket_fd, &success_msg);
    
    return 1;
}


// Main function for a dedicated thread handling a single client connection.
void* client_connection_thread_handler(void *client_info_ptr_arg) {
    client_info_t *client = (client_info_t *)client_info_ptr_arg;
    message_t received_message_from_client;
    int is_unexpected_disconnect = 1; // Assume unexpected until /exit or server shutdown

    // --- Login Phase ---
    // Client sends MSG_LOGIN first. We expect it within a certain timeout.
    // For simplicity, we'll do a blocking read here. A robust server might use select with timeout.
    if (receive_message(client->socket_fd, &received_message_from_client) <= 0) {
        log_server_event("INFO", "Client disconnected before sending login message or read error.");
        unregister_client(client, 1); // Pass true for unexpected
        return NULL;
    }

    if (!process_client_login(client, &received_message_from_client)) {
        // Login failed, process_client_login already sent error to client and logged.
        unregister_client(client, 1); // Login failure is also an "unexpected" end from server's perspective
        return NULL;
    }

    // --- Main Message Loop ---
    while (g_server_state->server_is_running && client->is_active) {
        // Use select for non-blocking read with timeout to check server_is_running flag
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(client->socket_fd, &read_fds);
        struct timeval timeout = {1, 0}; // 1 second timeout

        int activity = select(client->socket_fd + 1, &read_fds, NULL, NULL, &timeout);

        if (!g_server_state->server_is_running || !client->is_active) { // Check flags after select
             is_unexpected_disconnect = 0; // Server shutdown or client /exit
             break;
        }

        if (activity < 0) {
            if (errno == EINTR) continue; // Interrupted by signal, loop again
            log_server_event("ERROR", "select() error for client %s: %s", client->username, strerror(errno));
            break; // Assume connection error
        }

        if (activity == 0) { // Timeout
            continue; // Loop to check flags and select again
        }

        // If data is available on the socket
        if (FD_ISSET(client->socket_fd, &read_fds)) {
            if (receive_message(client->socket_fd, &received_message_from_client) <= 0) {
                // Client disconnected (gracefully or crash) or read error
                if (client->is_active) { // Log only if we weren't expecting it
                     // log_server_event("INFO", "Client %s disconnected or read error.", client->username);
                }
                break; 
            }
            // Successfully received a message, process it
            handle_client_message(client, &received_message_from_client);
        }
    }

    // --- Cleanup Phase ---
    if (client->is_active && g_server_state->server_is_running) {
        // This path means the loop broke due to an issue, not /exit or server shutdown.
    } else if (!client->is_active) { // Client sent /exit
        is_unexpected_disconnect = 0;
    } else if (!g_server_state->server_is_running) { // Server is shutting down
        is_unexpected_disconnect = 0;
        // Server main loop will handle sending shutdown messages if not already.
        // Or, we can send one final notification here.
        notify_client_of_shutdown(client->socket_fd);
    }

    unregister_client(client, is_unexpected_disconnect);
    return NULL;
}

// Routes client messages to appropriate handlers.
void handle_client_message(client_info_t *client, const message_t *message) {
    // PDF requirement: Log every user action with timestamps.
    // Specific handlers will call more detailed logging.
    // log_server_event("COMMAND_RAW", "User %s sent msg type %d", client->username, message->type);

    switch (message->type) {
        case MSG_JOIN_ROOM:
            handle_join_room_request(client, message->room);
            break;
        case MSG_LEAVE_ROOM:
            handle_leave_room_request(client);
            break;
        case MSG_BROADCAST:
            handle_broadcast_request(client, message->content);
            break;
        case MSG_WHISPER:
            handle_whisper_request(client, message->receiver, message->content);
            break;
        case MSG_FILE_TRANSFER_REQUEST: // Client wants to send a file
            handle_file_transfer_request(client, message);
            break;
        case MSG_DISCONNECT:
            client->is_active = 0; // Mark for cleanup, loop will exit
            // Server sends final "Disconnected. Goodbye!" message
            message_t bye_msg;
            memset(&bye_msg, 0, sizeof(bye_msg));
            bye_msg.type = MSG_SUCCESS; // Or a specific MSG_DISCONNECT_ACK
            strncpy(bye_msg.content, "Disconnected. Goodbye!", MESSAGE_BUF_SIZE -1);
            send_message(client->socket_fd, &bye_msg);
            break;
        // MSG_LOGIN should only be handled once at the start.
        default:
            log_server_event("WARNING", "Client %s sent unhandled message type: %d", client->username, message->type);
            send_error_to_client(client->socket_fd, "Unknown or unexpected command.");
            break;
    }
}

// Cleans up a client's resources and removes them from server's active list.
void unregister_client(client_info_t *client_info_to_remove, int is_unexpected_disconnect) {
    if (!client_info_to_remove) return;

    // Log disconnection (username might be empty if login failed)
    const char* username_to_log = (strlen(client_info_to_remove->username) > 0) ? client_info_to_remove->username : "UNKNOWN_USER";
    log_event_client_disconnected(username_to_log, is_unexpected_disconnect);

    // Remove from any room they were in
    if (strlen(client_info_to_remove->current_room_name) > 0) {
        remove_client_from_their_room(client_info_to_remove);
    }

    // Close socket
    if (client_info_to_remove->socket_fd >= 0) {
        shutdown(client_info_to_remove->socket_fd, SHUT_RDWR); // Graceful shutdown of TCP connection
        close(client_info_to_remove->socket_fd);
        client_info_to_remove->socket_fd = -1; // Mark as closed
    }
    client_info_to_remove->is_active = 0; // Ensure inactive

    // Remove from global clients list and decrement count
    pthread_mutex_lock(&g_server_state->clients_list_mutex);
    for (int i = 0; i < MAX_SERVER_CLIENTS; ++i) {
        if (g_server_state->connected_clients[i] == client_info_to_remove) {
            g_server_state->connected_clients[i] = NULL; // Free up the slot
            // Only decrement active_client_count if username was set (i.e., login was successful)
            if (strlen(client_info_to_remove->username) > 0) {
                 if (g_server_state->active_client_count > 0) g_server_state->active_client_count--;
            }
            break;
        }
    }
    pthread_mutex_unlock(&g_server_state->clients_list_mutex);

    // Free the client_info_t structure itself
    free(client_info_to_remove);
}

// Used during server shutdown sequence.
void notify_client_of_shutdown(int client_socket_fd) {
    message_t shutdown_notif_msg;
    memset(&shutdown_notif_msg, 0, sizeof(shutdown_notif_msg));
    shutdown_notif_msg.type = MSG_ERROR; // Client should interpret as critical
    strncpy(shutdown_notif_msg.sender, "SERVER", USERNAME_BUF_SIZE -1);
    strncpy(shutdown_notif_msg.content, "Server is shutting down. Disconnecting.", MESSAGE_BUF_SIZE - 1);
    send_message(client_socket_fd, &shutdown_notif_msg); // Best effort
}