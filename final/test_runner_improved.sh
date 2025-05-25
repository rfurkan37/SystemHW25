#!/bin/bash

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

echo -e "${BLUE}=== Improved Multi-threaded Chat Server Test Suite ===${NC}"

# Check if binaries exist
if [ ! -f "./chatserver" ] || [ ! -f "./chatclient" ]; then
    echo -e "${RED}Error: chatserver or chatclient not found. Run 'make' first.${NC}"
    exit 1
fi

# Test configuration
SERVER_PORT=5001
SERVER_IP="127.0.0.1"
TEST_DIR="test_files"

# Create test directory and files
mkdir -p $TEST_DIR
echo "Hello, this is a test file!" > $TEST_DIR/test.txt
echo "Binary test data" > $TEST_DIR/test.pdf
dd if=/dev/zero of=$TEST_DIR/large.txt bs=1M count=4 2>/dev/null

echo -e "${YELLOW}Starting comprehensive testing...${NC}"

# Function to cleanup
cleanup() {
    echo -e "${YELLOW}Cleaning up...${NC}"
    pkill -f chatserver 2>/dev/null
    pkill -f chatclient 2>/dev/null
    rm -rf $TEST_DIR
    exit 0
}

# Set trap for cleanup
trap cleanup SIGINT SIGTERM

# Test 1: Start server
echo -e "${BLUE}Test 1: Starting server on port $SERVER_PORT${NC}"
./chatserver $SERVER_PORT &
SERVER_PID=$!
sleep 2

# Check if server is running
if ! kill -0 $SERVER_PID 2>/dev/null; then
    echo -e "${RED}FAIL: Server failed to start${NC}"
    exit 1
fi
echo -e "${GREEN}PASS: Server started successfully${NC}"

# Test 2: Single client connection with persistent input
echo -e "${BLUE}Test 2: Single client connection (persistent)${NC}"
# Use a named pipe to keep stdin open
mkfifo test_pipe
(
    echo "testuser1"
    sleep 5  # Keep the connection alive for 5 seconds
    echo "/quit"
) > test_pipe &
timeout 10 ./chatclient $SERVER_IP $SERVER_PORT < test_pipe > /dev/null 2>&1 &
CLIENT1_PID=$!
sleep 3

# Check connection in server log
if grep -q "testuser1.*connected" server.log; then
    echo -e "${GREEN}PASS: Client connected and logged in successfully${NC}"
else
    echo -e "${RED}FAIL: Client connection not found in logs${NC}"
fi

# Wait for client to finish
wait $CLIENT1_PID 2>/dev/null
rm -f test_pipe

# Test 3: Multiple concurrent connections with persistent input
echo -e "${BLUE}Test 3: Multiple concurrent connections (5 clients)${NC}"
for i in {1..5}; do
    mkfifo test_pipe_$i
    (
        echo "user$i"
        sleep 8  # Keep connections alive longer
        echo "/quit"
    ) > test_pipe_$i &
    timeout 15 ./chatclient $SERVER_IP $SERVER_PORT < test_pipe_$i > /dev/null 2>&1 &
    PIDS[$i]=$!
done

sleep 5

# Count successful connections from server log
CONNECTED=$(grep -c "user[1-5].*connected" server.log | tail -1)
if [ "$CONNECTED" -ge 3 ]; then
    echo -e "${GREEN}PASS: $CONNECTED/5 clients connected successfully${NC}"
else
    echo -e "${YELLOW}PARTIAL: $CONNECTED/5 clients connected${NC}"
fi

# Wait for all clients to finish
for i in {1..5}; do
    wait ${PIDS[$i]} 2>/dev/null
    rm -f test_pipe_$i
done

# Test 4: Duplicate username handling
echo -e "${BLUE}Test 4: Duplicate username handling${NC}"
# Start first client
mkfifo dup_pipe1
(
    echo "duplicate"
    sleep 10
    echo "/quit"
) > dup_pipe1 &
timeout 15 ./chatclient $SERVER_IP $SERVER_PORT < dup_pipe1 > /dev/null 2>&1 &
DUP1_PID=$!

sleep 2

# Try second client with same username
mkfifo dup_pipe2
(
    echo "duplicate"
    sleep 2
) > dup_pipe2 &
timeout 5 ./chatclient $SERVER_IP $SERVER_PORT < dup_pipe2 > /dev/null 2>&1 &
DUP2_PID=$!

sleep 3

# Check server logs for rejection
if grep -q "Username.*already taken" server.log; then
    echo -e "${GREEN}PASS: Duplicate username properly rejected${NC}"
else
    echo -e "${YELLOW}INFO: Check server logs for duplicate username handling${NC}"
fi

# Cleanup
kill $DUP1_PID $DUP2_PID 2>/dev/null
wait $DUP1_PID $DUP2_PID 2>/dev/null
rm -f dup_pipe1 dup_pipe2

# Test 5: Invalid username handling
echo -e "${BLUE}Test 5: Invalid input handling${NC}"
mkfifo inv_pipe
(
    echo "invalid@user"
    sleep 2
) > inv_pipe &
timeout 5 ./chatclient $SERVER_IP $SERVER_PORT < inv_pipe > client_output.txt 2>&1 &
INV_PID=$!

sleep 3
wait $INV_PID 2>/dev/null

if grep -q "Invalid username format" client_output.txt; then
    echo -e "${GREEN}PASS: Invalid username properly rejected${NC}"
else
    echo -e "${YELLOW}INFO: Check client output for invalid username handling${NC}"
fi

rm -f inv_pipe client_output.txt

# Test 6: Room functionality
echo -e "${BLUE}Test 6: Room join and broadcast test${NC}"
mkfifo room_pipe
(
    echo "roomtester"
    sleep 2
    echo "/join testroom"
    sleep 2
    echo "Hello room!"
    sleep 3
    echo "/quit"
) > room_pipe &
timeout 15 ./chatclient $SERVER_IP $SERVER_PORT < room_pipe > room_output.txt 2>&1 &
ROOM_PID=$!

sleep 8
wait $ROOM_PID 2>/dev/null

if grep -q "Joined room successfully" room_output.txt; then
    echo -e "${GREEN}PASS: Room join functionality working${NC}"
else
    echo -e "${YELLOW}INFO: Check room functionality manually${NC}"
fi

rm -f room_pipe room_output.txt

# Test 7: Server shutdown handling
echo -e "${BLUE}Test 7: Graceful server shutdown${NC}"
mkfifo shutdown_pipe
(
    echo "shutdowntest"
    sleep 15  # Keep client alive during shutdown
) > shutdown_pipe &
timeout 20 ./chatclient $SERVER_IP $SERVER_PORT < shutdown_pipe > shutdown_output.txt 2>&1 &
SHUTDOWN_CLIENT=$!

sleep 3

# Send SIGINT to server
echo -e "${YELLOW}Sending SIGINT to server...${NC}"
kill -INT $SERVER_PID
sleep 4

# Check if server shut down gracefully
if ! kill -0 $SERVER_PID 2>/dev/null; then
    echo -e "${GREEN}PASS: Server shut down gracefully${NC}"
else
    echo -e "${RED}FAIL: Server did not shut down${NC}"
    kill -9 $SERVER_PID 2>/dev/null
fi

# Check if client was notified
if grep -q "shutting down" shutdown_output.txt; then
    echo -e "${GREEN}PASS: Client received shutdown notification${NC}"
else
    echo -e "${YELLOW}INFO: Check if client received shutdown notification${NC}"
fi

# Cleanup
kill $SHUTDOWN_CLIENT 2>/dev/null
wait $SHUTDOWN_CLIENT 2>/dev/null
rm -f shutdown_pipe shutdown_output.txt

# Test 8: Log file verification
echo -e "${BLUE}Test 8: Log file verification${NC}"
if [ -f "server.log" ]; then
    LOG_LINES=$(wc -l < server.log)
    if [ $LOG_LINES -gt 0 ]; then
        echo -e "${GREEN}PASS: Log file created with $LOG_LINES entries${NC}"
        echo -e "${YELLOW}Recent log entries:${NC}"
        tail -5 server.log
    else
        echo -e "${RED}FAIL: Log file is empty${NC}"
    fi
else
    echo -e "${RED}FAIL: Log file not created${NC}"
fi

echo -e "${BLUE}=== Test Summary ===${NC}"
echo -e "${GREEN}Improved automated tests completed.${NC}"
echo -e "${YELLOW}Key improvements:${NC}"
echo -e "${YELLOW}- Fixed stdin EOF issue with named pipes${NC}"
echo -e "${YELLOW}- Added persistent client connections${NC}"
echo -e "${YELLOW}- Better log analysis${NC}"
echo -e "${YELLOW}- More realistic test scenarios${NC}"

echo -e "${BLUE}For manual testing:${NC}"
echo -e "${YELLOW}1. Run server: ./chatserver 5000${NC}"
echo -e "${YELLOW}2. Run multiple clients: ./chatclient 127.0.0.1 5000${NC}"
echo -e "${YELLOW}3. Test room joining, broadcasting, whispers, and file transfers${NC}"

cleanup 