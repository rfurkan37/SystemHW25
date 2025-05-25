#include "common.h"

// Global client state for signal handling
client_state_t* g_client_state = NULL;

void signal_handler(int sig) {
    if (sig == SIGINT && g_client_state) {
        printf("\n\033[33mDisconnecting from server...\033[0m\n");
        send_disconnect_command(g_client_state);
        g_client_state->connected = 0;
    }
}

int handle_client_login(client_state_t* client) {
    char username[MAX_USERNAME_LEN + 1];
    
    printf("\033[36m=== Chat Client ===\033[0m\n");
    printf("Enter username (alphanumeric, max %d chars): ", MAX_USERNAME_LEN);
    fflush(stdout);
    
    if (!fgets(username, sizeof(username), stdin)) {
        return 0;
    }
    
    // Remove newline
    username[strcspn(username, "\n")] = '\0';
    
    if (!is_valid_username(username)) {
        printf("\033[31mInvalid username format!\033[0m\n");
        return 0;
    }
    
    // Send login message
    message_t login_msg;
    memset(&login_msg, 0, sizeof(login_msg));
    login_msg.type = MSG_LOGIN;
    strcpy(login_msg.sender, username);
    
    if (!send_message(client->socket_fd, &login_msg)) {
        printf("\033[31mFailed to send login message\033[0m\n");
        return 0;
    }
    
    // Wait for response
    message_t response;
    if (!receive_message(client->socket_fd, &response)) {
        printf("\033[31mNo response from server\033[0m\n");
        return 0;
    }
    
    if (response.type == MSG_SUCCESS) {
        strcpy(client->username, username);
        printf("\033[32m%s\033[0m\n", response.content);
        printf("\033[36mWelcome, %s! Type /help for available commands.\033[0m\n", username);
        return 1;
    } else {
        printf("\033[31mLogin failed: %s\033[0m\n", response.content);
        return 0;
    }
}

void* message_receiver(void* arg) {
    client_state_t* client = (client_state_t*)arg;
    message_t msg;
    
    while (client->connected) {
        if (receive_message(client->socket_fd, &msg) <= 0) {
            if (client->connected) {
                printf("\n\033[31mConnection lost to server\033[0m\n");
                client->connected = 0;
            }
            break;
        }
        
        switch (msg.type) {
            case MSG_BROADCAST:
                print_colored_message("BROADCAST", msg.sender, msg.content);
                break;
                
            case MSG_WHISPER:
                print_colored_message("WHISPER", msg.sender, msg.content);
                break;
                
            case MSG_SUCCESS:
                if (strlen(msg.room) > 0) {
                    strcpy(client->current_room, msg.room);
                }
                printf("\033[32m[SERVER] %s\033[0m\n", msg.content);
                break;
                
            case MSG_ERROR:
                printf("\033[31m[ERROR] %s\033[0m\n", msg.content);
                if (strstr(msg.content, "shutting down")) {
                    printf("\033[33mServer is shutting down. Disconnecting...\033[0m\n");
                    client->connected = 0;
                    // Close stdin to interrupt fgets() in main thread
                    close(STDIN_FILENO);
                }
                break;
                
            case MSG_FILE_DATA:
                printf("\033[35m[FILE] Receiving file '%s' from %s (%zu bytes)\033[0m\n", 
                       msg.filename, msg.sender, msg.file_size);
                
                // Receive file data
                char* file_data = malloc(msg.file_size);
                if (file_data) {
                    size_t bytes_received = 0;
                    while (bytes_received < msg.file_size) {
                        ssize_t received = recv(client->socket_fd, 
                                               file_data + bytes_received, 
                                               msg.file_size - bytes_received, 0);
                        if (received <= 0) break;
                        bytes_received += received;
                    }
                    
                    if (bytes_received == msg.file_size) {
                        // Save file
                        char filename[MAX_FILENAME_LEN + 20];
                        snprintf(filename, sizeof(filename), "received_%s", msg.filename);
                        
                        FILE* file = fopen(filename, "wb");
                        if (file) {
                            fwrite(file_data, 1, msg.file_size, file);
                            fclose(file);
                            printf("\033[32m[FILE] File saved as '%s'\033[0m\n", filename);
                        } else {
                            printf("\033[31m[FILE] Failed to save file\033[0m\n");
                        }
                    }
                    free(file_data);
                }
                break;
                
            default:
                break;
        }
        
        printf("> ");
        fflush(stdout);
    }
    
    return NULL;
}

void handle_user_input(client_state_t* client) {
    char input[MAX_MESSAGE_LEN + 100];
    
    printf("\n\033[36mYou are now connected! Type /help for commands.\033[0m\n");
    printf("> ");
    fflush(stdout);
    
    while (client->connected) {
        if (!fgets(input, sizeof(input), stdin)) {
            // fgets failed - either EOF or error
            if (client->connected) {
                // If we're still supposed to be connected, it means stdin was closed
                // (likely due to server shutdown), so break gracefully
                break;
            }
        } else {
            // Remove newline
            input[strcspn(input, "\n")] = '\0';
            
            if (strlen(input) > 0) {
                process_user_command(client, input);
            }
            
            if (client->connected) {
                printf("> ");
                fflush(stdout);
            }
        }
    }
}

void cleanup_client(client_state_t* client) {
    if (client->socket_fd >= 0) {
        close(client->socket_fd);
    }
}

int main(int argc, char* argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <server_ip> <port>\n", argv[0]);
        return 1;
    }
    
    client_state_t client;
    memset(&client, 0, sizeof(client));
    client.socket_fd = -1;
    g_client_state = &client;
    
    // Set up signal handler
    signal(SIGINT, signal_handler);
    
    // Connect to server
    if (!connect_to_server(&client, argv[1], atoi(argv[2]))) {
        return 1;
    }
    
    // Handle login
    if (!handle_client_login(&client)) {
        cleanup_client(&client);
        return 1;
    }
    
    client.connected = 1;
    
    // Start receiver thread
    pthread_create(&client.receiver_thread, NULL, message_receiver, &client);
    
    // Main input loop
    handle_user_input(&client);
    
    // Wait for receiver thread to finish
    if (client.connected) {
        client.connected = 0;
    }
    pthread_join(client.receiver_thread, NULL);
    
    // Cleanup
    cleanup_client(&client);
    printf("\033[36mGoodbye!\033[0m\n");
    
    return 0;
} 