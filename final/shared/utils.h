#ifndef SHARED_UTILS_H
#define SHARED_UTILS_H

#include "protocol.h" // For Message struct definition

// Socket communication wrapper functions
// Sends a Message struct over the given socket.
// Returns 1 on success (all bytes of Message sent), 0 on failure.
int sendMessage(int socket_fd, const Message *msg);

// Receives a Message struct from the given socket.
// Returns 1 on success (all bytes of Message received), 0 on failure or if connection closed.
int receiveMessage(int socket_fd, Message *msg);

// Validation functions (shared between client and server)
// Checks if a username is valid (alphanumeric, correct length).
// Returns 1 if valid, 0 otherwise.
int isValidUsername(const char *username);

// Checks if a room name is valid (alphanumeric, correct length, no spaces).
// Returns 1 if valid, 0 otherwise.
int isValidRoomName(const char *room_name);

// Checks if a filename has a supported extension.
// Returns 1 if type is valid, 0 otherwise.
int isValidFileType(const char *filename);

// File utility functions
// Gets the size of a file from its path.
// Returns file size on success, -1 on error (e.g., file not found).
long getFileSizeFromPath(const char *filepath);

#endif // SHARED_UTILS_H