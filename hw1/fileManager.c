#include "fileManager.h"
#include "utils.h" // Include utils.h for custom functions
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/file.h>
#include <dirent.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <time.h>

#define BUFFER_SIZE 4096
#define TIMESTAMP_SIZE 32
#define LOG_FILE "log.txt"

/* Create a directory */
void createDirectory(const char *dirName)
{
    struct stat st = {0};
    if (stat(dirName, &st) == -1)
    {
        if (mkdir(dirName, 0755) == -1)
        {
            char error_msg[100] = "Error creating directory: ";
            strcat(error_msg, strerror(errno));
            write(STDERR_FILENO, error_msg, strlen(error_msg));
            write(STDERR_FILENO, "\n", 1);
            return;
        }

        char log_msg[BUFFER_SIZE] = "Directory \"";
        strcat(log_msg, dirName);
        strcat(log_msg, "\" created successfully.");
        logOperation(log_msg);

        write(STDOUT_FILENO, log_msg, strlen(log_msg));
        write(STDOUT_FILENO, "\n", 1);
    }
    else
    {
        char error_msg[BUFFER_SIZE] = "Error: Directory \"";
        strcat(error_msg, dirName);
        strcat(error_msg, "\" already exists.");
        write(STDERR_FILENO, error_msg, strlen(error_msg));
        write(STDERR_FILENO, "\n", 1);
    }
}

/* Create a file */
void createFile(const char *fileName)
{
    struct stat st = {0};
    if (stat(fileName, &st) == -1)
    {
        int fd = open(fileName, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd == -1)
        {
            char error_msg[100] = "Error creating file: ";
            strcat(error_msg, strerror(errno));
            write(STDERR_FILENO, error_msg, strlen(error_msg));
            write(STDERR_FILENO, "\n", 1);
            return;
        }

        char timestamp[TIMESTAMP_SIZE];
        getCurrentTimestamp(timestamp);
        write(fd, timestamp, strlen(timestamp));
        write(fd, " File created\n", 14);

        close(fd);

        char log_msg[BUFFER_SIZE] = "File \"";
        strcat(log_msg, fileName);
        strcat(log_msg, "\" created successfully.");
        logOperation(log_msg);

        write(STDOUT_FILENO, log_msg, strlen(log_msg));
        write(STDOUT_FILENO, "\n", 1);
    }
    else
    {
        char error_msg[BUFFER_SIZE] = "Error: File \"";
        strcat(error_msg, fileName);
        strcat(error_msg, "\" already exists.");
        write(STDERR_FILENO, error_msg, strlen(error_msg));
        write(STDERR_FILENO, "\n", 1);
    }
}

/* List files in a directory */
void listDirectory(const char *dirName)
{
    struct stat st = {0};
    if (stat(dirName, &st) == -1)
    {
        char error_msg[BUFFER_SIZE] = "Error: Directory \"";
        strcat(error_msg, dirName);
        strcat(error_msg, "\" not found.");
        write(STDERR_FILENO, error_msg, strlen(error_msg));
        write(STDERR_FILENO, "\n", 1);
        return;
    }

    pid_t pid = fork();
    if (pid == -1)
    {
        char error_msg[100] = "Error forking process: ";
        strcat(error_msg, strerror(errno));
        write(STDERR_FILENO, error_msg, strlen(error_msg));
        write(STDERR_FILENO, "\n", 1);
        return;
    }

    if (pid == 0)
    { // Child process
        DIR *dir = opendir(dirName);
        if (dir == NULL)
        {
            char error_msg[100] = "Error opening directory: ";
            strcat(error_msg, strerror(errno));
            write(STDERR_FILENO, error_msg, strlen(error_msg));
            write(STDERR_FILENO, "\n", 1);
            exit(EXIT_FAILURE);
        }

        struct dirent *entry;
        char message[BUFFER_SIZE] = "Contents of directory \"";
        strcat(message, dirName);
        strcat(message, "\":");
        write(STDOUT_FILENO, message, strlen(message));
        write(STDOUT_FILENO, "\n", 1);

        while ((entry = readdir(dir)) != NULL)
        {
            char path[BUFFER_SIZE];
            strcpy(path, dirName);
            strcat(path, "/");
            strcat(path, entry->d_name);

            struct stat path_stat;
            stat(path, &path_stat);

            char type[10];
            if (S_ISDIR(path_stat.st_mode))
            {
                strcpy(type, "Directory");
            }
            else if (S_ISREG(path_stat.st_mode))
            {
                strcpy(type, "File");
            }
            else
            {
                strcpy(type, "Other");
            }

            char entry_msg[BUFFER_SIZE] = "  ";
            strcat(entry_msg, entry->d_name);
            strcat(entry_msg, " [");
            strcat(entry_msg, type);
            strcat(entry_msg, "]");
            write(STDOUT_FILENO, entry_msg, strlen(entry_msg));
            write(STDOUT_FILENO, "\n", 1);
        }

        closedir(dir);
        exit(EXIT_SUCCESS);
    }
    else
    { // Parent process
        int status;
        waitpid(pid, &status, 0);

        char log_msg[BUFFER_SIZE] = "Directory \"";
        strcat(log_msg, dirName);
        strcat(log_msg, "\" listed successfully.");
        logOperation(log_msg);
    }
}

/* List files with specific extension */
void listFilesByExtension(const char *dirName, const char *extension)
{
    struct stat st = {0};
    if (stat(dirName, &st) == -1)
    {
        char error_msg[BUFFER_SIZE] = "Error: Directory \"";
        strcat(error_msg, dirName);
        strcat(error_msg, "\" not found.");
        write(STDERR_FILENO, error_msg, strlen(error_msg));
        write(STDERR_FILENO, "\n", 1);
        return;
    }

    pid_t pid = fork();
    if (pid == -1)
    {
        char error_msg[100] = "Error forking process: ";
        strcat(error_msg, strerror(errno));
        write(STDERR_FILENO, error_msg, strlen(error_msg));
        write(STDERR_FILENO, "\n", 1);
        return;
    }

    if (pid == 0)
    { // Child process
        DIR *dir = opendir(dirName);
        if (dir == NULL)
        {
            char error_msg[100] = "Error opening directory: ";
            strcat(error_msg, strerror(errno));
            write(STDERR_FILENO, error_msg, strlen(error_msg));
            write(STDERR_FILENO, "\n", 1);
            exit(EXIT_FAILURE);
        }

        struct dirent *entry;
        int found = 0;
        char message[BUFFER_SIZE] = "Files with extension \"";
        strcat(message, extension);
        strcat(message, "\" in directory \"");
        strcat(message, dirName);
        strcat(message, "\":");
        write(STDOUT_FILENO, message, strlen(message));
        write(STDOUT_FILENO, "\n", 1);

        while ((entry = readdir(dir)) != NULL)
        {
            char *file_ext = strrchr(entry->d_name, '.');
            if (file_ext != NULL && strcmp(file_ext, extension) == 0)
            {
                found = 1;
                char path[BUFFER_SIZE];
                strcpy(path, dirName);
                strcat(path, "/");
                strcat(path, entry->d_name);

                struct stat path_stat;
                stat(path, &path_stat);

                char entry_msg[BUFFER_SIZE] = "  ";
                strcat(entry_msg, entry->d_name);
                write(STDOUT_FILENO, entry_msg, strlen(entry_msg));
                write(STDOUT_FILENO, "\n", 1);
            }
        }

        if (!found)
        {
            char notfound_msg[BUFFER_SIZE] = "No files with extension \"";
            strcat(notfound_msg, extension);
            strcat(notfound_msg, "\" found in \"");
            strcat(notfound_msg, dirName);
            strcat(notfound_msg, "\".");
            write(STDOUT_FILENO, notfound_msg, strlen(notfound_msg));
            write(STDOUT_FILENO, "\n", 1);
        }

        closedir(dir);
        exit(EXIT_SUCCESS);
    }
    else
    { // Parent process
        int status;
        waitpid(pid, &status, 0);

        char log_msg[BUFFER_SIZE] = "Listed files with extension \"";
        strcat(log_msg, extension);
        strcat(log_msg, "\" in directory \"");
        strcat(log_msg, dirName);
        strcat(log_msg, "\".");
        logOperation(log_msg);
    }
}

/* Read file content with simplified locking using flock() */
void readFile(const char *fileName)
{
    struct stat st = {0};
    if (stat(fileName, &st) == -1)
    {
        char error_msg[BUFFER_SIZE] = "Error: File \"";
        strcat(error_msg, fileName);
        strcat(error_msg, "\" not found.");
        write(STDERR_FILENO, error_msg, strlen(error_msg));
        write(STDERR_FILENO, "\n", 1);
        return;
    }

    int fd = open(fileName, O_RDONLY);
    if (fd == -1)
    {
        char error_msg[100] = "Error opening file: ";
        strcat(error_msg, strerror(errno));
        write(STDERR_FILENO, error_msg, strlen(error_msg));
        write(STDERR_FILENO, "\n", 1);
        return;
    }

    // Apply a shared lock (allows other reads but not writes)
    if (flock(fd, LOCK_SH) == -1)
    {
        char error_msg[BUFFER_SIZE] = "Error: Cannot read \"";
        strcat(error_msg, fileName);
        strcat(error_msg, "\". File is locked for writing.");
        write(STDERR_FILENO, error_msg, strlen(error_msg));
        write(STDERR_FILENO, "\n", 1);
        close(fd);
        return;
    }

    char buffer[BUFFER_SIZE];
    ssize_t bytes_read;

    char message[BUFFER_SIZE] = "Contents of file \"";
    strcat(message, fileName);
    strcat(message, "\":");
    write(STDOUT_FILENO, message, strlen(message));
    write(STDOUT_FILENO, "\n", 1);

    while ((bytes_read = read(fd, buffer, BUFFER_SIZE)) > 0)
    {
        write(STDOUT_FILENO, buffer, bytes_read);
    }

    write(STDOUT_FILENO, "\n", 1);

    // Release the lock
    flock(fd, LOCK_UN);
    close(fd);

    char log_msg[BUFFER_SIZE] = "File \"";
    strcat(log_msg, fileName);
    strcat(log_msg, "\" read successfully.");
    logOperation(log_msg);
}

/* Append content to file with simplified locking using flock() */
void appendToFile(const char *fileName, const char *content)
{
    struct stat st = {0};
    if (stat(fileName, &st) == -1)
    {
        char error_msg[BUFFER_SIZE] = "Error: File \"";
        strcat(error_msg, fileName);
        strcat(error_msg, "\" not found.");
        write(STDERR_FILENO, error_msg, strlen(error_msg));
        write(STDERR_FILENO, "\n", 1);
        return;
    }

    int fd = open(fileName, O_WRONLY | O_APPEND);
    if (fd == -1)
    {
        char error_msg[BUFFER_SIZE] = "Error: Cannot write to \"";
        strcat(error_msg, fileName);
        strcat(error_msg, "\". File is locked or read-only.");
        write(STDERR_FILENO, error_msg, strlen(error_msg));
        write(STDERR_FILENO, "\n", 1);
        return;
    }

    // Apply an exclusive lock (prevents all other access)
    if (flock(fd, LOCK_EX) == -1)
    {
        char error_msg[BUFFER_SIZE] = "Error: Cannot write to \"";
        strcat(error_msg, fileName);
        strcat(error_msg, "\". File is currently being accessed by another process.");
        write(STDERR_FILENO, error_msg, strlen(error_msg));
        write(STDERR_FILENO, "\n", 1);
        close(fd);
        return;
    }

    write(fd, "\n", 1);
    write(fd, content, strlen(content));

    // Release the lock
    flock(fd, LOCK_UN);
    close(fd);

    char log_msg[BUFFER_SIZE] = "Content appended to file \"";
    strcat(log_msg, fileName);
    strcat(log_msg, "\" successfully.");
    logOperation(log_msg);

    write(STDOUT_FILENO, log_msg, strlen(log_msg));
    write(STDOUT_FILENO, "\n", 1);
}

/* Delete a file */
void deleteFile(const char *fileName)
{
    struct stat st = {0};
    if (stat(fileName, &st) == -1)
    {
        char error_msg[BUFFER_SIZE] = "Error: File \"";
        strcat(error_msg, fileName);
        strcat(error_msg, "\" not found.");
        write(STDERR_FILENO, error_msg, strlen(error_msg));
        write(STDERR_FILENO, "\n", 1);
        return;
    }

    pid_t pid = fork();
    if (pid == -1)
    {
        char error_msg[100] = "Error forking process: ";
        strcat(error_msg, strerror(errno));
        write(STDERR_FILENO, error_msg, strlen(error_msg));
        write(STDERR_FILENO, "\n", 1);
        return;
    }

    if (pid == 0)
    { // Child process
        if (unlink(fileName) == -1)
        {
            char error_msg[100] = "Error deleting file: ";
            strcat(error_msg, strerror(errno));
            write(STDERR_FILENO, error_msg, strlen(error_msg));
            write(STDERR_FILENO, "\n", 1);
            exit(EXIT_FAILURE);
        }

        char success_msg[BUFFER_SIZE] = "File \"";
        strcat(success_msg, fileName);
        strcat(success_msg, "\" deleted successfully.");
        write(STDOUT_FILENO, success_msg, strlen(success_msg));
        write(STDOUT_FILENO, "\n", 1);

        exit(EXIT_SUCCESS);
    }
    else
    { // Parent process
        int status;
        waitpid(pid, &status, 0);

        if (WIFEXITED(status) && WEXITSTATUS(status) == EXIT_SUCCESS)
        {
            char log_msg[BUFFER_SIZE] = "File \"";
            strcat(log_msg, fileName);
            strcat(log_msg, "\" deleted successfully.");
            logOperation(log_msg);
        }
    }
}

/* Delete a directory */
void deleteDirectory(const char *dirName)
{
    struct stat st = {0};
    if (stat(dirName, &st) == -1)
    {
        char error_msg[BUFFER_SIZE] = "Error: Directory \"";
        strcat(error_msg, dirName);
        strcat(error_msg, "\" not found.");
        write(STDERR_FILENO, error_msg, strlen(error_msg));
        write(STDERR_FILENO, "\n", 1);
        return;
    }

    pid_t pid = fork();
    if (pid == -1)
    {
        char error_msg[100] = "Error forking process: ";
        strcat(error_msg, strerror(errno));
        write(STDERR_FILENO, error_msg, strlen(error_msg));
        write(STDERR_FILENO, "\n", 1);
        return;
    }

    if (pid == 0)
    { // Child process
        DIR *dir = opendir(dirName);
        if (dir == NULL)
        {
            char error_msg[100] = "Error opening directory: ";
            strcat(error_msg, strerror(errno));
            write(STDERR_FILENO, error_msg, strlen(error_msg));
            write(STDERR_FILENO, "\n", 1);
            exit(EXIT_FAILURE);
        }

        struct dirent *entry;
        int is_empty = 1;

        while ((entry = readdir(dir)) != NULL)
        {
            if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0)
            {
                is_empty = 0;
                break;
            }
        }

        closedir(dir);

        if (!is_empty)
        {
            char error_msg[BUFFER_SIZE] = "Error: Directory \"";
            strcat(error_msg, dirName);
            strcat(error_msg, "\" is not empty.");
            write(STDERR_FILENO, error_msg, strlen(error_msg));
            write(STDERR_FILENO, "\n", 1);
            exit(EXIT_FAILURE);
        }

        if (rmdir(dirName) == -1)
        {
            char error_msg[100] = "Error deleting directory: ";
            strcat(error_msg, strerror(errno));
            write(STDERR_FILENO, error_msg, strlen(error_msg));
            write(STDERR_FILENO, "\n", 1);
            exit(EXIT_FAILURE);
        }

        char success_msg[BUFFER_SIZE] = "Directory \"";
        strcat(success_msg, dirName);
        strcat(success_msg, "\" deleted successfully.");
        write(STDOUT_FILENO, success_msg, strlen(success_msg));
        write(STDOUT_FILENO, "\n", 1);

        exit(EXIT_SUCCESS);
    }
    else
    { // Parent process
        int status;
        waitpid(pid, &status, 0);

        if (WIFEXITED(status) && WEXITSTATUS(status) == EXIT_SUCCESS)
        {
            char log_msg[BUFFER_SIZE] = "Directory \"";
            strcat(log_msg, dirName);
            strcat(log_msg, "\" deleted successfully.");
            logOperation(log_msg);
        }
    }
}

/* Show log file */
void showLogs(void)
{
    struct stat st = {0};
    if (stat(LOG_FILE, &st) == -1)
    {
        char error_msg[BUFFER_SIZE] = "Error: Log file not found.";
        write(STDERR_FILENO, error_msg, strlen(error_msg));
        write(STDERR_FILENO, "\n", 1);
        return;
    }

    int fd = open(LOG_FILE, O_RDONLY);
    if (fd == -1)
    {
        char error_msg[100] = "Error opening log file: ";
        strcat(error_msg, strerror(errno));
        write(STDERR_FILENO, error_msg, strlen(error_msg));
        write(STDERR_FILENO, "\n", 1);
        return;
    }

    char buffer[BUFFER_SIZE];
    ssize_t bytes_read;

    char message[] = "Operation logs:\n";
    write(STDOUT_FILENO, message, strlen(message));

    while ((bytes_read = read(fd, buffer, BUFFER_SIZE)) > 0)
    {
        write(STDOUT_FILENO, buffer, bytes_read);
    }

    close(fd);

    char log_msg[BUFFER_SIZE] = "Logs displayed successfully.";
    logOperation(log_msg);
}