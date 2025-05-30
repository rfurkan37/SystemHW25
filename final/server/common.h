#ifndef SERVER_COMMON_H
#define SERVER_COMMON_H

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
#include <fcntl.h>      // For O_WRONLY etc. in logging
#include <sys/stat.h>   // For mode_t in open
#include <sys/types.h>  // For mode_t

#include "../shared/protocol.h" // Shared message structures and constants
#include "../shared/utils.h"    // Shared utility functions

// Server-specific limits
#define MAX_SERVER_CLIENTS 30    // PDF: "Supports at least 15 concurrent clients", test scenario "At least 30 clients"
#define MAX_ROOMS MAX_SERVER_CLIENTS // Max rooms can be same as max clients, or a different value
#define MAX_MEMBERS_PER_ROOM 15  // PDF constraint
#define MAX_UPLOAD_QUEUE_SIZE 5 // PDF constraint
#define SERVER_LOG_FILENAME "server.log"


// Client structure (server-side representation)
typedef struct {
    int socket_fd;
    char username[USERNAME_BUF_SIZE];
    char current_room_name[ROOM_NAME_BUF_SIZE];
    struct sockaddr_in client_address;
    pthread_t thread_id;
    volatile int is_active; // Client connection is active
    time_t connection_time;
} client_info_t;

// Room structure
typedef struct {
    char name[ROOM_NAME_BUF_SIZE];
    client_info_t* members[MAX_MEMBERS_PER_ROOM]; // Pointers to client_info_t structs
    int member_count;
    pthread_mutex_t room_lock; // Mutex to protect this room's member list
} chat_room_t;

// File transfer request in the server's queue
typedef struct file_transfer_task {
    char filename[FILENAME_BUF_SIZE];
    char sender_username[USERNAME_BUF_SIZE];
    char receiver_username[USERNAME_BUF_SIZE];
    size_t file_size;
    char *file_data_buffer;    // Server holds the data in memory while queued
    time_t enqueue_timestamp;
    struct file_transfer_task *next_task;
} file_transfer_task_t;

// File transfer queue
typedef struct {
    file_transfer_task_t *head;
    file_transfer_task_t *tail;
    int current_queue_length;
    // int active_transfers; // Tracked by semaphore count
    pthread_mutex_t queue_access_mutex;
    pthread_cond_t queue_not_empty_cond; // Signal when a new item is added
    sem_t available_upload_slots_sem; // Limits concurrent file processing
} file_upload_queue_t;

// Overall Server State
typedef struct {
    client_info_t *connected_clients[MAX_SERVER_CLIENTS]; // Array of pointers to client_info_t
    chat_room_t chat_rooms[MAX_ROOMS];         // Array of chat_room_t structs
    
    int active_client_count;
    int current_room_count;
    
    file_upload_queue_t file_transfer_manager;

    pthread_mutex_t clients_list_mutex; // Protects connected_clients array and active_client_count
    pthread_mutex_t rooms_list_mutex;   // Protects chat_rooms array and current_room_count

    int server_listen_socket_fd;
    volatile int server_is_running; // Flag to control main server loop and threads
    pthread_t file_worker_thread_ids[MAX_UPLOAD_QUEUE_SIZE]; // Can have multiple worker threads
} server_main_state_t;

// Global server state instance (declaration)
extern server_main_state_t *g_server_state;


// --- Function Declarations ---

// server/main.c
void initialize_server_state(void);
int setup_server_listening_socket(int port);
void accept_client_connections_loop(void);
void cleanup_server_resources(void);
void sigint_shutdown_handler(int signal_number);

// server/client_handler.c
void* client_connection_thread_handler(void *client_info_ptr_arg);
client_info_t* register_new_client_on_server(int client_socket_fd, struct sockaddr_in client_address);
int process_client_login(client_info_t *client_info, const message_t *login_message);
void handle_client_message(client_info_t *client_info, const message_t *message);
void unregister_client(client_info_t *client_info, int is_unexpected_disconnect);
void notify_client_of_shutdown(int client_socket_fd);


// server/room_manager.c
void initialize_room_system(void);
chat_room_t* find_or_create_chat_room(const char *room_name_to_find);
int add_client_to_room(client_info_t *client, chat_room_t *room);
void remove_client_from_their_room(client_info_t *client); // Removes from client->current_room_name
void handle_join_room_request(client_info_t *client, const char *room_name_requested);
void handle_leave_room_request(client_info_t *client);
void handle_broadcast_request(client_info_t *client_sender, const char *message_content);
void handle_whisper_request(client_info_t *client_sender, const char *receiver_username, const char *message_content);
void broadcast_message_to_room_members(chat_room_t *room, const message_t *message_to_send, const char *exclude_username);


// server/file_transfer.c
void initialize_file_transfer_system(void);
void* file_processing_worker_thread(void *arg); // Worker thread function
void handle_file_transfer_request(client_info_t *sender_client, const message_t *file_req_header);
int add_file_to_upload_queue(const char *filename, const char *sender_user, const char *receiver_user,
                             char *actual_file_data, size_t file_size_val);
file_transfer_task_t* get_next_file_from_queue(void);
void execute_file_transfer_to_recipient(file_transfer_task_t *task);
void cleanup_file_transfer_system(void);


// server/logging.c
int initialize_server_logging(const char* log_filename);
void finalize_server_logging(void);
void log_server_event(const char* tag, const char* details_format, ...);
// Specific logging helpers that call log_server_event
void log_event_server_start(int port);
void log_event_client_connected(const char* username, const char* ip_address);
void log_event_client_disconnected(const char* username, int is_unexpected);
void log_event_client_login_failed(const char* username_attempted, const char* ip_addr, const char* reason);
void log_event_room_created(const char* room_name);
void log_event_client_joined_room(const char* username, const char* room_name);
void log_event_client_left_room(const char* username, const char* room_name);
void log_event_client_switched_room(const char* username, const char* old_room, const char* new_room);
void log_event_broadcast(const char* sender_username, const char* room_name, const char* message_preview);
void log_event_whisper(const char* sender_username, const char* receiver_username); // Msg content not logged per PDF
void log_event_file_transfer_initiated(const char* sender_username, const char* receiver_username, const char* filename);
void log_event_file_queued(const char* sender_username, const char* filename, int current_q_size);
void log_event_file_rejected_oversized(const char* sender_username, const char* filename, size_t attempted_size);
void log_event_file_transfer_processing_start(const char* sender_username, const char* filename, long wait_time_seconds);
void log_event_file_transfer_completed(const char* sender_username, const char* receiver_username, const char* filename);
void log_event_file_transfer_failed(const char* sender_username, const char* receiver_username, const char* filename, const char* reason);
void log_event_sigint_shutdown(int num_clients_at_shutdown);


// server/utils_server.c (server-specific utilities)
client_info_t* find_client_by_socket(int socket_fd); // If needed
client_info_t* find_client_by_username(const char *username_to_find);
void send_error_to_client(int client_socket_fd, const char *error_message);
void send_success_to_client(int client_socket_fd, const char *success_message);
// Sends success with room context, useful for join
void send_success_with_room_to_client(int client_socket_fd, const char* message, const char* room_name);


#endif // SERVER_COMMON_H