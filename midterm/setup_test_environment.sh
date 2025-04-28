#!/bin/bash
# setup_test_environment.sh - Creates the test directory structure and copies scripts

# Create test directory structure
mkdir -p test_cases

# Move test scripts to test_cases directory
cp test_basic.sh test_cases/
cp test_concurrent.sh test_cases/
cp test_error_handling.sh test_cases/
cp test_recovery.sh test_cases/
cp test_signal_handling.sh test_cases/
cp test_stress.sh test_cases/

# Make all test scripts executable
chmod +x test_cases/*.sh
chmod +x test_suite.sh

echo "Test environment setup complete. Run ./test_suite.sh to execute all tests."