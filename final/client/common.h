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
#include <ctype.h>

// Maximum limits (must match server)
#define MAX_CLIENTS 15
#define MAX_USERNAME_LEN 16
#define MAX_ROOM_NAME_LEN 32
#define MAX_MESSAGE_LEN 1024
#define MAX_FILENAME_LEN 256
#define MAX_FILE_SIZE (3 * 1024 * 1024) // 3MB
#define BUFFER_SIZE 4096

// Message types (must match server)
typedef enum {
    MSG_LOGIN,
    MSG_JOIN_ROOM,
    MSG_LEAVE_ROOM,
    MSG_BROADCAST,
    MSG_WHISPER,
    MSG_FILE_TRANSFER,
    MSG_DISCONNECT,
    MSG_ERROR,
    MSG_SUCCESS,
    MSG_FILE_DATA
} message_type_t;

// Message structure (must match server)
typedef struct {
    message_type_t type;
    char sender[MAX_USERNAME_LEN + 1];
    char receiver[MAX_USERNAME_LEN + 1];
    char room[MAX_ROOM_NAME_LEN + 1];
    char content[MAX_MESSAGE_LEN];
    size_t content_length;
    char filename[MAX_FILENAME_LEN];
    size_t file_size;
} message_t;

// Client state
typedef struct {
    int socket_fd;
    char username[MAX_USERNAME_LEN + 1];
    char current_room[MAX_ROOM_NAME_LEN + 1];
    int connected;
    pthread_t receiver_thread;
} client_state_t;

// Function declarations
// Network functions
int connect_to_server(client_state_t* client, const char* server_ip, int port);
int send_message(int socket_fd, message_t* msg);
int receive_message(int socket_fd, message_t* msg);

// Client functions
int handle_client_login(client_state_t* client);
void* message_receiver(void* arg);
void handle_user_input(client_state_t* client);
void process_user_command(client_state_t* client, const char* input);
void cleanup_client(client_state_t* client);

// Command functions
void send_join_command(client_state_t* client, const char* room_name);
void send_leave_command(client_state_t* client);
void send_broadcast_command(client_state_t* client, const char* message);
void send_whisper_command(client_state_t* client, const char* receiver, const char* message);
void send_file_command(client_state_t* client, const char* receiver, const char* filename);
void send_disconnect_command(client_state_t* client);

// Utility functions
void print_help(void);
void print_colored_message(const char* type, const char* sender, const char* content);
int is_valid_username(const char* username);
int is_valid_room_name(const char* room_name);
int is_valid_file_type(const char* filename);
long get_file_size(FILE* file);

#endif // CLIENT_COMMON_H 