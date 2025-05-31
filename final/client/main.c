#include "common.h"
#include <sys/stat.h>
#include <libgen.h>

static ClientState *g_clientState_ptr = NULL;

void signalHandlerClient(int sig)
{
    if (sig == SIGINT && g_clientState_ptr != NULL && g_clientState_ptr->connected)
    {
        printf("\n\033[33mSIGINT received. Attempting to disconnect gracefully...\033[0m\n");
        sendDisconnectSignal(g_clientState_ptr);
        g_clientState_ptr->connected = 0;
        if (g_clientState_ptr->shutdown_pipe_fds[1] != -1)
        {
            char signal_byte = 's';
            if (write(g_clientState_ptr->shutdown_pipe_fds[1], &signal_byte, 1) == -1)
            {
                // Non-critical, main loop will eventually see connected flag
            }
        }
    }
}

static void receiveAndSaveFile(ClientState *client, const Message *fileHeaderMsg)
{
    // For simulation, we just acknowledge the receipt of the file transfer *header*
    // We don't receive actual data or save a file.

    printf("\033[35m[FILE SIMULATION]: Received notification for file '%s' (%zu bytes) from %s.\033[0m\n",
           fileHeaderMsg->filename, fileHeaderMsg->file_size, fileHeaderMsg->sender);
    printf("\033[32m[FILE SIMULATION]: This is a simulated transfer. No actual file content was transmitted or saved.\033[0m\n");

    // All original logic for buffer allocation, recv loop, file opening, writing is removed.
    // Client remains connected.
}

void *clientMessageReceiverThread(void *arg)
{
    ClientState *client = (ClientState *)arg;
    Message received_msg;

    // printf("\033[36mMessage receiver thread started.\033[0m\n");

    while (client->connected)
    {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(client->socket_fd, &read_fds);

        struct timeval timeout;
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;

        int activity = select(client->socket_fd + 1, &read_fds, NULL, NULL, &timeout);

        if (!client->connected)
            break;

        if (activity < 0)
        {
            if (errno == EINTR)
                continue;
            perror("\033[31mselect() error in receiver thread\033[0m");
            client->connected = 0;
            break;
        }
        if (activity == 0)
            continue;

        if (FD_ISSET(client->socket_fd, &read_fds))
        {
            if (receiveMessage(client->socket_fd, &received_msg) <= 0)
            {
                if (client->connected)
                {
                    fprintf(stdout, "\r\033[K");
                    printf("\n\033[31mConnection to server lost or server closed connection.\033[0m\n");
                    client->connected = 0;
                }
                break;
            }

            fprintf(stdout, "\r\033[K");
            switch (received_msg.type)
            {
            case MSG_BROADCAST:
                if (strcmp(client->current_room, received_msg.room) == 0)
                {
                    printf("\033[36m[%s] %s: %s\033[0m\n",
                           received_msg.room, received_msg.sender, received_msg.content);
                }
                break;
            case MSG_WHISPER:
                printf("\033[35m[WHISPER from %s]: %s\033[0m\n",
                       received_msg.sender, received_msg.content);
                break;
            case MSG_SERVER_NOTIFICATION:
            case MSG_SUCCESS:
                if (strstr(received_msg.content, "Joined room") && strlen(received_msg.room) > 0)
                {
                    strncpy(client->current_room, received_msg.room, ROOM_NAME_BUF_SIZE - 1);
                    printf("\033[32m[SERVER]: %s '%s'\033[0m\n", received_msg.content, received_msg.room);
                }
                else if (strstr(received_msg.content, "Left room"))
                {
                    printf("\033[32m[SERVER]: %s\033[0m\n", received_msg.content);
                    memset(client->current_room, 0, sizeof(client->current_room));
                }
                else if (strstr(received_msg.content, "Disconnected. Goodbye!"))
                {
                    printf("\033[33m[SERVER]: %s\033[0m\n", received_msg.content);
                    client->connected = 0;
                }
                else
                {
                    printf("\033[32m[SERVER]: %s\033[0m\n", received_msg.content);
                }
                break;
            case MSG_FILE_TRANSFER_ACCEPT: // Server confirms file is queued/being processed for *sending* client
                printf("\033[32m[SERVER]: %s (Filename: %s)\033[0m\n", received_msg.content, received_msg.filename);
                break;
            case MSG_ERROR:
            case MSG_LOGIN_FAILURE:
            case MSG_FILE_TRANSFER_REJECT: // Server rejected *sending* client's request
                printf("\033[31m[SERVER ERROR]: %s\033[0m\n", received_msg.content);
                if (strstr(received_msg.content, "shutting down"))
                {
                    client->connected = 0;
                }
                break;
            case MSG_FILE_TRANSFER_DATA: // This client is the *recipient*
                handleSimulatedFileReception(client, &received_msg);
                if (!client->connected)
                    goto receiver_loop_exit; // Exit if connection dropped during file save
                break;
            default:
                printf("\033[33m[DEBUG] Received unhandled message type %d from server: %s\033[0m\n",
                       received_msg.type, received_msg.content);
                break;
            }
            if (client->connected)
            {
                printf("> ");
                fflush(stdout);
            }
        }
    receiver_loop_exit:;
    }

    if (client->shutdown_pipe_fds[1] != -1)
    {
        char signal_byte = 's';
        write(client->shutdown_pipe_fds[1], &signal_byte, 1); // Best effort
    }
    if (!client->connected)
    {                                // If loop exited due to disconnection
        fprintf(stdout, "\r\033[K"); // Clear any partial input
    }
    // printf("\033[36mMessage receiver thread stopped.\033[0m\n");
    return NULL;
}

void handleUserInputLoop(ClientState *client)
{
    char input_buffer[MESSAGE_BUF_SIZE + FILENAME_BUF_SIZE + 20];
    printf("> ");
    fflush(stdout);

    while (client->connected)
    {
        fd_set read_fds_input;
        FD_ZERO(&read_fds_input);
        FD_SET(STDIN_FILENO, &read_fds_input);
        FD_SET(client->shutdown_pipe_fds[0], &read_fds_input);

        int max_fd_input = (client->shutdown_pipe_fds[0] > STDIN_FILENO) ? client->shutdown_pipe_fds[0] : STDIN_FILENO;
        int activity_input = select(max_fd_input + 1, &read_fds_input, NULL, NULL, NULL);

        if (!client->connected)
            break;

        if (activity_input < 0)
        {
            if (errno == EINTR && client->connected)
                continue;
            perror("\033[31mselect() error in input loop\033[0m");
            break;
        }

        if (FD_ISSET(client->shutdown_pipe_fds[0], &read_fds_input))
        {
            char buf[1];
            read(client->shutdown_pipe_fds[0], buf, 1); // Consume
            client->connected = 0;
            break;
        }

        if (FD_ISSET(STDIN_FILENO, &read_fds_input))
        {
            if (fgets(input_buffer, sizeof(input_buffer), stdin) != NULL)
            {
                input_buffer[strcspn(input_buffer, "\n")] = '\0';

                if (strlen(input_buffer) > 0)
                {
                    processUserCommand(client, input_buffer);
                }

                if (client->connected)
                {
                    printf("> ");
                    fflush(stdout);
                }
            }
            else
            {
                fprintf(stdout, "\r\033[K");

                if (feof(stdin))
                {
                    printf("\n\033[33mEOF detected on input. Disconnecting...\033[0m\n");
                }
                else
                {
                    printf("\n\033[31mError reading input. Disconnecting...\033[0m\n");
                }
                if (client->connected)
                {
                    sendDisconnectSignal(client);
                }
                client->connected = 0;
                break;
            }
        }
    }
    fprintf(stdout, "\r\033[K");
    printf("\033[36mInput handling stopped.\033[0m\n");
}

void cleanupClientResources(ClientState *clientState)
{
    printf("\033[36mCleaning up client resources...\033[0m\n");
    if (clientState->socket_fd >= 0)
    {
        shutdown(clientState->socket_fd, SHUT_RDWR);
        close(clientState->socket_fd);
        clientState->socket_fd = -1;
    }
    if (clientState->shutdown_pipe_fds[0] >= 0)
    {
        close(clientState->shutdown_pipe_fds[0]);
        clientState->shutdown_pipe_fds[0] = -1;
    }
    if (clientState->shutdown_pipe_fds[1] >= 0)
    {
        close(clientState->shutdown_pipe_fds[1]);
        clientState->shutdown_pipe_fds[1] = -1;
    }
}

int main(int argc, char *argv[])
{
    if (argc != 3)
    {
        fprintf(stderr, "Usage: %s <server_ip> <port>\nExample: %s 127.0.0.1 5000\n", argv[0], argv[0]);
        return EXIT_FAILURE;
    }

    const char *server_ip = argv[1];
    int port = atoi(argv[2]);
    if (port <= 0 || port > 65535)
    {
        fprintf(stderr, "\033[31mInvalid port number: %s. Must be 1-65535.\033[0m\n", argv[2]);
        return EXIT_FAILURE;
    }

    ClientState clientStateInstance;
    memset(&clientStateInstance, 0, sizeof(clientStateInstance));
    clientStateInstance.socket_fd = -1;
    clientStateInstance.shutdown_pipe_fds[0] = -1;
    clientStateInstance.shutdown_pipe_fds[1] = -1;

    g_clientState_ptr = &clientStateInstance;

    if (pipe(clientStateInstance.shutdown_pipe_fds) == -1)
    {
        perror("\033[31mFailed to create shutdown pipe\033[0m");
        return EXIT_FAILURE;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signalHandlerClient;
    sigaction(SIGINT, &sa, NULL);

    if (!connectClientToServer(&clientStateInstance, server_ip, port))
    {
        cleanupClientResources(&clientStateInstance);
        return EXIT_FAILURE;
    }

    if (!performClientLogin(&clientStateInstance))
    {
        cleanupClientResources(&clientStateInstance);
        return EXIT_FAILURE;
    }

    clientStateInstance.connected = 1;

    if (pthread_create(&clientStateInstance.receiver_thread_id, NULL, clientMessageReceiverThread, &clientStateInstance) != 0)
    {
        perror("\033[31mFailed to create message receiver thread\033[0m");
        clientStateInstance.connected = 0;
        sendDisconnectSignal(&clientStateInstance);
        cleanupClientResources(&clientStateInstance);
        return EXIT_FAILURE;
    }

    handleUserInputLoop(&clientStateInstance);

    clientStateInstance.connected = 0; // Ensure flag is set
    if (clientStateInstance.socket_fd != -1)
    {
        shutdown(clientStateInstance.socket_fd, SHUT_RD);
    }

    if (pthread_join(clientStateInstance.receiver_thread_id, NULL) != 0)
    {
        perror("\033[31mError joining message receiver thread\033[0m");
    }

    cleanupClientResources(&clientStateInstance);
    printf("\033[36mClient disconnected. Goodbye!\033[0m\n");
    return EXIT_SUCCESS;
}