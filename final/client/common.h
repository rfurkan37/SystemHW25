#ifndef CLIENT_COMMON_H
#define CLIENT_COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h> // For sig_atomic_t, signal handling
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>      // For fcntl, if needed for non-blocking
#include <sys/select.h> // For select

// Shared includes from the project
#include "../shared/protocol.h"
#include "../shared/utils.h"

// Client state structure
typedef struct ClientState
{
    int socket_fd;                         // Socket descriptor for server connection
    char username[USERNAME_BUF_SIZE];      // Client's chosen username
    char current_room[ROOM_NAME_BUF_SIZE]; // Current room client is in, empty if none
    volatile sig_atomic_t connected;       // Flag indicating connection status (1=connected, 0=disconnecting/disconnected)
    pthread_t receiver_thread_id;          // Thread ID for the message receiver thread
    int shutdown_pipe_fds[2];              // Pipe for signaling graceful shutdown between threads [0]=read, [1]=write
} ClientState;

// --- Function Declarations ---

// Located in: client/main.c
void signalHandlerClient(int sig);                     // Handles SIGINT for graceful shutdown
void cleanupClientResources(ClientState *clientState); // Cleans up client resources (sockets, pipes)
void *clientMessageReceiverThread(void *arg);          // Thread function to receive messages from server
void handleUserInputLoop(ClientState *clientState);    // Main loop for handling user input

// Located in: client/network.c
int connectClientToServer(ClientState *clientState, const char *server_ip, int port); // Establishes connection to server
int performClientLogin(ClientState *clientState);                                     // Handles the login process with the server

// Located in: client/commands.c
// Utility to trim leading whitespace from a string (internal or for parsing)
const char *trimLeadingWhitespace(const char *str);
// Processes a command string entered by the user
void processUserCommand(ClientState *clientState, const char *input_buffer);
// Command handlers - these prepare and send specific messages to the server
void sendJoinRoomCommand(ClientState *clientState, const char *room_name);
void sendLeaveRoomCommand(ClientState *clientState);
void sendBroadcastCommand(ClientState *clientState, const char *message_content);
void sendWhisperCommand(ClientState *clientState, const char *target_username, const char *message_content);
void sendFileRequestCommand(ClientState *clientState, const char *filepath, const char *target_username);
void sendDisconnectSignal(ClientState *clientState); // Sends a disconnect message to the server
void displayHelpMessage(void);                       // Displays available commands to the user

#endif // CLIENT_COMMON_H