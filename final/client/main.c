#include "common.h"
#include <sys/stat.h> // For S_IRUSR, S_IWUSR for received files
#include <libgen.h> // For basename
#include <time.h>   // For nanosleep

// Global client state pointer for signal handler
static client_state_t *g_client_state_ptr = NULL;

void signal_handler_client(int sig) {
    if (sig == SIGINT && g_client_state_ptr != NULL && g_client_state_ptr->connected) {
        printf("\n\033[33mSIGINT received. Attempting to disconnect gracefully...\033[0m\n");
        
        // Inform server of disconnection
        send_disconnect_signal(g_client_state_ptr);
        
        // Set connected flag to 0 to stop loops
        g_client_state_ptr->connected = 0; 

        // Signal the main input loop thread to wake up and exit
        // Writing a byte to the pipe is a common way to unblock select/poll
        if (g_client_state_ptr->shutdown_pipe_fds[1] != -1) {
            char signal_byte = 's'; // Arbitrary byte
            if (write(g_client_state_ptr->shutdown_pipe_fds[1], &signal_byte, 1) == -1) {
                // perror("write to shutdown_pipe failed in signal_handler");
                // If write fails, the select in input loop might not unblock immediately
                // but connected flag should still stop it.
            }
        }
        // The main thread will handle further cleanup.
    }
}

void *client_message_receiver_thread(void *arg) {
    client_state_t *client = (client_state_t *)arg;
    message_t received_msg;

    printf("\033[36mMessage receiver thread started.\033[0m\n");

    while (client->connected) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(client->socket_fd, &read_fds);

        struct timeval timeout;
        timeout.tv_sec = 1;  // Check for client->connected status every 1 second
        timeout.tv_usec = 0;

        int activity = select(client->socket_fd + 1, &read_fds, NULL, NULL, &timeout);

        if (!client->connected) break; // Check flag immediately after select or timeout

        if (activity < 0) {
            if (errno == EINTR) continue; // Interrupted by a signal (like SIGINT), loop and check connected flag
            perror("\033[31mselect() error in receiver thread\033[0m");
            client->connected = 0; // Assume connection lost on other errors
            break;
        }

        if (activity == 0) { // Timeout
            continue; // Loop back to check client->connected and select again
        }

        // If there's activity on the client socket
        if (FD_ISSET(client->socket_fd, &read_fds)) {
            if (receive_message(client->socket_fd, &received_msg) <= 0) {
                if (client->connected) { // Avoid message if already intentionally disconnecting
                    printf("\n\033[31mConnection to server lost or server closed connection.\033[0m\n");
                    client->connected = 0; // Signal main thread and self to stop
                }
                break;
            }

            // Process received message
            printf("\n"); // Newline to separate from user's potential input prompt line
            switch (received_msg.type) {
                case MSG_BROADCAST:
                    // Only display if client is in the room the broadcast is for
                    if (strcmp(client->current_room, received_msg.room) == 0) {
                        printf("\033[36m[%s] %s: %s\033[0m\n", 
                               received_msg.room, received_msg.sender, received_msg.content);
                    }
                    break;
                case MSG_WHISPER:
                    printf("\033[35m[WHISPER from %s]: %s\033[0m\n", 
                           received_msg.sender, received_msg.content);
                    break;
                case MSG_SERVER_NOTIFICATION: // For generic server messages like "User X joined"
                case MSG_SUCCESS: // For command success confirmations
                    if (strstr(received_msg.content, "Joined room") && strlen(received_msg.room) > 0) {
                        strncpy(client->current_room, received_msg.room, ROOM_NAME_BUF_SIZE -1);
                        printf("\033[32m[SERVER]: %s '%s'\033[0m\n", received_msg.content, received_msg.room);
                    } else if (strstr(received_msg.content, "Left room")) {
                        printf("\033[32m[SERVER]: %s\033[0m\n", received_msg.content);
                        memset(client->current_room, 0, sizeof(client->current_room));
                    } else if (strstr(received_msg.content, "Disconnected. Goodbye!")) {
                         printf("\033[33m[SERVER]: %s\033[0m\n", received_msg.content);
                         client->connected = 0; // Server confirms disconnect
                    }
                     else {
                        printf("\033[32m[SERVER]: %s\033[0m\n", received_msg.content);
                    }
                    break;
                case MSG_FILE_TRANSFER_ACCEPT: // Server confirms file is queued/being processed
                     printf("\033[32m[SERVER]: %s (Filename: %s)\033[0m\n", received_msg.content, received_msg.filename);
                    break;
                case MSG_ERROR:
                case MSG_LOGIN_FAILURE: // Handled at login, but good to catch here too
                case MSG_FILE_TRANSFER_REJECT:
                    printf("\033[31m[SERVER ERROR]: %s\033[0m\n", received_msg.content);
                    if (strstr(received_msg.content, "shutting down")) {
                        client->connected = 0;
                    }
                    break;
                case MSG_FILE_TRANSFER_DATA: // Header for incoming file
                    printf("\033[35m[FILE]: Receiving '%s' (%zu bytes) from %s.\033[0m\n",
                           received_msg.filename, received_msg.file_size, received_msg.sender);
                    
                    if (received_msg.file_size == 0 || received_msg.file_size > MAX_FILE_SIZE) {
                        printf("\033[31m[FILE]: Invalid file size. Transfer aborted.\033[0m\n");
                        // Client should ideally inform server, but for now, just skip receiving data
                        break; 
                    }

                    char *file_buffer = malloc(received_msg.file_size);
                    if (!file_buffer) {
                        printf("\033[31m[FILE]: Memory allocation failed for receiving file. (%zu bytes)\033[0m\n", received_msg.file_size);
                        // Skip receiving data
                        break; 
                    }

                    size_t total_bytes_received = 0;
                    while (total_bytes_received < received_msg.file_size) {
                        ssize_t chunk_received = recv(client->socket_fd, 
                                                      file_buffer + total_bytes_received,
                                                      received_msg.file_size - total_bytes_received, 0);
                        if (chunk_received <= 0) {
                            printf("\033[31m[FILE]: Failed to receive file data chunk or connection lost.\033[0m\n");
                            free(file_buffer);
                            file_buffer = NULL; // Mark as freed
                            client->connected = 0; // Assume critical error
                            goto file_receive_loop_exit; // Exit outer while loop
                        }
                        total_bytes_received += chunk_received;
                    }
                    
                    if (file_buffer && total_bytes_received == received_msg.file_size) {
                        char local_filename[FILENAME_BUF_SIZE + 20]; // Extra space for "received_" and counter
                        char original_basename[FILENAME_BUF_SIZE];
                        
                        // Use a copy for basename() as it might modify its argument
                        char temp_path[FILENAME_BUF_SIZE];
                        strncpy(temp_path, received_msg.filename, FILENAME_BUF_SIZE -1);
                        strncpy(original_basename, basename(temp_path), FILENAME_BUF_SIZE -1);

                        snprintf(local_filename, sizeof(local_filename), "received_%s", original_basename);
                        
                        int counter = 0;
                        // Handle filename collision (Test Scenario 9)
                        while (access(local_filename, F_OK) == 0) {
                            counter++;
                            snprintf(local_filename, sizeof(local_filename), "received_%s_%d", original_basename, counter);
                            if (counter > 100) { // Safety break
                                printf("\033[31m[FILE]: Could not find a unique name for '%s' after 100 attempts.\033[0m\n", original_basename);
                                free(file_buffer); file_buffer = NULL;
                                break;
                            }
                        }

                        if (file_buffer) { // Check if still valid after collision check
                            int out_fd = open(local_filename, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
                            if (out_fd >= 0) {
                                ssize_t bytes_written = write(out_fd, file_buffer, received_msg.file_size);
                                close(out_fd);
                                if (bytes_written == (ssize_t)received_msg.file_size) {
                                    printf("\033[32m[FILE]: File '%s' received and saved as '%s'.\033[0m\n", original_basename, local_filename);
                                } else {
                                    printf("\033[31m[FILE]: Error writing received file '%s' to disk.\033[0m\n", local_filename);
                                }
                            } else {
                                printf("\033[31m[FILE]: Could not open local file '%s' for writing: %s\033[0m\n", local_filename, strerror(errno));
                            }
                        }
                    }
                    if (file_buffer) free(file_buffer);
                    break;
                default:
                    printf("\033[33m[DEBUG] Received unhandled message type from server: %d\033[0m\n", received_msg.type);
                    // For robustness, you might want to print more details from received_msg
                    // printf("Content: %s\n", received_msg.content);
                    break;
            }
            if (client->connected) { // Only print prompt if still connected
                printf("> ");
                fflush(stdout);
            }
        }
        file_receive_loop_exit:; // Label for goto if critical error during file reception
    }
    
    if (client->shutdown_pipe_fds[1] != -1) { // Signal input loop if receiver exits first
         char signal_byte = 's';
         write(client->shutdown_pipe_fds[1], &signal_byte, 1); // Best effort
    }

    printf("\033[36mMessage receiver thread stopped.\033[0m\n");
    return NULL;
}


void handle_user_input_loop(client_state_t *client) {
    char input_buffer[MESSAGE_BUF_SIZE + FILENAME_BUF_SIZE + 20]; // Ample space for commands and args

    printf("> ");
    fflush(stdout);

    while (client->connected) {
        fd_set read_fds_input;
        FD_ZERO(&read_fds_input);
        FD_SET(STDIN_FILENO, &read_fds_input);
        FD_SET(client->shutdown_pipe_fds[0], &read_fds_input); // Listen on the read end of the pipe

        int max_fd_input = (client->shutdown_pipe_fds[0] > STDIN_FILENO) ? client->shutdown_pipe_fds[0] : STDIN_FILENO;

        int activity_input = select(max_fd_input + 1, &read_fds_input, NULL, NULL, NULL); // Blocking select

        if (!client->connected) break; // Check flag after select returns

        if (activity_input < 0) {
            if (errno == EINTR && client->connected) continue; // Interrupted by signal (e.g. SIGINT handled), re-check connected
            perror("\033[31mselect() error in input loop\033[0m");
            break; // Exit on other errors
        }

        // Check if shutdown was signaled via pipe (e.g., by receiver thread or SIGINT)
        if (FD_ISSET(client->shutdown_pipe_fds[0], &read_fds_input)) {
            char buf[1];
            read(client->shutdown_pipe_fds[0], buf, 1); // Consume the byte
            // printf("Shutdown signal received via pipe in input loop.\n");
            client->connected = 0; // Ensure flag is set
            break; 
        }

        // Check for user input from STDIN
        if (FD_ISSET(STDIN_FILENO, &read_fds_input)) {
            if (fgets(input_buffer, sizeof(input_buffer), stdin) != NULL) {
                input_buffer[strcspn(input_buffer, "\n")] = '\0'; // Remove newline

                if (strlen(input_buffer) > 0) {
                    process_user_command(client, input_buffer);
                }
                if (client->connected) { // Show prompt only if still connected after processing command
                    printf("> ");
                    fflush(stdout);
                }
            } else {
                // fgets returned NULL (EOF or error)
                if (feof(stdin)) {
                    printf("\n\033[33mEOF detected on input. Disconnecting...\033[0m\n");
                } else {
                    printf("\n\033[31mError reading input. Disconnecting...\033[0m\n");
                }
                if (client->connected) { // If not already disconnected by a command like /exit
                   send_disconnect_signal(client);
                }
                client->connected = 0; // Signal loop to stop
                break;
            }
        }
    }
     printf("\033[36mInput handling stopped.\033[0m\n");
}


void cleanup_client_resources(client_state_t *client_state) {
    printf("\033[36mCleaning up client resources...\033[0m\n");
    if (client_state->socket_fd >= 0) {
        // Shutdown can provide a more graceful TCP close
        shutdown(client_state->socket_fd, SHUT_RDWR); 
        close(client_state->socket_fd);
        client_state->socket_fd = -1;
    }
    // Close pipe FDs
    if (client_state->shutdown_pipe_fds[0] >= 0) {
        close(client_state->shutdown_pipe_fds[0]);
        client_state->shutdown_pipe_fds[0] = -1;
    }
    if (client_state->shutdown_pipe_fds[1] >= 0) {
        close(client_state->shutdown_pipe_fds[1]);
        client_state->shutdown_pipe_fds[1] = -1;
    }
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <server_ip> <port>\n", argv[0]);
        fprintf(stderr, "Example: %s 127.0.0.1 5000\n", argv[0]);
        return EXIT_FAILURE;
    }

    const char *server_ip = argv[1];
    int port = atoi(argv[2]);
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "\033[31mInvalid port number: %s. Must be 1-65535.\033[0m\n", argv[2]);
        return EXIT_FAILURE;
    }

    client_state_t client_state_instance;
    memset(&client_state_instance, 0, sizeof(client_state_instance));
    client_state_instance.socket_fd = -1; // Initialize to invalid
    client_state_instance.shutdown_pipe_fds[0] = -1;
    client_state_instance.shutdown_pipe_fds[1] = -1;
    
    g_client_state_ptr = &client_state_instance; // For signal handler

    // Create pipe for shutdown signaling between threads
    if (pipe(client_state_instance.shutdown_pipe_fds) == -1) {
        perror("\033[31mFailed to create shutdown pipe\033[0m");
        return EXIT_FAILURE;
    }

    // Set up SIGINT handler (Ctrl+C)
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler_client;
    sigaction(SIGINT, &sa, NULL);
    // sigaction(SIGTERM, &sa, NULL); // Optionally handle SIGTERM too

    if (!connect_client_to_server(&client_state_instance, server_ip, port)) {
        cleanup_client_resources(&client_state_instance);
        return EXIT_FAILURE;
    }

    if (!perform_client_login(&client_state_instance)) {
        cleanup_client_resources(&client_state_instance);
        return EXIT_FAILURE;
    }

    client_state_instance.connected = 1; // Set connected state after successful login

    // Start the message receiver thread
    if (pthread_create(&client_state_instance.receiver_thread_id, NULL, client_message_receiver_thread, &client_state_instance) != 0) {
        perror("\033[31mFailed to create message receiver thread\033[0m");
        client_state_instance.connected = 0; // Prevent input loop from starting
        send_disconnect_signal(&client_state_instance); // Attempt to notify server
        cleanup_client_resources(&client_state_instance);
        return EXIT_FAILURE;
    }

    // Start user input loop in the main thread
    handle_user_input_loop(&client_state_instance);
    
    // --- Shutdown sequence ---
    client_state_instance.connected = 0; // Ensure flag is set if not already

    // Signal receiver thread to stop, if it hasn't already (e.g. if input loop exited first)
    // Closing socket_fd will make recv in receiver thread return 0 or -1
    if (client_state_instance.socket_fd != -1) {
         shutdown(client_state_instance.socket_fd, SHUT_RD); // Stop further receives
    }


    // Wait for the receiver thread to terminate
    if (pthread_join(client_state_instance.receiver_thread_id, NULL) != 0) {
        perror("\033[31mError joining message receiver thread\033[0m");
    }
    
    // Final cleanup
    cleanup_client_resources(&client_state_instance);
    
    printf("\033[36mClient disconnected. Goodbye!\033[0m\n");
    return EXIT_SUCCESS;
}