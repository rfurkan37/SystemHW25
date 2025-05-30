#include "common.h"

int connect_client_to_server(client_state_t *client_state, const char *server_ip, int port) {
    struct sockaddr_in server_address;

    client_state->socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (client_state->socket_fd < 0) {
        perror("\033[31mSocket creation failed\033[0m");
        return 0;
    }

    memset(&server_address, 0, sizeof(server_address));
    server_address.sin_family = AF_INET;
    server_address.sin_port = htons(port);

    if (inet_pton(AF_INET, server_ip, &server_address.sin_addr) <= 0) {
        printf("\033[31mInvalid server IP address: %s\033[0m\n", server_ip);
        close(client_state->socket_fd);
        client_state->socket_fd = -1;
        return 0;
    }

    if (connect(client_state->socket_fd, (struct sockaddr *)&server_address, sizeof(server_address)) < 0) {
        printf("\033[31mConnection to server %s:%d failed: %s\033[0m\n", server_ip, port, strerror(errno));
        close(client_state->socket_fd);
        client_state->socket_fd = -1;
        return 0;
    }

    printf("\033[32mSuccessfully connected to server %s:%d\033[0m\n", server_ip, port);
    return 1;
}

int perform_client_login(client_state_t *client_state) {
    char username_input[USERNAME_BUF_SIZE]; // Buffer for username

    printf("Enter your username (alphanumeric, 1-%d chars): ", MAX_USERNAME_LEN);
    fflush(stdout);

    if (!fgets(username_input, sizeof(username_input), stdin)) {
        printf("\033[31mFailed to read username.\033[0m\n");
        return 0; // EOF or error
    }
    username_input[strcspn(username_input, "\n")] = '\0'; // Remove trailing newline

    if (!is_valid_username(username_input)) {
        printf("\033[31mInvalid username. Must be alphanumeric, 1-%d characters, no spaces.\033[0m\n", MAX_USERNAME_LEN);
        return 0;
    }

    // Prepare login message
    message_t login_req_msg;
    memset(&login_req_msg, 0, sizeof(login_req_msg));
    login_req_msg.type = MSG_LOGIN;
    strncpy(login_req_msg.sender, username_input, USERNAME_BUF_SIZE - 1);

    if (!send_message(client_state->socket_fd, &login_req_msg)) {
        printf("\033[31mFailed to send login request to server.\033[0m\n");
        return 0;
    }

    // Wait for server's response to login
    message_t server_response_msg;
    if (!receive_message(client_state->socket_fd, &server_response_msg)) {
        printf("\033[31mNo response from server during login, or connection lost.\033[0m\n");
        return 0;
    }

    // Check response type (using new specific types for login)
    if (server_response_msg.type == MSG_LOGIN_SUCCESS) {
        strncpy(client_state->username, username_input, USERNAME_BUF_SIZE -1);
        printf("\033[32m%s\033[0m\n", server_response_msg.content); // e.g., "Login successful."
        printf("\033[36mWelcome, %s! Type /help for commands.\033[0m\n", client_state->username);
        return 1;
    } else if (server_response_msg.type == MSG_LOGIN_FAILURE) {
        printf("\033[31mLogin failed: %s\033[0m\n", server_response_msg.content);
        return 0;
    } else {
        printf("\033[31mUnexpected response from server during login (type %d).\033[0m\n", server_response_msg.type);
        return 0;
    }
}