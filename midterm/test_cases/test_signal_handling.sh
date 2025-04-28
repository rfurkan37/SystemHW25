#!/bin/bash
# test_signal_handling.sh - Tests the system's response to signals

echo "Creating test client files..."

# Create a client file with a large number of operations to ensure it runs for a while
cat > signal_client.file << EOF
N deposit 100
EOF

# Create a script to generate many operations
for i in $(seq 1 100); do
    echo "BankID_0 deposit 10" >> signal_client.file
done

echo "Starting bank server..."
./bank_server AdaBank &
SERVER_PID=$!

# Wait for server to initialize
sleep 1

echo "Starting client in background..."
./bank_client signal_client.file AdaBank &
CLIENT_PID=$!

# Wait a bit to ensure client is running
sleep 2

# Send SIGINT to the client
echo "Sending SIGINT to client (PID $CLIENT_PID)..."
kill -SIGINT $CLIENT_PID

# Wait for client to handle signal and exit
wait $CLIENT_PID
CLIENT_EXIT=$?

echo "Client exited with status $CLIENT_EXIT after receiving SIGINT"

# Check if server is still running
if ps -p $SERVER_PID > /dev/null; then
    echo "Server survived client termination as expected."
else
    echo "ERROR: Server died after client received signal."
    exit 1
fi

# Create a client to check if server is still functional
cat > check_client.file << EOF
BankID_0 withdraw 50
EOF

echo "Running check client to verify server functionality..."
./bank_client check_client.file AdaBank
CHECK_RESULT=$?

if [ $CHECK_RESULT -ne 0 ]; then
    echo "ERROR: Check client failed with status $CHECK_RESULT"
    kill -SIGINT $SERVER_PID
    exit 1
fi

echo "Gracefully stopping server..."
kill -SIGINT $SERVER_PID
wait $SERVER_PID
SERVER_EXIT=$?

echo "Server exited with status $SERVER_EXIT after receiving SIGINT"

echo "Checking log file..."
if [ -f AdaBank.bankLog ]; then
    # Account should exist with partial operations completed
    if grep -q "BankID_00" AdaBank.bankLog; then
        echo "Account exists in log file."
        
        # Extract the final balance (last number on the line)
        BALANCE=$(grep "BankID_00" AdaBank.bankLog | awk '{print $NF}')
        echo "Final balance: $BALANCE"
        
        # Balance should be at least 100 (initial deposit) and less than 1100 (all operations)
        # Since we interrupted the client, not all deposits may have completed
        if [ $BALANCE -ge 50 ] && [ $BALANCE -lt 1100 ]; then
            echo "Balance is within expected range after interruption."
        else
            echo "ERROR: Balance $BALANCE is outside expected range."
            exit 1
        fi
    else
        echo "ERROR: Account not found in log file."
        exit 1
    fi
else
    echo "ERROR: Log file doesn't exist."
    exit 1
fi

echo "Signal handling test passed!"
exit 0