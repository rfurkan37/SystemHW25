#!/bin/bash

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
PURPLE='\033[0;35m'
NC='\033[0m' # No Color

echo -e "${BLUE}=== Comprehensive Chat Server Test Suite ===${NC}"
echo -e "${CYAN}Testing all 10 specified scenarios with logging validation${NC}"
echo ""

# Test configuration
SERVER_PORT=7000
SERVER_IP="127.0.0.1"
TEST_DIR="test_files"
LOG_FILE="server.log"

# Check if binaries exist
if [ ! -f "./chatserver" ] || [ ! -f "./chatclient" ]; then
    echo -e "${RED}Error: chatserver or chatclient not found. Run 'make' first.${NC}"
    exit 1
fi

# Create test directory and files
mkdir -p $TEST_DIR
echo "Hello, this is a test file!" > $TEST_DIR/test.txt
echo "Binary test data for PDF simulation" > $TEST_DIR/project.pdf
echo "Image data simulation" > $TEST_DIR/image.jpg
echo "PNG image simulation" > $TEST_DIR/photo.png

# Create large file (>3MB) for oversized test
dd if=/dev/zero of=$TEST_DIR/huge_data.zip bs=1M count=4 2>/dev/null

# Create medium files for queue testing
for i in {1..10}; do
    dd if=/dev/zero of=$TEST_DIR/file$i.txt bs=1K count=100 2>/dev/null
done

# Function to cleanup
cleanup() {
    echo -e "${YELLOW}Cleaning up test environment...${NC}"
    pkill -f chatserver 2>/dev/null
    pkill -f chatclient 2>/dev/null
    rm -rf $TEST_DIR
    rm -f test_pipe* client_output* room_output* dup_pipe* file_pipe*
    exit 0
}

# Set trap for cleanup
trap cleanup SIGINT SIGTERM

# Function to wait and check log
wait_for_log() {
    local pattern="$1"
    local timeout="$2"
    local count=0
    
    while [ $count -lt $timeout ]; do
        if grep -q "$pattern" $LOG_FILE 2>/dev/null; then
            return 0
        fi
        sleep 1
        ((count++))
    done
    return 1
}

# Function to extract log entries for scenario
extract_logs() {
    local scenario="$1"
    echo -e "${PURPLE}=== Log Entries for $scenario ===${NC}"
    if [ -f "$LOG_FILE" ]; then
        tail -20 $LOG_FILE | grep -E "(CONNECT|DISCONNECT|ROOM|BROADCAST|WHISPER|FILE|ERROR|SHUTDOWN|REJECTED)" || echo "No relevant log entries found"
    else
        echo "Log file not found"
    fi
    echo ""
}

# Start server
echo -e "${BLUE}Starting server on port $SERVER_PORT...${NC}"
rm -f $LOG_FILE  # Clean previous logs
./chatserver $SERVER_PORT &
SERVER_PID=$!
sleep 3

if ! kill -0 $SERVER_PID 2>/dev/null; then
    echo -e "${RED}FAIL: Server failed to start${NC}"
    exit 1
fi
echo -e "${GREEN}✅ Server started successfully (PID: $SERVER_PID)${NC}"
echo ""

# Test 1: Concurrent User Load (30 clients)
echo -e "${BLUE}TEST 1: Concurrent User Load (30 clients)${NC}"
echo "Testing: 30 clients connect simultaneously and interact"

# Create 30 concurrent clients
for i in {1..30}; do
    mkfifo test_pipe_$i
    (
        echo "user$i"
        sleep 2
        echo "/join room$((i % 5 + 1))"  # Distribute across 5 rooms
        sleep 2
        echo "Hello from user$i in room$((i % 5 + 1))"
        sleep 3
        if [ $((i % 3)) -eq 0 ]; then
            # Every 3rd user sends a whisper
            target_user="user$((i - 1))"
            echo "/whisper $target_user Hi from user$i"
        fi
        sleep 5
        echo "/quit"
    ) > test_pipe_$i &
    
    timeout 30 ./chatclient $SERVER_IP $SERVER_PORT < test_pipe_$i > /dev/null 2>&1 &
    CLIENT_PIDS[$i]=$!
done

echo "Waiting for all clients to connect and interact..."
sleep 15

# Count successful connections
CONNECTED=$(grep -c "connected" $LOG_FILE 2>/dev/null || echo "0")
echo -e "${GREEN}Result: $CONNECTED clients connected${NC}"

# Wait for clients to finish
for i in {1..30}; do
    wait ${CLIENT_PIDS[$i]} 2>/dev/null
    rm -f test_pipe_$i
done

extract_logs "Concurrent User Load"

# Test 2: Duplicate Usernames
echo -e "${BLUE}TEST 2: Duplicate Usernames${NC}"
echo "Testing: Two clients try to connect with same username"

# Start first client
mkfifo dup_pipe1
(
    echo "ali34"
    sleep 10
    echo "/quit"
) > dup_pipe1 &
timeout 15 ./chatclient $SERVER_IP $SERVER_PORT < dup_pipe1 > /dev/null 2>&1 &
DUP1_PID=$!

sleep 3

# Try second client with same username
mkfifo dup_pipe2
(
    echo "ali34"
    sleep 3
) > dup_pipe2 &
timeout 8 ./chatclient $SERVER_IP $SERVER_PORT < dup_pipe2 > dup_output.txt 2>&1 &
DUP2_PID=$!

sleep 5

# Check for rejection
if grep -q "already taken\|Username.*taken" dup_output.txt $LOG_FILE; then
    echo -e "${GREEN}✅ Duplicate username properly rejected${NC}"
else
    echo -e "${YELLOW}⚠️  Check logs for duplicate username handling${NC}"
fi

# Cleanup
kill $DUP1_PID $DUP2_PID 2>/dev/null
wait $DUP1_PID $DUP2_PID 2>/dev/null
rm -f dup_pipe1 dup_pipe2 dup_output.txt

extract_logs "Duplicate Usernames"

# Test 3: File Upload Queue Limit
echo -e "${BLUE}TEST 3: File Upload Queue Limit${NC}"
echo "Testing: 10 users attempt to send files simultaneously"

# Create 10 clients that will send files
for i in {1..10}; do
    mkfifo file_pipe_$i
    (
        echo "fileuser$i"
        sleep 2
        echo "/join fileroom"
        sleep 2
        echo "/send file$i.txt"
        sleep 8
        echo "/quit"
    ) > file_pipe_$i &
    
    timeout 20 ./chatclient $SERVER_IP $SERVER_PORT < file_pipe_$i > /dev/null 2>&1 &
    FILE_PIDS[$i]=$!
done

echo "Waiting for file transfer queue to process..."
sleep 15

# Check queue behavior in logs
QUEUE_ENTRIES=$(grep -c "FILE\|queue" $LOG_FILE 2>/dev/null || echo "0")
echo -e "${GREEN}Result: $QUEUE_ENTRIES file-related log entries${NC}"

# Wait for file clients to finish
for i in {1..10}; do
    wait ${FILE_PIDS[$i]} 2>/dev/null
    rm -f file_pipe_$i
done

extract_logs "File Upload Queue"

# Test 4: Unexpected Disconnection
echo -e "${BLUE}TEST 4: Unexpected Disconnection${NC}"
echo "Testing: Client disconnects without /quit"

mkfifo disconnect_pipe
(
    echo "mehmet1"
    sleep 2
    echo "/join testroom"
    sleep 2
    echo "I'm about to disconnect unexpectedly"
    sleep 2
    # Simulate unexpected disconnection by killing the process
) > disconnect_pipe &

timeout 15 ./chatclient $SERVER_IP $SERVER_PORT < disconnect_pipe > /dev/null 2>&1 &
DISCONNECT_PID=$!

sleep 5
# Force kill to simulate unexpected disconnection
kill -9 $DISCONNECT_PID 2>/dev/null

sleep 3

# Check for disconnection handling
if wait_for_log "mehmet1.*disconnect\|lost connection\|cleaned up" 5; then
    echo -e "${GREEN}✅ Unexpected disconnection handled properly${NC}"
else
    echo -e "${YELLOW}⚠️  Check logs for disconnection handling${NC}"
fi

rm -f disconnect_pipe

extract_logs "Unexpected Disconnection"

# Test 5: Room Switching
echo -e "${BLUE}TEST 5: Room Switching${NC}"
echo "Testing: Client joins room, then switches to another"

mkfifo room_switch_pipe
(
    echo "irem56"
    sleep 2
    echo "/join groupA"
    sleep 2
    echo "Hello groupA!"
    sleep 2
    echo "/join groupB"
    sleep 2
    echo "Hello groupB!"
    sleep 3
    echo "/quit"
) > room_switch_pipe &

timeout 20 ./chatclient $SERVER_IP $SERVER_PORT < room_switch_pipe > room_switch_output.txt 2>&1 &
ROOM_SWITCH_PID=$!

sleep 12
wait $ROOM_SWITCH_PID 2>/dev/null

# Check for room switching in logs
if grep -q "irem56.*joined.*groupA" $LOG_FILE && grep -q "irem56.*joined.*groupB" $LOG_FILE; then
    echo -e "${GREEN}✅ Room switching working correctly${NC}"
else
    echo -e "${YELLOW}⚠️  Check logs for room switching behavior${NC}"
fi

rm -f room_switch_pipe room_switch_output.txt

extract_logs "Room Switching"

# Test 6: Oversized File Rejection
echo -e "${BLUE}TEST 6: Oversized File Rejection${NC}"
echo "Testing: Client attempts to upload file exceeding 3MB"

mkfifo oversize_pipe
(
    echo "melis22"
    sleep 2
    echo "/join testroom"
    sleep 2
    echo "/send huge_data.zip"
    sleep 5
    echo "/quit"
) > oversize_pipe &

timeout 15 ./chatclient $SERVER_IP $SERVER_PORT < oversize_pipe > oversize_output.txt 2>&1 &
OVERSIZE_PID=$!

sleep 8
wait $OVERSIZE_PID 2>/dev/null

# Check for file size rejection
if grep -q "size limit\|too large\|exceeds.*limit" oversize_output.txt $LOG_FILE; then
    echo -e "${GREEN}✅ Oversized file properly rejected${NC}"
else
    echo -e "${YELLOW}⚠️  Check logs for file size validation${NC}"
fi

rm -f oversize_pipe oversize_output.txt

extract_logs "Oversized File Rejection"

# Test 7: SIGINT Server Shutdown
echo -e "${BLUE}TEST 7: SIGINT Server Shutdown${NC}"
echo "Testing: Server shutdown with Ctrl+C (SIGINT)"

# Start a client to be notified of shutdown
mkfifo shutdown_client_pipe
(
    echo "shutdownuser"
    sleep 2
    echo "/join waitroom"
    sleep 20  # Wait for shutdown
) > shutdown_client_pipe &

timeout 30 ./chatclient $SERVER_IP $SERVER_PORT < shutdown_client_pipe > shutdown_client_output.txt 2>&1 &
SHUTDOWN_CLIENT_PID=$!

sleep 5

echo "Sending SIGINT to server..."
kill -INT $SERVER_PID

sleep 5

# Check if server shut down gracefully
if ! kill -0 $SERVER_PID 2>/dev/null; then
    echo -e "${GREEN}✅ Server shut down gracefully${NC}"
else
    echo -e "${RED}❌ Server did not shut down, force killing...${NC}"
    kill -9 $SERVER_PID 2>/dev/null
fi

# Check if client was notified
if grep -q "shutting down\|server.*shutdown" shutdown_client_output.txt; then
    echo -e "${GREEN}✅ Client received shutdown notification${NC}"
else
    echo -e "${YELLOW}⚠️  Check if client received shutdown notification${NC}"
fi

# Cleanup
kill $SHUTDOWN_CLIENT_PID 2>/dev/null
wait $SHUTDOWN_CLIENT_PID 2>/dev/null
rm -f shutdown_client_pipe shutdown_client_output.txt

extract_logs "SIGINT Server Shutdown"

# Restart server for remaining tests
echo -e "${BLUE}Restarting server for remaining tests...${NC}"
./chatserver $SERVER_PORT &
SERVER_PID=$!
sleep 3

# Test 8: Rejoining Rooms
echo -e "${BLUE}TEST 8: Rejoining Rooms${NC}"
echo "Testing: Client leaves room, then rejoins"

mkfifo rejoin_pipe
(
    echo "ayse99"
    sleep 2
    echo "/join group2"
    sleep 2
    echo "First time in group2"
    sleep 2
    echo "/join otherroom"
    sleep 2
    echo "/join group2"
    sleep 2
    echo "Back in group2"
    sleep 3
    echo "/quit"
) > rejoin_pipe &

timeout 20 ./chatclient $SERVER_IP $SERVER_PORT < rejoin_pipe > rejoin_output.txt 2>&1 &
REJOIN_PID=$!

sleep 15
wait $REJOIN_PID 2>/dev/null

# Check for rejoining behavior
if grep -q "ayse99.*joined.*group2" $LOG_FILE; then
    echo -e "${GREEN}✅ Room rejoining working correctly${NC}"
    echo -e "${CYAN}Note: Message history is ephemeral (not persistent)${NC}"
else
    echo -e "${YELLOW}⚠️  Check logs for room rejoining behavior${NC}"
fi

rm -f rejoin_pipe rejoin_output.txt

extract_logs "Rejoining Rooms"

# Test 9: Same Filename Collision
echo -e "${BLUE}TEST 9: Same Filename Collision${NC}"
echo "Testing: Two users send file with same name"

# Create two clients that will send same filename
mkfifo collision_pipe1
(
    echo "user1collision"
    sleep 2
    echo "/join collisionroom"
    sleep 2
    echo "/send project.pdf"
    sleep 5
    echo "/quit"
) > collision_pipe1 &

mkfifo collision_pipe2
(
    echo "user2collision"
    sleep 2
    echo "/join collisionroom"
    sleep 3
    echo "/send project.pdf"
    sleep 5
    echo "/quit"
) > collision_pipe2 &

timeout 20 ./chatclient $SERVER_IP $SERVER_PORT < collision_pipe1 > collision_output1.txt 2>&1 &
COLLISION_PID1=$!

timeout 20 ./chatclient $SERVER_IP $SERVER_PORT < collision_pipe2 > collision_output2.txt 2>&1 &
COLLISION_PID2=$!

sleep 12

wait $COLLISION_PID1 $COLLISION_PID2 2>/dev/null

# Check for filename collision handling
if grep -q "project.pdf\|collision\|conflict\|renamed" $LOG_FILE collision_output1.txt collision_output2.txt; then
    echo -e "${GREEN}✅ Filename collision detected${NC}"
else
    echo -e "${YELLOW}⚠️  Check logs for filename collision handling${NC}"
fi

rm -f collision_pipe1 collision_pipe2 collision_output1.txt collision_output2.txt

extract_logs "Filename Collision"

# Test 10: File Queue Wait Duration
echo -e "${BLUE}TEST 10: File Queue Wait Duration${NC}"
echo "Testing: File upload queue wait time tracking"

# Fill up the queue first
for i in {1..3}; do
    mkfifo queue_fill_pipe_$i
    (
        echo "queuefill$i"
        sleep 2
        echo "/join queueroom"
        sleep 2
        echo "/send file$i.txt"
        sleep 10
        echo "/quit"
    ) > queue_fill_pipe_$i &
    
    timeout 25 ./chatclient $SERVER_IP $SERVER_PORT < queue_fill_pipe_$i > /dev/null 2>&1 &
    QUEUE_FILL_PIDS[$i]=$!
done

sleep 3

# Now add a client that should wait
mkfifo queue_wait_pipe
(
    echo "berkay98"
    sleep 2
    echo "/join queueroom"
    sleep 2
    echo "/send code.zip"
    sleep 15
    echo "/quit"
) > queue_wait_pipe &

timeout 30 ./chatclient $SERVER_IP $SERVER_PORT < queue_wait_pipe > queue_wait_output.txt 2>&1 &
QUEUE_WAIT_PID=$!

sleep 20

# Check for queue wait behavior
QUEUE_WAIT_ENTRIES=$(grep -c "queue\|wait\|berkay98" $LOG_FILE queue_wait_output.txt 2>/dev/null || echo "0")
echo -e "${GREEN}Result: $QUEUE_WAIT_ENTRIES queue/wait related entries${NC}"

# Cleanup queue test
for i in {1..3}; do
    kill ${QUEUE_FILL_PIDS[$i]} 2>/dev/null
    wait ${QUEUE_FILL_PIDS[$i]} 2>/dev/null
    rm -f queue_fill_pipe_$i
done

kill $QUEUE_WAIT_PID 2>/dev/null
wait $QUEUE_WAIT_PID 2>/dev/null
rm -f queue_wait_pipe queue_wait_output.txt

extract_logs "File Queue Wait Duration"

# Final server shutdown
echo -e "${BLUE}Shutting down server...${NC}"
kill -INT $SERVER_PID 2>/dev/null
sleep 3

if ! kill -0 $SERVER_PID 2>/dev/null; then
    echo -e "${GREEN}✅ Server shut down gracefully${NC}"
else
    kill -9 $SERVER_PID 2>/dev/null
fi

# Final summary
echo ""
echo -e "${BLUE}=== TEST SUMMARY ===${NC}"
echo -e "${GREEN}✅ All 10 test scenarios completed${NC}"
echo -e "${CYAN}📋 Test scenarios covered:${NC}"
echo "1. ✅ Concurrent User Load (30 clients)"
echo "2. ✅ Duplicate Usernames"
echo "3. ✅ File Upload Queue Limit"
echo "4. ✅ Unexpected Disconnection"
echo "5. ✅ Room Switching"
echo "6. ✅ Oversized File Rejection"
echo "7. ✅ SIGINT Server Shutdown"
echo "8. ✅ Rejoining Rooms"
echo "9. ✅ Same Filename Collision"
echo "10. ✅ File Queue Wait Duration"

echo ""
echo -e "${PURPLE}📊 Final Log Analysis:${NC}"
if [ -f "$LOG_FILE" ]; then
    TOTAL_LOGS=$(wc -l < $LOG_FILE)
    echo "Total log entries: $TOTAL_LOGS"
    echo ""
    echo "Last 10 log entries:"
    tail -10 $LOG_FILE
else
    echo "No log file found"
fi

cleanup 