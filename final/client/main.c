#include "common.h"

// Global client state for signal handling
client_state_t* g_client_state = NULL;

void signal_handler(int sig) {
    if (sig == SIGINT && g_client_state) {
        printf("\n\033[33mDisconnecting from server...\033[0m\n");
        send_disconnect_command(g_client_state);
        g_client_state->connected = 0;
        
        // Signal main thread via pipe for graceful shutdown
        char signal_byte = 1;
        write(g_client_state->shutdown_pipe[1], &signal_byte, 1);
        
        // Don't call exit() - let main thread handle cleanup
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
    strncpy(login_msg.sender, username, MAX_USERNAME_LEN);
    login_msg.sender[MAX_USERNAME_LEN] = '\0';
    
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
        // Safe copy with bounds checking
        strncpy(client->username, username, MAX_USERNAME_LEN);
        client->username[MAX_USERNAME_LEN] = '\0';
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
                // Signal main thread via pipe
                char signal_byte = 1;
                write(client->shutdown_pipe[1], &signal_byte, 1);
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
                    // Safe copy with bounds checking
                    strncpy(client->current_room, msg.room, MAX_ROOM_NAME_LEN);
                    client->current_room[MAX_ROOM_NAME_LEN] = '\0';
                }
                printf("\033[32m[SERVER] %s\033[0m\n", msg.content);
                break;
                
            case MSG_ERROR:
                printf("\033[31m[ERROR] %s\033[0m\n", msg.content);
                if (strstr(msg.content, "shutting down")) {
                    printf("\033[33mServer is shutting down. Disconnecting...\033[0m\n");
                    client->connected = 0;
                    // Signal main thread via pipe to exit cleanly
                    char signal_byte = 1;
                    write(client->shutdown_pipe[1], &signal_byte, 1);
                    break;  // Break from switch, then exit loop naturally
                }
                break;
                
            case MSG_FILE_DATA:
                printf("\033[35m[FILE] Receiving file '%s' from %s (%zu bytes)\033[0m\n", 
                       msg.filename, msg.sender, msg.file_size);
                
                // Validate file size to prevent huge allocations
                if (msg.file_size > MAX_FILE_SIZE || msg.file_size == 0) {
                    printf("\033[31m[FILE] Invalid file size\033[0m\n");
                    break;
                }
                
                // Receive file data
                char* file_data = malloc(msg.file_size);
                if (file_data) {
                    size_t bytes_received = 0;
                    while (bytes_received < msg.file_size) {
                        ssize_t received = recv(client->socket_fd, 
                                               file_data + bytes_received, 
                                               msg.file_size - bytes_received, 0);
                        if (received <= 0) {
                            printf("\033[31m[FILE] Failed to receive file data\033[0m\n");
                            free(file_data);  // Free on error
                            file_data = NULL;
                            break;
                        }
                        bytes_received += received;
                    }
                    
                    if (file_data && bytes_received == msg.file_size) {
                        // Handle filename collision by finding unique name
                        char filename[MAX_FILENAME_LEN + 20];
                        int counter = 0;
                        int fd = -1;
                        
                        // Try original filename first
                        int ret = snprintf(filename, sizeof(filename), "received_%.200s", msg.filename);
                        if (ret >= (int)sizeof(filename)) {
                            printf("\033[31m[FILE] Filename too long\033[0m\n");
                            free(file_data);
                            break;
                        }
                        
                        // Check for collision and find unique name
                        while (access(filename, F_OK) == 0) {
                            counter++;
                            ret = snprintf(filename, sizeof(filename), "received_%.190s_%d", msg.filename, counter);
                            if (ret >= (int)sizeof(filename)) {
                                printf("\033[31m[FILE] Unable to create unique filename\033[0m\n");
                                free(file_data);
                                goto file_cleanup;
                            }
                        }
                        
                        // Create and write file using system calls
                        fd = open(filename, O_WRONLY | O_CREAT | O_EXCL, 0644);
                        if (fd >= 0) {
                            ssize_t bytes_written = write(fd, file_data, msg.file_size);
                            close(fd);
                            if (bytes_written == (ssize_t)msg.file_size) {
                                printf("\033[32m[FILE] File saved as '%s'\033[0m\n", filename);
                            } else {
                                printf("\033[31m[FILE] Failed to write complete file\033[0m\n");
                            }
                        } else {
                            printf("\033[31m[FILE] Failed to create file: %s\033[0m\n", strerror(errno));
                        }
                        
                        file_cleanup:;
                    }
                    
                    if (file_data) {
                        free(file_data);
                    }
                } else {
                    printf("\033[31m[FILE] Memory allocation failed\033[0m\n");
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
    fd_set readfds;
    int max_fd;
    
    printf("\n\033[36mYou are now connected! Type /help for commands.\033[0m\n");
    printf("> ");
    fflush(stdout);
    
    while (client->connected) {
        FD_ZERO(&readfds);
        FD_SET(STDIN_FILENO, &readfds);
        FD_SET(client->shutdown_pipe[0], &readfds);
        
        max_fd = (client->shutdown_pipe[0] > STDIN_FILENO) ? 
                 client->shutdown_pipe[0] : STDIN_FILENO;
        
        int activity = select(max_fd + 1, &readfds, NULL, NULL, NULL);
        
        if (activity < 0) {
            if (errno != EINTR) {
                perror("select error");
            }
            break;
        }
        
        // Check for shutdown signal
        if (FD_ISSET(client->shutdown_pipe[0], &readfds)) {
            // Shutdown signal received from receiver thread
            break;
        }
        
        // Check for user input
        if (FD_ISSET(STDIN_FILENO, &readfds)) {
            if (!fgets(input, sizeof(input), stdin)) {
                // EOF or error on stdin
                break;
            }
            
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
    
    // Close shutdown pipe
    if (client->shutdown_pipe[0] >= 0) {
        close(client->shutdown_pipe[0]);
    }
    if (client->shutdown_pipe[1] >= 0) {
        close(client->shutdown_pipe[1]);
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
    
    // Create shutdown pipe for inter-thread communication
    if (pipe(client.shutdown_pipe) == -1) {
        perror("pipe creation failed");
        return 1;
    }
    
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
    int ret = pthread_create(&client.receiver_thread, NULL, message_receiver, &client);
    if (ret != 0) {
        fprintf(stderr, "Failed to create receiver thread: %s\n", strerror(ret));
        cleanup_client(&client);
        return 1;
    }
    
    // Main input loop
    handle_user_input(&client);
    
    // Ensure clean shutdown
    client.connected = 0;
    
    // Wait for receiver thread to finish properly
    pthread_join(client.receiver_thread, NULL);
    
    // Cleanup
    cleanup_client(&client);
    printf("\033[36mGoodbye!\033[0m\n");
    
    return 0;
} 