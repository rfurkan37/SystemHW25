#include "common.h"

// Registers a new client connection on the server.
// Allocates ClientInfo, adds to server's list. Does NOT handle login.
// Returns pointer to ClientInfo on success, NULL on failure (e.g., max clients, malloc error).
ClientInfo *registerNewClientOnServer(int client_socket_fd, struct sockaddr_in client_address_info)
{
    if (!g_server_state)
    { // Should be initialized by main
        fprintf(stderr, "CRITICAL: g_server_state is NULL in registerNewClientOnServer.\n");
        close(client_socket_fd);
        return NULL;
    }

    ClientInfo *new_client_info = malloc(sizeof(ClientInfo));
    if (!new_client_info)
    {
        logServerEvent("ERROR", "Memory allocation failed for new ClientInfo structure.");
        close(client_socket_fd);
        return NULL;
    }
    memset(new_client_info, 0, sizeof(ClientInfo)); // Initialize all fields

    new_client_info->socket_fd = client_socket_fd;
    new_client_info->client_address = client_address_info;
    new_client_info->is_active = 0; // Client is not active (logged in) yet
    new_client_info->connection_time = time(NULL);
    new_client_info->num_received_files = 0; // For filename collision tracking

    // Initialize mutex for this client's received_filenames list
    if (pthread_mutex_init(&new_client_info->received_files_lock, NULL) != 0)
    {
        logServerEvent("ERROR", "Failed to initialize received_files_lock for client_fd %d: %s", client_socket_fd, strerror(errno));
        free(new_client_info);
        close(client_socket_fd);
        return NULL;
    }

    // Add to the global list of clients
    pthread_mutex_lock(&g_server_state->clients_list_mutex);
    int client_slot_idx = -1;
    for (int i = 0; i < MAX_SERVER_CLIENTS; ++i)
    {
        if (g_server_state->connected_clients[i] == NULL) // Find an empty slot
        {
            g_server_state->connected_clients[i] = new_client_info;
            client_slot_idx = i;
            break;
        }
    }
    pthread_mutex_unlock(&g_server_state->clients_list_mutex);

    if (client_slot_idx == -1) // No available slot
    {
        logServerEvent("WARNING", "Server at maximum client capacity (%d). New connection from %s rejected.",
                       MAX_SERVER_CLIENTS, inet_ntoa(client_address_info.sin_addr));
        sendErrorToClient(client_socket_fd, "Server is currently at maximum capacity. Please try again later.");
        pthread_mutex_destroy(&new_client_info->received_files_lock); // Clean up initialized mutex
        free(new_client_info);
        close(client_socket_fd);
        return NULL;
    }

    // logServerEvent("DEBUG", "Client fd %d registered in slot %d. Awaiting login.", client_socket_fd, client_slot_idx);
    return new_client_info;
}

// Processes a login request from a client.
// Validates username, checks for duplicates, and updates client state.
// Returns 1 on successful login, 0 on failure.
int processClientLogin(ClientInfo *client_info, const Message *login_message)
{
    if (!client_info || !login_message || login_message->type != MSG_LOGIN || !g_server_state)
    {
        if (client_info && client_info->socket_fd >= 0)
            sendErrorToClient(client_info->socket_fd, "Invalid login sequence or internal server error.");
        return 0; // Invalid parameters or state
    }

    char client_ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client_info->client_address.sin_addr, client_ip_str, INET_ADDRSTRLEN);

    // Validate username format
    if (!isValidUsername(login_message->sender))
    {
        logEventClientLoginFailed(login_message->sender, client_ip_str, "Invalid username format.");
        Message fail_msg; // Send specific failure message
        memset(&fail_msg, 0, sizeof(fail_msg));
        fail_msg.type = MSG_LOGIN_FAILURE;
        strncpy(fail_msg.content, "Invalid username: Must be alphanumeric, 1-16 characters, no spaces.", MESSAGE_BUF_SIZE - 1);
        fail_msg.content[MESSAGE_BUF_SIZE - 1] = '\0';
        sendMessage(client_info->socket_fd, &fail_msg);
        return 0;
    }

    // Check for duplicate username (requires clients_list_mutex)
    pthread_mutex_lock(&g_server_state->clients_list_mutex);
    // findClientByUsername checks for active clients with the same name.
    if (findClientByUsername(login_message->sender) != NULL)
    {
        pthread_mutex_unlock(&g_server_state->clients_list_mutex);
        logEventClientLoginFailed(login_message->sender, client_ip_str, "Duplicate username."); // Matches PDF log
        Message fail_msg;
        memset(&fail_msg, 0, sizeof(fail_msg));
        fail_msg.type = MSG_LOGIN_FAILURE;
        strncpy(fail_msg.content, "Username already taken. Please choose another.", MESSAGE_BUF_SIZE - 1);
        fail_msg.content[MESSAGE_BUF_SIZE - 1] = '\0';
        sendMessage(client_info->socket_fd, &fail_msg);
        return 0;
    }

    // Username is valid and unique. Proceed with login.
    strncpy(client_info->username, login_message->sender, USERNAME_BUF_SIZE - 1);
    client_info->username[USERNAME_BUF_SIZE - 1] = '\0'; // Ensure null termination
    client_info->is_active = 1;                          // Mark client as active (logged in)
    g_server_state->active_client_count++;               // Increment server's active client counter
    pthread_mutex_unlock(&g_server_state->clients_list_mutex);

    logEventClientConnected(client_info->username, client_ip_str); // Use specific log function
    Message success_msg;
    memset(&success_msg, 0, sizeof(success_msg));
    success_msg.type = MSG_LOGIN_SUCCESS;
    strncpy(success_msg.content, "Login successful. Welcome to the chat server!", MESSAGE_BUF_SIZE - 1);
    success_msg.content[MESSAGE_BUF_SIZE - 1] = '\0';
    sendMessage(client_info->socket_fd, &success_msg);
    return 1; // Login successful
}

// Main thread function for handling a single client connection.
// Manages login, message loop, and disconnection.
void *clientConnectionThreadHandler(void *client_info_ptr_arg)
{
    ClientInfo *client = (ClientInfo *)client_info_ptr_arg;
    Message received_message;
    int is_unexpected_disconnect = 1; // Assume unexpected until graceful disconnect

    // Detach the thread as per project Q&A, allowing it to clean up its own resources.
    // The server main thread will not join these client handler threads.
    pthread_detach(pthread_self());

    if (!g_server_state)
    {
        logServerEvent("CRITICAL_THREAD", "g_server_state is NULL in client thread for fd %d. Terminating handler.", client ? client->socket_fd : -1);
        if (client)
        { // Minimal cleanup if client struct exists
            if (client->socket_fd >= 0)
                close(client->socket_fd);
            pthread_mutex_destroy(&client->received_files_lock); // Attempt to destroy client-specific mutex
            free(client);
        }
        return NULL;
    }

    // --- Login Phase ---
    // Expecting the first message from client to be MSG_LOGIN
    if (receiveMessage(client->socket_fd, &received_message) <= 0)
    {
        // Client disconnected before sending any message, or read error.
        char client_ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client->client_address.sin_addr, client_ip_str, INET_ADDRSTRLEN);
        logServerEvent("INFO", "Client (fd %d from %s) disconnected before login attempt or initial read error.", client->socket_fd, client_ip_str);
        unregisterClient(client, 1); // Treat as unexpected, clean up client slot
        return NULL;
    }

    if (!processClientLogin(client, &received_message))
    {
        // Login failed (e.g., bad username, duplicate). processClientLogin sent error to client.
        unregisterClient(client, 1); // Clean up client slot
        return NULL;
    }
    // Client is now logged in and active. client->is_active = 1.

    // --- Main Message Loop ---
    while (g_server_state->server_is_running && client->is_active)
    {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(client->socket_fd, &read_fds);
        struct timeval timeout = {1, 0}; // 1-second timeout for select

        int activity = select(client->socket_fd + 1, &read_fds, NULL, NULL, &timeout);

        if (!g_server_state->server_is_running || !client->is_active)
        {
            // Server shutting down or client explicitly disconnected (/exit)
            is_unexpected_disconnect = 0; // Mark as graceful or server-initiated
            break;                        // Exit loop
        }

        if (activity < 0)
        { // select() error
            if (errno == EINTR)
                continue; // Interrupted by a signal, loop to re-check flags
            logServerEvent("ERROR", "select() error for client %s (fd %d): %s. Assuming disconnection.",
                           client->username, client->socket_fd, strerror(errno));
            is_unexpected_disconnect = 1; // Treat as unexpected network error
            break;                        // Exit loop
        }

        if (activity == 0)
            continue; // Timeout, loop back to check flags and wait again

        // If data is available on the client's socket
        if (FD_ISSET(client->socket_fd, &read_fds))
        {
            if (receiveMessage(client->socket_fd, &received_message) <= 0)
            {
                // receiveMessage returns <= 0 on connection closed or error.
                // If client->is_active is still true here, it's an unexpected disconnect.
                // If client->is_active became false due to MSG_DISCONNECT processing, it's expected.
                // The 'is_unexpected_disconnect' flag will be based on client->is_active value *before* this block.
                // However, the final check before unregisterClient will use current client->is_active.
                if (client->is_active)
                { // If still marked active, this is unexpected.
                    is_unexpected_disconnect = 1;
                }
                else
                { // Was already marked inactive (e.g., by /exit command)
                    is_unexpected_disconnect = 0;
                }
                break; // Exit loop, will proceed to unregisterClient
            }
            // Successfully received a message, handle it
            handleClientMessage(client, &received_message);
        }
    } // End of main message loop

    // --- Cleanup Phase for this client ---
    // Determine if disconnection was truly unexpected at this final stage
    if (!client->is_active)
    {
        // Client explicitly sent /exit or was marked inactive by a handler.
        is_unexpected_disconnect = 0;
    }
    if (!g_server_state->server_is_running && client->is_active)
    {
        // Server is shutting down while client was still active.
        is_unexpected_disconnect = 0;              // Not client's fault
        notifyClientOfShutdown(client->socket_fd); // Attempt to notify client
    }
    // If loop exited due to network error (select or recv error) and client was active,
    // is_unexpected_disconnect remains 1.

    unregisterClient(client, is_unexpected_disconnect);
    // logServerEvent("DEBUG", "Client handler thread for %s (fd %d) terminating.", client_username_before_free, client_socket_fd_before_free);
    client = NULL; // Avoid dangling pointer if used after free in theory, though thread exits.
    return NULL;
}

// Dispatches client messages to appropriate handler functions.
void handleClientMessage(ClientInfo *client, const Message *message)
{
    if (!client || !message || !client->is_active || !g_server_state || !g_server_state->server_is_running)
    {
        return; // Ignore messages if client/server not in a valid state
    }

    switch (message->type)
    {
    case MSG_JOIN_ROOM:
        handleJoinRoomRequest(client, message->room);
        break;
    case MSG_LEAVE_ROOM:
        handleLeaveRoomRequest(client);
        break;
    case MSG_BROADCAST:
        handleBroadcastRequest(client, message->content);
        break;
    case MSG_WHISPER:
        handleWhisperRequest(client, message->receiver, message->content);
        break;
    case MSG_FILE_TRANSFER_REQUEST:
        handleFileTransferRequest(client, message);
        break;
    case MSG_DISCONNECT:
        client->is_active = 0; // Mark client as inactive (graceful disconnect initiated by client)
        // The main loop in clientConnectionThreadHandler will see this and exit.
        // Confirmation message sent to client.
        Message bye_msg;
        memset(&bye_msg, 0, sizeof(bye_msg));
        bye_msg.type = MSG_SUCCESS; // Or MSG_SERVER_NOTIFICATION
        strncpy(bye_msg.content, "Disconnected. Goodbye!", MESSAGE_BUF_SIZE - 1);
        bye_msg.content[MESSAGE_BUF_SIZE - 1] = '\0';
        sendMessage(client->socket_fd, &bye_msg);
        // Logging of this graceful disconnect will be handled by unregisterClient
        // when is_unexpected_disconnect is false.
        break;
    default:
        logServerEvent("WARNING", "Client %s (fd %d) sent unhandled or malformed message type: %d",
                       client->username, client->socket_fd, message->type);
        sendErrorToClient(client->socket_fd, "Unknown or malformed command received by server.");
        break;
    }
}

// Unregisters a client from the server.
// Cleans up resources: removes from room, closes socket, frees ClientInfo.
void unregisterClient(ClientInfo *client_to_remove, int is_unexpected_disconnect)
{
    if (!client_to_remove || !g_server_state)
        return;

    // Capture username and fd for logging before potential free/invalidation.
    char client_username_log[USERNAME_BUF_SIZE];
    strncpy(client_username_log, client_to_remove->username, USERNAME_BUF_SIZE - 1);
    client_username_log[USERNAME_BUF_SIZE - 1] = '\0';
    if (strlen(client_username_log) == 0)
    { // If client disconnected before login
        snprintf(client_username_log, USERNAME_BUF_SIZE, "fd_%d_unauthed", client_to_remove->socket_fd);
    }

    // 1. Ensure client is marked inactive (should be done by caller or loop exit logic)
    client_to_remove->is_active = 0;

    // 2. Log disconnection event
    logEventClientDisconnected(client_username_log, is_unexpected_disconnect);

    // 3. Remove client from any room they were in
    if (strlen(client_to_remove->current_room_name) > 0)
    {
        // removeClientFromTheirRoom also logs the "left room" part.
        // It's important that room operations are safe even if client is disconnecting.
        ChatRoom *room_they_were_in = findOrCreateChatRoom(client_to_remove->current_room_name);
        if (room_they_were_in)
        {
            // Notify room members that this client has left (due to disconnect)
            // Check if server is still running to avoid notifications during shutdown flood
            if (g_server_state->server_is_running)
            {
                notifyRoomOfClientAction(client_to_remove, room_they_were_in, "disconnected and left");
            }
        }
        removeClientFromTheirRoom(client_to_remove);
    }

    // 4. Close client's socket
    if (client_to_remove->socket_fd >= 0)
    {
        shutdown(client_to_remove->socket_fd, SHUT_RDWR); // Gracefully signal other end
        close(client_to_remove->socket_fd);
        client_to_remove->socket_fd = -1; // Mark as closed
    }

    // 5. Remove client from the global list and decrement active_client_count
    pthread_mutex_lock(&g_server_state->clients_list_mutex);
    for (int i = 0; i < MAX_SERVER_CLIENTS; ++i)
    {
        if (g_server_state->connected_clients[i] == client_to_remove)
        {
            g_server_state->connected_clients[i] = NULL; // Free up the slot in server's list
            // Only decrement active_client_count if client had successfully logged in (had a username)
            // and active_client_count is positive.
            if (strlen(client_to_remove->username) > 0 && g_server_state->active_client_count > 0)
            {
                g_server_state->active_client_count--;
            }
            break; // Found and removed
        }
    }
    pthread_mutex_unlock(&g_server_state->clients_list_mutex);

    // 6. Destroy client-specific mutex (for received_filenames list)
    pthread_mutex_destroy(&client_to_remove->received_files_lock);

    // 7. Free the ClientInfo structure itself
    free(client_to_remove);
    client_to_remove = NULL; // Avoid dangling pointer for the caller (though thread exits)

    // logServerEvent("DEBUG", "Client %s (formerly fd %d) fully unregistered.", client_username_log, client_fd_log);
}

// Sends a shutdown notification message to a client.
// Used when server is shutting down and needs to inform connected clients.
void notifyClientOfShutdown(int client_socket_fd)
{
    if (client_socket_fd < 0)
        return;

    Message shutdown_notif_msg;
    memset(&shutdown_notif_msg, 0, sizeof(shutdown_notif_msg));
    shutdown_notif_msg.type = MSG_ERROR; // Or MSG_SERVER_NOTIFICATION with specific content
                                         // Using MSG_ERROR makes client display it prominently.
    strncpy(shutdown_notif_msg.sender, "SERVER", USERNAME_BUF_SIZE - 1);
    shutdown_notif_msg.sender[USERNAME_BUF_SIZE - 1] = '\0';
    strncpy(shutdown_notif_msg.content, "Server is shutting down. You will be disconnected.", MESSAGE_BUF_SIZE - 1);
    shutdown_notif_msg.content[MESSAGE_BUF_SIZE - 1] = '\0';

    sendMessage(client_socket_fd, &shutdown_notif_msg); // Best effort send
}