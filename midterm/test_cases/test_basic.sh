#!/bin/bash
# test_basic.sh - Tests basic deposit and withdrawal functionality

echo "Creating test client files..."

# Create a client file with basic operations
cat > client1.file << EOF
N deposit 100
BankID_0 deposit 50
BankID_0 withdraw 75
BankID_0 withdraw 75
EOF

# Create a second client file
cat > client2.file << EOF
N deposit 200
BankID_1 withdraw 50
EOF

echo "Starting bank server..."
./bank_server AdaBank &
SERVER_PID=$!

# Wait for server to initialize
sleep 1

echo "Running first client..."
./bank_client client1.file AdaBank
CLIENT1_RESULT=$?

echo "Running second client..."
./bank_client client2.file AdaBank
CLIENT2_RESULT=$?

echo "Gracefully stopping server..."
kill -SIGINT $SERVER_PID
wait $SERVER_PID
SERVER_EXIT=$?

echo "Checking log file..."
if [ -f AdaBank.bankLog ]; then
    echo "Log file exists."
    
    # Check if BankID_0 is closed (should be prefixed with #)
    if grep -q "^# BankID_00" AdaBank.bankLog; then
        echo "Account 0 correctly closed."
    else
        echo "ERROR: Account 0 should be closed, but isn't marked as such."
        exit 1
    fi
    
    # Check if BankID_1 is active with balance 150
    if grep -q "BankID_01.*150$" AdaBank.bankLog; then
        echo "Account 1 has correct final balance."
    else
        echo "ERROR: Account 1 doesn't have the expected balance."
        exit 1
    fi
else
    echo "ERROR: Log file doesn't exist."
    exit 1
fi

# Check client exit statuses
if [ $CLIENT1_RESULT -ne 0 ] || [ $CLIENT2_RESULT -ne 0 ]; then
    echo "ERROR: One or more clients failed."
    exit 1
fi

echo "Basic functionality test passed!"
exit 0