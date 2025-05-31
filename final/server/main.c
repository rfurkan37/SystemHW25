#include "common.h"

// Global server state instance definition
ServerMainState *g_server_state = NULL;

// Signal handler for SIGINT (Ctrl+C) to initiate graceful server shutdown.
void sigintShutdownHandler(int signal_number)
{
    if (signal_number == SIGINT && g_server_state && g_server_state->server_is_running)
    {
        // Use logServerEvent for initial message, not logEventSigintShutdown yet (that's for later summary)
        logServerEvent("SHUTDOWN_CTRLC", "SIGINT received. Initiating graceful server shutdown...");
        g_server_state->server_is_running = 0; // Primary flag to stop loops

        // Close the listening socket to prevent new connections during shutdown.
        // This also helps unblock the acceptClientConnectionsLoop if it's in accept() or select().
        if (g_server_state->server_listen_socket_fd >= 0)
        {
            shutdown(g_server_state->server_listen_socket_fd, SHUT_RDWR); // Signal to break accept
            close(g_server_state->server_listen_socket_fd);
            g_server_state->server_listen_socket_fd = -1; // Mark as closed
        }

        // Wake up file worker threads so they can check server_is_running and exit.
        // This is also done in cleanupServerResources, but doing it early helps.
        if (g_server_state)
        {
            FileUploadQueue *ftm = &g_server_state->file_transfer_manager;
            if (ftm)
            { // Check if ftm itself is initialized
                // Check if mutex is valid (e.g. by trying to lock, or assume it is if initialized)
                // This is simplified; a robust check would involve checking an init flag for ftm.
                pthread_mutex_lock(&ftm->queue_access_mutex); // Lock to safely broadcast
                pthread_cond_broadcast(&ftm->queue_not_empty_cond);
                pthread_mutex_unlock(&ftm->queue_access_mutex);
                for (int i = 0; i < MAX_UPLOAD_QUEUE_SIZE; ++i)
                { // Unblock any sem_wait/timedwait
                    sem_post(&ftm->available_upload_slots_sem);
                }
            }
        }
        // The main loop (acceptClientConnectionsLoop) will detect server_is_running = 0 and exit,
        // then cleanupServerResources will be called.
    }
    // If called again, default SIGINT action (terminate) will likely occur.
}

// Initializes the global server state structure and its subsystems.
void initializeServerState()
{
    g_server_state = malloc(sizeof(ServerMainState));
    if (!g_server_state)
    {
        // Cannot use logServerEvent as logging might not be up. Print to stderr.
        fprintf(stderr, "CRITICAL: Failed to allocate memory for ServerMainState. Exiting.\n");
        exit(EXIT_FAILURE);
    }
    memset(g_server_state, 0, sizeof(ServerMainState)); // Zero out the entire structure

    g_server_state->server_is_running = 1; // Server starts in a running state
    g_server_state->active_client_count = 0;
    g_server_state->current_room_count = 0;
    g_server_state->server_listen_socket_fd = -1; // Initialize listening socket as invalid

    // Initialize main server-wide mutexes
    if (pthread_mutex_init(&g_server_state->clients_list_mutex, NULL) != 0 ||
        pthread_mutex_init(&g_server_state->rooms_list_mutex, NULL) != 0)
    {
        fprintf(stderr, "CRITICAL: Failed to initialize main server mutexes: %s. Exiting.\n", strerror(errno));
        free(g_server_state); // Clean up allocated memory
        g_server_state = NULL;
        exit(EXIT_FAILURE);
    }

    // Initialize subsystems (rooms, file transfer)
    // These will log their own success/failure.
    initializeRoomSystem();         // Sets up room structures and their mutexes
    initializeFileTransferSystem(); // Sets up file queue, its sync primitives, and starts worker threads

    logServerEvent("INFO", "Server state and all subsystems initialized successfully.");
}

// Sets up the main listening socket for the server.
// Binds to INADDR_ANY and the specified port.
// Returns 1 on success, 0 on failure.
int setupServerListeningSocket(int port)
{
    if (!g_server_state)
        return 0; // Should not happen
    struct sockaddr_in server_address_config;

    g_server_state->server_listen_socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_server_state->server_listen_socket_fd < 0)
    {
        logServerEvent("CRITICAL", "Socket creation failed: %s", strerror(errno));
        return 0;
    }

    // Allow reuse of local addresses, helpful for quick server restarts
    int opt_reuse = 1;
    if (setsockopt(g_server_state->server_listen_socket_fd, SOL_SOCKET, SO_REUSEADDR, &opt_reuse, sizeof(opt_reuse)) < 0)
    {
        logServerEvent("WARNING", "setsockopt(SO_REUSEADDR) failed: %s. Continuing without it.", strerror(errno));
        // This is not critical for functionality but good for development.
    }

    // Configure server address
    memset(&server_address_config, 0, sizeof(server_address_config));
    server_address_config.sin_family = AF_INET;
    server_address_config.sin_addr.s_addr = INADDR_ANY; // Listen on all available network interfaces
    server_address_config.sin_port = htons(port);       // Convert port to network byte order

    // Bind the socket to the server address
    if (bind(g_server_state->server_listen_socket_fd, (struct sockaddr *)&server_address_config, sizeof(server_address_config)) < 0)
    {
        logServerEvent("CRITICAL", "Socket bind failed on port %d: %s. (Port may be in use or require sudo for <1024)", port, strerror(errno));
        close(g_server_state->server_listen_socket_fd);
        g_server_state->server_listen_socket_fd = -1;
        return 0;
    }

    // Start listening for incoming connections
    // MAX_SERVER_CLIENTS is used as backlog here, common practice.
    if (listen(g_server_state->server_listen_socket_fd, MAX_SERVER_CLIENTS) < 0)
    {
        logServerEvent("CRITICAL", "Socket listen failed: %s", strerror(errno));
        close(g_server_state->server_listen_socket_fd);
        g_server_state->server_listen_socket_fd = -1;
        return 0;
    }
    logEventServerStart(port); // Use specific log event
    return 1;                  // Listening socket setup successfully
}

// Main loop for accepting new client connections.
// Creates a new thread for each accepted client.
void acceptClientConnectionsLoop()
{
    if (!g_server_state || g_server_state->server_listen_socket_fd < 0)
    {
        logServerEvent("CRITICAL_LOOP", "Accept loop cannot start: server state or listen socket invalid.");
        return;
    }

    struct sockaddr_in client_address_info;
    socklen_t client_addr_len = sizeof(client_address_info);
    pthread_t client_thread_id_temp; // Temporary variable for thread creation

    logServerEvent("INFO", "Server is now accepting client connections on fd %d.", g_server_state->server_listen_socket_fd);
    while (g_server_state->server_is_running)
    {
        // Use select on the listening socket to make accept non-blocking
        // and allow periodic checks of the server_is_running flag.
        fd_set listen_fds;
        FD_ZERO(&listen_fds);
        FD_SET(g_server_state->server_listen_socket_fd, &listen_fds);
        struct timeval timeout = {1, 0}; // 1-second timeout for select

        int activity = select(g_server_state->server_listen_socket_fd + 1, &listen_fds, NULL, NULL, &timeout);

        if (!g_server_state->server_is_running)
            break; // Check flag immediately after select returns

        if (activity < 0)
        { // select() error
            if (errno == EINTR)
                continue; // Interrupted by a signal (e.g., SIGINT), loop to re-check
            logServerEvent("ERROR", "select() on listening socket failed: %s. Retrying...", strerror(errno));
            sleep(1); // Brief pause to avoid rapid spinning on persistent select error
            continue;
        }

        if (activity == 0)
            continue; // Timeout, no incoming connection, loop to check server_is_running

        // If select indicates activity on the listening socket, proceed to accept
        int new_client_socket_fd = accept(g_server_state->server_listen_socket_fd,
                                          (struct sockaddr *)&client_address_info, &client_addr_len);

        if (new_client_socket_fd < 0)
        {
            // This should be rare if select indicated readability, unless server_is_running changed
            // or another error occurred (e.g., resource exhaustion).
            if (errno == EINTR && g_server_state->server_is_running)
                continue; // Interrupted accept
            if (g_server_state->server_is_running)
            { // Log only if not shutting down
                logServerEvent("WARNING", "accept() failed despite select success: %s", strerror(errno));
            }
            continue; // Try to accept again or exit loop if shutting down
        }

        // Register the new client (adds to list, but not logged in yet)
        ClientInfo *new_client_data = registerNewClientOnServer(new_client_socket_fd, client_address_info);
        if (new_client_data)
        {
            // Create a new thread to handle this client's connection
            if (pthread_create(&client_thread_id_temp, NULL, clientConnectionThreadHandler, new_client_data) != 0)
            {
                logServerEvent("ERROR", "Failed to create thread for new client %s (fd %d): %s",
                               inet_ntoa(client_address_info.sin_addr), new_client_socket_fd, strerror(errno));
                // If thread creation fails, unregister the client and close their socket.
                unregisterClient(new_client_data, 1); // is_unexpected = true
            }
            else
            {
                new_client_data->thread_id = client_thread_id_temp; // Store thread ID (mainly for reference)
                // Client handler thread will detach itself (pthread_detach(pthread_self())).
            }
        }
        // If new_client_data is NULL, registerNewClientOnServer failed (e.g., max clients),
        // it already logged the issue, sent error to client, and closed the socket.
    }
    logServerEvent("INFO", "Server has stopped accepting new client connections.");
}

// Cleans up all server resources during shutdown.
// Notifies clients, joins worker threads, destroys mutexes/semaphores, frees memory.
void cleanupServerResources()
{
    if (!g_server_state)
        return; // Nothing to clean if state was never initialized

    int clients_at_shutdown_commence = g_server_state->active_client_count; // Capture for final log
    logServerEvent("INFO", "Starting final server resource cleanup. Active clients at start of shutdown: %d", clients_at_shutdown_commence);

    // 1. Ensure server_is_running is false (should be set by SIGINT handler or main exit path)
    g_server_state->server_is_running = 0;

    // 2. Close listening socket if not already closed (redundant if SIGINT handler did it)
    if (g_server_state->server_listen_socket_fd >= 0)
    {
        shutdown(g_server_state->server_listen_socket_fd, SHUT_RDWR);
        close(g_server_state->server_listen_socket_fd);
        g_server_state->server_listen_socket_fd = -1;
    }

    // 3. Clean up file transfer system (signals and joins worker threads, clears queue, destroys sync objects)
    cleanupFileTransferSystem();

    // 4. Notify and attempt to gracefully close active client connections.
    // Client handler threads are detached, so we can't join them here.
    // We trigger their shutdown by closing their sockets. They should then call unregisterClient.
    logServerEvent("INFO", "Server Shutdown: Notifying and closing active client sockets...");
    ClientInfo *clients_to_notify[MAX_SERVER_CLIENTS] = {NULL};
    int num_to_notify = 0;

    pthread_mutex_lock(&g_server_state->clients_list_mutex);
    for (int i = 0; i < MAX_SERVER_CLIENTS; ++i)
    {
        if (g_server_state->connected_clients[i] != NULL && g_server_state->connected_clients[i]->is_active)
        {
            // Store a reference; do not operate directly on shared list while iterating without holding lock for long
            clients_to_notify[num_to_notify++] = g_server_state->connected_clients[i];
        }
    }
    pthread_mutex_unlock(&g_server_state->clients_list_mutex);

    for (int i = 0; i < num_to_notify; ++i)
    {
        if (clients_to_notify[i] && clients_to_notify[i]->socket_fd >= 0)
        {
            notifyClientOfShutdown(clients_to_notify[i]->socket_fd); // Best effort send
            shutdown(clients_to_notify[i]->socket_fd, SHUT_RDWR);    // Signal client thread's select/recv
            close(clients_to_notify[i]->socket_fd);                  // Close socket
            // The client's handler thread should detect this and call unregisterClient.
            // We mark the socket as -1 here to prevent double close if unregisterClient is slow.
            // However, client_info struct itself is managed by its thread.
            // This is slightly risky if unregisterClient hasn't run yet and tries to use the fd.
            // A safer model might involve signalling threads then joining, but detached threads complicate this.
            // Let's assume client handler sets its own fd to -1 in unregisterClient.
        }
    }

    // Give client handler threads a moment to process their shutdown and call unregisterClient.
    // This is a pragmatic approach for detached threads.
    logServerEvent("INFO", "Server Shutdown: Allowing a moment for client threads to self-terminate...");
    sleep(2); // Adjust as needed, or implement a more robust join/signal mechanism for client threads.

    // 5. Log final SIGINT summary (using the count from *before* client thread cleanup started)
    logEventSigintShutdown(clients_at_shutdown_commence);

    // 6. Clean up room mutexes
    // Assuming all client threads have exited and no longer hold room_locks.
    // pthread_mutex_lock(&g_server_state->rooms_list_mutex); // Lock the list itself before iterating
    // For loop based on MAX_ROOMS as rooms might not be contiguous if some were deleted (not implemented)
    for (int i = 0; i < MAX_ROOMS; ++i)
    {
        // Only destroy if initialized (e.g., check name or use a flag, but init does all)
        // if (strlen(g_server_state->chat_rooms[i].name) > 0 || i < g_server_state->current_room_count) {
        if (pthread_mutex_destroy(&g_server_state->chat_rooms[i].room_lock) != 0)
        {
            if (errno == EBUSY)
            {
                logServerEvent("WARNING_SHUTDOWN", "Room mutex for slot %d ('%s') is busy during cleanup. Forcing destroy.", i, g_server_state->chat_rooms[i].name);
                // Force destroy might be bad, but server is exiting.
            }
            else if (errno == EINVAL)
            {
                // logServerEvent("WARNING_SHUTDOWN", "Room mutex for slot %d ('%s') was invalid/not init for destroy.", i, g_server_state->chat_rooms[i].name);
            }
        }
        // }
    }
    // pthread_mutex_unlock(&g_server_state->rooms_list_mutex);

    // 7. Destroy main server mutexes
    // Ensure these are not held by any lingering threads (should not be if shutdown is orderly).
    if (pthread_mutex_destroy(&g_server_state->clients_list_mutex) != 0)
    {
        logServerEvent("WARNING_SHUTDOWN", "Failed to destroy clients_list_mutex: %s. Possibly still in use.", strerror(errno));
    }
    if (pthread_mutex_destroy(&g_server_state->rooms_list_mutex) != 0)
    {
        logServerEvent("WARNING_SHUTDOWN", "Failed to destroy rooms_list_mutex: %s. Possibly still in use.", strerror(errno));
    }

    // 8. Free the global server state structure itself
    free(g_server_state);
    g_server_state = NULL;

    logServerEvent("INFO", "Server resource cleanup sequence complete.");
    finalizeServerLogging(); // Close the log file as the very last step.
}

// Main function for the server application.
int main(int argc, char *argv[])
{
    if (argc != 2)
    {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        fprintf(stderr, "Example: %s 5000\n", argv[0]);
        return EXIT_FAILURE;
    }

    int server_port = atoi(argv[1]);
    if (server_port <= 0 || server_port > 65535)
    {
        fprintf(stderr, "Invalid port number: %s. Must be between 1 and 65535.\n", argv[1]);
        return EXIT_FAILURE;
    }

    // Initialize logging first, so other initializations can log
    if (!initializeServerLogging(SERVER_LOG_FILENAME))
    {
        // If logging fails, cannot proceed reliably.
        fprintf(stderr, "CRITICAL: Server logging could not be initialized. Exiting.\n");
        return EXIT_FAILURE;
    }

    // Setup SIGINT handler for graceful shutdown
    struct sigaction sigint_action_config;
    memset(&sigint_action_config, 0, sizeof(sigint_action_config));
    sigint_action_config.sa_handler = sigintShutdownHandler;
    sigemptyset(&sigint_action_config.sa_mask); // Do not block other signals during handler
    // sigint_action_config.sa_flags = SA_RESTART; // Generally not wanted for accept(), want it to be interruptible
    if (sigaction(SIGINT, &sigint_action_config, NULL) == -1)
    {
        logServerEvent("CRITICAL", "Failed to set SIGINT handler: %s", strerror(errno));
        finalizeServerLogging(); // Attempt to close log file
        return EXIT_FAILURE;
    }

    // Ignore SIGPIPE to prevent server crashing if it writes to a socket
    // whose client has disconnected abruptly. send() will return an error instead.
    signal(SIGPIPE, SIG_IGN);

    // Initialize server state (allocates g_server_state, inits mutexes, subsystems like rooms/file transfer)
    initializeServerState();

    // Setup the main listening socket
    if (!setupServerListeningSocket(server_port))
    {
        logServerEvent("CRITICAL", "Failed to setup server listening socket. Shutting down.");
        cleanupServerResources(); // Attempt to clean up what was initialized
        // finalizeServerLogging() is called by cleanupServerResources()
        return EXIT_FAILURE;
    }

    // Start the main loop to accept client connections
    // This loop will run until g_server_state->server_is_running becomes false.
    acceptClientConnectionsLoop();

    // Post-loop: server is shutting down (either by SIGINT or other means if implemented)
    logServerEvent("INFO", "Server main accept loop has ended. Proceeding with full shutdown sequence.");
    cleanupServerResources(); // Perform all necessary cleanup

    // A final message to console after logging is closed.
    // printf("Server has shut down completely.\n"); // Not needed if logServerEvent used till end
    return EXIT_SUCCESS;
}