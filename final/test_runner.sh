#!/bin/bash

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

echo -e "${BLUE}=== Multi-threaded Chat Server Test Suite ===${NC}"

# Check if binaries exist
if [ ! -f "./chatserver" ] || [ ! -f "./chatclient" ]; then
    echo -e "${RED}Error: chatserver or chatclient not found. Run 'make' first.${NC}"
    exit 1
fi

# Test configuration
SERVER_PORT=5000
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

# Test 2: Single client connection
echo -e "${BLUE}Test 2: Single client connection${NC}"
echo "testuser1" | timeout 5 ./chatclient $SERVER_IP $SERVER_PORT > /dev/null 2>&1 &
CLIENT1_PID=$!
sleep 2

if kill -0 $CLIENT1_PID 2>/dev/null; then
    echo -e "${GREEN}PASS: Client connected successfully${NC}"
    kill $CLIENT1_PID 2>/dev/null
else
    echo -e "${RED}FAIL: Client connection failed${NC}"
fi

# Test 3: Multiple concurrent connections
echo -e "${BLUE}Test 3: Multiple concurrent connections (10 clients)${NC}"
for i in {1..10}; do
    echo "user$i" | timeout 10 ./chatclient $SERVER_IP $SERVER_PORT > /dev/null 2>&1 &
    PIDS[$i]=$!
done

sleep 3
CONNECTED=0
for i in {1..10}; do
    if kill -0 ${PIDS[$i]} 2>/dev/null; then
        ((CONNECTED++))
        kill ${PIDS[$i]} 2>/dev/null
    fi
done

if [ $CONNECTED -ge 8 ]; then
    echo -e "${GREEN}PASS: $CONNECTED/10 clients connected${NC}"
else
    echo -e "${RED}FAIL: Only $CONNECTED/10 clients connected${NC}"
fi

# Test 4: Duplicate username handling
echo -e "${BLUE}Test 4: Duplicate username handling${NC}"
echo "duplicate" | timeout 5 ./chatclient $SERVER_IP $SERVER_PORT > /dev/null 2>&1 &
DUP1_PID=$!
sleep 1
echo "duplicate" | timeout 5 ./chatclient $SERVER_IP $SERVER_PORT > /dev/null 2>&1 &
DUP2_PID=$!
sleep 2

# Check if second client was rejected
if ! kill -0 $DUP2_PID 2>/dev/null; then
    echo -e "${GREEN}PASS: Duplicate username rejected${NC}"
else
    echo -e "${RED}FAIL: Duplicate username accepted${NC}"
    kill $DUP2_PID 2>/dev/null
fi
kill $DUP1_PID 2>/dev/null

# Test 5: Server capacity limit
echo -e "${BLUE}Test 5: Server capacity limit (attempting 20 connections)${NC}"
for i in {1..20}; do
    echo "capacity$i" | timeout 10 ./chatclient $SERVER_IP $SERVER_PORT > /dev/null 2>&1 &
    CAP_PIDS[$i]=$!
done

sleep 3
CAP_CONNECTED=0
for i in {1..20}; do
    if kill -0 ${CAP_PIDS[$i]} 2>/dev/null; then
        ((CAP_CONNECTED++))
        kill ${CAP_PIDS[$i]} 2>/dev/null
    fi
done

if [ $CAP_CONNECTED -le 15 ]; then
    echo -e "${GREEN}PASS: Server limited connections to $CAP_CONNECTED (max 15)${NC}"
else
    echo -e "${RED}FAIL: Server accepted $CAP_CONNECTED connections (should be max 15)${NC}"
fi

# Test 6: File transfer test
echo -e "${BLUE}Test 6: File transfer functionality${NC}"
# This test requires manual verification or more complex automation
echo -e "${YELLOW}INFO: File transfer test requires manual verification${NC}"
echo -e "${YELLOW}      Test files created in $TEST_DIR/${NC}"

# Test 7: Invalid inputs
echo -e "${BLUE}Test 7: Invalid input handling${NC}"
echo "invalid@user" | timeout 3 ./chatclient $SERVER_IP $SERVER_PORT > /dev/null 2>&1 &
INV_PID=$!
sleep 2

if ! kill -0 $INV_PID 2>/dev/null; then
    echo -e "${GREEN}PASS: Invalid username rejected${NC}"
else
    echo -e "${RED}FAIL: Invalid username accepted${NC}"
    kill $INV_PID 2>/dev/null
fi

# Test 8: Server shutdown handling
echo -e "${BLUE}Test 8: Graceful server shutdown${NC}"
echo "shutdowntest" | timeout 10 ./chatclient $SERVER_IP $SERVER_PORT > /dev/null 2>&1 &
SHUTDOWN_CLIENT=$!
sleep 2

# Send SIGINT to server
kill -INT $SERVER_PID
sleep 3

# Check if server shut down gracefully
if ! kill -0 $SERVER_PID 2>/dev/null; then
    echo -e "${GREEN}PASS: Server shut down gracefully${NC}"
else
    echo -e "${RED}FAIL: Server did not shut down${NC}"
    kill -9 $SERVER_PID 2>/dev/null
fi

# Check if client was notified
if ! kill -0 $SHUTDOWN_CLIENT 2>/dev/null; then
    echo -e "${GREEN}PASS: Client disconnected on server shutdown${NC}"
else
    echo -e "${YELLOW}INFO: Client still connected after server shutdown${NC}"
    kill $SHUTDOWN_CLIENT 2>/dev/null
fi

# Test 9: Log file verification
echo -e "${BLUE}Test 9: Log file verification${NC}"
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

# Test 10: Memory leak check (basic)
echo -e "${BLUE}Test 10: Basic memory usage check${NC}"
echo -e "${YELLOW}INFO: For detailed memory analysis, run with valgrind${NC}"
echo -e "${YELLOW}      Example: valgrind --leak-check=full ./chatserver 5000${NC}"

echo -e "${BLUE}=== Test Summary ===${NC}"
echo -e "${GREEN}All basic functionality tests completed.${NC}"
echo -e "${YELLOW}For comprehensive testing:${NC}"
echo -e "${YELLOW}1. Run server: ./chatserver 5000${NC}"
echo -e "${YELLOW}2. Run multiple clients: ./chatclient 127.0.0.1 5000${NC}"
echo -e "${YELLOW}3. Test room joining, broadcasting, whispers, and file transfers${NC}"

# Cleanup
cleanup 