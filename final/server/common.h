#ifndef SERVER_COMMON_H
#define SERVER_COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h> // For sig_atomic_t, signal handling
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/select.h> // For fd_set (though mostly used in .c files)

// Shared project includes
#include "../shared/protocol.h"
#include "../shared/utils.h"

// Server-specific configuration and limits
#define MAX_SERVER_CLIENTS 30            // Project: "Supports at least 15 concurrent clients"
#define MAX_ROOMS MAX_SERVER_CLIENTS     // A reasonable upper bound, can be adjusted
#define MAX_MEMBERS_PER_ROOM 15          // As per PDF: "Each room has a max capacity of 15 users"
#define MAX_UPLOAD_QUEUE_SIZE 5          // PDF: "max 5 uploads at a time" (concurrent processing slots)
#define SERVER_LOG_FILENAME "server.log" // Name of the server log file
#define MAX_RECEIVED_FILES_TRACKED 50    // For Test Scenario 9: Same Filename Collision (per user)

// Forward declarations for structs
typedef struct ClientInfo ClientInfo;
typedef struct ChatRoom ChatRoom;
typedef struct FileTransferTask FileTransferTask;

// Structure representing a connected client on the server side
struct ClientInfo
{
    int socket_fd;                              // Client's socket descriptor
    char username[USERNAME_BUF_SIZE];           // Client's authenticated username
    char current_room_name[ROOM_NAME_BUF_SIZE]; // Name of the room client is currently in
    struct sockaddr_in client_address;          // Client's network address (for logging IP)
    pthread_t thread_id;                        // Thread ID handling this client's connection
    volatile sig_atomic_t is_active;            // Flag: 1 if client is logged in and active, 0 otherwise
    time_t connection_time;                     // Timestamp of initial connection

    // For Test Scenario 9: Filename collision detection
    char received_filenames[MAX_RECEIVED_FILES_TRACKED][FILENAME_BUF_SIZE]; // Tracks names of files received by this user
    int num_received_files;                                                 // Count of files in received_filenames
    pthread_mutex_t received_files_lock;                                    // Mutex to protect access to received_filenames list
};

// Structure representing a chat room
struct ChatRoom
{
    char name[ROOM_NAME_BUF_SIZE];             // Name of the chat room
    ClientInfo *members[MAX_MEMBERS_PER_ROOM]; // Array of pointers to members in this room
    int member_count;                          // Current number of members in the room
    pthread_mutex_t room_lock;                 // Mutex to protect members list and member_count
};

// Structure representing a file transfer task in the server's queue
struct FileTransferTask
{
    char filename[FILENAME_BUF_SIZE];          // Name of the file being transferred
    char sender_username[USERNAME_BUF_SIZE];   // Username of the file sender
    char receiver_username[USERNAME_BUF_SIZE]; // Username of the file recipient
    size_t file_size;                          // Size of the file
    time_t enqueue_timestamp;                  // Timestamp when task was added to queue (for wait duration logging)
    struct FileTransferTask *next_task;        // Pointer for linked list implementation of the queue
};

// Structure for managing the file upload queue and worker threads
typedef struct FileUploadQueue
{
    FileTransferTask *head;   // Head of the file transfer task queue
    FileTransferTask *tail;   // Tail of the file transfer task queue
    int current_queue_length; // Number of tasks currently in the queue (waiting for a worker)

    pthread_mutex_t queue_access_mutex;  // Mutex to protect queue (head, tail, length)
    pthread_cond_t queue_not_empty_cond; // Condition variable to signal workers when queue has tasks
    sem_t available_upload_slots_sem;    // Semaphore limiting concurrent file processing workers
} FileUploadQueue;

// Structure for the overall server state
typedef struct ServerMainState
{
    ClientInfo *connected_clients[MAX_SERVER_CLIENTS]; // Array of pointers to client structures
    ChatRoom chat_rooms[MAX_ROOMS];                    // Array of chat room structures

    int active_client_count; // Count of currently logged-in (active) clients
    int current_room_count;  // Count of currently active (created) rooms

    FileUploadQueue file_transfer_manager; // Manages the file upload queue and workers

    pthread_mutex_t clients_list_mutex; // Mutex for connected_clients array and active_client_count
    pthread_mutex_t rooms_list_mutex;   // Mutex for chat_rooms array and current_room_count

    int server_listen_socket_fd;                             // Listening socket for incoming connections
    volatile sig_atomic_t server_is_running;                 // Flag for graceful server shutdown (1=running, 0=shutting down)
    pthread_t file_worker_thread_ids[MAX_UPLOAD_QUEUE_SIZE]; // IDs of file processing worker threads
} ServerMainState;

// Global pointer to the server state instance
extern ServerMainState *g_server_state;

// --- Function Declarations ---

// Located in: server/main.c
void initializeServerState(void);              // Initializes the global server state structure and subsystems
int setupServerListeningSocket(int port);      // Sets up and binds the main listening socket
void acceptClientConnectionsLoop(void);        // Main loop for accepting new client connections
void cleanupServerResources(void);             // Cleans up all server resources on shutdown
void sigintShutdownHandler(int signal_number); // Handles SIGINT for graceful server shutdown

// Located in: server/client_handler.c
void *clientConnectionThreadHandler(void *clientInfoPtrArg);                                    // Thread function for handling a single client connection
ClientInfo *registerNewClientOnServer(int client_socket_fd, struct sockaddr_in client_address); // Adds a new client to server list (pre-login)
int processClientLogin(ClientInfo *clientInfo, const Message *login_message);                   // Processes a login request from a client
void handleClientMessage(ClientInfo *clientInfo, const Message *message);                       // Main dispatcher for client messages
void unregisterClient(ClientInfo *clientInfo, int is_unexpected_disconnect);                    // Removes client from server, cleans up resources
void notifyClientOfShutdown(int client_socket_fd);                                              // Sends a shutdown notification to a client

// Located in: server/room_manager.c
void initializeRoomSystem(void);                                                                                  // Initializes the chat room management system
ChatRoom *findOrCreateChatRoom(const char *room_name_to_find);                                                    // Finds an existing room or creates a new one
int addClientToRoom(ClientInfo *client, ChatRoom *room);                                                          // Adds a client to a specified room
void removeClientFromTheirRoom(ClientInfo *client);                                                               // Removes a client from their current room
void handleJoinRoomRequest(ClientInfo *client, const char *room_name_requested);                                  // Handles a client's /join request
void handleLeaveRoomRequest(ClientInfo *client);                                                                  // Handles a client's /leave request
void handleBroadcastRequest(ClientInfo *client_sender, const char *message_content);                              // Handles a /broadcast request
void handleWhisperRequest(ClientInfo *client_sender, const char *receiver_username, const char *message_content); // Handles /whisper
void broadcastMessageToRoomMembers(ChatRoom *room, const Message *message_to_send, const char *exclude_username); // Sends msg to room
void notifyRoomOfClientAction(ClientInfo *acting_client, ChatRoom *room, const char *action_verb);                // Notifies room members of a client's action (e.g., joined, left, disconnected)

// Located in: server/file_transfer.c
void initializeFileTransferSystem(void);     // Initializes the file transfer queue and worker threads
void *fileProcessingWorkerThread(void *arg); // Thread function for a file processing worker
int addFileToUploadQueue(const char *filename, const char *sender_user, const char *receiver_user,
                         size_t file_size_val);                                            // Adds a file (metadata) to the upload queue (simulation)
void handleFileTransferRequest(ClientInfo *sender_client, const Message *file_req_header); // Handles /sendfile request
void executeFileTransferToRecipient(FileTransferTask *task);                               // Simulates the actual file transfer to recipient
void cleanupFileTransferSystem(void);                                                      // Cleans up file transfer system resources on shutdown

// Located in: server/logging.c
int initializeServerLogging(const char *log_filename);                 // Initializes the server logging mechanism
void finalizeServerLogging(void);                                      // Finalizes logging, flushes and closes log file
void logServerEvent(const char *tag, const char *details_format, ...); // General purpose logging function
// Specific event logging functions for consistent formatting (as per PDF examples)
void logEventServerStart(int port);
void logEventClientConnected(const char *username, const char *ip_address);
void logEventClientDisconnected(const char *username, int is_unexpected);
void logEventClientLoginFailed(const char *username_attempted, const char *ip_addr, const char *reason);
void logEventRoomCreated(const char *room_name);
void logEventClientJoinedRoom(const char *username, const char *room_name);
void logEventClientLeftRoom(const char *username, const char *room_name);
void logEventClientSwitchedRoom(const char *username, const char *old_room, const char *new_room);
void logEventBroadcast(const char *sender_username, const char *room_name, const char *message_content);
void logEventWhisper(const char *sender_username, const char *receiver_username, const char *message_preview);
void logEventFileTransferInitiated(const char *sender_username, const char *receiver_username, const char *filename);
void logEventFileQueued(const char *sender_username, const char *filename, int current_q_size);
void logEventFileRejectedOversized(const char *sender_username, const char *filename, size_t attempted_size);
void logEventFileTransferProcessingStart(const char *sender_username, const char *filename, long wait_time_seconds);
void logEventFileTransferCompleted(const char *sender_username, const char *receiver_username, const char *filename);
void logEventFileTransferFailed(const char *sender_username, const char *receiver_username, const char *filename, const char *reason);
void logEventFileCollision(const char *original_name, const char *new_name, const char *recipient_user, const char *sender_user);
void logEventSigintShutdown(int num_clients_at_shutdown);

// Located in: server/utils_server.c
// Finds an active client by username. Expects clients_list_mutex to be HELD by the caller.
ClientInfo *findClientByUsername(const char *username_to_find);
// Helper functions to send standardized messages to clients
void sendErrorToClient(int client_socket_fd, const char *error_message);
void sendSuccessToClient(int client_socket_fd, const char *success_message);
void sendSuccessWithRoomToClient(int client_socket_fd, const char *message, const char *room_name);
void sendServerNotificationToClient(int client_socket_fd, const char *notification_message, const char *room_context);
// Generates a new filename in case of collision (e.g., file.txt -> file_1.txt)
void generate_collided_filename(const char *original_filename, int collision_num, char *output_buffer, size_t buffer_size);

#endif // SERVER_COMMON_H