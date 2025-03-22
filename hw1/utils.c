#include "utils.h"
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <string.h>
#include <errno.h>


/* Get current timestamp in format: [YYYY-MM-DD HH:MM:SS] */
void getCurrentTimestamp(char* timestamp) {
    time_t now = time(NULL);
    struct tm* timeinfo = localtime(&now);
    strftime(timestamp, TIMESTAMP_SIZE, "[%Y-%m-%d %H:%M:%S]", timeinfo);
}

/* Log operation to log file */
void logOperation(const char* message) {
    int fd = open(LOG_FILE, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd == -1) {
        const char* prefix = "Error opening log file: ";
        write(STDERR_FILENO, prefix, strlen(prefix));
        write(STDERR_FILENO, strerror(errno), strlen(strerror(errno)));
        write(STDERR_FILENO, "\n", 1);
        return;
    }

    char timestamp[TIMESTAMP_SIZE];
    getCurrentTimestamp(timestamp);

    write(fd, timestamp, strlen(timestamp));
    write(fd, " ", 1);
    write(fd, message, strlen(message));
    write(fd, "\n", 1);
    close(fd);
}

/* Display help */
void displayHelp(void) {
    char help_msg[] = "Usage: fileManager <command> [arguments]\n"
                      "Commands:\n"
                      "  createDir \"folderName\" - Create a new directory\n"
                      "  createFile \"fileName\" - Create a new file\n"
                      "  listDir \"folderName\" - List all files in a directory\n"
                      "  listFilesByExtension \"folderName\" \".txt\" - List files with specific extension\n"
                      "  readFile \"fileName\" - Read a file's content\n"
                      "  appendToFile \"fileName\" \"new content\" - Append content to a file\n"
                      "  deleteFile \"fileName\" - Delete a file\n"
                      "  deleteDir \"folderName\" - Delete an empty directory\n"
                      "  showLogs - Display operation logs\n";
    
    write(STDOUT_FILENO, help_msg, strlen(help_msg));
}
