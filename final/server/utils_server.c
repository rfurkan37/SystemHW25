#include "common.h"

// Finds an active client by username.
// IMPORTANT: Assumes g_server_state->clients_list_mutex is HELD by the caller
// to ensure thread-safe access to the shared client list.
ClientInfo *findClientByUsername(const char *username_to_find)
{
    if (!username_to_find || !g_server_state) // Basic validation
        return NULL;

    for (int i = 0; i < MAX_SERVER_CLIENTS; i++)
    {
        if (g_server_state->connected_clients[i] != NULL &&
            g_server_state->connected_clients[i]->is_active && // Only consider active (logged-in) clients
            strncmp(g_server_state->connected_clients[i]->username, username_to_find, USERNAME_BUF_SIZE) == 0)
        {
            return g_server_state->connected_clients[i]; // Client found
        }
    }
    return NULL; // Client not found
}

// Sends a standardized error message to the client.
void sendErrorToClient(int client_socket_fd, const char *error_message)
{
    if (client_socket_fd < 0 || !error_message)
        return;

    Message err_msg;
    memset(&err_msg, 0, sizeof(err_msg));
    err_msg.type = MSG_ERROR;
    strncpy(err_msg.sender, "SERVER", USERNAME_BUF_SIZE - 1);
    strncpy(err_msg.content, error_message, MESSAGE_BUF_SIZE - 1);
    // Ensure null termination for safety
    err_msg.sender[USERNAME_BUF_SIZE - 1] = '\0';
    err_msg.content[MESSAGE_BUF_SIZE - 1] = '\0';

    sendMessage(client_socket_fd, &err_msg); // sendMessage handles actual send errors
}

// Sends a standardized success message to the client.
void sendSuccessToClient(int client_socket_fd, const char *success_message)
{
    if (client_socket_fd < 0 || !success_message)
        return;

    Message suc_msg;
    memset(&suc_msg, 0, sizeof(suc_msg));
    suc_msg.type = MSG_SUCCESS;
    strncpy(suc_msg.sender, "SERVER", USERNAME_BUF_SIZE - 1);
    strncpy(suc_msg.content, success_message, MESSAGE_BUF_SIZE - 1);
    suc_msg.sender[USERNAME_BUF_SIZE - 1] = '\0';
    suc_msg.content[MESSAGE_BUF_SIZE - 1] = '\0';

    sendMessage(client_socket_fd, &suc_msg);
}

// Sends a success message that includes room context to the client.
// Used, for example, when confirming joining a room.
void sendSuccessWithRoomToClient(int client_socket_fd, const char *message, const char *room_name)
{
    if (client_socket_fd < 0 || !message)
        return;

    Message suc_msg;
    memset(&suc_msg, 0, sizeof(suc_msg));
    suc_msg.type = MSG_SUCCESS; // Or MSG_SERVER_NOTIFICATION depending on desired client handling
    strncpy(suc_msg.sender, "SERVER", USERNAME_BUF_SIZE - 1);
    strncpy(suc_msg.content, message, MESSAGE_BUF_SIZE - 1);
    if (room_name)
    {
        strncpy(suc_msg.room, room_name, ROOM_NAME_BUF_SIZE - 1);
        suc_msg.room[ROOM_NAME_BUF_SIZE - 1] = '\0';
    }
    suc_msg.sender[USERNAME_BUF_SIZE - 1] = '\0';
    suc_msg.content[MESSAGE_BUF_SIZE - 1] = '\0';

    sendMessage(client_socket_fd, &suc_msg);
}

// Sends a general server notification message to the client.
void sendServerNotificationToClient(int client_socket_fd, const char *notification_message, const char *room_context)
{
    if (client_socket_fd < 0 || !notification_message)
        return;

    Message notif_msg;
    memset(&notif_msg, 0, sizeof(notif_msg));
    notif_msg.type = MSG_SERVER_NOTIFICATION;
    strncpy(notif_msg.sender, "SERVER", USERNAME_BUF_SIZE - 1);
    strncpy(notif_msg.content, notification_message, MESSAGE_BUF_SIZE - 1);
    if (room_context)
    { // Optionally include room context if relevant
        strncpy(notif_msg.room, room_context, ROOM_NAME_BUF_SIZE - 1);
        notif_msg.room[ROOM_NAME_BUF_SIZE - 1] = '\0';
    }
    notif_msg.sender[USERNAME_BUF_SIZE - 1] = '\0';
    notif_msg.content[MESSAGE_BUF_SIZE - 1] = '\0';

    sendMessage(client_socket_fd, &notif_msg);
}

// Generates a new filename to resolve collisions, e.g., "file.txt" -> "file_1.txt".
// original_filename: The base name of the file.
// collision_num: The attempt number (e.g., 1 for the first collision, 2 for the second).
// output_buffer: Buffer to store the newly generated filename.
// buffer_size: Size of the output_buffer.
void generate_collided_filename(const char *original_filename, int collision_num, char *output_buffer, size_t buffer_size)
{
    if (!original_filename || !output_buffer || buffer_size == 0)
        return;

    char base_name[FILENAME_BUF_SIZE];
    char extension[FILENAME_BUF_SIZE]; // Can be long if no dot, or just the dot and ext

    strncpy(base_name, original_filename, sizeof(base_name) - 1);
    base_name[sizeof(base_name) - 1] = '\0'; // Ensure null termination

    extension[0] = '\0'; // Default to no extension

    char *dot = strrchr(base_name, '.');
    // A dot is considered an extension separator if it's present, not the first character,
    // and there's something after it.
    if (dot != NULL && dot != base_name && *(dot + 1) != '\0')
    {
        strncpy(extension, dot, sizeof(extension) - 1); // Includes the dot, e.g., ".txt"
        extension[sizeof(extension) - 1] = '\0';
        *dot = '\0'; // Null-terminate base_name before the dot (base_name becomes "file")
    }
    // If no valid dot, base_name remains the original_filename and extension remains empty.

    // Construct the new filename: base_name + "_" + collision_num + extension
    snprintf(output_buffer, buffer_size, "%s_%d%s", base_name, collision_num, extension);
    output_buffer[buffer_size - 1] = '\0'; // Ensure null termination for safety if snprintf truncates
}