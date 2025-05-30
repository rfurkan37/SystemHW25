#include "utils.h"
#include <string.h>
#include <ctype.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h> // For lseek, access
#include <fcntl.h>  // For open
#include <stdio.h>  // For perror (though errors often handled by caller)
#include <errno.h>  // For errno

int send_message(int socket_fd, const message_t *msg)
{
    if (socket_fd < 0)
        return 0;
    ssize_t bytes_sent = send(socket_fd, msg, sizeof(message_t), 0);
    if (bytes_sent < 0)
    {
        // Non-critical error for send, let caller handle if it's a disconnect
        // perror("send_message failed");
        return 0;
    }
    return (bytes_sent == sizeof(message_t)) ? 1 : 0;
}

int receive_message(int socket_fd, message_t *msg)
{
    if (socket_fd < 0)
        return 0;
    memset(msg, 0, sizeof(message_t)); // Clear message struct before receiving
    ssize_t bytes_received = recv(socket_fd, msg, sizeof(message_t), 0);

    if (bytes_received == sizeof(message_t))
    {
        // Ensure all string fields are null-terminated for safety,
        // even if server/client sending is trusted.
        msg->sender[MAX_USERNAME_LEN] = '\0';
        msg->receiver[MAX_USERNAME_LEN] = '\0';
        msg->room[MAX_ROOM_NAME_LEN] = '\0';
        msg->content[MESSAGE_BUF_SIZE - 1] = '\0';
        msg->filename[FILENAME_BUF_SIZE - 1] = '\0';
        return 1;
    }
    if (bytes_received < 0)
    {
        // Non-critical error for recv, let caller handle if it's a disconnect
        // perror("receive_message failed");
    }
    // If bytes_received is 0, it's a graceful shutdown by peer.
    // If it's > 0 but not sizeof(message_t), it's a partial read (problematic for this simple protocol)
    return 0; // Indicate failure or closed connection
}

int is_valid_username(const char *username)
{
    if (!username || strlen(username) == 0 || strlen(username) > MAX_USERNAME_LEN)
    {
        return 0;
    }
    for (size_t i = 0; i < strlen(username); i++)
    {
        if (!isalnum((unsigned char)username[i])) // Cast to unsigned char for isalnum
        {
            return 0;
        }
    }
    return 1;
}

int is_valid_room_name(const char *room_name)
{
    if (!room_name || strlen(room_name) == 0 || strlen(room_name) > MAX_ROOM_NAME_LEN)
    {
        return 0;
    }
    for (size_t i = 0; i < strlen(room_name); i++)
    {
        // PDF: "No spaces or special characters allowed." isalnum covers this.
        if (!isalnum((unsigned char)room_name[i]))
        {
            return 0;
        }
    }
    return 1;
}

int is_valid_file_type(const char *filename)
{
    if (!filename)
        return 0;

    const char *ext = strrchr(filename, '.');
    if (!ext || ext == filename) // No extension or starts with '.' (hidden file)
        return 0;

    // PDF: .txt, .pdf, .jpg, .png
    if (strcmp(ext, ".txt") == 0)
        return 1;
    if (strcmp(ext, ".pdf") == 0)
        return 1;
    if (strcmp(ext, ".jpg") == 0)
        return 1;
    if (strcmp(ext, ".png") == 0)
        return 1;

    return 0;
}

long get_file_size_from_fd(int fd)
{
    if (fd < 0)
        return -1;
    struct stat st;
    if (fstat(fd, &st) == -1)
    {
        perror("fstat failed to get file size");
        return -1;
    }
    return (long)st.st_size;
}

long get_file_size_from_path(const char *filepath)
{
    if (!filepath)
        return -1;
    struct stat st;
    if (stat(filepath, &st) == -1)
    {
        // This can happen if file doesn't exist, not always an error to print perror
        // perror("stat failed to get file size");
        return -1;
    }
    return (long)st.st_size;
}