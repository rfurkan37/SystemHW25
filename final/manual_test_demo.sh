#!/bin/bash

echo "=== Manual Chat Server Demonstration ==="
echo "This script demonstrates the full functionality of the chat server."
echo ""

# Start server
echo "1. Starting chat server on port 6000..."
./chatserver 6000 &
SERVER_PID=$!
sleep 2

echo "2. Server started with PID: $SERVER_PID"
echo ""

# Create demonstration clients
echo "3. Creating demonstration clients..."

# Client 1: Alice joins general room and broadcasts
echo "Creating Client 1 (Alice)..."
mkfifo alice_pipe
(
    echo "Alice"
    sleep 2
    echo "/join general"
    sleep 2
    echo "Hello everyone! I'm Alice."
    sleep 3
    echo "/whisper Bob Hi Bob, how are you?"
    sleep 5
    echo "This is a great chat server!"
    sleep 3
    echo "/quit"
) > alice_pipe &

gnome-terminal --title="Alice" -- bash -c "
echo 'Alice connecting to chat server...';
./chatclient 127.0.0.1 6000 < alice_pipe;
echo 'Alice disconnected. Press Enter to close.';
read
" &

sleep 3

# Client 2: Bob joins general room
echo "Creating Client 2 (Bob)..."
mkfifo bob_pipe
(
    echo "Bob"
    sleep 2
    echo "/join general"
    sleep 2
    echo "Hey Alice! I'm doing great, thanks for asking."
    sleep 3
    echo "/whisper Alice Nice to meet you!"
    sleep 5
    echo "Yes, this server is awesome!"
    sleep 3
    echo "/quit"
) > bob_pipe &

gnome-terminal --title="Bob" -- bash -c "
echo 'Bob connecting to chat server...';
./chatclient 127.0.0.1 6000 < bob_pipe;
echo 'Bob disconnected. Press Enter to close.';
read
" &

sleep 3

# Client 3: Charlie joins different room
echo "Creating Client 3 (Charlie)..."
mkfifo charlie_pipe
(
    echo "Charlie"
    sleep 2
    echo "/join developers"
    sleep 2
    echo "Anyone here interested in C programming?"
    sleep 5
    echo "/join general"
    sleep 2
    echo "Hi Alice and Bob! Charlie here."
    sleep 3
    echo "/quit"
) > charlie_pipe &

gnome-terminal --title="Charlie" -- bash -c "
echo 'Charlie connecting to chat server...';
./chatclient 127.0.0.1 6000 < charlie_pipe;
echo 'Charlie disconnected. Press Enter to close.';
read
" &

echo ""
echo "4. Three clients are now running in separate terminal windows:"
echo "   - Alice: Joins 'general' room, broadcasts messages, sends whisper to Bob"
echo "   - Bob: Joins 'general' room, responds to Alice's whisper"
echo "   - Charlie: Joins 'developers' room first, then switches to 'general'"
echo ""
echo "5. Watch the terminal windows to see the chat interaction!"
echo ""
echo "6. Server log will show all connections and activities."
echo ""

# Wait for clients to finish
sleep 20

echo "7. Demonstration complete. Shutting down server..."
kill -INT $SERVER_PID
sleep 3

if ! kill -0 $SERVER_PID 2>/dev/null; then
    echo "✅ Server shut down gracefully"
else
    echo "❌ Server still running, force killing..."
    kill -9 $SERVER_PID
fi

# Cleanup
rm -f alice_pipe bob_pipe charlie_pipe

echo ""
echo "8. Check server.log for complete activity log:"
echo "----------------------------------------"
tail -20 server.log
echo "----------------------------------------"
echo ""
echo "Demonstration complete!" 