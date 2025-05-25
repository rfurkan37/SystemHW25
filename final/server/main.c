#include "common.h"

// Global server state
server_state_t* g_server_state;
static volatile int shutdown_requested = 0;

// Signal handler for graceful shutdown
void signal_handler(int sig) {
    if (sig == SIGINT && !shutdown_requested) {
        shutdown_requested = 1;
        log_message("SHUTDOWN", "SIGINT received. Initiating graceful shutdown.");
        g_server_state->running = 0;
        
        // Notify all clients
        broadcast_shutdown_message();
        
        // Close server socket to stop accepting new connections
        close(g_server_state->server_socket);
        
        // Signal file transfer worker to wake up
        pthread_mutex_lock(&g_server_state->file_queue->queue_mutex);
        pthread_cond_signal(&g_server_state->file_queue->queue_cond);
        pthread_mutex_unlock(&g_server_state->file_queue->queue_mutex);
    }
}

void init_server_state(void) {
    g_server_state = malloc(sizeof(server_state_t));
    memset(g_server_state, 0, sizeof(server_state_t));
    
    g_server_state->running = 1;
    g_server_state->client_count = 0;
    g_server_state->room_count = 0;
    
    // Initialize mutexes
    pthread_mutex_init(&g_server_state->clients_mutex, NULL);
    pthread_mutex_init(&g_server_state->rooms_mutex, NULL);
    
    // Initialize file queue
    init_file_queue();
    
    log_message("INIT", "Server state initialized");
}

void setup_server_socket(int port) {
    struct sockaddr_in server_addr;
    
    // Create socket
    g_server_state->server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (g_server_state->server_socket < 0) {
        perror("Socket creation failed");
        exit(1);
    }
    
    // Set socket options
    int opt = 1;
    if (setsockopt(g_server_state->server_socket, SOL_SOCKET, SO_REUSEADDR, 
                   &opt, sizeof(opt)) < 0) {
        perror("Setsockopt failed");
        exit(1);
    }
    
    // Configure server address
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);
    
    // Bind socket
    if (bind(g_server_state->server_socket, (struct sockaddr*)&server_addr, 
             sizeof(server_addr)) < 0) {
        perror("Bind failed");
        exit(1);
    }
    
    // Listen for connections
    if (listen(g_server_state->server_socket, MAX_CLIENTS) < 0) {
        perror("Listen failed");
        exit(1);
    }
    
    char log_msg[256];
    snprintf(log_msg, sizeof(log_msg), "Server listening on port %d", port);
    log_message("INIT", log_msg);
}

void accept_connections(void) {
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    
    while (g_server_state->running) {
        int client_socket = accept(g_server_state->server_socket, 
                                 (struct sockaddr*)&client_addr, &addr_len);
        
        if (client_socket < 0) {
            if (g_server_state->running) {
                perror("Accept failed");
            } else {
                // Server is shutting down, break out of loop
                break;
            }
            continue;
        }
        
        // Check if server is full
        pthread_mutex_lock(&g_server_state->clients_mutex);
        if (g_server_state->client_count >= MAX_CLIENTS) {
            pthread_mutex_unlock(&g_server_state->clients_mutex);
            send_error_message(client_socket, "Server is full");
            close(client_socket);
            continue;
        }
        pthread_mutex_unlock(&g_server_state->clients_mutex);
        
        // Create new client
        client_t* new_client = create_client(client_socket, client_addr);
        if (new_client) {
            pthread_create(&new_client->thread_id, NULL, 
                          client_handler, (void*)new_client);
            pthread_detach(new_client->thread_id);
        }
    }
}

void cleanup_server(void) {
    log_message("SHUTDOWN", "Cleaning up server resources");
    
    // Give threads a moment to finish
    sleep(1);
    
    // Close all client connections
    pthread_mutex_lock(&g_server_state->clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (g_server_state->clients[i]) {
            g_server_state->clients[i]->is_active = 0;
            close(g_server_state->clients[i]->socket_fd);
        }
    }
    pthread_mutex_unlock(&g_server_state->clients_mutex);
    
    // Clean up file queue
    if (g_server_state->file_queue) {
        pthread_mutex_destroy(&g_server_state->file_queue->queue_mutex);
        pthread_cond_destroy(&g_server_state->file_queue->queue_cond);
        sem_destroy(&g_server_state->file_queue->queue_semaphore);
        free(g_server_state->file_queue);
    }
    
    // Clean up mutexes
    pthread_mutex_destroy(&g_server_state->clients_mutex);
    pthread_mutex_destroy(&g_server_state->rooms_mutex);
    
    // Clean up room mutexes
    for (int i = 0; i < g_server_state->room_count; i++) {
        pthread_mutex_destroy(&g_server_state->rooms[i].room_mutex);
    }
    
    close(g_server_state->server_socket);
    free(g_server_state);
    
    log_message("SHUTDOWN", "Server shutdown complete");
}

int main(int argc, char* argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        return 1;
    }
    
    // Initialize logging first
    init_logging();
    
    // Initialize server state
    init_server_state();
    
    // Set up signal handler
    signal(SIGINT, signal_handler);
    
    // Create server socket
    int port = atoi(argv[1]);
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Invalid port number\n");
        return 1;
    }
    
    setup_server_socket(port);
    
    // Start file transfer thread
    pthread_t file_thread;
    pthread_create(&file_thread, NULL, file_transfer_worker, NULL);
    pthread_detach(file_thread);
    
    log_message("START", "Chat server started successfully");
    
    // Main accept loop
    accept_connections();
    
    // Cleanup
    cleanup_server();
    return 0;
} 