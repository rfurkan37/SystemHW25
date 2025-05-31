#include "common.h"

// Establishes a TCP connection to the server.
// Returns 1 on successful connection, 0 on failure.
int connectClientToServer(ClientState *clientState, const char *server_ip, int port)
{
    struct sockaddr_in server_address;

    clientState->socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (clientState->socket_fd < 0)
    {
        perror("\033[31mSocket creation failed\033[0m");
        return 0;
    }

    memset(&server_address, 0, sizeof(server_address));
    server_address.sin_family = AF_INET;
    server_address.sin_port = htons(port);

    // Convert IP address from text to binary form
    if (inet_pton(AF_INET, server_ip, &server_address.sin_addr) <= 0)
    {
        fprintf(stderr, "\033[31mInvalid server IP address format: %s\033[0m\n", server_ip);
        close(clientState->socket_fd);
        clientState->socket_fd = -1;
        return 0;
    }

    // Connect to the server
    if (connect(clientState->socket_fd, (struct sockaddr *)&server_address, sizeof(server_address)) < 0)
    {
        fprintf(stderr, "\033[31mConnection to server %s:%d failed: %s\033[0m\n", server_ip, port, strerror(errno));
        close(clientState->socket_fd);
        clientState->socket_fd = -1;
        return 0;
    }

    printf("\033[32mSuccessfully connected to server %s:%d\033[0m\n", server_ip, port);
    return 1;
}

// Handles the client login process.
// Prompts user for username, sends login request, and processes server response.
// Returns 1 on successful login, 0 on failure.
int performClientLogin(ClientState *clientState)
{
    char username_input[USERNAME_BUF_SIZE];
    Message login_req_msg;
    Message server_response_msg;

    // Loop until a syntactically valid username is entered
    while (1)
    {
        printf("Enter your username (alphanumeric, 1-%d chars): ", MAX_USERNAME_LEN);
        fflush(stdout); // Ensure prompt is displayed before input

        if (fgets(username_input, sizeof(username_input), stdin) == NULL)
        {
            fprintf(stderr, "\n\033[31mFailed to read username (EOF or input error).\033[0m\n");
            return 0; // Critical input failure
        }
        username_input[strcspn(username_input, "\n")] = '\0'; // Remove trailing newline

        if (isValidUsername(username_input))
        {
            break; // Username format is valid, proceed
        }
        else
        {
            printf("\033[31mInvalid username format. Must be alphanumeric, 1-%d characters, no spaces. Please try again.\033[0m\n", MAX_USERNAME_LEN);
        }
    }

    // Prepare login message
    memset(&login_req_msg, 0, sizeof(login_req_msg));
    login_req_msg.type = MSG_LOGIN;
    strncpy(login_req_msg.sender, username_input, USERNAME_BUF_SIZE - 1);
    // Ensure null termination, though strncpy with USERNAME_BUF_SIZE-1 handles it if source is shorter or equal.
    login_req_msg.sender[USERNAME_BUF_SIZE - 1] = '\0';

    // Send login request to server
    if (!sendMessage(clientState->socket_fd, &login_req_msg))
    {
        fprintf(stderr, "\033[31mFailed to send login request to server. Connection may be lost.\033[0m\n");
        return 0;
    }

    // Wait for server's response
    if (!receiveMessage(clientState->socket_fd, &server_response_msg))
    {
        fprintf(stderr, "\033[31mNo response from server during login, or connection lost.\033[0m\n");
        return 0;
    }

    // Process server's response
    if (server_response_msg.type == MSG_LOGIN_SUCCESS)
    {
        strncpy(clientState->username, username_input, USERNAME_BUF_SIZE - 1);
        clientState->username[USERNAME_BUF_SIZE - 1] = '\0';        // Ensure null termination
        printf("\033[32m%s\033[0m\n", server_response_msg.content); // Display server's success message
        printf("\033[36mWelcome, %s! Type /help for a list of commands.\033[0m\n", clientState->username);
        return 1; // Login successful
    }
    else if (server_response_msg.type == MSG_LOGIN_FAILURE)
    {
        printf("\033[31mLogin failed: %s\033[0m\n", server_response_msg.content); // Display server's reason for failure
        return 0;                                                                 // Login failed
    }
    else
    {
        printf("\033[31mUnexpected response from server during login (type %d). Content: %s\033[0m\n",
               server_response_msg.type, server_response_msg.content);
        return 0; // Unexpected response
    }
}