#ifndef COMMON_H
#define COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

// Maximum limits
#define MAX_CLIENTS 32
#define MAX_MEMBERS_PER_ROOM 15
#define MAX_USERNAME_LEN 16
#define MAX_ROOM_NAME_LEN 32
#define MAX_MESSAGE_LEN 1024
#define MAX_FILENAME_LEN 256
#define MAX_FILE_SIZE (3 * 1024 * 1024) // 3MB
#define MAX_UPLOAD_QUEUE 5
#define BUFFER_SIZE 4096

// Message types
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

// Message structure
typedef struct {
    message_type_t type;
    char sender[MAX_USERNAME_LEN + 1];
    char receiver[MAX_USERNAME_LEN + 1];
    char room[MAX_ROOM_NAME_LEN + 1];
    char content[MAX_MESSAGE_LEN];
    char filename[MAX_FILENAME_LEN];
    size_t file_size;
} message_t;

// Client structure
typedef struct {
    int socket_fd;
    char username[MAX_USERNAME_LEN + 1];
    char current_room[MAX_ROOM_NAME_LEN + 1];
    struct sockaddr_in address;
    pthread_t thread_id;
    int is_active;
} client_t;

// Room structure
typedef struct {
    char name[MAX_ROOM_NAME_LEN + 1];
    client_t* members[MAX_MEMBERS_PER_ROOM];
    int member_count;
    pthread_mutex_t room_mutex;
} room_t;

// File transfer request
typedef struct file_request {
    char filename[MAX_FILENAME_LEN];
    char sender[MAX_USERNAME_LEN + 1];
    char receiver[MAX_USERNAME_LEN + 1];
    size_t file_size;
    char* file_data;
    time_t enqueue_time;
    struct file_request* next;
} file_request_t;

// File queue
typedef struct {
    file_request_t* head;
    file_request_t* tail;
    int count;
    int active_transfers;
    pthread_mutex_t queue_mutex;
    pthread_cond_t queue_cond;
    sem_t queue_semaphore;
} file_queue_t;

// Server state
typedef struct {
    client_t* clients[MAX_CLIENTS];
    room_t rooms[MAX_CLIENTS]; // Dynamic room creation
    int client_count;
    int room_count;
    file_queue_t* file_queue;
    pthread_mutex_t clients_mutex;
    pthread_mutex_t rooms_mutex;
    int server_socket;
    int running;
} server_state_t;

// Global server state
extern server_state_t* g_server_state;

// Function declarations
// Main server functions
void init_server_state(void);
void setup_server_socket(int port);
void accept_connections(void);
void cleanup_server(void);
void signal_handler(int sig);

// Client handling
void* client_handler(void* arg);
client_t* create_client(int socket_fd, struct sockaddr_in address);
void cleanup_client(client_t* client);
int handle_login(client_t* client);
void process_client_message(client_t* client, message_t* msg);
void remove_client_from_server(client_t* client);

// Room management
room_t* find_or_create_room(const char* room_name);
int join_room(client_t* client, const char* room_name);
void leave_current_room(client_t* client);
void remove_client_from_room(client_t* client);
void handle_join_room(client_t* client, const char* room_name);
void handle_leave_room(client_t* client);
void handle_broadcast(client_t* client, const char* message);
void handle_whisper(client_t* client, const char* receiver, const char* message);
void broadcast_to_room(const char* room_name, message_t* msg, client_t* sender);

// File transfer
void init_file_queue(void);
void* file_transfer_worker(void* arg);
int enqueue_file_request(const char* filename, const char* sender, 
                        const char* receiver, char* file_data, size_t file_size);
file_request_t* dequeue_file_request(void);
void process_file_transfer(file_request_t* request);
void handle_file_transfer(client_t* client, message_t* msg);

// Logging
void init_logging(void);
void cleanup_logging(void);
void log_message(const char* type, const char* message);
void log_connection(client_t* client);
void log_disconnection(client_t* client);
void log_room_join(client_t* client, const char* room_name);
void log_room_leave(client_t* client, const char* room_name);
void log_failed_login(const char* username, const char* reason);
void log_whisper(const char* sender, const char* receiver, const char* message);
void log_broadcast(const char* sender, const char* room, const char* message);
void log_file_queued(const char* filename, const char* sender, int queue_size);

// Utilities
int is_valid_username(const char* username);
int is_valid_room_name(const char* room_name);
int is_valid_file_type(const char* filename);
int send_message(int socket_fd, message_t* msg);
int receive_message(int socket_fd, message_t* msg);
void send_error_message(int socket_fd, const char* error);
void send_success_message(int socket_fd, const char* message);
client_t* find_client_by_username(const char* username);
void broadcast_shutdown_message(void);

#endif // COMMON_H 