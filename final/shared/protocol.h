#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stddef.h> // For size_t

// Maximum limits as per project description
#define MAX_USERNAME_LEN 16
#define MAX_ROOM_NAME_LEN 32            // PDF: "up to 32 characters"
#define MAX_MESSAGE_LEN 1024            // Arbitrary reasonable limit for chat messages
#define MAX_FILENAME_LEN 256            // Standard OS limit often around 255/256
#define MAX_FILE_SIZE (3 * 1024 * 1024) // 3MB as per PDF

// Buffer sizes (including null terminator)
#define USERNAME_BUF_SIZE (MAX_USERNAME_LEN + 1)
#define ROOM_NAME_BUF_SIZE (MAX_ROOM_NAME_LEN + 1)
#define FILENAME_BUF_SIZE (MAX_FILENAME_LEN + 1)
#define MESSAGE_BUF_SIZE (MAX_MESSAGE_LEN) // Max length of content, null terminator handled by usage

typedef enum MessageType
{
    // Client to Server
    MSG_LOGIN,
    MSG_JOIN_ROOM,
    MSG_LEAVE_ROOM,
    MSG_BROADCAST,             // Client requests to broadcast to their current room
    MSG_WHISPER,               // Client requests to send private message
    MSG_FILE_TRANSFER_REQUEST, // Client initiates file transfer (sends metadata)
    MSG_DISCONNECT,            // Client explicitly disconnects

    // Server to Client
    MSG_LOGIN_SUCCESS,
    MSG_LOGIN_FAILURE,
    MSG_FILE_TRANSFER_DATA,   // Server sends file data (or notification of simulated transfer) to recipient
    MSG_FILE_TRANSFER_ACCEPT, // Server accepts the sender's file transfer request (e.g., added to queue)
    MSG_FILE_TRANSFER_REJECT, // Server rejects the sender's file transfer request
    MSG_ERROR,                // Generic error message from server
    MSG_SUCCESS,              // Generic success message from server (e.g., command processed)
    MSG_SERVER_NOTIFICATION,  // Server-initiated notifications (e.g., user joined/left room, server shutdown)

} MessageType;

// Message structure for communication
typedef struct Message
{
    MessageType type;
    char sender[USERNAME_BUF_SIZE];   // Username of the message sender
    char receiver[USERNAME_BUF_SIZE]; // Username of the message recipient (for whisper/file)
    char room[ROOM_NAME_BUF_SIZE];    // Room name (for join/broadcast)
    char content[MESSAGE_BUF_SIZE];   // Text content of the message
    char filename[FILENAME_BUF_SIZE]; // Filename for file transfers
    size_t file_size;                 // File size for file transfers
} Message;

#endif // PROTOCOL_H