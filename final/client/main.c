#include "common.h"
#include <sys/stat.h> // For stat (not directly used here, but often related to file ops)
#include <libgen.h>   // For basename (not directly used here)

// Global pointer to client state, primarily for the signal handler.
// This is a common pattern for signal handlers that need to access application state.
static ClientState *g_clientState_ptr = NULL;

// Signal handler for SIGINT (Ctrl+C).
// Attempts to gracefully disconnect the client.
void signalHandlerClient(int sig)
{
    if (sig == SIGINT && g_clientState_ptr != NULL && g_clientState_ptr->connected)
    {
        // Clear current line (e.g., user typing at prompt) and print shutdown message
        fprintf(stdout, "\r\033[K"); // Erase current line
        printf("\n\033[33mSIGINT received. Attempting to disconnect gracefully...\033[0m\n");

        // Send a disconnect signal to the server (best effort)
        sendDisconnectSignal(g_clientState_ptr);

        // Set connected flag to 0 to signal other threads (input loop, receiver thread) to terminate
        g_clientState_ptr->connected = 0;

        // Write to the shutdown pipe to unblock select() in the input loop,
        // ensuring it checks the 'connected' flag and exits.
        if (g_clientState_ptr->shutdown_pipe_fds[1] != -1) // Check if write-end of pipe is valid
        {
            char signal_byte = 's'; // Arbitrary byte to signal shutdown
            if (write(g_clientState_ptr->shutdown_pipe_fds[1], &signal_byte, 1) == -1)
            {
                // Non-critical error if write fails, already initiated shutdown.
                // perror("Error writing to shutdown pipe in signal handler");
            }
        }
    }
    // If another SIGINT is received while already shutting down, it will likely terminate the process.
}

// Simulates receiving and "saving" a file.
// In this project, actual file content transfer is not required (Q&A #10).
// This function just prints a notification.
static void simulateReceiveFileNotification(ClientState *client, const Message *fileHeaderMsg)
{
    (void)client; // ClientState not strictly needed for this simulated version

    // Clear current input line (e.g., "> " prompt) before printing notification
    fprintf(stdout, "\r\033[K");
    printf("\033[35m[FILE TRANSFER]: Received notification for file '%s' (%zu bytes) from %s.\033[0m\n",
           fileHeaderMsg->filename, fileHeaderMsg->file_size, fileHeaderMsg->sender);
    printf("\033[32m[INFO]: This is a simulated transfer. No actual file content was transmitted or saved locally.\033[0m\n");
    // No change to client->connected here, as this is just a notification.
}

// Thread function for receiving messages from the server.
void *clientMessageReceiverThread(void *arg)
{
    ClientState *client = (ClientState *)arg;
    Message received_msg;

    pthread_detach(pthread_self()); // As per Q&A, detach is okay

    while (client->connected)
    {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(client->socket_fd, &read_fds);

        // Use select with a timeout to periodically check the client->connected flag
        struct timeval timeout;
        timeout.tv_sec = 1; // Check client->connected flag roughly every 1 second
        timeout.tv_usec = 0;

        int activity = select(client->socket_fd + 1, &read_fds, NULL, NULL, &timeout);

        // Check connection status immediately after select returns or times out
        if (!client->connected)
            break; // Exit loop if disconnected

        if (activity < 0)
        {
            if (errno == EINTR)
                continue; // Interrupted by a signal (e.g., SIGINT), loop to re-check
            perror("\033[31mselect() error in receiver thread\033[0m");
            client->connected = 0; // Signal other threads/loops to stop on critical error
            break;
        }

        if (activity == 0)
            continue; // Timeout, loop back to check client->connected and wait again

        // If there's data on the socket
        if (FD_ISSET(client->socket_fd, &read_fds))
        {
            // receiveMessage returns 0 if connection closed, <0 (mapped to 0 by wrapper) for error
            if (receiveMessage(client->socket_fd, &received_msg) <= 0)
            {
                if (client->connected)
                {                                // Only print if disconnection was unexpected
                    fprintf(stdout, "\r\033[K"); // Clear prompt
                    printf("\n\033[31mConnection to server lost or server closed connection.\033[0m\n");
                    client->connected = 0; // Critical to signal other parts of the client
                }
                break; // Exit thread
            }

            fprintf(stdout, "\r\033[K"); // Clear the "> " prompt line before printing message
            switch (received_msg.type)
            {
            case MSG_BROADCAST:
                // Only display broadcast if client is in the message's target room
                if (strlen(client->current_room) > 0 && strcmp(client->current_room, received_msg.room) == 0)
                {
                    printf("\033[36m[%s] %s: %s\033[0m\n",
                           received_msg.room, received_msg.sender, received_msg.content);
                } // Silently ignore broadcasts for other rooms client is not in.
                break;
            case MSG_WHISPER:
                printf("\033[35m[WHISPER from %s]: %s\033[0m\n",
                       received_msg.sender, received_msg.content);
                break;
            case MSG_SERVER_NOTIFICATION: // Generic server messages (e.g., user join/left)
            case MSG_SUCCESS:             // Success messages from server operations (e.g., join/leave confirmation)
                if (strstr(received_msg.content, "Joined room") && strlen(received_msg.room) > 0)
                {
                    strncpy(client->current_room, received_msg.room, ROOM_NAME_BUF_SIZE - 1);
                    client->current_room[ROOM_NAME_BUF_SIZE - 1] = '\0'; // Ensure null termination
                    printf("\033[32m[SERVER]: %s '%s'\033[0m\n", received_msg.content, received_msg.room);
                }
                else if (strstr(received_msg.content, "Left room"))
                {
                    printf("\033[32m[SERVER]: %s\033[0m\n", received_msg.content);
                    memset(client->current_room, 0, sizeof(client->current_room)); // Clear current room
                }
                else if (strstr(received_msg.content, "Disconnected. Goodbye!"))
                {
                    printf("\033[33m[SERVER]: %s\033[0m\n", received_msg.content);
                    client->connected = 0; // Server confirmed disconnect
                }
                else
                {
                    printf("\033[32m[SERVER]: %s\033[0m\n", received_msg.content);
                }
                break;
            case MSG_FILE_TRANSFER_ACCEPT: // For the *sending* client: server accepted file request for queuing
                printf("\033[32m[SERVER]: %s (Filename: %s)\033[0m\n", received_msg.content, received_msg.filename);
                break;
            case MSG_ERROR:
            case MSG_LOGIN_FAILURE:
            case MSG_FILE_TRANSFER_REJECT: // For the *sending* client: server rejected file request
                printf("\033[31m[SERVER ERROR]: %s\033[0m\n", received_msg.content);
                if (strstr(received_msg.content, "shutting down"))
                { // Server is shutting down
                    client->connected = 0;
                }
                break;
            case MSG_FILE_TRANSFER_DATA: // This client is the *recipient* of a file transfer (simulated)
                simulateReceiveFileNotification(client, &received_msg);
                if (!client->connected)
                    break; // Check if simulateReceive somehow changed state (should not)
                break;
            default:
                printf("\033[33m[DEBUG] Received unhandled or unexpected message type %d from server. Content: '%s'\033[0m\n",
                       received_msg.type, received_msg.content);
                break;
            }

            if (client->connected)
            { // If still connected, re-print prompt
                printf("> ");
                fflush(stdout);
            }
        }
    }

    // Thread is exiting. Signal the input loop to stop if it hasn't already been signaled.
    if (client->shutdown_pipe_fds[1] != -1)
    {
        char signal_byte = 's';
        write(client->shutdown_pipe_fds[1], &signal_byte, 1); // Best effort, ignore error
    }

    // If loop exited due to disconnection, clear any partial input line that might be there.
    if (!client->connected)
    {
        fprintf(stdout, "\r\033[K");
    }
    // printf("Message receiver thread stopping.\n"); // Debug
    return NULL;
}

// Main loop for handling user input from stdin.
void handleUserInputLoop(ClientState *client)
{
    char input_buffer[MESSAGE_BUF_SIZE + FILENAME_BUF_SIZE + 64]; // Generous buffer for commands and arguments

    printf("> "); // Initial prompt
    fflush(stdout);

    while (client->connected)
    {
        fd_set read_fds_input;
        FD_ZERO(&read_fds_input);
        FD_SET(STDIN_FILENO, &read_fds_input);                 // Monitor stdin for user input
        FD_SET(client->shutdown_pipe_fds[0], &read_fds_input); // Monitor pipe for shutdown signal

        // Determine max_fd for select
        int max_fd_input = (client->shutdown_pipe_fds[0] > STDIN_FILENO) ? client->shutdown_pipe_fds[0] : STDIN_FILENO;

        // select() will block until input is available or shutdown signal is received
        int activity_input = select(max_fd_input + 1, &read_fds_input, NULL, NULL, NULL);

        // Check connection status immediately after select returns
        if (!client->connected)
            break;

        if (activity_input < 0)
        {
            if (errno == EINTR && client->connected)
                continue; // Interrupted by signal, re-check connected status and loop
            perror("\033[31mselect() error in input loop\033[0m");
            client->connected = 0; // Assume critical error, signal shutdown
            break;
        }

        // Check if shutdown signal received on the pipe
        if (FD_ISSET(client->shutdown_pipe_fds[0], &read_fds_input))
        {
            char buf[1];                                // Buffer to consume the byte from pipe
            read(client->shutdown_pipe_fds[0], buf, 1); // Consume the signal
            client->connected = 0;                      // Ensure flag is set for loop termination
            break;
        }

        // Check if input is available on stdin
        if (FD_ISSET(STDIN_FILENO, &read_fds_input))
        {
            if (fgets(input_buffer, sizeof(input_buffer), stdin) != NULL)
            {
                input_buffer[strcspn(input_buffer, "\n")] = '\0'; // Remove newline

                if (strlen(input_buffer) > 0)
                { // Process only if input is not empty
                    processUserCommand(client, input_buffer);
                }

                if (client->connected)
                { // Re-prompt if still connected after command processing
                    printf("> ");
                    fflush(stdout);
                }
            }
            else // fgets returned NULL (EOF or error)
            {
                fprintf(stdout, "\r\033[K"); // Clear current line
                if (feof(stdin))
                {
                    printf("\n\033[33mEOF detected on input. Disconnecting...\033[0m\n");
                }
                else
                {
                    printf("\n\033[31mError reading input. Disconnecting...\033[0m\n");
                }

                if (client->connected)
                {                                 // If not already disconnected by a command like /exit
                    sendDisconnectSignal(client); // Best effort to notify server
                }
                client->connected = 0; // Signal to exit
                break;                 // Exit input loop
            }
        }
    }
    fprintf(stdout, "\r\033[K"); // Clear prompt line on exit from loop
    // printf("User input handling loop stopped.\n"); // Debug
}

// Cleans up client resources: socket and shutdown pipe.
void cleanupClientResources(ClientState *clientState)
{
    // printf("\033[36mCleaning up client resources...\033[0m\n"); // User-facing message
    if (clientState->socket_fd >= 0)
    {
        close(clientState->socket_fd);
        clientState->socket_fd = -1;
    }
    if (clientState->shutdown_pipe_fds[0] >= 0) // Read end
    {
        close(clientState->shutdown_pipe_fds[0]);
        clientState->shutdown_pipe_fds[0] = -1;
    }
    if (clientState->shutdown_pipe_fds[1] >= 0) // Write end
    {
        close(clientState->shutdown_pipe_fds[1]);
        clientState->shutdown_pipe_fds[1] = -1;
    }
}

int main(int argc, char *argv[])
{
    if (argc != 3)
    {
        fprintf(stderr, "Usage: %s <server_ip> <port>\n", argv[0]);
        fprintf(stderr, "Example: %s 127.0.0.1 5000\n", argv[0]);
        return EXIT_FAILURE;
    }

    const char *server_ip = argv[1];
    int port = atoi(argv[2]);
    if (port <= 0 || port > 65535) // Port 0 is not usable for TCP client connection to specific server
    {
        fprintf(stderr, "\033[31mInvalid port number: %s. Must be between 1 and 65535.\033[0m\n", argv[2]);
        return EXIT_FAILURE;
    }

    ClientState clientStateInstance;
    memset(&clientStateInstance, 0, sizeof(clientStateInstance));
    clientStateInstance.socket_fd = -1; // Mark as not connected initially
    clientStateInstance.shutdown_pipe_fds[0] = -1;
    clientStateInstance.shutdown_pipe_fds[1] = -1;
    clientStateInstance.connected = 0; // Start as not connected

    g_clientState_ptr = &clientStateInstance; // Set global pointer for signal handler access

    // Create pipe for inter-thread shutdown signaling
    if (pipe(clientStateInstance.shutdown_pipe_fds) == -1)
    {
        perror("\033[31mFailed to create shutdown pipe\033[0m");
        // No other resources to clean yet other than g_clientState_ptr if it pointed to heap.
        return EXIT_FAILURE;
    }

    // Setup SIGINT (Ctrl+C) handler
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signalHandlerClient;
    sigemptyset(&sa.sa_mask); // Clear mask, no signals blocked during handler
    // sa.sa_flags = SA_RESTART; // Not strictly needed here, default behavior is fine.
    if (sigaction(SIGINT, &sa, NULL) == -1)
    {
        perror("\033[31mFailed to set SIGINT handler\033[0m");
        cleanupClientResources(&clientStateInstance); // Clean up pipes
        return EXIT_FAILURE;
    }

    // Ignore SIGPIPE to prevent crash on write to a broken pipe (server disconnects)
    // sendMessage will return an error instead, which can be handled.
    signal(SIGPIPE, SIG_IGN);

    if (!connectClientToServer(&clientStateInstance, server_ip, port))
    {
        cleanupClientResources(&clientStateInstance);
        return EXIT_FAILURE;
    }

    if (!performClientLogin(&clientStateInstance))
    {
        // Login failed, server might have closed connection or client chose not to proceed.
        // Connection is still open technically if server didn't close it.
        if (clientStateInstance.socket_fd >= 0)
            sendDisconnectSignal(&clientStateInstance); // Polite disconnect
        cleanupClientResources(&clientStateInstance);
        return EXIT_FAILURE;
    }

    // Login successful, client is now fully connected and ready for interaction
    clientStateInstance.connected = 1;

    // Create the message receiver thread
    if (pthread_create(&clientStateInstance.receiver_thread_id, NULL, clientMessageReceiverThread, &clientStateInstance) != 0)
    {
        perror("\033[31mFailed to create message receiver thread\033[0m");
        clientStateInstance.connected = 0; // Ensure flag is set to stop if something else started
        if (clientStateInstance.socket_fd >= 0)
            sendDisconnectSignal(&clientStateInstance); // Try to notify server
        cleanupClientResources(&clientStateInstance);
        return EXIT_FAILURE;
    }
    // Receiver thread is detached within its own function, no need to join explicitly here if that's the model.
    // However, for a clean exit, joining is better. If it's detached, main might exit before it cleans up.
    // The Q&A allowed pthread_detach(). If we join, signal handling in receiver thread might need adjustment
    // or the join call might block indefinitely if receiver isn't exiting.
    // For now, assuming detached as per Q&A. Shutdown pipe mechanism is key.

    // Start the user input loop (this is blocking until client.connected is 0)
    handleUserInputLoop(&clientStateInstance);

    // --- Shutdown Sequence ---
    // This point is reached when client.connected becomes 0, either by /exit, SIGINT, EOF, or connection loss.
    clientStateInstance.connected = 0; // Ensure flag is set, though it should be already.

    // Signal the receiver thread again via pipe, just in case it was stuck in select and missed flag change.
    // This is mostly redundant if receiver thread is well-behaved with timeouts and flag checks.
    if (clientStateInstance.shutdown_pipe_fds[1] != -1)
    {
        char signal_byte = 's';
        write(clientStateInstance.shutdown_pipe_fds[1], &signal_byte, 1); // Best effort
    }

    // Attempt to shutdown read part of socket to unblock receiver if it's on recv()
    // This can help the receiver thread to exit cleanly if it was blocked on network I/O.
    if (clientStateInstance.socket_fd >= 0)
    {
        shutdown(clientStateInstance.socket_fd, SHUT_RD);
    }

    // Since receiver_thread is detached, we cannot pthread_join it here directly.
    // We rely on it exiting cleanly upon clientStateInstance.connected = 0 and socket events.
    // A brief pause can allow it to print its exit messages, though not ideal.
    // A joinable thread model would be more robust for ensuring sequential cleanup.
    // For detached:
    // sleep(1); // Give a moment for the detached receiver thread to finish. Crude.

    // For a joinable model (if receiver is not detached):
    // pthread_join(clientStateInstance.receiver_thread_id, NULL);
    // For this project, with detached receiver, we proceed to cleanup.
    // The `pthread_detach` call is in `clientMessageReceiverThread`.

    cleanupClientResources(&clientStateInstance);
    printf("\033[36mClient has disconnected. Goodbye!\033[0m\n");
    return EXIT_SUCCESS;
}