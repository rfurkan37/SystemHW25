#!/bin/bash
# test_recovery.sh - Tests the system's ability to recover from crashes

echo "[DEBUG] Starting test_recovery.sh script..."

echo "[DEBUG] Creating setup_client.file..."
cat > setup_client.file << EOF
N deposit 1000
N deposit 2000
EOF
echo "[DEBUG] Contents of setup_client.file:"
cat setup_client.file

echo "[DEBUG] Creating recovery_client.file..."
cat > recovery_client.file << EOF
BankID_0 deposit 500
BankID_1 withdraw 1000
EOF
echo "[DEBUG] Contents of recovery_client.file:"
cat recovery_client.file

echo "[DEBUG] Starting bank server in the background..."
./bank_server AdaBank &
SERVER_PID=$!
echo "[DEBUG] Bank server started with PID: $SERVER_PID"

# Wait for server to initialize
echo "[DEBUG] Waiting for server to initialize (1 second)..."
sleep 1

echo "[DEBUG] Running setup client..."
./bank_client setup_client.file AdaBank
SETUP_RESULT=$?
echo "[DEBUG] Setup client finished with exit code: $SETUP_RESULT"

if [ $SETUP_RESULT -ne 0 ]; then
    echo "ERROR: Setup client failed with status $SETUP_RESULT"
    echo "[DEBUG] Sending SIGINT to server PID $SERVER_PID due to setup failure..."
    kill -SIGINT $SERVER_PID
    exit 1
fi

# Don't attempt to check the log during runtime
echo "[DEBUG] Setup completed successfully."

echo "[DEBUG] Abruptly terminating server (simulating crash) with SIGKILL on PID $SERVER_PID..."
kill -KILL $SERVER_PID
echo "[DEBUG] Waiting after kill -KILL (1 second)..."
sleep 1

# The log should now be in its permanent format
echo "[DEBUG] Checking log state after crash..."
if [ -f AdaBank.bankLog ]; then
    echo "[DEBUG] Log file AdaBank.bankLog exists after crash."
    echo "[DEBUG] Contents of AdaBank.bankLog after crash:"
    cat AdaBank.bankLog
else
    echo "ERROR: Log file AdaBank.bankLog doesn't exist after initial run and crash."
    exit 1
fi

echo "[DEBUG] Starting server again to test recovery..."
./bank_server AdaBank &
SERVER_PID=$!
echo "[DEBUG] Bank server restarted with PID: $SERVER_PID"
echo "[DEBUG] Waiting for restarted server to initialize (1 second)..."
sleep 1

echo "[DEBUG] Running recovery client..."
./bank_client recovery_client.file AdaBank
RECOVERY_RESULT=$?
echo "[DEBUG] Recovery client finished with exit code: $RECOVERY_RESULT"

if [ $RECOVERY_RESULT -ne 0 ]; then
    echo "ERROR: Recovery client failed with status $RECOVERY_RESULT"
    echo "[DEBUG] Sending SIGINT to server PID $SERVER_PID due to recovery failure..."
    kill -SIGINT $SERVER_PID
    exit 1
fi

echo "[DEBUG] Gracefully stopping server with SIGINT on PID $SERVER_PID..."
kill -SIGINT $SERVER_PID
echo "[DEBUG] Waiting for server PID $SERVER_PID to terminate..."
wait $SERVER_PID
echo "[DEBUG] Server PID $SERVER_PID terminated."

echo "[DEBUG] Checking final log state..."
if [ -f AdaBank.bankLog ]; then
    echo "[DEBUG] Final log file AdaBank.bankLog exists."
    echo "[DEBUG] Final log contents:"
    cat AdaBank.bankLog

    # Basic check for expected content without strict format checking
    # Now using the correct account IDs (BankID_10 and BankID_11)
    echo "[DEBUG] Checking final log for expected account states..."
    if grep -q "BankID_00.*1500" AdaBank.bankLog && \
       grep -q "BankID_01.*1000" AdaBank.bankLog; then
        echo "[DEBUG] Final account state verified after recovery."
    else
        echo "ERROR: Final account state not as expected after recovery."
        # Optionally print the expected vs actual for clarity
        echo "[DEBUG] Expected approximately: BankID_00 ... 1500 and BankID_01 ... 1000" # Updated expected IDs here too
        exit 1
    fi
else
    echo "ERROR: Log file AdaBank.bankLog doesn't exist after recovery."
    exit 1
fi

echo "[DEBUG] Recovery test passed!"
exit 0