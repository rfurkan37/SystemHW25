#include "utils.h"
#include <string.h>     // For strlen, strcmp, strrchr, memset
#include <ctype.h>      // For isalnum
#include <sys/socket.h> // For send, recv
#include <sys/stat.h>   // For stat
#include <unistd.h>     // For read, write (if used directly, not here)
#include <stdio.h>      // For perror (though generally avoided here for library-style functions)
#include <errno.h>      // For errno

// Sends a Message struct over the socket.
// Returns 1 if the entire message was sent, 0 otherwise.
int sendMessage(int socket_fd, const Message *msg)
{
    if (socket_fd < 0 || !msg)
    {
        return 0; // Invalid arguments
    }
    ssize_t bytes_sent = send(socket_fd, msg, sizeof(Message), 0);
    if (bytes_sent < 0)
    {
        // Caller can check errno if needed (e.g., EPIPE for broken pipe)
        return 0; // Send error
    }
    return (bytes_sent == sizeof(Message)) ? 1 : 0; // Success only if all bytes sent
}

// Receives a Message struct from the socket.
// Returns 1 if a complete message was received, 0 if connection closed or error.
int receiveMessage(int socket_fd, Message *msg)
{
    if (socket_fd < 0 || !msg)
    {
        return 0; // Invalid arguments
    }
    memset(msg, 0, sizeof(Message)); // Clear message struct before receiving
    ssize_t bytes_received = recv(socket_fd, msg, sizeof(Message), 0);

    if (bytes_received == sizeof(Message))
    {
        // Ensure null termination for all string fields for safety,
        // even if server/client is expected to send them null-terminated.
        msg->sender[USERNAME_BUF_SIZE - 1] = '\0';
        msg->receiver[USERNAME_BUF_SIZE - 1] = '\0';
        msg->room[ROOM_NAME_BUF_SIZE - 1] = '\0';
        msg->content[MESSAGE_BUF_SIZE - 1] = '\0';
        msg->filename[FILENAME_BUF_SIZE - 1] = '\0';
        return 1; // Successfully received a full message
    }
    else if (bytes_received == 0)
    {
        return 0; // Connection closed by peer
    }
    else
    {
        // Error (bytes_received < 0) or partial message (0 < bytes_received < sizeof(Message))
        // For this project, partial messages are treated as errors.
        return 0;
    }
}

// Validates username: 1 to MAX_USERNAME_LEN characters, alphanumeric.
int isValidUsername(const char *username)
{
    if (!username)
        return 0;
    size_t len = strlen(username);
    if (len == 0 || len > MAX_USERNAME_LEN)
    {
        return 0; // Invalid length
    }
    for (size_t i = 0; i < len; i++)
    {
        if (!isalnum((unsigned char)username[i]))
        {
            return 0; // Contains non-alphanumeric characters
        }
    }
    return 1; // Username is valid
}

// Validates room name: 1 to MAX_ROOM_NAME_LEN characters, alphanumeric.
int isValidRoomName(const char *room_name)
{
    if (!room_name)
        return 0;
    size_t len = strlen(room_name);
    if (len == 0 || len > MAX_ROOM_NAME_LEN)
    {
        return 0; // Invalid length
    }
    for (size_t i = 0; i < len; i++)
    {
        // As per PDF: "Room names must be alphanumeric... No spaces or special characters allowed."
        if (!isalnum((unsigned char)room_name[i]))
        {
            return 0; // Contains non-alphanumeric characters
        }
    }
    return 1; // Room name is valid
}

// Validates file type based on extension.
// Supported: .txt, .pdf, .jpg, .png
int isValidFileType(const char *filename)
{
    if (!filename)
        return 0;
    const char *dot = strrchr(filename, '.');
    if (!dot || dot == filename || *(dot + 1) == '\0') // No dot, dot is first char, or dot is last char (e.g. "file.")
        return 0;                                      // Invalid or no extension

    const char *valid_extensions[] = {".txt", ".pdf", ".jpg", ".png"};
    for (size_t i = 0; i < sizeof(valid_extensions) / sizeof(valid_extensions[0]); ++i)
    {
        // Project description implies case-sensitive comparison is sufficient.
        if (strcmp(dot, valid_extensions[i]) == 0)
        {
            return 1; // Found a valid extension
        }
    }
    return 0; // Extension not in the valid list
}

// Gets file size from a given path using stat.
// Returns file size as long, or -1 on error.
long getFileSizeFromPath(const char *filepath)
{
    if (!filepath)
        return -1;
    struct stat st;
    if (stat(filepath, &st) == -1)
    {
        // Caller can check errno (e.g., ENOENT for file not found)
        return -1; // stat failed
    }
    return (long)st.st_size; // Return file size
}