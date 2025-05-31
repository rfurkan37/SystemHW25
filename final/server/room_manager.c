#include "common.h"

// Initializes the room system.
// Sets up mutexes for each potential room slot.
void initializeRoomSystem()
{
    if (!g_server_state)
    {
        // This should not happen if server main initializes g_server_state first.
        fprintf(stderr, "CRITICAL: g_server_state is NULL in initializeRoomSystem.\n");
        exit(EXIT_FAILURE); // Cannot proceed without server state
    }

    // Lock the main rooms_list_mutex to safely initialize the chat_rooms array
    pthread_mutex_lock(&g_server_state->rooms_list_mutex);
    for (int i = 0; i < MAX_ROOMS; ++i)
    {
        memset(&g_server_state->chat_rooms[i], 0, sizeof(ChatRoom)); // Zero out room structure
        if (pthread_mutex_init(&g_server_state->chat_rooms[i].room_lock, NULL) != 0)
        {
            pthread_mutex_unlock(&g_server_state->rooms_list_mutex); // Unlock before logging/exiting
            logServerEvent("CRITICAL", "Failed to initialize mutex for room slot %d: %s", i, strerror(errno));
            // This is a critical failure, proper cleanup of already initialized mutexes might be needed
            // if we were to attempt recovery. For now, exit.
            exit(EXIT_FAILURE);
        }
        // name field is already zeroed by memset, member_count is 0.
    }
    g_server_state->current_room_count = 0; // No rooms active yet
    pthread_mutex_unlock(&g_server_state->rooms_list_mutex);
    logServerEvent("INFO", "Room management system initialized successfully.");
}

// Finds an existing chat room by name or creates a new one if it doesn't exist and there's space.
// Returns a pointer to the ChatRoom structure, or NULL on failure (e.g., max rooms reached).
ChatRoom *findOrCreateChatRoom(const char *room_name_to_find)
{
    if (!g_server_state || !isValidRoomName(room_name_to_find))
    {
        return NULL; // Invalid input or server state
    }

    pthread_mutex_lock(&g_server_state->rooms_list_mutex); // Lock for accessing rooms list and current_room_count

    // Try to find an existing room
    for (int i = 0; i < g_server_state->current_room_count; ++i)
    {
        // Safely compare names, assuming g_server_state->chat_rooms[i].name is null-terminated
        if (strncmp(g_server_state->chat_rooms[i].name, room_name_to_find, ROOM_NAME_BUF_SIZE) == 0)
        {
            pthread_mutex_unlock(&g_server_state->rooms_list_mutex);
            return &g_server_state->chat_rooms[i]; // Room found
        }
    }

    // If room not found, try to create a new one
    if (g_server_state->current_room_count < MAX_ROOMS)
    {
        // Get the next available room slot
        ChatRoom *new_room = &g_server_state->chat_rooms[g_server_state->current_room_count];
        // The room_lock for this slot was initialized in initializeRoomSystem.

        // Now, lock this specific room's mutex before modifying its contents
        // (though not strictly necessary here as no other thread should access it yet, good practice)
        // pthread_mutex_lock(&new_room->room_lock); // Not strictly needed for initialization of a new room slot

        strncpy(new_room->name, room_name_to_find, ROOM_NAME_BUF_SIZE - 1);
        new_room->name[ROOM_NAME_BUF_SIZE - 1] = '\0'; // Ensure null termination
        new_room->member_count = 0;                    // Initialize member count
        // members array is already zeroed from initialization or previous use if rooms are reused (not current model)

        // pthread_mutex_unlock(&new_room->room_lock); // If locked above

        g_server_state->current_room_count++; // Increment active room count

        pthread_mutex_unlock(&g_server_state->rooms_list_mutex); // Unlock rooms list mutex
        logEventRoomCreated(new_room->name);                     // Log room creation
        return new_room;                                         // Return pointer to the new room
    }

    // Max rooms reached, cannot create new room
    pthread_mutex_unlock(&g_server_state->rooms_list_mutex);
    logServerEvent("WARNING", "Could not create room '%s': Maximum room limit (%d) reached.", room_name_to_find, MAX_ROOMS);
    return NULL;
}

// Adds a client to a specified chat room.
// Handles checks for room capacity and if client is already a member.
// Returns 1 on success, 0 on failure (e.g., room full).
int addClientToRoom(ClientInfo *client, ChatRoom *room)
{
    if (!client || !room)
        return 0;

    pthread_mutex_lock(&room->room_lock); // Lock specific room for modifying its member list

    if (room->member_count >= MAX_MEMBERS_PER_ROOM)
    {
        pthread_mutex_unlock(&room->room_lock);
        logServerEvent("INFO", "Client %s failed to join room '%s': Room is full (capacity %d).",
                       client->username, room->name, MAX_MEMBERS_PER_ROOM);
        return 0; // Room is full
    }

    // Defensive check: ensure client is not already listed as a member
    for (int i = 0; i < room->member_count; ++i)
    {
        if (room->members[i] == client)
        {
            pthread_mutex_unlock(&room->room_lock);
            // logServerEvent("DEBUG", "Client %s already in room '%s'. Join request ignored.", client->username, room->name);
            return 1; // Already a member, treat as success
        }
    }

    // Add client to the room
    room->members[room->member_count++] = client;
    pthread_mutex_unlock(&room->room_lock);

    // Update client's state to reflect current room
    strncpy(client->current_room_name, room->name, ROOM_NAME_BUF_SIZE - 1);
    client->current_room_name[ROOM_NAME_BUF_SIZE - 1] = '\0';
    return 1; // Successfully added
}

// Removes a client from their currently associated chat room.
// This function is called when a client leaves a room, disconnects, or switches rooms.
void removeClientFromTheirRoom(ClientInfo *client)
{
    if (!client || strlen(client->current_room_name) == 0 || !g_server_state)
    {
        return; // Client not in a room or invalid state
    }

    ChatRoom *room_to_leave = NULL;
    // Find the room structure. Requires locking rooms_list_mutex.
    pthread_mutex_lock(&g_server_state->rooms_list_mutex);
    for (int i = 0; i < g_server_state->current_room_count; ++i)
    {
        if (strncmp(g_server_state->chat_rooms[i].name, client->current_room_name, ROOM_NAME_BUF_SIZE) == 0)
        {
            room_to_leave = &g_server_state->chat_rooms[i];
            break;
        }
    }
    pthread_mutex_unlock(&g_server_state->rooms_list_mutex);

    if (!room_to_leave)
    {
        logServerEvent("WARNING", "Client %s tried to leave room '%s', but room was not found in active list.",
                       client->username, client->current_room_name);
        memset(client->current_room_name, 0, ROOM_NAME_BUF_SIZE); // Clear client's room state anyway
        return;
    }

    // Now, lock the specific room to modify its member list
    pthread_mutex_lock(&room_to_leave->room_lock);
    int found_idx = -1;
    for (int i = 0; i < room_to_leave->member_count; ++i)
    {
        if (room_to_leave->members[i] == client)
        {
            found_idx = i;
            break;
        }
    }

    if (found_idx != -1)
    {
        // Shift members to fill the gap
        for (int i = found_idx; i < room_to_leave->member_count - 1; ++i)
        {
            room_to_leave->members[i] = room_to_leave->members[i + 1];
        }
        room_to_leave->members[room_to_leave->member_count - 1] = NULL; // Clear last slot
        room_to_leave->member_count--;
    }
    // If found_idx == -1, client was not in member list - inconsistent state, but proceed.
    pthread_mutex_unlock(&room_to_leave->room_lock);

    // Log the event if client was actually removed
    if (found_idx != -1)
    {
        // Log with the name of the room they *were* in, before clearing it.
        logEventClientLeftRoom(client->username, room_to_leave->name);
    }

    // Clear the client's current room name state
    memset(client->current_room_name, 0, ROOM_NAME_BUF_SIZE);

    // Note: Room deletion if empty is not implemented as per common chat server designs.
    // Rooms persist until server shutdown unless explicitly managed otherwise.
}

// Notifies other members of a room about a client's action (join/leave).
void notifyRoomOfClientAction(ClientInfo *acting_client, ChatRoom *room, const char *action_verb)
{
    if (!room || !acting_client || !action_verb || !g_server_state || !g_server_state->server_is_running)
        return;

    char notification_content[MESSAGE_BUF_SIZE];
    snprintf(notification_content, sizeof(notification_content), "User '%s' has %s the room.", acting_client->username, action_verb);
    notification_content[sizeof(notification_content) - 1] = '\0';

    Message notification_msg;
    memset(&notification_msg, 0, sizeof(notification_msg));
    notification_msg.type = MSG_SERVER_NOTIFICATION;
    strncpy(notification_msg.sender, "SERVER", USERNAME_BUF_SIZE - 1);
    notification_msg.sender[USERNAME_BUF_SIZE - 1] = '\0';
    strncpy(notification_msg.room, room->name, ROOM_NAME_BUF_SIZE - 1); // Context for client
    notification_msg.room[ROOM_NAME_BUF_SIZE - 1] = '\0';
    strncpy(notification_msg.content, notification_content, MESSAGE_BUF_SIZE - 1);
    notification_msg.content[MESSAGE_BUF_SIZE - 1] = '\0';

    // Send to all members of the room, EXCLUDING the client who performed the action.
    broadcastMessageToRoomMembers(room, &notification_msg, acting_client->username);
}

// Handles a client's request to join a room.
void handleJoinRoomRequest(ClientInfo *client, const char *room_name_requested)
{
    if (!client || !client->is_active)
        return;

    if (!isValidRoomName(room_name_requested))
    {
        sendErrorToClient(client->socket_fd, "Invalid room name format. Must be alphanumeric, 1-32 chars, no spaces.");
        return;
    }

    char old_room_name_log[ROOM_NAME_BUF_SIZE];
    memset(old_room_name_log, 0, sizeof(old_room_name_log));
    int was_in_another_room = 0;

    // If client is already in a room, handle leaving that room first
    if (strlen(client->current_room_name) > 0)
    {
        if (strncmp(client->current_room_name, room_name_requested, ROOM_NAME_BUF_SIZE) == 0)
        {
            // Client is trying to join the room they are already in
            sendSuccessWithRoomToClient(client->socket_fd, "You are already in this room.", room_name_requested);
            return;
        }
        // Client is switching rooms
        strncpy(old_room_name_log, client->current_room_name, ROOM_NAME_BUF_SIZE - 1);
        old_room_name_log[ROOM_NAME_BUF_SIZE - 1] = '\0';
        was_in_another_room = 1;

        ChatRoom *old_room = findOrCreateChatRoom(client->current_room_name); // Should always find it if client->current_room_name is set
        if (old_room)
        {
            notifyRoomOfClientAction(client, old_room, "left"); // Notify old room
        }
        removeClientFromTheirRoom(client); // This also logs the "left room" part for the old room.
    }

    // Find or create the target room
    ChatRoom *target_room = findOrCreateChatRoom(room_name_requested);
    if (!target_room)
    {
        sendErrorToClient(client->socket_fd, "Failed to find or create the requested room (server limit may be reached).");
        // If client was switching rooms, they are now in no room. This needs careful state management if old room removal failed.
        // However, findOrCreateChatRoom failure is rare (only if MAX_ROOMS hit).
        return;
    }

    // Attempt to add client to the target room
    if (addClientToRoom(client, target_room))
    {
        sendSuccessWithRoomToClient(client->socket_fd, "Joined room", target_room->name); // Short success message

        // Log event: either switched or joined for the first time/from no room
        if (was_in_another_room)
        {
            logEventClientSwitchedRoom(client->username, old_room_name_log, target_room->name);
        }
        else
        {
            logEventClientJoinedRoom(client->username, target_room->name);
        }
        notifyRoomOfClientAction(client, target_room, "joined"); // Notify new room
    }
    else
    {
        // Failed to add (e.g., room full)
        sendErrorToClient(client->socket_fd, "Failed to join room (it might be full or an internal error occurred).");
        // Client's current_room_name should still be empty or the old room if they were switching and add failed.
        // addClientToRoom only updates client->current_room_name on success.
    }
}

// Handles a client's request to leave their current room.
void handleLeaveRoomRequest(ClientInfo *client)
{
    if (!client || !client->is_active)
        return;

    if (strlen(client->current_room_name) == 0)
    {
        sendErrorToClient(client->socket_fd, "You are not currently in any room.");
        return;
    }

    ChatRoom *room_being_left = findOrCreateChatRoom(client->current_room_name); // Should find existing
    if (room_being_left)
    {
        notifyRoomOfClientAction(client, room_being_left, "left"); // Notify before actual removal
    }
    // removeClientFromTheirRoom logs the leave and clears client->current_room_name
    removeClientFromTheirRoom(client);
    sendSuccessToClient(client->socket_fd, "You have successfully left the room.");
}

// Handles a client's request to broadcast a message to their current room.
void handleBroadcastRequest(ClientInfo *client_sender, const char *message_content)
{
    if (!client_sender || !client_sender->is_active)
        return;

    if (strlen(client_sender->current_room_name) == 0)
    {
        sendErrorToClient(client_sender->socket_fd, "You must be in a room to broadcast a message.");
        return;
    }
    if (strlen(message_content) == 0 || strlen(message_content) >= MESSAGE_BUF_SIZE)
    {
        sendErrorToClient(client_sender->socket_fd, "Invalid message content: Cannot be empty or too long.");
        return;
    }

    ChatRoom *current_room = findOrCreateChatRoom(client_sender->current_room_name); // Should find
    if (!current_room)
    {
        // This indicates a server-side inconsistency if client->current_room_name is set but room not found.
        sendErrorToClient(client_sender->socket_fd, "Error: Your current room seems to be invalid on the server.");
        logServerEvent("ERROR", "Client %s in room '%s' which was not found during broadcast attempt.",
                       client_sender->username, client_sender->current_room_name);
        return;
    }

    // Prepare the broadcast message
    Message broadcast_msg;
    memset(&broadcast_msg, 0, sizeof(broadcast_msg));
    broadcast_msg.type = MSG_BROADCAST; // Client receiver threads will identify this type
    strncpy(broadcast_msg.sender, client_sender->username, USERNAME_BUF_SIZE - 1);
    broadcast_msg.sender[USERNAME_BUF_SIZE - 1] = '\0';
    strncpy(broadcast_msg.room, current_room->name, ROOM_NAME_BUF_SIZE - 1);
    broadcast_msg.room[ROOM_NAME_BUF_SIZE - 1] = '\0';
    strncpy(broadcast_msg.content, message_content, MESSAGE_BUF_SIZE - 1);
    broadcast_msg.content[MESSAGE_BUF_SIZE - 1] = '\0';

    // Send to all members in the room (including the sender themselves, as per typical chat behavior)
    broadcastMessageToRoomMembers(current_room, &broadcast_msg, NULL); // NULL for exclude_username means send to all

    // Log the broadcast event (server-side)
    logEventBroadcast(client_sender->username, current_room->name, message_content);

    // Send confirmation to the sender
    char confirmation_text[128];
    snprintf(confirmation_text, sizeof(confirmation_text), "Message sent to room '%s'", current_room->name);
    confirmation_text[sizeof(confirmation_text) - 1] = '\0';
    sendSuccessToClient(client_sender->socket_fd, confirmation_text);
}

// Handles a client's request to send a private message (whisper) to another user.
void handleWhisperRequest(ClientInfo *client_sender, const char *receiver_username_str, const char *message_content)
{
    if (!client_sender || !client_sender->is_active)
        return;

    if (!isValidUsername(receiver_username_str))
    {
        sendErrorToClient(client_sender->socket_fd, "Invalid recipient username format for whisper.");
        return;
    }
    if (strlen(message_content) == 0 || strlen(message_content) >= MESSAGE_BUF_SIZE)
    {
        sendErrorToClient(client_sender->socket_fd, "Invalid message content for whisper: Cannot be empty or too long.");
        return;
    }
    if (strncmp(client_sender->username, receiver_username_str, USERNAME_BUF_SIZE) == 0)
    {
        sendErrorToClient(client_sender->socket_fd, "You cannot whisper a message to yourself.");
        return;
    }

    ClientInfo *receiver_client = NULL;
    // Find the recipient client. Requires locking clients_list_mutex.
    pthread_mutex_lock(&g_server_state->clients_list_mutex);
    receiver_client = findClientByUsername(receiver_username_str); // This utility expects lock to be held
    pthread_mutex_unlock(&g_server_state->clients_list_mutex);

    if (!receiver_client || !receiver_client->is_active) // Check if found and if that client is still active
    {
        sendErrorToClient(client_sender->socket_fd, "Recipient user not found or is currently offline.");
        return;
    }

    // Prepare the whisper message
    Message whisper_msg;
    memset(&whisper_msg, 0, sizeof(whisper_msg));
    whisper_msg.type = MSG_WHISPER; // Client receiver threads will identify this type
    strncpy(whisper_msg.sender, client_sender->username, USERNAME_BUF_SIZE - 1);
    whisper_msg.sender[USERNAME_BUF_SIZE - 1] = '\0';
    strncpy(whisper_msg.receiver, receiver_username_str, USERNAME_BUF_SIZE - 1); // For recipient's context
    whisper_msg.receiver[USERNAME_BUF_SIZE - 1] = '\0';
    strncpy(whisper_msg.content, message_content, MESSAGE_BUF_SIZE - 1);
    whisper_msg.content[MESSAGE_BUF_SIZE - 1] = '\0';

    // Send the message to the recipient's socket
    if (sendMessage(receiver_client->socket_fd, &whisper_msg))
    {
        char confirmation_text[128];
        snprintf(confirmation_text, sizeof(confirmation_text), "Whisper successfully sent to %s", receiver_username_str);
        confirmation_text[sizeof(confirmation_text) - 1] = '\0';
        sendSuccessToClient(client_sender->socket_fd, confirmation_text);
        logEventWhisper(client_sender->username, receiver_username_str, message_content); // Log full content for server record if desired, or preview
    }
    else
    {
        sendErrorToClient(client_sender->socket_fd, "Failed to deliver whisper message (recipient connection issue or server error).");
        logServerEvent("ERROR", "Failed to send whisper message from %s to %s via socket %d.",
                       client_sender->username, receiver_username_str, receiver_client->socket_fd);
    }
}

// Broadcasts a message to all active members of a given room.
// Can optionally exclude one user (typically the sender of a broadcast or notifier of an action).
void broadcastMessageToRoomMembers(ChatRoom *room, const Message *message_to_send, const char *exclude_username)
{
    if (!room || !message_to_send || !g_server_state)
        return;

    pthread_mutex_lock(&room->room_lock); // Lock the room to safely iterate its members
    for (int i = 0; i < room->member_count; ++i)
    {
        ClientInfo *member = room->members[i];
        if (member && member->is_active && member->socket_fd >= 0) // Check if member is valid, active, and has a socket
        {
            if (exclude_username && strncmp(member->username, exclude_username, USERNAME_BUF_SIZE) == 0)
            {
                continue; // Skip the excluded user
            }
            // Send the message. sendMessage handles errors internally (e.g., client disconnected).
            if (!sendMessage(member->socket_fd, message_to_send))
            {
                // Log if sending to a specific member failed, might indicate that member disconnected abruptly.
                // The main handler for that client will eventually clean them up.
                // logServerEvent("DEBUG", "Failed to send broadcast/notification to %s in room %s (socket %d). Client might have disconnected.",
                //                member->username, room->name, member->socket_fd);
            }
        }
    }
    pthread_mutex_unlock(&room->room_lock);
}