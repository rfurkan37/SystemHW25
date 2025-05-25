#include "common.h"

void process_user_command(client_state_t* client, const char* input) {
    char command[64], arg1[256], arg2[1024];
    int args = sscanf(input, "%s %s %[^\n]", command, arg1, arg2);
    
    if (strcmp(command, "/join") == 0) {
        if (args >= 2) {
            send_join_command(client, arg1);
        } else {
            printf("\033[31mUsage: /join <room_name>\033[0m\n");
        }
    }
    else if (strcmp(command, "/leave") == 0) {
        send_leave_command(client);
    }
    else if (strcmp(command, "/broadcast") == 0) {
        if (args >= 2) {
            // Reconstruct the full message
            const char* message_start = input + strlen("/broadcast ");
            send_broadcast_command(client, message_start);
        } else {
            printf("\033[31mUsage: /broadcast <message>\033[0m\n");
        }
    }
    else if (strcmp(command, "/whisper") == 0) {
        if (args >= 3) {
            send_whisper_command(client, arg1, arg2);
        } else {
            printf("\033[31mUsage: /whisper <user> <message>\033[0m\n");
        }
    }
    else if (strcmp(command, "/sendfile") == 0) {
        if (args >= 3) {
            send_file_command(client, arg1, arg2);
        } else {
            printf("\033[31mUsage: /sendfile <user> <filename>\033[0m\n");
            printf("\033[32mSupported file types: .txt, .pdf, .jpg, .png (max 3MB)\033[0m\n");
        }
    }
    else if (strcmp(command, "/exit") == 0) {
        send_disconnect_command(client);
        client->connected = 0;
    }
    else if (strcmp(command, "/help") == 0) {
        print_help();
    }
    else {
        printf("\033[31mUnknown command. Type /help for available commands.\033[0m\n");
    }
}

void send_join_command(client_state_t* client, const char* room_name) {
    if (!is_valid_room_name(room_name)) {
        printf("\033[31mInvalid room name format!\033[0m\n");
        return;
    }
    
    message_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = MSG_JOIN_ROOM;
    strcpy(msg.sender, client->username);
    strcpy(msg.room, room_name);
    
    if (send_message(client->socket_fd, &msg)) {
        printf("\033[33mJoining room '%s'...\033[0m\n", room_name);
    } else {
        printf("\033[31mFailed to send join command\033[0m\n");
    }
}

void send_leave_command(client_state_t* client) {
    message_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = MSG_LEAVE_ROOM;
    strcpy(msg.sender, client->username);
    
    if (send_message(client->socket_fd, &msg)) {
        printf("\033[33mLeaving current room...\033[0m\n");
        memset(client->current_room, 0, sizeof(client->current_room));
    } else {
        printf("\033[31mFailed to send leave command\033[0m\n");
    }
}

void send_broadcast_command(client_state_t* client, const char* message) {
    if (strlen(client->current_room) == 0) {
        printf("\033[31mYou must join a room first to broadcast messages\033[0m\n");
        return;
    }
    
    message_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = MSG_BROADCAST;
    strcpy(msg.sender, client->username);
    strcpy(msg.room, client->current_room);
    strncpy(msg.content, message, MAX_MESSAGE_LEN - 1);
    msg.content[MAX_MESSAGE_LEN - 1] = '\0';
    
    if (send_message(client->socket_fd, &msg)) {
        printf("\033[36m[%s] %s: %s\033[0m\n", client->current_room, client->username, message);
    } else {
        printf("\033[31mFailed to send broadcast message\033[0m\n");
    }
}

void send_whisper_command(client_state_t* client, const char* receiver, const char* message) {
    if (!is_valid_username(receiver)) {
        printf("\033[31mInvalid receiver username format!\033[0m\n");
        return;
    }
    
    message_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = MSG_WHISPER;
    strcpy(msg.sender, client->username);
    strcpy(msg.receiver, receiver);
    strncpy(msg.content, message, MAX_MESSAGE_LEN - 1);
    msg.content[MAX_MESSAGE_LEN - 1] = '\0';
    
    if (send_message(client->socket_fd, &msg)) {
        printf("\033[35m[WHISPER to %s] %s\033[0m\n", receiver, message);
    } else {
        printf("\033[31mFailed to send whisper message\033[0m\n");
    }
}

void send_file_command(client_state_t* client, const char* receiver, const char* filename) {
    if (!is_valid_username(receiver)) {
        printf("\033[31mInvalid receiver username format!\033[0m\n");
        return;
    }
    
    if (!is_valid_file_type(filename)) {
        printf("\033[31mInvalid file type! Supported: .txt, .pdf, .jpg, .png\033[0m\n");
        return;
    }
    
    // Open and read file
    FILE* file = fopen(filename, "rb");
    if (!file) {
        printf("\033[31mFailed to open file '%s': %s\033[0m\n", filename, strerror(errno));
        return;
    }
    
    // Get file size
    long file_size = get_file_size(file);
    if (file_size <= 0 || file_size > MAX_FILE_SIZE) {
        printf("\033[31mInvalid file size (max %d MB)\033[0m\n", MAX_FILE_SIZE / (1024 * 1024));
        fclose(file);
        return;
    }
    
    // Read file data
    char* file_data = malloc(file_size);
    if (!file_data) {
        printf("\033[31mMemory allocation failed\033[0m\n");
        fclose(file);
        return;
    }
    
    if (fread(file_data, 1, file_size, file) != (size_t)file_size) {
        printf("\033[31mFailed to read file data\033[0m\n");
        free(file_data);
        fclose(file);
        return;
    }
    fclose(file);
    
    // Send file transfer message
    message_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = MSG_FILE_TRANSFER;
    strcpy(msg.sender, client->username);
    strcpy(msg.receiver, receiver);
    strcpy(msg.filename, filename);
    msg.file_size = file_size;
    
    if (send_message(client->socket_fd, &msg)) {
        // Send file data
        size_t bytes_sent = 0;
        while (bytes_sent < (size_t)file_size) {
            ssize_t sent = send(client->socket_fd, file_data + bytes_sent, 
                               file_size - bytes_sent, 0);
            if (sent <= 0) {
                printf("\033[31mFailed to send file data\033[0m\n");
                break;
            }
            bytes_sent += sent;
        }
        
        if (bytes_sent == (size_t)file_size) {
            printf("\033[32mFile '%s' sent to %s (%ld bytes)\033[0m\n", 
                   filename, receiver, file_size);
        }
    } else {
        printf("\033[31mFailed to send file transfer message\033[0m\n");
    }
    
    free(file_data);
}

void send_disconnect_command(client_state_t* client) {
    message_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = MSG_DISCONNECT;
    strcpy(msg.sender, client->username);
    
    send_message(client->socket_fd, &msg);
} 