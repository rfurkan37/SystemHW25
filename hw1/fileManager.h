#ifndef FILE_MANAGER_H
#define FILE_MANAGER_H

#include "utils.h"  // Include utils.h for utility functions

#define BUFFER_SIZE 4096
#define TIMESTAMP_SIZE 32
#define LOG_FILE "log.txt"

/* Function prototypes */
void createDirectory(const char* dirName);
void createFile(const char* fileName);
void listDirectory(const char* dirName);
void listFilesByExtension(const char* dirName, const char* extension);
void readFile(const char* fileName);
void appendToFile(const char* fileName, const char* content);
void deleteFile(const char* fileName);
void deleteDirectory(const char* dirName);
void showLogs(void);
void displayHelp(void);
void logOperation(const char* message);
void getCurrentTimestamp(char* timestamp);

#endif /* FILE_MANAGER_H */
