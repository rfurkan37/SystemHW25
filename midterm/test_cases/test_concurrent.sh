#!/bin/bash
# test_concurrent.sh - Tests multiple clients connecting simultaneously

echo "Creating test client files..."

# Create 10 client files with various operations
NUM_CLIENTS=10
for i in $(seq 1 $NUM_CLIENTS); do
    cat >client${i}.file <<EOF
N deposit $((100 * $i))
BankID_$(($i - 1)) deposit 50
BankID_$(($i - 1)) withdraw 25
EOF
done

echo "Starting bank server..."
./bank_server AdaBank &
SERVER_PID=$!

# Wait for server to initialize
sleep 1

echo "Starting $NUM_CLIENTS clients simultaneously..."
# Array to track client PIDs
CLIENT_PIDS=()

# Start all clients in parallel
for i in $(seq 1 $NUM_CLIENTS); do
    ./bank_client client${i}.file AdaBank &
    CLIENT_PIDS+=($!)
done

# Wait for all clients to finish
echo "Waiting for clients to complete..."
for pid in "${CLIENT_PIDS[@]}"; do
    wait $pid
    STATUS=$?
    if [ $STATUS -ne 0 ]; then
        echo "ERROR: Client with PID $pid failed with status $STATUS"
        kill -SIGINT $SERVER_PID
        exit 1
    fi
done

echo "All clients completed successfully."

echo "Gracefully stopping server..."
kill -SIGINT $SERVER_PID
wait $SERVER_PID

echo "Checking log file..."
if [ -f AdaBank.bankLog ]; then
    echo "Log file exists."

    # Count active accounts (should be $NUM_CLIENTS)
    ACTIVE_ACCOUNTS=$(grep -v "^#" AdaBank.bankLog | grep "BankID_" | wc -l)

    if [ $ACTIVE_ACCOUNTS -eq $NUM_CLIENTS ]; then
        echo "Found $ACTIVE_ACCOUNTS active accounts as expected."
    else
        echo "ERROR: Expected $NUM_CLIENTS active accounts, found $ACTIVE_ACCOUNTS"
        exit 1
    fi

    # Verify each account has the expected transaction pattern
    for i in $(seq 0 $(($NUM_CLIENTS - 1))); do
        ACCOUNT_LINE=$(grep "BankID_0${i}" AdaBank.bankLog)

        if [ -n "$ACCOUNT_LINE" ]; then
            # Count operations (initial deposit (D), +deposit (D), -withdraw (W))
            DEPOSIT_COUNT=$(echo "$ACCOUNT_LINE" | grep -o "D" | wc -l)
            WITHDRAW_COUNT=$(echo "$ACCOUNT_LINE" | grep -o "W" | wc -l)

            echo "Account $i has $DEPOSIT_COUNT deposits and $WITHDRAW_COUNT withdrawals"

            # Ensure at least the initial deposit happened
            if [ $DEPOSIT_COUNT -ge 1 ]; then
                echo "Account $i initialized successfully"
            else
                echo "ERROR: Account $i missing initial deposit"
                exit 1
            fi
        else
            echo "ERROR: Account $i doesn't exist in the log"
            exit 1
        fi
    done
else
    echo "ERROR: Log file doesn't exist."
    exit 1
fi

echo "Concurrent clients test passed!"
exit 0
