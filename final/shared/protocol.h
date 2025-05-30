#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stddef.h> // For size_t
#include <time.h>   // For time_t

// Maximum limits
#define MAX_USERNAME_LEN 16
#define MAX_ROOM_NAME_LEN 32
#define MAX_MESSAGE_LEN 1024
#define MAX_FILENAME_LEN 256
#define MAX_FILE_SIZE (3 * 1024 * 1024) // 3MB

// Buffer sizes (including null terminator)
#define USERNAME_BUF_SIZE (MAX_USERNAME_LEN + 1)
#define ROOM_NAME_BUF_SIZE (MAX_ROOM_NAME_LEN + 1)
#define FILENAME_BUF_SIZE (MAX_FILENAME_LEN + 1) // Max filename length is 255 + null
#define MESSAGE_BUF_SIZE (MAX_MESSAGE_LEN)       // Max message content length is 1023 + null

typedef enum
{
    MSG_LOGIN,
    MSG_LOGIN_SUCCESS, // Specific success for login differentiation if needed by client
    MSG_LOGIN_FAILURE, // Specific failure for login
    MSG_JOIN_ROOM,
    MSG_LEAVE_ROOM,
    MSG_BROADCAST,
    MSG_WHISPER,
    MSG_FILE_TRANSFER_REQUEST, // Client sends this to initiate
    MSG_FILE_TRANSFER_DATA,    // Server sends this header to recipient, then raw data
    MSG_FILE_TRANSFER_ACCEPT,  // Server sends to sender: "added to queue" / "wait"
    MSG_FILE_TRANSFER_REJECT,  // Server sends to sender: "oversized", "queue full", "user offline"
    MSG_DISCONNECT,
    MSG_ERROR,              // General server error/info to client
    MSG_SUCCESS,            // General server success/info to client
    MSG_SERVER_NOTIFICATION // For server-initiated messages like "user X joined"
} message_type_t;

// Message structure
typedef struct
{
    message_type_t type;
    char sender[USERNAME_BUF_SIZE];
    char receiver[USERNAME_BUF_SIZE];
    char room[ROOM_NAME_BUF_SIZE];
    char content[MESSAGE_BUF_SIZE];
    char filename[FILENAME_BUF_SIZE];
    size_t file_size;
    // No actual file data in this struct; it's sent separately after a header.
} message_t;

#endif // PROTOCOL_H