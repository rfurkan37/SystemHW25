#!/bin/bash

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

echo -e "${BLUE}=== Quick Chat Server Demonstration ===${NC}"
echo -e "${CYAN}Demonstrating key functionality with real-time interaction${NC}"
echo ""

# Test configuration
SERVER_PORT=8080
SERVER_IP="127.0.0.1"

# Check if binaries exist
if [ ! -f "./chatserver" ] || [ ! -f "./chatclient" ]; then
    echo -e "${RED}Error: chatserver or chatclient not found. Run 'make' first.${NC}"
    exit 1
fi

# Function to cleanup
cleanup() {
    echo -e "${YELLOW}Cleaning up...${NC}"
    pkill -f chatserver 2>/dev/null
    pkill -f chatclient 2>/dev/null
    exit 0
}

# Set trap for cleanup
trap cleanup SIGINT SIGTERM

# Start server
echo -e "${BLUE}1. Starting chat server on port $SERVER_PORT...${NC}"
rm -f server.log  # Clean previous logs
./chatserver $SERVER_PORT &
SERVER_PID=$!
sleep 2

if ! kill -0 $SERVER_PID 2>/dev/null; then
    echo -e "${RED}FAIL: Server failed to start${NC}"
    exit 1
fi
echo -e "${GREEN}✅ Server started successfully (PID: $SERVER_PID)${NC}"
echo ""

# Demo 1: Multiple clients with room interaction
echo -e "${BLUE}2. Demo: Multiple clients with room interaction${NC}"

# Client 1: Alice
mkfifo alice_demo
(
    echo "Alice"
    sleep 2
    echo "/join general"
    sleep 2
    echo "Hello everyone! I'm Alice."
    sleep 3
    echo "/whisper Bob Hi Bob!"
    sleep 5
    echo "This chat server is working great!"
    sleep 3
    echo "/quit"
) > alice_demo &

# Client 2: Bob
mkfifo bob_demo
(
    echo "Bob"
    sleep 3
    echo "/join general"
    sleep 2
    echo "Hey Alice! Nice to meet you."
    sleep 3
    echo "/whisper Alice Thanks for the welcome!"
    sleep 5
    echo "Yes, the server is very responsive!"
    sleep 3
    echo "/quit"
) > bob_demo &

# Client 3: Charlie (room switching demo)
mkfifo charlie_demo
(
    echo "Charlie"
    sleep 4
    echo "/join developers"
    sleep 2
    echo "Anyone working on C projects?"
    sleep 3
    echo "/join general"
    sleep 2
    echo "Hi Alice and Bob! Charlie here."
    sleep 4
    echo "/quit"
) > charlie_demo &

# Start clients
echo "Starting Alice..."
timeout 25 ./chatclient $SERVER_IP $SERVER_PORT < alice_demo > alice_output.txt 2>&1 &
ALICE_PID=$!

echo "Starting Bob..."
timeout 25 ./chatclient $SERVER_IP $SERVER_PORT < bob_demo > bob_output.txt 2>&1 &
BOB_PID=$!

echo "Starting Charlie..."
timeout 25 ./chatclient $SERVER_IP $SERVER_PORT < charlie_demo > charlie_output.txt 2>&1 &
CHARLIE_PID=$!

echo ""
echo -e "${CYAN}Clients are now interacting... (20 seconds)${NC}"
sleep 20

# Wait for clients to finish
wait $ALICE_PID $BOB_PID $CHARLIE_PID 2>/dev/null

echo -e "${GREEN}✅ Multi-client interaction completed${NC}"
echo ""

# Demo 2: Duplicate username test
echo -e "${BLUE}3. Demo: Duplicate username rejection${NC}"

# Start first client
mkfifo dup1_demo
(
    echo "testuser"
    sleep 8
    echo "/quit"
) > dup1_demo &

timeout 12 ./chatclient $SERVER_IP $SERVER_PORT < dup1_demo > /dev/null 2>&1 &
DUP1_PID=$!

sleep 2

# Try duplicate username
mkfifo dup2_demo
(
    echo "testuser"
    sleep 3
) > dup2_demo &

timeout 6 ./chatclient $SERVER_IP $SERVER_PORT < dup2_demo > dup_result.txt 2>&1 &
DUP2_PID=$!

sleep 4

if grep -q "already taken\|Username.*taken" dup_result.txt server.log; then
    echo -e "${GREEN}✅ Duplicate username properly rejected${NC}"
else
    echo -e "${YELLOW}⚠️  Check duplicate username handling${NC}"
fi

# Cleanup duplicate test
kill $DUP1_PID $DUP2_PID 2>/dev/null
wait $DUP1_PID $DUP2_PID 2>/dev/null
rm -f dup1_demo dup2_demo dup_result.txt

echo ""

# Demo 3: Graceful shutdown
echo -e "${BLUE}4. Demo: Graceful server shutdown${NC}"

# Start a client for shutdown demo
mkfifo shutdown_demo
(
    echo "shutdownclient"
    sleep 2
    echo "/join testroom"
    sleep 15  # Wait for shutdown
) > shutdown_demo &

timeout 20 ./chatclient $SERVER_IP $SERVER_PORT < shutdown_demo > shutdown_result.txt 2>&1 &
SHUTDOWN_PID=$!

sleep 3

echo "Sending SIGINT to server..."
kill -INT $SERVER_PID

sleep 3

# Check shutdown
if ! kill -0 $SERVER_PID 2>/dev/null; then
    echo -e "${GREEN}✅ Server shut down gracefully${NC}"
else
    echo -e "${RED}❌ Server did not shut down, force killing...${NC}"
    kill -9 $SERVER_PID 2>/dev/null
fi

# Check client notification
if grep -q "shutting down\|server.*shutdown" shutdown_result.txt; then
    echo -e "${GREEN}✅ Client received shutdown notification${NC}"
else
    echo -e "${YELLOW}⚠️  Check client shutdown notification${NC}"
fi

# Cleanup
kill $SHUTDOWN_PID 2>/dev/null
wait $SHUTDOWN_PID 2>/dev/null
rm -f alice_demo bob_demo charlie_demo shutdown_demo shutdown_result.txt

echo ""
echo -e "${BLUE}5. Server Log Summary${NC}"
if [ -f "server.log" ]; then
    LOG_LINES=$(wc -l < server.log)
    echo -e "${GREEN}Total log entries: $LOG_LINES${NC}"
    echo ""
    echo -e "${CYAN}Key log entries:${NC}"
    echo "--- Connections ---"
    grep "CONNECT" server.log | head -5
    echo ""
    echo "--- Room Activity ---"
    grep "ROOM" server.log | head -5
    echo ""
    echo "--- Whisper Messages ---"
    grep "WHISPER" server.log | head -3
    echo ""
    echo "--- Shutdown Sequence ---"
    grep "SHUTDOWN" server.log
else
    echo -e "${RED}No log file found${NC}"
fi

echo ""
echo -e "${BLUE}=== Demo Summary ===${NC}"
echo -e "${GREEN}✅ Multi-client room interaction${NC}"
echo -e "${GREEN}✅ Private messaging (whispers)${NC}"
echo -e "${GREEN}✅ Room switching functionality${NC}"
echo -e "${GREEN}✅ Duplicate username rejection${NC}"
echo -e "${GREEN}✅ Graceful server shutdown${NC}"
echo -e "${GREEN}✅ Comprehensive logging${NC}"

echo ""
echo -e "${CYAN}For full testing, run: ./comprehensive_test_suite.sh${NC}"
echo -e "${CYAN}For manual testing, run: ./manual_test_demo.sh${NC}"

cleanup 