#ifndef CLIENT_COMMON_H
#define CLIENT_COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h> // For O_RDONLY etc.
#include <sys/select.h> // For fd_set, select

#include "../shared/protocol.h" // Message structures and constants
#include "../shared/utils.h"    // Shared utility functions like send_message

// Client state
typedef struct
{
    int socket_fd;
    char username[USERNAME_BUF_SIZE];
    char current_room[ROOM_NAME_BUF_SIZE];
    volatile int connected;      // Ensure visibility across threads
    pthread_t receiver_thread_id;
    int shutdown_pipe_fds[2]; // Pipe for signaling shutdown between threads
} client_state_t;

// Function declarations for client-specific logic

// client/main.c
void signal_handler_client(int sig); // Renamed to avoid conflict if ever linked together
void cleanup_client_resources(client_state_t *client_state);

// client/network.c
int connect_client_to_server(client_state_t *client_state, const char *server_ip, int port);
int perform_client_login(client_state_t *client_state);

// client/commands.c
void process_user_command(client_state_t *client_state, const char *input_buffer);
void send_join_room_command(client_state_t *client_state, const char *room_name);
void send_leave_room_command(client_state_t *client_state);
void send_broadcast_command(client_state_t *client_state, const char *message_content);
void send_whisper_command(client_state_t *client_state, const char *target_username, const char *message_content);
void send_file_request_command(client_state_t *client_state, const char *filepath, const char *target_username);
void send_disconnect_signal(client_state_t *client_state);
void display_help_message(void);


// client/main.c (message_receiver thread function)
void *client_message_receiver_thread(void *arg);
void handle_user_input_loop(client_state_t *client_state);


#endif // CLIENT_COMMON_H