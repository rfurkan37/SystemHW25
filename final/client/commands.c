#include "common.h" // Includes shared/protocol.h and shared/utils.h
#include <ctype.h> // For isspace

// Helper to trim leading whitespace, useful for message part of commands
const char *trim_leading_whitespace(const char *str) {
    while (*str && isspace((unsigned char)*str)) {
        str++;
    }
    return str;
}

void process_user_command(client_state_t *client, const char *input) {
    char command_buffer[64]; // For the command itself e.g. "/join"
    char arg1_buffer[FILENAME_BUF_SIZE]; // Max for filename or room_name or username
    char arg2_buffer[MESSAGE_BUF_SIZE];  // Max for message content or username

    // Clear buffers
    memset(command_buffer, 0, sizeof(command_buffer));
    memset(arg1_buffer, 0, sizeof(arg1_buffer));
    memset(arg2_buffer, 0, sizeof(arg2_buffer));

    const char *ptr = input;
    int i = 0;

    // Extract command
    while (*ptr && !isspace((unsigned char)*ptr) && i < (int)sizeof(command_buffer) - 1) {
        command_buffer[i++] = *ptr++;
    }
    command_buffer[i] = '\0';
    ptr = trim_leading_whitespace(ptr);

    // Special handling for commands where the rest of the line is a single argument (message)
    if (strcmp(command_buffer, "/broadcast") == 0) {
        if (strlen(ptr) > 0) { // Check if there is a message
            strncpy(arg1_buffer, ptr, sizeof(arg1_buffer) - 1); // Message is arg1
            send_broadcast_command(client, arg1_buffer);
        } else {
            printf("\033[31mUsage: /broadcast <message>\033[0m\n");
        }
        return;
    }

    if (strcmp(command_buffer, "/whisper") == 0) {
        const char *username_end = ptr;
        i = 0;
        // Extract username (arg1)
        while (*username_end && !isspace((unsigned char)*username_end) && i < MAX_USERNAME_LEN) {
            arg1_buffer[i++] = *username_end++;
        }
        arg1_buffer[i] = '\0';
        username_end = trim_leading_whitespace(username_end);

        if (strlen(arg1_buffer) > 0 && strlen(username_end) > 0) { // Check for username and message
            strncpy(arg2_buffer, username_end, sizeof(arg2_buffer) - 1); // Message is arg2
            send_whisper_command(client, arg1_buffer, arg2_buffer);
        } else {
            printf("\033[31mUsage: /whisper <username> <message>\033[0m\n");
        }
        return;
    }

    // For other commands, parse up to two arguments
    i = 0;
    // Extract arg1
    while (*ptr && !isspace((unsigned char)*ptr) && i < (int)sizeof(arg1_buffer) - 1) {
        arg1_buffer[i++] = *ptr++;
    }
    arg1_buffer[i] = '\0';
    ptr = trim_leading_whitespace(ptr);

    i = 0;
    // Extract arg2
    while (*ptr && !isspace((unsigned char)*ptr) && i < (int)sizeof(arg2_buffer) - 1) {
        arg2_buffer[i++] = *ptr++;
    }
     // For sendfile, arg2 is username, doesn't contain spaces. For others, arg2 might not be used.
    arg2_buffer[i] = '\0';


    // --- Command dispatching ---
    if (strcmp(command_buffer, "/join") == 0) {
        if (strlen(arg1_buffer) > 0) {
            send_join_room_command(client, arg1_buffer);
        } else {
            printf("\033[31mUsage: /join <room_name>\033[0m\n");
        }
    } else if (strcmp(command_buffer, "/leave") == 0) {
        send_leave_room_command(client);
    } else if (strcmp(command_buffer, "/sendfile") == 0) {
        // /sendfile <filename> <username>
        if (strlen(arg1_buffer) > 0 && strlen(arg2_buffer) > 0) {
            send_file_request_command(client, arg1_buffer, arg2_buffer); // arg1=filename, arg2=username
        } else {
            printf("\033[31mUsage: /sendfile <filename> <username>\033[0m\n");
            printf("\033[32mSupported file types: .txt, .pdf, .jpg, .png (max 3MB)\033[0m\n");
        }
    } else if (strcmp(command_buffer, "/exit") == 0) {
        send_disconnect_signal(client);
        client->connected = 0; // Signal input loop to stop
    } else if (strcmp(command_buffer, "/help") == 0) {
        display_help_message();
    } else {
        printf("\033[31mUnknown command: '%s'. Type /help for available commands.\033[0m\n", command_buffer);
    }
}

void send_join_room_command(client_state_t *client, const char *room_name) {
    if (!is_valid_room_name(room_name)) {
        printf("\033[31mInvalid room name: Alphanumeric, 1-%d chars, no spaces.\033[0m\n", MAX_ROOM_NAME_LEN);
        return;
    }

    message_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = MSG_JOIN_ROOM;
    strncpy(msg.sender, client->username, USERNAME_BUF_SIZE -1);
    strncpy(msg.room, room_name, ROOM_NAME_BUF_SIZE - 1);

    if (!send_message(client->socket_fd, &msg)) {
        printf("\033[31mFailed to send join command to server.\033[0m\n");
    }
    // Server will send a success/error message, handled by receiver_thread
}

void send_leave_room_command(client_state_t *client) {
    if (strlen(client->current_room) == 0) {
        printf("\033[31mYou are not currently in any room.\033[0m\n");
        return;
    }
    message_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = MSG_LEAVE_ROOM;
    strncpy(msg.sender, client->username, USERNAME_BUF_SIZE -1);
    // Server knows current room from client state on server-side, but can send if needed
    // strncpy(msg.room, client->current_room, ROOM_NAME_BUF_SIZE - 1); 

    if (!send_message(client->socket_fd, &msg)) {
        printf("\033[31mFailed to send leave command to server.\033[0m\n");
    }
    // Server confirmation will update client's current_room state.
}

void send_broadcast_command(client_state_t *client, const char *message_content) {
    if (strlen(client->current_room) == 0) {
        printf("\033[31mYou must be in a room to broadcast. Use /join <room_name>.\033[0m\n");
        return;
    }
    if (strlen(message_content) == 0) {
        printf("\033[31mCannot broadcast an empty message.\033[0m\n");
        return;
    }
    if (strlen(message_content) >= MESSAGE_BUF_SIZE) {
        printf("\033[31mMessage too long (max %d chars).\033[0m\n", MESSAGE_BUF_SIZE -1);
        return;
    }

    message_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = MSG_BROADCAST;
    strncpy(msg.sender, client->username, USERNAME_BUF_SIZE -1);
    strncpy(msg.room, client->current_room, ROOM_NAME_BUF_SIZE -1); // Server uses this to route
    strncpy(msg.content, message_content, MESSAGE_BUF_SIZE - 1);

    if (!send_message(client->socket_fd, &msg)) {
        printf("\033[31mFailed to send broadcast message.\033[0m\n");
    }
    // Client does NOT print its own broadcast. Server sends confirmation/echo.
    // PDF example: > /broadcast Hello team! -> [Server]: Message sent to room 'teamchat'
    // Then, client receives the actual broadcast message through message_receiver thread.
}

void send_whisper_command(client_state_t *client, const char *target_username, const char *message_content) {
    if (!is_valid_username(target_username)) {
        printf("\033[31mInvalid target username: Alphanumeric, 1-%d chars.\033[0m\n", MAX_USERNAME_LEN);
        return;
    }
    if (strcmp(client->username, target_username) == 0) {
        printf("\033[31mYou cannot whisper to yourself.\033[0m\n");
        return;
    }
    if (strlen(message_content) == 0) {
        printf("\033[31mCannot whisper an empty message.\033[0m\n");
        return;
    }
    if (strlen(message_content) >= MESSAGE_BUF_SIZE) {
        printf("\033[31mMessage too long (max %d chars).\033[0m\n", MESSAGE_BUF_SIZE -1);
        return;
    }


    message_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = MSG_WHISPER;
    strncpy(msg.sender, client->username, USERNAME_BUF_SIZE -1);
    strncpy(msg.receiver, target_username, USERNAME_BUF_SIZE -1);
    strncpy(msg.content, message_content, MESSAGE_BUF_SIZE - 1);

    if (!send_message(client->socket_fd, &msg)) {
        printf("\033[31mFailed to send whisper message.\033[0m\n");
    }
    // Client does NOT print its own whisper. Server sends confirmation.
    // PDF example: > /whisper john42 Can you check this? -> [Server]: Whisper sent to john42
}

void send_file_request_command(client_state_t *client, const char *filepath, const char *target_username) {
    if (!is_valid_username(target_username)) {
        printf("\033[31mInvalid receiver username (alphanumeric, max %d chars).\033[0m\n", MAX_USERNAME_LEN);
        return;
    }
    if (strcmp(client->username, target_username) == 0) {
        printf("\033[31mYou cannot send a file to yourself.\033[0m\n");
        return;
    }

    const char *filename_ptr = strrchr(filepath, '/');
    filename_ptr = (filename_ptr) ? filename_ptr + 1 : filepath;

    if (strlen(filename_ptr) >= FILENAME_BUF_SIZE) {
        printf("\033[31mFilename too long (max %d characters).\033[0m\n", FILENAME_BUF_SIZE - 1);
        return;
    }
    if (!is_valid_file_type(filename_ptr)) {
        printf("\033[31mInvalid file type! Supported: .txt, .pdf, .jpg, .png\033[0m\n");
        return;
    }

    int fd = open(filepath, O_RDONLY);
    if (fd < 0) {
        printf("\033[31mFailed to open file '%s': %s\033[0m\n", filepath, strerror(errno));
        return;
    }

    long file_size = get_file_size_from_fd(fd); // Using shared utility
    if (file_size < 0) {
        printf("\033[31mFailed to get file size for '%s'.\033[0m\n", filepath);
        close(fd);
        return;
    }
    if (file_size == 0) {
        printf("\033[31mCannot send an empty file.\033[0m\n");
        close(fd);
        return;
    }
    if (file_size > MAX_FILE_SIZE) {
        printf("\033[31mFile size %ld bytes exceeds limit of %d MB.\033[0m\n", file_size, MAX_FILE_SIZE / (1024 * 1024));
        close(fd);
        return;
    }

    char *file_data_buffer = malloc(file_size);
    if (!file_data_buffer) {
        printf("\033[31mMemory allocation failed for file data.\033[0m\n");
        close(fd);
        return;
    }

    ssize_t bytes_read = read(fd, file_data_buffer, file_size);
    close(fd); // Close file descriptor as soon as read is done

    if (bytes_read != file_size) {
        printf("\033[31mFailed to read entire file ('%s'). Read %zd, expected %ld.\033[0m\n", filepath, bytes_read, file_size);
        free(file_data_buffer);
        return;
    }

    message_t msg_header;
    memset(&msg_header, 0, sizeof(msg_header));
    msg_header.type = MSG_FILE_TRANSFER_REQUEST; // Client requests to send a file
    strncpy(msg_header.sender, client->username, USERNAME_BUF_SIZE -1);
    strncpy(msg_header.receiver, target_username, USERNAME_BUF_SIZE -1);
    strncpy(msg_header.filename, filename_ptr, FILENAME_BUF_SIZE - 1);
    msg_header.file_size = (size_t)file_size;

    if (send_message(client->socket_fd, &msg_header)) {
        // If header sent successfully, now send the actual file data
        size_t total_bytes_sent = 0;
        char *current_pos_in_buffer = file_data_buffer;
        while (total_bytes_sent < (size_t)file_size) {
            ssize_t sent_now = send(client->socket_fd, current_pos_in_buffer, (size_t)file_size - total_bytes_sent, 0);
            if (sent_now <= 0) {
                printf("\033[31mFailed to send file data chunk: %s. Transfer aborted.\033[0m\n", strerror(errno));
                // Server might be unaware of the partial send. Client needs to handle this state.
                // For this project, we assume the server will timeout or the client disconnects.
                break; 
            }
            total_bytes_sent += sent_now;
            current_pos_in_buffer += sent_now;
        }

        if (total_bytes_sent == (size_t)file_size) {
            // Successfully sent header and all data. Now wait for server confirmation
            // like "[Server]: File added to the upload queue." via message_receiver thread.
            // No local print here, as per PDF.
        } else {
            printf("\033[31mIncomplete file data transfer for '%s' to %s. Sent %zu of %ld bytes.\033[0m\n",
                   filename_ptr, target_username, total_bytes_sent, file_size);
        }
    } else {
        printf("\033[31mFailed to send file transfer request header for '%s'.\033[0m\n", filename_ptr);
    }

    free(file_data_buffer); // IMPORTANT: free the buffer after sending or on failure
}

void send_disconnect_signal(client_state_t *client) {
    message_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = MSG_DISCONNECT;
    strncpy(msg.sender, client->username, USERNAME_BUF_SIZE-1);

    // Best effort send, client is exiting anyway.
    if(send_message(client->socket_fd, &msg)){
         // printf("\033[33mDisconnect signal sent to server.\033[0m\n"); // Optional feedback
    } else {
        // printf("\033[31mFailed to send disconnect signal. Closing locally.\033[0m\n"); // Optional
    }
}

void display_help_message(void) {
    printf("\033[36m=== Available Commands ===\033[0m\n");
    printf("\033[33m/join <room_name>\033[0m          - Join or create a chat room (e.g., /join general)\n");
    printf("\033[33m/leave\033[0m                     - Leave the current chat room\n");
    printf("\033[33m/broadcast <message>\033[0m       - Send a message to everyone in your current room\n");
    printf("\033[33m                               (e.g., /broadcast Hello everyone!)\n");
    printf("\033[33m/whisper <username> <message>\033[0m - Send a private message to a specific user\n");
    printf("\033[33m                               (e.g., /whisper alice Hi Alice)\n");
    printf("\033[33m/sendfile <filepath> <user>\033[0m - Send a file to a specific user\n");
    printf("\033[33m                               (e.g., /sendfile documents/report.pdf bob)\n");
    printf("\033[33m/help\033[0m                      - Show this help message\n");
    printf("\033[33m/exit\033[0m                      - Disconnect from the server and exit the client\n");
    printf("\033[36m========================\033[0m\n");
    printf("\033[32mFile Info: Supported types: .txt, .pdf, .jpg, .png. Max size: 3MB.\033[0m\n");
    printf("\033[32mUsernames & Room Names: Alphanumeric, no spaces.\n");
    printf("\033[32mMax Username: %d chars. Max Room Name: %d chars.\033[0m\n", MAX_USERNAME_LEN, MAX_ROOM_NAME_LEN);
}