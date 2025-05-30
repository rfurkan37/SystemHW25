#include "common.h"

void initialize_room_system() {
    pthread_mutex_lock(&g_server_state->rooms_list_mutex);
    for (int i = 0; i < MAX_ROOMS; ++i) {
        memset(&g_server_state->chat_rooms[i], 0, sizeof(chat_room_t));
        // Initialize mutex for each potential room structure.
        // Alternatively, initialize only when room is actually created.
        // For simplicity and fixed array, initializing all upfront is okay.
        if (pthread_mutex_init(&g_server_state->chat_rooms[i].room_lock, NULL) != 0) {
            log_server_event("CRITICAL", "Failed to initialize mutex for room slot %d", i);
            // This is a critical failure, server might not be able to run safely.
            // Consider exiting or a more robust error handling.
        }
    }
    g_server_state->current_room_count = 0;
    pthread_mutex_unlock(&g_server_state->rooms_list_mutex);
    log_server_event("INFO", "Room management system initialized.");
}

// Finds an existing room or creates a new one if possible.
// Returns pointer to the room, or NULL if not found and cannot create.
chat_room_t* find_or_create_chat_room(const char *room_name_to_find) {
    if (!is_valid_room_name(room_name_to_find)) {
        return NULL; // Invalid name
    }

    pthread_mutex_lock(&g_server_state->rooms_list_mutex); // Lock for accessing/modifying rooms_list

    // Try to find existing room
    for (int i = 0; i < g_server_state->current_room_count; ++i) {
        if (strcmp(g_server_state->chat_rooms[i].name, room_name_to_find) == 0) {
            pthread_mutex_unlock(&g_server_state->rooms_list_mutex);
            return &g_server_state->chat_rooms[i]; // Found
        }
    }

    // If not found, try to create if space available
    if (g_server_state->current_room_count < MAX_ROOMS) {
        chat_room_t *new_room = &g_server_state->chat_rooms[g_server_state->current_room_count];
        // The room_lock for this slot should already be initialized by initialize_room_system

        strncpy(new_room->name, room_name_to_find, ROOM_NAME_BUF_SIZE - 1);
        new_room->member_count = 0; 
        // new_room->members array is already zeroed out or will be managed.
        
        g_server_state->current_room_count++;
        pthread_mutex_unlock(&g_server_state->rooms_list_mutex);
        
        log_event_room_created(new_room->name);
        return new_room;
    }

    pthread_mutex_unlock(&g_server_state->rooms_list_mutex);
    log_server_event("WARNING", "Could not create room '%s': Maximum room limit reached.", room_name_to_find);
    return NULL; // Max rooms reached
}

// Adds a client to a given room. Assumes room pointer is valid.
// Returns 1 on success, 0 on failure (e.g., room full).
int add_client_to_room(client_info_t *client, chat_room_t *room) {
    if (!client || !room) return 0;

    pthread_mutex_lock(&room->room_lock); // Lock specific room

    if (room->member_count >= MAX_MEMBERS_PER_ROOM) {
        pthread_mutex_unlock(&room->room_lock);
        log_server_event("INFO", "Client %s failed to join room '%s': Room is full.", client->username, room->name);
        return 0; // Room is full
    }

    // Check if client is already in this room (should not happen if logic is correct elsewhere)
    for (int i = 0; i < room->member_count; ++i) {
        if (room->members[i] == client) {
            pthread_mutex_unlock(&room->room_lock);
            return 1; // Already a member
        }
    }

    // Add client to room
    room->members[room->member_count] = client;
    room->member_count++;
    
    pthread_mutex_unlock(&room->room_lock);
    
    // Update client's state
    strncpy(client->current_room_name, room->name, ROOM_NAME_BUF_SIZE - 1);
    return 1; // Success
}

// Removes a client from the room they are currently in.
void remove_client_from_their_room(client_info_t *client) {
    if (!client || strlen(client->current_room_name) == 0) {
        return; // Client is not in any room or invalid client
    }

    // Find the room first (needs rooms_list_mutex to safely iterate g_server_state->chat_rooms)
    chat_room_t *room_to_leave = NULL;
    pthread_mutex_lock(&g_server_state->rooms_list_mutex);
    for (int i = 0; i < g_server_state->current_room_count; ++i) {
        if (strcmp(g_server_state->chat_rooms[i].name, client->current_room_name) == 0) {
            room_to_leave = &g_server_state->chat_rooms[i];
            break;
        }
    }
    pthread_mutex_unlock(&g_server_state->rooms_list_mutex);

    if (!room_to_leave) {
        log_server_event("WARNING", "Client %s attempting to leave non-existent room '%s'.", client->username, client->current_room_name);
        memset(client->current_room_name, 0, ROOM_NAME_BUF_SIZE); // Clear client's room state anyway
        return;
    }

    pthread_mutex_lock(&room_to_leave->room_lock); // Lock the specific room
    int found_idx = -1;
    for (int i = 0; i < room_to_leave->member_count; ++i) {
        if (room_to_leave->members[i] == client) {
            found_idx = i;
            break;
        }
    }

    if (found_idx != -1) {
        // Shift members to fill the gap
        for (int i = found_idx; i < room_to_leave->member_count - 1; ++i) {
            room_to_leave->members[i] = room_to_leave->members[i+1];
        }
        room_to_leave->members[room_to_leave->member_count - 1] = NULL; // Clear last ptr
        room_to_leave->member_count--;
    }
    pthread_mutex_unlock(&room_to_leave->room_lock);

    // Clear client's current room state
    char old_room_name[ROOM_NAME_BUF_SIZE]; // For logging
    strncpy(old_room_name, client->current_room_name, ROOM_NAME_BUF_SIZE-1);
    memset(client->current_room_name, 0, ROOM_NAME_BUF_SIZE);
    
    if (found_idx != -1) { // Log only if client was actually removed
         log_event_client_left_room(client->username, old_room_name);
    }
}

// Handles a client's request to join a room.
void handle_join_room_request(client_info_t *client, const char *room_name_requested) {
    if (!is_valid_room_name(room_name_requested)) {
        send_error_to_client(client->socket_fd, "Invalid room name format.");
        return;
    }
    
    char old_room_name_for_log[ROOM_NAME_BUF_SIZE];
    int was_in_room = strlen(client->current_room_name) > 0;
    if(was_in_room) strncpy(old_room_name_for_log, client->current_room_name, ROOM_NAME_BUF_SIZE-1);


    // If client is already in a room, leave it first.
    if (was_in_room) {
        if (strcmp(client->current_room_name, room_name_requested) == 0) {
            send_success_with_room_to_client(client->socket_fd, "You are already in this room.", room_name_requested);
            return;
        }
        // Notify old room members about departure
        chat_room_t* old_room = find_or_create_chat_room(client->current_room_name); // Should exist
        if (old_room) {
            char notification_msg_content[MESSAGE_BUF_SIZE];
            snprintf(notification_msg_content, sizeof(notification_msg_content), "%s has left the room.", client->username);
            message_t leave_notification;
            memset(&leave_notification, 0, sizeof(leave_notification));
            leave_notification.type = MSG_SERVER_NOTIFICATION;
            strncpy(leave_notification.sender, "SERVER", USERNAME_BUF_SIZE-1);
            strncpy(leave_notification.room, old_room->name, ROOM_NAME_BUF_SIZE-1);
            strncpy(leave_notification.content, notification_msg_content, MESSAGE_BUF_SIZE-1);
            broadcast_message_to_room_members(old_room, &leave_notification, client->username); // Exclude self
        }
        remove_client_from_their_room(client); // This also logs the leave
    }

    chat_room_t *target_room = find_or_create_chat_room(room_name_requested);
    if (!target_room) {
        send_error_to_client(client->socket_fd, "Failed to find or create the room (server limit may be reached).");
        return;
    }

    if (add_client_to_room(client, target_room)) {
        send_success_with_room_to_client(client->socket_fd, "Joined room successfully.", target_room->name);
        
        if(was_in_room) {
            log_event_client_switched_room(client->username, old_room_name_for_log, target_room->name);
        } else {
            log_event_client_joined_room(client->username, target_room->name);
        }

        // Notify new room members about arrival
        char notification_msg_content[MESSAGE_BUF_SIZE];
        snprintf(notification_msg_content, sizeof(notification_msg_content), "%s has joined the room.", client->username);
        message_t join_notification;
        memset(&join_notification, 0, sizeof(join_notification));
        join_notification.type = MSG_SERVER_NOTIFICATION;
        strncpy(join_notification.sender, "SERVER", USERNAME_BUF_SIZE-1);
        strncpy(join_notification.room, target_room->name, ROOM_NAME_BUF_SIZE-1);
        strncpy(join_notification.content, notification_msg_content, MESSAGE_BUF_SIZE-1);
        broadcast_message_to_room_members(target_room, &join_notification, client->username); // Exclude self

    } else {
        send_error_to_client(client->socket_fd, "Failed to join room (it might be full).");
        // If client was in old_room and failed to join new one, they are now in no room.
        // This state is consistent.
    }
}

void handle_leave_room_request(client_info_t *client) {
    if (strlen(client->current_room_name) == 0) {
        send_error_to_client(client->socket_fd, "You are not in any room.");
        return;
    }
    
    char room_that_was_left[ROOM_NAME_BUF_SIZE];
    strncpy(room_that_was_left, client->current_room_name, ROOM_NAME_BUF_SIZE-1);

    // Notify room members about departure
    chat_room_t* old_room = find_or_create_chat_room(client->current_room_name); // Should exist
    if (old_room) {
        char notification_msg_content[MESSAGE_BUF_SIZE];
        snprintf(notification_msg_content, sizeof(notification_msg_content), "%s has left the room.", client->username);
        message_t leave_notification;
        memset(&leave_notification, 0, sizeof(leave_notification));
        leave_notification.type = MSG_SERVER_NOTIFICATION;
        strncpy(leave_notification.sender, "SERVER", USERNAME_BUF_SIZE-1);
        strncpy(leave_notification.room, old_room->name, ROOM_NAME_BUF_SIZE-1);
        strncpy(leave_notification.content, notification_msg_content, MESSAGE_BUF_SIZE-1);
        broadcast_message_to_room_members(old_room, &leave_notification, client->username); // Exclude self
    }
    
    remove_client_from_their_room(client); // This logs the leave internally
    send_success_to_client(client->socket_fd, "You have left the room.");
}

// Handles a broadcast message from a client.
void handle_broadcast_request(client_info_t *client_sender, const char *message_content) {
    if (strlen(client_sender->current_room_name) == 0) {
        send_error_to_client(client_sender->socket_fd, "You must be in a room to broadcast.");
        return;
    }
    if (strlen(message_content) == 0 || strlen(message_content) >= MESSAGE_BUF_SIZE) {
        send_error_to_client(client_sender->socket_fd, "Invalid message content for broadcast.");
        return;
    }

    chat_room_t *current_room = find_or_create_chat_room(client_sender->current_room_name); // Should exist
    if (!current_room) {
        send_error_to_client(client_sender->socket_fd, "Error: Your current room seems to be invalid.");
        log_server_event("ERROR", "Client %s in room %s which was not found during broadcast.", client_sender->username, client_sender->current_room_name);
        return;
    }

    message_t broadcast_msg_to_send;
    memset(&broadcast_msg_to_send, 0, sizeof(broadcast_msg_to_send));
    broadcast_msg_to_send.type = MSG_BROADCAST;
    strncpy(broadcast_msg_to_send.sender, client_sender->username, USERNAME_BUF_SIZE - 1);
    strncpy(broadcast_msg_to_send.room, current_room->name, ROOM_NAME_BUF_SIZE - 1);
    strncpy(broadcast_msg_to_send.content, message_content, MESSAGE_BUF_SIZE - 1);

    // Send to all members of the room, including the sender (as per typical chat behavior)
    broadcast_message_to_room_members(current_room, &broadcast_msg_to_send, NULL); // NULL exclude means send to all

    log_event_broadcast(client_sender->username, current_room->name, message_content);
    
    // Send confirmation to the sender client (as per PDF page 4 "Message sent to room 'teamchat'")
    char confirmation_text[100];
    snprintf(confirmation_text, sizeof(confirmation_text), "Message sent to room '%s'", current_room->name);
    send_success_to_client(client_sender->socket_fd, confirmation_text);
}


// Handles a whisper message from a client.
void handle_whisper_request(client_info_t *client_sender, const char *receiver_username, const char *message_content) {
    if (!is_valid_username(receiver_username)) {
        send_error_to_client(client_sender->socket_fd, "Invalid recipient username for whisper.");
        return;
    }
    if (strlen(message_content) == 0 || strlen(message_content) >= MESSAGE_BUF_SIZE) {
        send_error_to_client(client_sender->socket_fd, "Invalid message content for whisper.");
        return;
    }
    if (strcmp(client_sender->username, receiver_username) == 0) {
        send_error_to_client(client_sender->socket_fd, "You cannot whisper to yourself.");
        return;
    }

    pthread_mutex_lock(&g_server_state->clients_list_mutex); // Lock before finding client
    client_info_t *receiver_client = find_client_by_username(receiver_username); // find_client needs review for locking
    pthread_mutex_unlock(&g_server_state->clients_list_mutex);

    if (!receiver_client || !receiver_client->is_active) {
        send_error_to_client(client_sender->socket_fd, "Recipient user not found or is offline.");
        return;
    }

    message_t whisper_msg_to_send;
    memset(&whisper_msg_to_send, 0, sizeof(whisper_msg_to_send));
    whisper_msg_to_send.type = MSG_WHISPER;
    strncpy(whisper_msg_to_send.sender, client_sender->username, USERNAME_BUF_SIZE - 1);
    strncpy(whisper_msg_to_send.receiver, receiver_username, USERNAME_BUF_SIZE - 1); // For receiver's context if needed
    strncpy(whisper_msg_to_send.content, message_content, MESSAGE_BUF_SIZE - 1);

    if (send_message(receiver_client->socket_fd, &whisper_msg_to_send)) {
        // Send confirmation to the sender client (as per PDF page 4 "Whisper sent to john42")
        char confirmation_text[100];
        snprintf(confirmation_text, sizeof(confirmation_text), "Whisper sent to %s", receiver_username);
        send_success_to_client(client_sender->socket_fd, confirmation_text);
        log_event_whisper(client_sender->username, receiver_username);
    } else {
        send_error_to_client(client_sender->socket_fd, "Failed to deliver whisper (recipient connection issue?).");
        log_server_event("ERROR", "Failed to send whisper from %s to %s socket.", client_sender->username, receiver_username);
    }
}


// Helper to send a message to all members of a room.
// If exclude_username is not NULL, that user will be skipped.
void broadcast_message_to_room_members(chat_room_t *room, const message_t *message_to_send, const char *exclude_username) {
    if (!room || !message_to_send) return;

    pthread_mutex_lock(&room->room_lock); // Lock the room for safe iteration
    for (int i = 0; i < room->member_count; ++i) {
        client_info_t *member = room->members[i];
        if (member && member->is_active) {
            if (exclude_username && strcmp(member->username, exclude_username) == 0) {
                continue; // Skip excluded user
            }
            send_message(member->socket_fd, message_to_send);
        }
    }
    pthread_mutex_unlock(&room->room_lock);
}