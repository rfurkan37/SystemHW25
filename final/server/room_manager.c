#include "common.h"

room_t* find_or_create_room(const char* room_name) {
    pthread_mutex_lock(&g_server_state->rooms_mutex);
    
    // Find existing room
    for (int i = 0; i < g_server_state->room_count; i++) {
        if (strcmp(g_server_state->rooms[i].name, room_name) == 0) {
            pthread_mutex_unlock(&g_server_state->rooms_mutex);
            return &g_server_state->rooms[i];
        }
    }
    
    // Create new room if space available
    if (g_server_state->room_count < MAX_CLIENTS) {
        room_t* new_room = &g_server_state->rooms[g_server_state->room_count++];
        strcpy(new_room->name, room_name);
        new_room->member_count = 0;
        memset(new_room->members, 0, sizeof(new_room->members));
        pthread_mutex_init(&new_room->room_mutex, NULL);
        
        pthread_mutex_unlock(&g_server_state->rooms_mutex);
        
        char log_msg[256];
        snprintf(log_msg, sizeof(log_msg), "Created new room: %s", room_name);
        log_message("ROOM", log_msg);
        
        return new_room;
    }
    
    pthread_mutex_unlock(&g_server_state->rooms_mutex);
    return NULL;
}

int join_room(client_t* client, const char* room_name) {
    // Validate room name
    if (!is_valid_room_name(room_name)) {
        return 0;
    }
    
    room_t* room = find_or_create_room(room_name);
    if (!room) {
        return 0;
    }
    
    pthread_mutex_lock(&room->room_mutex);
    
    // Check room capacity
    if (room->member_count >= MAX_MEMBERS_PER_ROOM) {
        pthread_mutex_unlock(&room->room_mutex);
        return 0;
    }
    
    // Remove from current room first
    if (strlen(client->current_room) > 0) {
        pthread_mutex_unlock(&room->room_mutex);
        leave_current_room(client);
        pthread_mutex_lock(&room->room_mutex);
    }
    
    // Check if already in this room
    for (int i = 0; i < room->member_count; i++) {
        if (room->members[i] == client) {
            pthread_mutex_unlock(&room->room_mutex);
            return 1; // Already in room
        }
    }
    
    // Add to new room
    room->members[room->member_count++] = client;
    strcpy(client->current_room, room_name);
    
    pthread_mutex_unlock(&room->room_mutex);
    
    log_room_join(client, room_name);
    return 1;
}

void leave_current_room(client_t* client) {
    if (strlen(client->current_room) == 0) {
        return; // Not in any room
    }
    
    pthread_mutex_lock(&g_server_state->rooms_mutex);
    
    // Find the room
    room_t* room = NULL;
    for (int i = 0; i < g_server_state->room_count; i++) {
        if (strcmp(g_server_state->rooms[i].name, client->current_room) == 0) {
            room = &g_server_state->rooms[i];
            break;
        }
    }
    
    pthread_mutex_unlock(&g_server_state->rooms_mutex);
    
    if (!room) {
        return;
    }
    
    pthread_mutex_lock(&room->room_mutex);
    
    // Remove client from room
    for (int i = 0; i < room->member_count; i++) {
        if (room->members[i] == client) {
            // Shift remaining members
            for (int j = i; j < room->member_count - 1; j++) {
                room->members[j] = room->members[j + 1];
            }
            room->member_count--;
            break;
        }
    }
    
    pthread_mutex_unlock(&room->room_mutex);
    
    char log_msg[256];
    snprintf(log_msg, sizeof(log_msg), "User '%s' left room '%s'", 
             client->username, client->current_room);
    log_message("ROOM", log_msg);
    
    memset(client->current_room, 0, sizeof(client->current_room));
}

void remove_client_from_room(client_t* client) {
    leave_current_room(client);
}

void handle_join_room(client_t* client, const char* room_name) {
    if (join_room(client, room_name)) {
        // Send success message with room name
        message_t success_msg;
        memset(&success_msg, 0, sizeof(success_msg));
        success_msg.type = MSG_SUCCESS;
        strcpy(success_msg.sender, "SERVER");
        strcpy(success_msg.room, room_name);  // Include room name for client to update
        strcpy(success_msg.content, "Joined room successfully");
        send_message(client->socket_fd, &success_msg);
        
        // Notify other room members
        message_t notification;
        memset(&notification, 0, sizeof(notification));
        notification.type = MSG_SUCCESS;
        strcpy(notification.sender, "SERVER");
        strcpy(notification.room, room_name);
        snprintf(notification.content, sizeof(notification.content), 
                "%s joined the room", client->username);
        
        broadcast_to_room(room_name, &notification, client);
    } else {
        send_error_message(client->socket_fd, "Failed to join room");
    }
}

void handle_leave_room(client_t* client) {
    if (strlen(client->current_room) == 0) {
        send_error_message(client->socket_fd, "Not in any room");
        return;
    }
    
    char room_name[MAX_ROOM_NAME_LEN + 1];
    strcpy(room_name, client->current_room);
    
    leave_current_room(client);
    send_success_message(client->socket_fd, "Left room successfully");
    
    // Notify other room members
    message_t notification;
    memset(&notification, 0, sizeof(notification));
    notification.type = MSG_SUCCESS;
    strcpy(notification.sender, "SERVER");
    strcpy(notification.room, room_name);
    snprintf(notification.content, sizeof(notification.content), 
            "%s left the room", client->username);
    
    broadcast_to_room(room_name, &notification, NULL);
}

void handle_broadcast(client_t* client, const char* message) {
    if (strlen(client->current_room) == 0) {
        send_error_message(client->socket_fd, "Must join a room first");
        return;
    }
    
    message_t broadcast_msg;
    memset(&broadcast_msg, 0, sizeof(broadcast_msg));
    broadcast_msg.type = MSG_BROADCAST;
    strcpy(broadcast_msg.sender, client->username);
    strcpy(broadcast_msg.room, client->current_room);
    strcpy(broadcast_msg.content, message);
    
    broadcast_to_room(client->current_room, &broadcast_msg, client);
    
    char log_msg[512];
    snprintf(log_msg, sizeof(log_msg), "Broadcast from '%s' in room '%s': %s", 
             client->username, client->current_room, message);
    log_message("BROADCAST", log_msg);
}

void handle_whisper(client_t* client, const char* receiver, const char* message) {
    client_t* target = find_client_by_username(receiver);
    if (!target) {
        send_error_message(client->socket_fd, "User not found");
        return;
    }
    
    message_t whisper_msg;
    memset(&whisper_msg, 0, sizeof(whisper_msg));
    whisper_msg.type = MSG_WHISPER;
    strcpy(whisper_msg.sender, client->username);
    strcpy(whisper_msg.receiver, receiver);
    strcpy(whisper_msg.content, message);
    
    if (send_message(target->socket_fd, &whisper_msg) > 0) {
        send_success_message(client->socket_fd, "Whisper sent");
        
        char log_msg[512];
        snprintf(log_msg, sizeof(log_msg), "Whisper from '%s' to '%s': %s", 
                 client->username, receiver, message);
        log_message("WHISPER", log_msg);
    } else {
        send_error_message(client->socket_fd, "Failed to send whisper");
    }
}

void broadcast_to_room(const char* room_name, message_t* msg, client_t* sender) {
    pthread_mutex_lock(&g_server_state->rooms_mutex);
    
    room_t* room = NULL;
    for (int i = 0; i < g_server_state->room_count; i++) {
        if (strcmp(g_server_state->rooms[i].name, room_name) == 0) {
            room = &g_server_state->rooms[i];
            break;
        }
    }
    
    pthread_mutex_unlock(&g_server_state->rooms_mutex);
    
    if (!room) {
        return;
    }
    
    pthread_mutex_lock(&room->room_mutex);
    
    for (int i = 0; i < room->member_count; i++) {
        if (room->members[i] && room->members[i] != sender) {
            send_message(room->members[i]->socket_fd, msg);
        }
    }
    
    pthread_mutex_unlock(&room->room_mutex);
} 