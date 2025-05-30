#ifndef SHARED_UTILS_H
#define SHARED_UTILS_H

#include "protocol.h" // For message_t

// Socket communication functions
int send_message(int socket_fd, const message_t *msg);
int receive_message(int socket_fd, message_t *msg);

// Validation functions
int is_valid_username(const char *username);
int is_valid_room_name(const char *room_name);
int is_valid_file_type(const char *filename); // Used by client and server for pre-check

// File utility (client-side primarily for sending)
long get_file_size_from_path(const char *filepath); // Gets size from path
long get_file_size_from_fd(int fd);                 // Gets size from open file descriptor

#endif // SHARED_UTILS_H