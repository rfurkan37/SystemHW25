#!/bin/bash
# test_suite.sh - Main test script for AdaBank Simulator
# This script runs a series of test cases to validate the system

# Colors for better readability
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Function to clean up previous test runs
cleanup() {
    echo -e "${YELLOW}Cleaning up previous test runs...${NC}"
    killall bank_server 2>/dev/null
    killall bank_client 2>/dev/null
    rm -f AdaBank.bankLog
    rm -f /tmp/bank_*_req
    rm -f /tmp/bank_*_res
    # Remove shared memory segments
    rm -f /dev/shm/adabank_shm 2>/dev/null
    sleep 1
}

# Function to run a test and report results
run_test() {
    TEST_NAME=$1
    TEST_SCRIPT=$2
    
    echo -e "${YELLOW}======================================${NC}"
    echo -e "${YELLOW}Running test: ${TEST_NAME}${NC}"
    echo -e "${YELLOW}======================================${NC}"
    
    # Run the test script
    if bash "$TEST_SCRIPT"; then
        echo -e "${GREEN}Test passed: ${TEST_NAME}${NC}"
        return 0
    else
        echo -e "${RED}Test failed: ${TEST_NAME}${NC}"
        return 1
    fi
}

# Make sure all scripts are executable
chmod +x *.sh

# Clean up before running tests
cleanup

# Run all tests
TESTS_PASSED=0
TESTS_FAILED=0

for test_script in test_cases/test_*.sh; do
    # Extract test name from filename
    test_name=$(basename "$test_script" .sh | sed 's/test_//')
    
    # Run the test
    if run_test "$test_name" "$test_script"; then
        ((TESTS_PASSED++))
    else
        ((TESTS_FAILED++))
    fi
    
    # Clean up after each test
    cleanup
done

# Report overall results
echo -e "${YELLOW}======================================${NC}"
echo -e "${YELLOW}Test Summary:${NC}"
echo -e "${GREEN}Tests passed: ${TESTS_PASSED}${NC}"
echo -e "${RED}Tests failed: ${TESTS_FAILED}${NC}"
echo -e "${YELLOW}======================================${NC}"

if [ $TESTS_FAILED -eq 0 ]; then
    echo -e "${GREEN}All tests passed!${NC}"
    exit 0
else
    echo -e "${RED}Some tests failed.${NC}"
    exit 1
fi