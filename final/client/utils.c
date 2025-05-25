#include "common.h"

void print_help(void) {
    printf("\033[36m=== Available Commands ===\033[0m\n");
    printf("\033[33m/join <room_name>\033[0m     - Join a chat room\n");
    printf("\033[33m/leave\033[0m                - Leave current room\n");
    printf("\033[33m/broadcast <message>\033[0m  - Send message to current room\n");
    printf("\033[33m/whisper <user> <msg>\033[0m - Send private message\n");
    printf("\033[33m/sendfile <user> <file>\033[0m - Send file to user\n");
    printf("\033[33m/help\033[0m                 - Show this help\n");
    printf("\033[33m/exit\033[0m                 - Disconnect from server\n");
    printf("\033[36m========================\033[0m\n");
    printf("\033[32mSupported file types: .txt, .pdf, .jpg, .png (max 3MB)\033[0m\n");
}

void print_colored_message(const char* type, const char* sender, const char* content) {
    if (strcmp(type, "BROADCAST") == 0) {
        printf("\033[36m[BROADCAST] %s: %s\033[0m\n", sender, content);
    } else if (strcmp(type, "WHISPER") == 0) {
        printf("\033[35m[WHISPER from %s] %s\033[0m\n", sender, content);
    } else {
        printf("[%s] %s: %s\n", type, sender, content);
    }
}

int is_valid_username(const char* username) {
    if (!username || strlen(username) == 0 || strlen(username) > MAX_USERNAME_LEN) {
        return 0;
    }
    
    for (int i = 0; username[i]; i++) {
        if (!isalnum(username[i])) {
            return 0;
        }
    }
    return 1;
}

int is_valid_room_name(const char* room_name) {
    if (!room_name || strlen(room_name) == 0 || strlen(room_name) > MAX_ROOM_NAME_LEN) {
        return 0;
    }
    
    for (int i = 0; room_name[i]; i++) {
        if (!isalnum(room_name[i])) {
            return 0;
        }
    }
    return 1;
}

int is_valid_file_type(const char* filename) {
    if (!filename) return 0;
    
    const char* ext = strrchr(filename, '.');
    if (!ext) return 0;
    
    return (strcmp(ext, ".txt") == 0 || strcmp(ext, ".pdf") == 0 ||
            strcmp(ext, ".jpg") == 0 || strcmp(ext, ".png") == 0);
}

long get_file_size(FILE* file) {
    if (!file) return -1;
    
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    
    return size;
} 