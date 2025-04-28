#!/bin/bash
# test_memory_leaks.sh - Tests for memory leaks using Valgrind

# Check if Valgrind is installed
if ! command -v valgrind &> /dev/null; then
    echo "Valgrind is not installed. Please install it first:"
    echo "  Ubuntu/Debian: sudo apt-get install valgrind"
    echo "  CentOS/RHEL: sudo yum install valgrind"
    exit 1
fi

echo "Creating test client file..."
cat > memtest_client.file << EOF
N deposit 100
BankID_0 deposit 200
BankID_0 withdraw 50
EOF

echo "Starting bank server with Valgrind..."
valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes --verbose \
    --log-file=valgrind_server.log ./bank_server AdaBank &
SERVER_PID=$!

# Wait for server to initialize (may take longer under Valgrind)
sleep 3

echo "Running client..."
./bank_client memtest_client.file AdaBank
CLIENT_RESULT=$?

if [ $CLIENT_RESULT -ne 0 ]; then
    echo "ERROR: Client failed with status $CLIENT_RESULT"
    kill -SIGINT $SERVER_PID
    exit 1
fi

echo "Gracefully stopping server..."
kill -SIGINT $SERVER_PID
wait $SERVER_PID

echo "Checking Valgrind output for server..."
if grep -q "ERROR SUMMARY: 0 errors" valgrind_server.log; then
    echo "No memory errors detected in server."
else
    echo "Memory errors detected in server. See valgrind_server.log for details."
    # Extract and show leak summary
    grep -A 5 "LEAK SUMMARY" valgrind_server.log
    exit 1
fi

echo "Testing client with Valgrind..."
valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes --verbose \
    --log-file=valgrind_client.log ./bank_server AdaBank &
SERVER_PID=$!

# Wait for server to initialize
sleep 3

echo "Running client with Valgrind..."
valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes --verbose \
    --log-file=valgrind_client.log ./bank_client memtest_client.file AdaBank
CLIENT_RESULT=$?

if [ $CLIENT_RESULT -ne 0 ]; then
    echo "ERROR: Client under Valgrind failed with status $CLIENT_RESULT"
    kill -SIGINT $SERVER_PID
    exit 1
fi

echo "Gracefully stopping server..."
kill -SIGINT $SERVER_PID
wait $SERVER_PID

echo "Checking Valgrind output for client..."
if grep -q "ERROR SUMMARY: 0 errors" valgrind_client.log; then
    echo "No memory errors detected in client."
else
    echo "Memory errors detected in client. See valgrind_client.log for details."
    # Extract and show leak summary
    grep -A 5 "LEAK SUMMARY" valgrind_client.log
    exit 1
fi

echo "Memory leak test passed!"
exit 0