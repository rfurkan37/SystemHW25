#include "common.h"

// Define global server state instance
server_main_state_t *g_server_state = NULL;

void sigint_shutdown_handler(int signal_number) {
    if (signal_number == SIGINT && g_server_state && g_server_state->server_is_running) {
        // Use a more direct log without client count yet, as it might change during shutdown
        log_server_event("SHUTDOWN", "SIGINT received. Initiating graceful server shutdown...");
        
        g_server_state->server_is_running = 0; // Signal all threads to stop

        // Close the listening socket to prevent new connections
        if (g_server_state->server_listen_socket_fd >= 0) {
            shutdown(g_server_state->server_listen_socket_fd, SHUT_RDWR); // Stop accepting
            close(g_server_state->server_listen_socket_fd);
            g_server_state->server_listen_socket_fd = -1; // Mark as closed
        }
        
        // File transfer system cleanup will be called by cleanup_server_resources,
        // which will signal and join worker threads.
        // The g_server_state->server_is_running = 0 will make worker loops exit.
        // Broadcasting on condvar and posting to semaphore helps them wake up to check.
        pthread_mutex_lock(&g_server_state->file_transfer_manager.queue_access_mutex);
        pthread_cond_broadcast(&g_server_state->file_transfer_manager.queue_not_empty_cond);
        pthread_mutex_unlock(&g_server_state->file_transfer_manager.queue_access_mutex);
        for(int i=0; i < MAX_UPLOAD_QUEUE_SIZE; ++i) { // Unblock any sem_wait
            sem_post(&g_server_state->file_transfer_manager.available_upload_slots_sem);
        }


        // Client threads will also see server_is_running = 0 and start exiting.
        // The main accept_client_connections_loop will exit too.
        // Actual client disconnection and resource cleanup happens in cleanup_server_resources.
    }
}


void initialize_server_state() {
    g_server_state = malloc(sizeof(server_main_state_t));
    if (!g_server_state) {
        // No logging system yet, so use stderr.
        fprintf(stderr, "CRITICAL: Failed to allocate memory for server_main_state_t. Exiting.\n");
        exit(EXIT_FAILURE);
    }
    memset(g_server_state, 0, sizeof(server_main_state_t)); // Zero out all members

    g_server_state->server_is_running = 1;
    g_server_state->active_client_count = 0;
    g_server_state->current_room_count = 0;
    g_server_state->server_listen_socket_fd = -1; // Initialize to invalid

    // Initialize top-level mutexes
    if (pthread_mutex_init(&g_server_state->clients_list_mutex, NULL) != 0 ||
        pthread_mutex_init(&g_server_state->rooms_list_mutex, NULL) != 0) {
        fprintf(stderr, "CRITICAL: Failed to initialize main server mutexes. Exiting.\n");
        free(g_server_state); // Free allocated memory before exit
        exit(EXIT_FAILURE);
    }
    
    // Initialize subsystems (logging is initialized first in main)
    initialize_room_system();       // Initializes room structures and their mutexes
    initialize_file_transfer_system(); // Initializes file queue, sem, cond, and starts workers

    log_server_event("INFO", "Server state and subsystems initialized successfully.");
}

int setup_server_listening_socket(int port) {
    struct sockaddr_in server_address_config;

    g_server_state->server_listen_socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_server_state->server_listen_socket_fd < 0) {
        log_server_event("CRITICAL", "Socket creation failed: %s", strerror(errno));
        return 0; // Failure
    }

    // Allow address reuse (helpful for quick server restarts)
    int opt_reuse = 1;
    if (setsockopt(g_server_state->server_listen_socket_fd, SOL_SOCKET, SO_REUSEADDR, &opt_reuse, sizeof(opt_reuse)) < 0) {
        log_server_event("WARNING", "setsockopt(SO_REUSEADDR) failed: %s", strerror(errno));
        // Not critical, but good to log
    }

    memset(&server_address_config, 0, sizeof(server_address_config));
    server_address_config.sin_family = AF_INET;
    server_address_config.sin_addr.s_addr = INADDR_ANY; // Listen on all available interfaces
    server_address_config.sin_port = htons(port);

    if (bind(g_server_state->server_listen_socket_fd, (struct sockaddr *)&server_address_config, sizeof(server_address_config)) < 0) {
        log_server_event("CRITICAL", "Socket bind failed on port %d: %s", port, strerror(errno));
        close(g_server_state->server_listen_socket_fd);
        g_server_state->server_listen_socket_fd = -1;
        return 0; // Failure
    }

    if (listen(g_server_state->server_listen_socket_fd, MAX_SERVER_CLIENTS) < 0) { // Backlog size
        log_server_event("CRITICAL", "Socket listen failed: %s", strerror(errno));
        close(g_server_state->server_listen_socket_fd);
        g_server_state->server_listen_socket_fd = -1;
        return 0; // Failure
    }

    log_event_server_start(port); // Specific log for this event
    return 1; // Success
}

void accept_client_connections_loop() {
    struct sockaddr_in client_address_info;
    socklen_t client_addr_len = sizeof(client_address_info);
    pthread_t client_thread_id_temp; // To store ID of newly created thread

    log_server_event("INFO", "Server starting to accept client connections.");
    while (g_server_state->server_is_running) {
        int new_client_socket_fd = accept(g_server_state->server_listen_socket_fd, 
                                         (struct sockaddr *)&client_address_info, &client_addr_len);

        if (!g_server_state->server_is_running) break; // Check flag immediately after accept returns

        if (new_client_socket_fd < 0) {
            if (errno == EINTR ) continue; // Interrupted by signal (e.g. SIGINT), re-check running flag
            log_server_event("WARNING", "accept() failed or server socket closed: %s", strerror(errno));
            // If server_is_running is false, this is expected during shutdown.
            // Otherwise, it might be a temporary error or listen socket issue.
            sleep(1); // Prevent rapid spin on persistent accept error
            continue;
        }

        // Check if server can handle more clients
        pthread_mutex_lock(&g_server_state->clients_list_mutex);
        // Note: active_client_count is for LOGGED IN clients. We need to check available slots.
        int available_slot = 0;
        for(int i=0; i < MAX_SERVER_CLIENTS; ++i) if(g_server_state->connected_clients[i] == NULL) available_slot = 1;

        if (!available_slot) {
        // if (g_server_state->active_client_count >= MAX_SERVER_CLIENTS) { // This check is slightly off
            pthread_mutex_unlock(&g_server_state->clients_list_mutex);
            log_server_event("INFO", "Max client limit (%d) reached. Rejecting new connection from %s.", 
                             MAX_SERVER_CLIENTS, inet_ntoa(client_address_info.sin_addr));
            send_error_to_client(new_client_socket_fd, "Server is currently full. Please try again later.");
            close(new_client_socket_fd);
            continue;
        }
        pthread_mutex_unlock(&g_server_state->clients_list_mutex);


        client_info_t *new_client_data = register_new_client_on_server(new_client_socket_fd, client_address_info);
        if (new_client_data) {
            if (pthread_create(&client_thread_id_temp, NULL, client_connection_thread_handler, new_client_data) != 0) {
                log_server_event("ERROR", "Failed to create thread for new client %s: %s", 
                                 inet_ntoa(client_address_info.sin_addr), strerror(errno));
                unregister_client(new_client_data, 1); // Clean up the client struct and close socket
            } else {
                new_client_data->thread_id = client_thread_id_temp; // Store thread ID if needed for join (usually detach)
                pthread_detach(client_thread_id_temp); // Detach as per Q&A suggestion for pthread_detach
                // log_server_event("DEBUG", "Thread %lu created for client from %s.", 
                //                  (unsigned long)client_thread_id_temp, inet_ntoa(client_address_info.sin_addr));
            }
        } else {
            // register_new_client_on_server failed and already logged/closed socket.
        }
    }
    log_server_event("INFO", "Server has stopped accepting new connections.");
}

void cleanup_server_resources() {
    log_server_event("INFO", "Starting final server resource cleanup...");
    int clients_at_shutdown = g_server_state->active_client_count; // Before clearing them

    // File transfer system cleanup (joins worker threads)
    cleanup_file_transfer_system();

    // Notify and close remaining client connections
    // Client threads should exit due to server_is_running=0.
    // This is a final sweep.
    pthread_mutex_lock(&g_server_state->clients_list_mutex);
    for (int i = 0; i < MAX_SERVER_CLIENTS; ++i) {
        if (g_server_state->connected_clients[i] != NULL) {
            client_info_t *client = g_server_state->connected_clients[i];
            if (client->is_active) { // If thread didn't clean it up yet
                notify_client_of_shutdown(client->socket_fd);
                // Client thread is responsible for its own unregister_client call.
                // But if server shuts down abruptly, we might need to force close here.
                // For pthread_detach, client thread might still be running.
                // Setting is_active=0 and closing socket here ensures it stops.
                client->is_active = 0;
                if(client->socket_fd >=0) {
                    shutdown(client->socket_fd, SHUT_RDWR);
                    close(client->socket_fd);
                    client->socket_fd = -1;
                }
            }
            // The client_info_t struct itself will be freed by its handler thread upon exit.
            // If we free it here, the handler might double-free or use freed memory.
            // For detached threads, it's tricky. A joinable model is safer for this kind of cleanup.
            // Given pthread_detach recommendation, rely on client_handler to free its own client_info.
            // So, we might not explicitly free g_server_state->connected_clients[i] here.
            // Alternative: client_handler sets connected_clients[i] to NULL before freeing.
            // For this submission, let's assume client_handler frees its own client_info.
             g_server_state->connected_clients[i] = NULL; // Remove from list
        }
    }
    g_server_state->active_client_count = 0;
    pthread_mutex_unlock(&g_server_state->clients_list_mutex);


    // Clean up room mutexes (room_system was initialized, so locks exist)
    pthread_mutex_lock(&g_server_state->rooms_list_mutex);
    for (int i = 0; i < MAX_ROOMS; ++i) { // Iterate all possible room slots
        // Check if room was actually used (e.g. name is set)
        // if (strlen(g_server_state->chat_rooms[i].name) > 0) {
            pthread_mutex_destroy(&g_server_state->chat_rooms[i].room_lock);
        // }
    }
    pthread_mutex_unlock(&g_server_state->rooms_list_mutex);


    // Destroy main server mutexes
    pthread_mutex_destroy(&g_server_state->clients_list_mutex);
    pthread_mutex_destroy(&g_server_state->rooms_list_mutex);

    log_event_sigint_shutdown(clients_at_shutdown); // Log with count before client list is fully cleared

    // Free the global server state itself
    if (g_server_state) {
        free(g_server_state);
        g_server_state = NULL;
    }

    log_server_event("INFO", "Server shutdown complete. All resources released.");
    finalize_server_logging(); // Close log file as the very last step
}


int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        fprintf(stderr, "Example: %s 5000\n", argv[0]);
        return EXIT_FAILURE;
    }

    int server_port = atoi(argv[1]);
    if (server_port <= 0 || server_port > 65535) {
        fprintf(stderr, "Invalid port number: %s. Must be between 1 and 65535.\n", argv[1]);
        return EXIT_FAILURE;
    }

    // Initialize logging system first
    if (!initialize_server_logging(SERVER_LOG_FILENAME)) {
        // Error already printed by init function
        return EXIT_FAILURE; 
    }

    // Setup signal handler for SIGINT (Ctrl+C)
    struct sigaction sigint_action_config;
    memset(&sigint_action_config, 0, sizeof(sigint_action_config));
    sigint_action_config.sa_handler = sigint_shutdown_handler;
    // sigint_action_config.sa_flags = SA_RESTART; // Important for syscalls like accept
    if (sigaction(SIGINT, &sigint_action_config, NULL) == -1) {
        log_server_event("CRITICAL", "Failed to set SIGINT handler: %s", strerror(errno));
        finalize_server_logging();
        return EXIT_FAILURE;
    }

    // Initialize server state (allocates g_server_state, inits mutexes, subsystems)
    initialize_server_state(); // This now also starts file worker threads

    // Setup listening socket
    if (!setup_server_listening_socket(server_port)) {
        // Error already logged
        cleanup_server_resources(); // Attempt cleanup of what was initialized
        return EXIT_FAILURE;
    }

    // Main loop to accept client connections
    accept_client_connections_loop();

    // --- Shutdown sequence (triggered by SIGINT or loop exit condition) ---
    log_server_event("INFO", "Server main loop ended. Proceeding with full shutdown sequence.");
    cleanup_server_resources();

    // printf("Server has shut down.\n"); // Final console message after logging is closed.
    return EXIT_SUCCESS;
}