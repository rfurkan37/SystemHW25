#ifndef UTILS_H
#define UTILS_H

#ifndef NULL
#define NULL ((void*)0)
#endif

/* Get current timestamp and log functions */
void getCurrentTimestamp(char* timestamp);
void logOperation(const char* message);
void displayHelp(void);

#define BUFFER_SIZE 4096
#define TIMESTAMP_SIZE 32
#define LOG_FILE "log.txt"

#endif /* UTILS_H */