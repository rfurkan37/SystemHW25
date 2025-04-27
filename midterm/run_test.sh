#!/bin/bash

# Basic test script for AdaBank simulation

SERVER_EXE="./bank_server"
CLIENT_EXE="./bank_client"
SERVER_FIFO="AdaBank" # Must match the name passed to the server
LOG_FILE="AdaBank.bankLog" # Must match LOG_FILE_NAME in common.h
CLIENT1_CMDS="client_01.txt"
CLIENT2_CMDS="client_02.txt"
CLIENT1_OUT="client_01.out"
CLIENT2_OUT="client_02.out"

echo "--- AdaBank Test ---"

# --- 1. Clean and Compile ---
echo "[1] Cleaning previous run..."
make clean > /dev/null
# Clean specific output files
rm -f $CLIENT1_OUT $CLIENT2_OUT

echo "[1] Compiling source code..."
make
if [ $? -ne 0 ]; then
    echo "!!! Compilation Failed !!!"
    exit 1
fi
echo "[1] Compilation successful."

# --- 2. Create Client Command Files ---
# (Assumes client_01.txt and client_02.txt are already created manually)
if [ ! -f "$CLIENT1_CMDS" ] || [ ! -f "$CLIENT2_CMDS" ]; then
    echo "!!! Client command files ($CLIENT1_CMDS, $CLIENT2_CMDS) not found! Create them first."
    # You could generate them here using cat << EOF > filename if needed
    exit 1
fi
echo "[2] Using client command files: $CLIENT1_CMDS, $CLIENT2_CMDS"

# --- 3. Start Server ---
echo "[3] Starting Bank Server ($SERVER_EXE $SERVER_FIFO) in background..."
$SERVER_EXE $SERVER_FIFO &
SERVER_PID=$!
echo "[3] Server PID: $SERVER_PID"

# Give the server a moment to initialize (create FIFO, SHM, etc.)
# A better approach would be to poll for the FIFO/SHM existence.
echo "[3] Waiting 2 seconds for server initialization..."
sleep 2

# Check if server FIFO exists
if [ ! -p "$SERVER_FIFO" ]; then
    echo "!!! Server FIFO '$SERVER_FIFO' not found after waiting!"
    echo "!!! Server might have failed to start."
    # Try to kill server if it's running somehow
    kill $SERVER_PID 2>/dev/null
    wait $SERVER_PID 2>/dev/null
    exit 1
fi
echo "[3] Server FIFO found. Server likely running."

# --- 4. Start Clients ---
echo "[4] Starting Client 1 ($CLIENT_EXE $CLIENT1_CMDS)..."
$CLIENT_EXE $CLIENT1_CMDS > $CLIENT1_OUT &
CLIENT1_PID=$!

echo "[4] Starting Client 2 ($CLIENT_EXE $CLIENT2_CMDS)..."
$CLIENT_EXE $CLIENT2_CMDS > $CLIENT2_OUT &
CLIENT2_PID=$!

echo "[4] Client PIDs: Client1=$CLIENT1_PID, Client2=$CLIENT2_PID"

# --- 5. Wait for Clients ---
echo "[5] Waiting for clients to finish..."
wait $CLIENT1_PID
CLIENT1_EXIT=$?
wait $CLIENT2_PID
CLIENT2_EXIT=$?
echo "[5] Clients finished. Exit Statuses: Client1=$CLIENT1_EXIT, Client2=$CLIENT2_EXIT"

# Display Client Output
echo "--- Client 1 Output ($CLIENT1_OUT): ---"
cat $CLIENT1_OUT
echo "---------------------------------------"
echo "--- Client 2 Output ($CLIENT2_OUT): ---"
cat $CLIENT2_OUT
echo "---------------------------------------"


# --- 6. Stop Server ---
echo "[6] Sending SIGINT to Server (PID $SERVER_PID) for graceful shutdown..."
kill -INT $SERVER_PID
if [ $? -ne 0 ]; then
    echo "!!! Failed to send SIGINT to server PID $SERVER_PID. Was it still running?"
fi

echo "[6] Waiting for server (PID $SERVER_PID) to terminate..."
# Wait for the server process to actually terminate
# Use a timeout in case it hangs
TIMEOUT=10 # seconds
ELAPSED=0
while kill -0 $SERVER_PID 2>/dev/null; do
    sleep 1
    ELAPSED=$((ELAPSED + 1))
    if [ $ELAPSED -ge $TIMEOUT ]; then
        echo "!!! Server did not terminate after $TIMEOUT seconds! Sending SIGKILL..."
        kill -KILL $SERVER_PID 2>/dev/null
        break
    fi
done

if kill -0 $SERVER_PID 2>/dev/null; then
     echo "[6] Server (PID $SERVER_PID) failed to terminate even after SIGKILL."
else
     echo "[6] Server (PID $SERVER_PID) terminated."
fi
# Optional: Check server exit status if wait works after SIGINT
# wait $SERVER_PID
# SERVER_EXIT=$?
# echo "[6] Server exited with status: $SERVER_EXIT"


# --- 7. Check Log File ---
echo "[7] Final Log File ($LOG_FILE) content:"
echo "--- START LOG ---"
if [ -f "$LOG_FILE" ]; then
    cat $LOG_FILE
else
    echo "!!! Log file '$LOG_FILE' not found!"
fi
echo "--- END LOG ---"


echo "--- Test Complete ---"
exit 0