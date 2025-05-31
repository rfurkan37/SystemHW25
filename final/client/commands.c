#include "common.h"
#include <ctype.h> // For isspace

// Helper to trim leading whitespace from a string.
// Returns a pointer to the first non-whitespace character.
const char *trimLeadingWhitespace(const char *str)
{
    while (*str && isspace((unsigned char)*str))
    {
        str++;
    }
    return str;
}

// Helper to validate local file metadata before sending a request.
// Checks filename length, type, and size.
// Returns 1 if file is valid for transfer, 0 otherwise.
static int prepareLocalFile(const char *filepath, const char *filename_only, long *out_file_size)
{
    if (strlen(filename_only) >= FILENAME_BUF_SIZE)
    {
        printf("\033[31mFilename '%s' is too long (max %d characters allowed).\033[0m\n", filename_only, FILENAME_BUF_SIZE - 1);
        return 0;
    }
    if (!isValidFileType(filename_only))
    {
        printf("\033[31mInvalid file type for '%s'. Supported types: .txt, .pdf, .jpg, .png\033[0m\n", filename_only);
        return 0;
    }

    *out_file_size = getFileSizeFromPath(filepath); // Uses shared utility
    if (*out_file_size < 0)
    {
        printf("\033[31mFile '%s' not found or cannot be accessed.\033[0m\n", filepath);
        return 0;
    }
    if (*out_file_size == 0)
    {
        // Per project spec, file transfer is simulated, but a zero-size file is often problematic/uninteresting.
        printf("\033[31mCannot send an empty file (0 bytes).\033[0m\n");
        return 0;
    }
    if (*out_file_size > MAX_FILE_SIZE)
    {
        printf("\033[31mFile '%s' (%ld bytes) exceeds the maximum allowed size of %dMB.\033[0m\n",
               filename_only, *out_file_size, MAX_FILE_SIZE / (1024 * 1024));
        return 0;
    }
    return 1; // File metadata is valid
}

// Parses and processes user commands from input.
void processUserCommand(ClientState *client, const char *input)
{
    char command_buffer[64];             // For the command itself (e.g., "/join")
    char arg1_buffer[FILENAME_BUF_SIZE]; // For first argument (room_name, username, filepath)
    char arg2_buffer[MESSAGE_BUF_SIZE];  // For second argument (message, target_username for sendfile)

    // Initialize buffers to prevent garbage data
    memset(command_buffer, 0, sizeof(command_buffer));
    memset(arg1_buffer, 0, sizeof(arg1_buffer));
    memset(arg2_buffer, 0, sizeof(arg2_buffer));

    const char *ptr = input;
    int i = 0;

    // Extract the command token
    while (*ptr && !isspace((unsigned char)*ptr) && i < (int)sizeof(command_buffer) - 1)
    {
        command_buffer[i++] = *ptr++;
    }
    command_buffer[i] = '\0';
    ptr = trimLeadingWhitespace(ptr); // Move past command and subsequent spaces

    // Special handling for commands where the message is the last argument and can contain spaces
    if (strcmp(command_buffer, "/broadcast") == 0)
    {
        if (strlen(ptr) > 0)
        { // ptr now points to the start of the message
            strncpy(arg1_buffer, ptr, sizeof(arg1_buffer) - 1);
            arg1_buffer[sizeof(arg1_buffer) - 1] = '\0'; // Ensure null termination
            sendBroadcastCommand(client, arg1_buffer);
        }
        else
        {
            printf("\033[31mUsage: /broadcast <message>\033[0m\n");
        }
        return;
    }

    if (strcmp(command_buffer, "/whisper") == 0)
    {
        // Extract target username (arg1_buffer)
        i = 0;
        while (*ptr && !isspace((unsigned char)*ptr) && i < MAX_USERNAME_LEN) // Use MAX_USERNAME_LEN for username part
        {
            arg1_buffer[i++] = *ptr++;
        }
        arg1_buffer[i] = '\0';
        ptr = trimLeadingWhitespace(ptr); // Move past username and subsequent spaces

        if (strlen(arg1_buffer) > 0 && strlen(ptr) > 0)
        { // ptr now points to the message
            strncpy(arg2_buffer, ptr, sizeof(arg2_buffer) - 1);
            arg2_buffer[sizeof(arg2_buffer) - 1] = '\0'; // Ensure null termination
            sendWhisperCommand(client, arg1_buffer, arg2_buffer);
        }
        else
        {
            printf("\033[31mUsage: /whisper <username> <message>\033[0m\n");
        }
        return;
    }

    // For other commands, parse arg1 and arg2 (if it exists) as single tokens
    i = 0;
    while (*ptr && !isspace((unsigned char)*ptr) && i < (int)sizeof(arg1_buffer) - 1)
    {
        arg1_buffer[i++] = *ptr++;
    }
    arg1_buffer[i] = '\0';
    ptr = trimLeadingWhitespace(ptr);

    i = 0;
    while (*ptr && !isspace((unsigned char)*ptr) && i < (int)sizeof(arg2_buffer) - 1) // For /sendfile's target username
    {
        arg2_buffer[i++] = *ptr++;
    }
    arg2_buffer[i] = '\0';
    // ptr = trimLeadingWhitespace(ptr); // Not strictly needed if no third argument is expected

    // --- Command Dispatch ---
    if (strcmp(command_buffer, "/join") == 0)
    {
        if (strlen(arg1_buffer) > 0)
            sendJoinRoomCommand(client, arg1_buffer);
        else
            printf("\033[31mUsage: /join <room_name>\033[0m\n");
    }
    else if (strcmp(command_buffer, "/leave") == 0)
    {
        sendLeaveRoomCommand(client);
    }
    else if (strcmp(command_buffer, "/sendfile") == 0)
    {
        if (strlen(arg1_buffer) > 0 && strlen(arg2_buffer) > 0)
        {
            // arg1_buffer is filepath, arg2_buffer is target_username
            sendFileRequestCommand(client, arg1_buffer, arg2_buffer);
        }
        else
        {
            printf("\033[31mUsage: /sendfile <filepath> <username>\033[0m\n");
            printf("\033[32mInfo: Supported file types: .txt, .pdf, .jpg, .png (max %dMB)\033[0m\n", MAX_FILE_SIZE / (1024 * 1024));
        }
    }
    else if (strcmp(command_buffer, "/exit") == 0)
    {
        sendDisconnectSignal(client); // Notify server
        client->connected = 0;        // Signal main loop and receiver thread to exit
    }
    else if (strcmp(command_buffer, "/help") == 0)
    {
        displayHelpMessage();
    }
    else
    {
        printf("\033[31mUnknown command: '%s'. Type /help for available commands.\033[0m\n", command_buffer);
    }
}

void sendJoinRoomCommand(ClientState *client, const char *room_name)
{
    if (!isValidRoomName(room_name))
    {
        printf("\033[31mInvalid room name: Must be alphanumeric, 1-%d characters, no spaces.\033[0m\n", MAX_ROOM_NAME_LEN);
        return;
    }

    Message msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = MSG_JOIN_ROOM;
    strncpy(msg.sender, client->username, USERNAME_BUF_SIZE - 1);
    strncpy(msg.room, room_name, ROOM_NAME_BUF_SIZE - 1);
    // Ensure null termination (though strncpy with SIZE-1 does this if src is shorter or equal)
    msg.sender[USERNAME_BUF_SIZE - 1] = '\0';
    msg.room[ROOM_NAME_BUF_SIZE - 1] = '\0';

    if (!sendMessage(client->socket_fd, &msg))
    {
        printf("\033[31mFailed to send join command to server. Connection may be lost.\033[0m\n");
        // Consider setting client->connected = 0 or other more robust error handling for critical failures
    }
}

void sendLeaveRoomCommand(ClientState *client)
{
    if (strlen(client->current_room) == 0)
    {
        printf("\033[31mYou are not currently in any room.\033[0m\n");
        return;
    }
    Message msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = MSG_LEAVE_ROOM;
    strncpy(msg.sender, client->username, USERNAME_BUF_SIZE - 1);
    msg.sender[USERNAME_BUF_SIZE - 1] = '\0';
    // Server knows which room the client is in, no need to send client->current_room

    if (!sendMessage(client->socket_fd, &msg))
    {
        printf("\033[31mFailed to send leave command to server. Connection may be lost.\033[0m\n");
    }
}

void sendBroadcastCommand(ClientState *client, const char *message_content)
{
    if (strlen(client->current_room) == 0)
    {
        printf("\033[31mYou must be in a room to broadcast. Use /join <room_name> first.\033[0m\n");
        return;
    }
    if (strlen(message_content) == 0)
    {
        printf("\033[31mCannot broadcast an empty message.\033[0m\n");
        return;
    }
    if (strlen(message_content) >= MESSAGE_BUF_SIZE)
    { // content buffer is MESSAGE_BUF_SIZE, can hold MESSAGE_BUF_SIZE-1 chars + null
        printf("\033[31mMessage is too long (max %d characters).\033[0m\n", MESSAGE_BUF_SIZE - 1);
        return;
    }

    Message msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = MSG_BROADCAST;
    strncpy(msg.sender, client->username, USERNAME_BUF_SIZE - 1);
    strncpy(msg.room, client->current_room, ROOM_NAME_BUF_SIZE - 1); // Server uses this to route
    strncpy(msg.content, message_content, MESSAGE_BUF_SIZE - 1);
    // Ensure null termination
    msg.sender[USERNAME_BUF_SIZE - 1] = '\0';
    msg.room[ROOM_NAME_BUF_SIZE - 1] = '\0';
    msg.content[MESSAGE_BUF_SIZE - 1] = '\0';

    if (!sendMessage(client->socket_fd, &msg))
    {
        printf("\033[31mFailed to send broadcast message. Connection may be lost.\033[0m\n");
    }
}

void sendWhisperCommand(ClientState *client, const char *target_username, const char *message_content)
{
    if (!isValidUsername(target_username))
    {
        printf("\033[31mInvalid target username: Must be alphanumeric, 1-%d characters.\033[0m\n", MAX_USERNAME_LEN);
        return;
    }
    if (strcmp(client->username, target_username) == 0)
    {
        printf("\033[31mYou cannot whisper to yourself.\033[0m\n");
        return;
    }
    if (strlen(message_content) == 0)
    {
        printf("\033[31mCannot whisper an empty message.\033[0m\n");
        return;
    }
    if (strlen(message_content) >= MESSAGE_BUF_SIZE)
    {
        printf("\033[31mMessage is too long (max %d characters).\033[0m\n", MESSAGE_BUF_SIZE - 1);
        return;
    }

    Message msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = MSG_WHISPER;
    strncpy(msg.sender, client->username, USERNAME_BUF_SIZE - 1);
    strncpy(msg.receiver, target_username, USERNAME_BUF_SIZE - 1);
    strncpy(msg.content, message_content, MESSAGE_BUF_SIZE - 1);
    // Ensure null termination
    msg.sender[USERNAME_BUF_SIZE - 1] = '\0';
    msg.receiver[USERNAME_BUF_SIZE - 1] = '\0';
    msg.content[MESSAGE_BUF_SIZE - 1] = '\0';

    if (!sendMessage(client->socket_fd, &msg))
    {
        printf("\033[31mFailed to send whisper message. Connection may be lost.\033[0m\n");
    }
}

void sendFileRequestCommand(ClientState *client, const char *filepath, const char *target_username)
{
    if (!isValidUsername(target_username))
    {
        printf("\033[31mInvalid recipient username: Must be alphanumeric, 1-%d characters.\033[0m\n", MAX_USERNAME_LEN);
        return;
    }
    if (strcmp(client->username, target_username) == 0)
    {
        printf("\033[31mYou cannot send a file to yourself.\033[0m\n");
        return;
    }

    // Extract filename from the full path
    const char *filename_ptr = strrchr(filepath, '/');
    if (filename_ptr)
    {
        filename_ptr++; // Move past the '/' to the actual filename
    }
    else
    {
        filename_ptr = filepath; // No '/' in path, so filepath is the filename
    }

    long file_size = 0;

    // Validate local file (type, size, existence) using the helper function
    if (!prepareLocalFile(filepath, filename_ptr, &file_size))
    {
        // Error message already printed by prepareLocalFile
        return;
    }

    // 1. Send File Transfer Request (Header with metadata only)
    Message msg_header;
    memset(&msg_header, 0, sizeof(msg_header));
    msg_header.type = MSG_FILE_TRANSFER_REQUEST;
    strncpy(msg_header.sender, client->username, USERNAME_BUF_SIZE - 1);
    strncpy(msg_header.receiver, target_username, USERNAME_BUF_SIZE - 1);
    strncpy(msg_header.filename, filename_ptr, FILENAME_BUF_SIZE - 1);
    msg_header.file_size = (size_t)file_size; // Cast from long to size_t

    // Ensure null termination for safety
    msg_header.sender[USERNAME_BUF_SIZE - 1] = '\0';
    msg_header.receiver[USERNAME_BUF_SIZE - 1] = '\0';
    msg_header.filename[FILENAME_BUF_SIZE - 1] = '\0';

    if (!sendMessage(client->socket_fd, &msg_header))
    {
        printf("\033[31mFailed to send file transfer request header for '%s'. Connection may be lost.\033[0m\n", filename_ptr);
        return;
    }

    // 2. Wait for server's response (Accept or Reject)
    // This is a blocking receive. The receiver thread handles asynchronous messages.
    // For simplicity in command flow, this specific response is handled here.
    // A more complex UI might integrate this into the general message flow.
    Message server_response;
    if (!receiveMessage(client->socket_fd, &server_response))
    {
        printf("\033[31mFailed to receive server response for file transfer request. Connection may be lost.\033[0m\n");
        // If connection is lost, main loops should handle exit.
        return;
    }

    // Process server response (specific to file transfer context)
    if (server_response.type == MSG_FILE_TRANSFER_REJECT)
    {
        // Use red for rejection message from server
        printf("\033[31m[SERVER]: File transfer rejected: %s (File: '%s')\033[0m\n", server_response.content, server_response.filename);
    }
    else if (server_response.type == MSG_FILE_TRANSFER_ACCEPT)
    {
        // Use green for acceptance message from server
        printf("\033[32m[SERVER]: %s (File: '%s' to %s)\033[0m\n", server_response.content, server_response.filename, target_username);
        // As per Q&A #10, actual binary transfer is not mandatory.
        // Server handles the "transfer" (simulation) after this acceptance.
        // Client does not need to send file data chunks.
    }
    else
    {
        // Unexpected response type from server
        printf("\033[31m[SERVER]: Unexpected response type %d to file transfer request: %s\033[0m\n",
               server_response.type, server_response.content);
    }
}

// Sends a disconnect signal to the server.
// This is a best-effort notification.
void sendDisconnectSignal(ClientState *client)
{
    if (client->socket_fd < 0)
        return; // Socket already closed or never opened

    Message msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = MSG_DISCONNECT;
    strncpy(msg.sender, client->username, USERNAME_BUF_SIZE - 1);
    msg.sender[USERNAME_BUF_SIZE - 1] = '\0';

    // sendMessage is best-effort; if server is down, it will fail silently here (or errno set)
    // The client will proceed with local shutdown regardless.
    sendMessage(client->socket_fd, &msg);
}

// Displays the help message with available commands and usage instructions.
void displayHelpMessage(void)
{
    // Using color codes for better readability
    // \033[36m for Cyan (borders, titles)
    // \033[33m for Yellow (commands)
    // \033[32m for Green (info text)
    // \033[0m  for Reset color
    printf("\n\033[36m╔══════════════════════════════════════════════════════════════════╗\033[0m\n");
    printf("\033[36m║                        \033[1mAvailable Commands\033[0m                        \033[36m║\033[0m\n");
    printf("\033[36m╠══════════════════════════════════════════════════════════════════╣\033[0m\n");
    printf("\033[36m║\033[0m \033[33m/join <room_name>\033[0m           \033[36m│\033[0m Join or create a chat room         \033[36m║\033[0m\n");
    printf("\033[36m║\033[0m \033[33m/leave\033[0m                      \033[36m│\033[0m Leave the current chat room        \033[36m║\033[0m\n");
    printf("\033[36m║\033[0m \033[33m/broadcast <message>\033[0m        \033[36m│\033[0m Send message to all in current room\033[36m║\033[0m\n");
    printf("\033[36m║\033[0m \033[33m/whisper <user> <message>\033[0m   \033[36m│\033[0m Send a private message to a user   \033[36m║\033[0m\n");
    printf("\033[36m║\033[0m \033[33m/sendfile <filepath> <user>\033[0m \033[36m│\033[0m Send a file to a specific user     \033[36m║\033[0m\n");
    printf("\033[36m║\033[0m \033[33m/help\033[0m                       \033[36m│\033[0m Show this help message             \033[36m║\033[0m\n");
    printf("\033[36m║\033[0m \033[33m/exit\033[0m                       \033[36m│\033[0m Disconnect from server and exit    \033[36m║\033[0m\n");
    printf("\033[36m╠══════════════════════════════════════════════════════════════════╣\033[0m\n");
    printf("\033[36m║\033[0m \033[1m\033[32mFile Transfer Info:\033[0m                                              \033[36m║\033[0m\n");
    printf("\033[36m║\033[0m   • Supported types: .txt, .pdf, .jpg, .png                      \033[36m║\033[0m\n");
    printf("\033[36m║\033[0m   • Maximum file size: %dMB                                       \033[36m║\033[0m\n", MAX_FILE_SIZE / (1024 * 1024));
    printf("\033[36m║\033[0m                                                                  \033[36m║\033[0m\n");
    printf("\033[36m║\033[0m \033[1m\033[32mNaming Conventions:\033[0m                                              \033[36m║\033[0m\n");
    printf("\033[36m║\033[0m   • Usernames: Alphanumeric, 1-%2d characters                     \033[36m║\033[0m\n", MAX_USERNAME_LEN);
    printf("\033[36m║\033[0m   • Room names: Alphanumeric, 1-%2d characters, no spaces         \033[36m║\033[0m\n", MAX_ROOM_NAME_LEN);
    printf("\033[36m╚══════════════════════════════════════════════════════════════════╝\033[0m\n\n");
    fflush(stdout);
}