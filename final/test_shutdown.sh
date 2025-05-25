#!/bin/bash

echo "=== Testing Client Shutdown Behavior ==="

# Start server
echo "Starting server..."
./chatserver 8080 &
SERVER_PID=$!
sleep 2

# Start client in background with input
echo "Starting client..."
(
    echo "testuser"
    sleep 2
    echo "/help"
    sleep 2
    # Client will wait here for more input
) | ./chatclient 127.0.0.1 8080 &
CLIENT_PID=$!

sleep 3

echo "Sending SIGINT to server..."
kill -INT $SERVER_PID

# Wait a bit and check if both processes terminated
sleep 3

echo "Checking if processes terminated..."
if kill -0 $SERVER_PID 2>/dev/null; then
    echo "❌ Server still running"
    kill -9 $SERVER_PID
else
    echo "✅ Server terminated properly"
fi

if kill -0 $CLIENT_PID 2>/dev/null; then
    echo "❌ Client still running"
    kill -9 $CLIENT_PID
else
    echo "✅ Client terminated properly"
fi

echo "=== Test Complete ==="
echo "Server log (last 5 lines):"
tail -5 server.log 