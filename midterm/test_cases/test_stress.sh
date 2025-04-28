#!/bin/bash
# test_stress.sh - Tests the system under heavy load

echo "[DEBUG] Cleaning up previous log file..."
rm -f AdaBank.bankLog # Ensure clean state

echo "Creating test client files..."

# Number of clients to run simultaneously
NUM_CLIENTS=20
# Number of operations per client
NUM_OPS=50

# Create client files
for i in $(seq 1 $NUM_CLIENTS); do
    # First create the account
    echo "N deposit 1000" > stress_client${i}.file

    # Add alternating deposit/withdraw operations
    for j in $(seq 1 $NUM_OPS); do
        # Calculate the BankID for this client (0-indexed)
        BANK_ID=$(($i-1))
        if [ $((j % 2)) -eq 0 ]; then
            # Use printf for consistent formatting (e.g., BankID_00, BankID_01, ...)
            printf "BankID_%02d withdraw 10\n" $BANK_ID >> stress_client${i}.file
        else
            printf "BankID_%02d deposit 10\n" $BANK_ID >> stress_client${i}.file
        fi
    done
done

echo "Starting bank server..."
./bank_server AdaBank &
SERVER_PID=$!

# Wait for server to initialize
sleep 1

echo "Starting $NUM_CLIENTS clients with $NUM_OPS operations each..."
# Array to track client PIDs
CLIENT_PIDS=()

# Start time
START_TIME=$(date +%s)

# Start all clients in parallel
for i in $(seq 1 $NUM_CLIENTS); do
    ./bank_client stress_client${i}.file AdaBank &
    CLIENT_PIDS+=($!)
done

# Wait for all clients to finish
echo "Waiting for clients to complete..."
CLIENT_FAIL=0
for pid in "${CLIENT_PIDS[@]}"; do
    wait $pid
    STATUS=$?
    if [ $STATUS -ne 0 ]; then
        echo "ERROR: Client with PID $pid failed with status $STATUS"
        CLIENT_FAIL=1
    fi
done

# End time
END_TIME=$(date +%s)
ELAPSED=$((END_TIME - START_TIME))

if [ $CLIENT_FAIL -ne 0 ]; then
    echo "ERROR: One or more clients failed. Stopping server."
    kill -SIGINT $SERVER_PID
    exit 1
fi

echo "All clients completed successfully in $ELAPSED seconds."
TOTAL_OPS=$((NUM_CLIENTS * (NUM_OPS + 1)))
echo "Total operations processed: $TOTAL_OPS"

# Check if bc is installed before trying to use it
if command -v bc &> /dev/null; then
    if [ $ELAPSED -gt 0 ]; then
        OPS_PER_SEC=$(echo "scale=2; $TOTAL_OPS / $ELAPSED" | bc)
        echo "Operations per second: $OPS_PER_SEC"
    else
         echo "Operations per second: N/A (elapsed time was zero)"
    fi
else
    echo "WARN: 'bc' command not found. Cannot calculate operations per second."
    echo "      Install 'bc' (e.g., 'sudo apt install bc' or 'sudo yum install bc') to see this metric."
fi


echo "Gracefully stopping server..."
kill -SIGINT $SERVER_PID
wait $SERVER_PID

echo "Checking log file..."
if [ -f AdaBank.bankLog ]; then
    echo "Log file exists."
    echo "Final log contents:"
    cat AdaBank.bankLog

    # Count active accounts (should be $NUM_CLIENTS)
    # Use a more precise grep if possible, matching the start of the line
    ACTIVE_ACCOUNTS=$(grep -c "^BankID_" AdaBank.bankLog)

    echo "[DEBUG] Found $ACTIVE_ACCOUNTS accounts in log."

    if [ "$ACTIVE_ACCOUNTS" -eq "$NUM_CLIENTS" ]; then
        echo "Found $ACTIVE_ACCOUNTS active accounts as expected."
        echo "Verifying final balances (should all be 1000)..."
        # Print expected state for manual verification
        ALL_CORRECT=1
        for i in $(seq 0 $(($NUM_CLIENTS-1))); do
             # Format BankID consistently (e.g., BankID_00, BankID_01)
             BANK_ID_STR=$(printf "BankID_%02d" $i)
             # Extract balance - adjust awk/grep based on exact log format
             # This assumes the balance is the last field on the line starting with the BankID
             BALANCE=$(grep "^${BANK_ID_STR}[^0-9]" AdaBank.bankLog | awk '{print $NF}')

             if [ "$BALANCE" == "1000" ]; then
                 echo "[VERIFY] Account $BANK_ID_STR has expected balance 1000."
             else
                 echo "[VERIFY FAILED] Account $BANK_ID_STR has balance '$BALANCE', expected 1000."
                 ALL_CORRECT=0
             fi
        done
        if [ $ALL_CORRECT -eq 1 ]; then
             echo "[VERIFICATION] All checked accounts have the expected balance."
        else
             echo "[VERIFICATION] One or more accounts have unexpected balances. Please check log."
        fi
    else
        echo "ERROR: Expected $NUM_CLIENTS active accounts, found $ACTIVE_ACCOUNTS. Cannot verify balances."
        # Exit here as the number of accounts is wrong, indicating a fundamental issue
        exit 1
    fi
else
    echo "ERROR: Log file doesn't exist."
    exit 1
fi

echo "Stress test finished. Please review log and verification messages above."
exit 0