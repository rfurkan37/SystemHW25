#include "common.h"

int connect_to_server(client_state_t* client, const char* server_ip, int port) {
    struct sockaddr_in server_addr;
    
    // Create socket
    client->socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (client->socket_fd < 0) {
        perror("Socket creation failed");
        return 0;
    }
    
    // Configure server address
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    
    if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) <= 0) {
        printf("\033[31mInvalid server IP address\033[0m\n");
        close(client->socket_fd);
        return 0;
    }
    
    // Connect to server
    if (connect(client->socket_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        printf("\033[31mConnection to server failed: %s\033[0m\n", strerror(errno));
        close(client->socket_fd);
        return 0;
    }
    
    printf("\033[32mConnected to server %s:%d\033[0m\n", server_ip, port);
    return 1;
}

int send_message(int socket_fd, message_t* msg) {
    ssize_t bytes_sent = send(socket_fd, msg, sizeof(message_t), 0);
    return (bytes_sent == sizeof(message_t)) ? 1 : 0;
}

int receive_message(int socket_fd, message_t* msg) {
    ssize_t bytes_received = recv(socket_fd, msg, sizeof(message_t), 0);
    if (bytes_received == sizeof(message_t)) {
        // Ensure all string fields are null-terminated for safety
        msg->sender[MAX_USERNAME_LEN] = '\0';
        msg->receiver[MAX_USERNAME_LEN] = '\0';
        msg->room[MAX_ROOM_NAME_LEN] = '\0';
        msg->content[MAX_MESSAGE_LEN - 1] = '\0';
        msg->filename[MAX_FILENAME_LEN - 1] = '\0';
        return 1;
    }
    return 0;
} 