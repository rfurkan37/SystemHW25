CC = gcc
CFLAGS = -Wall -Wextra -g -pthread
LIBS = -lrt

COMMON_DEPS = common.h
COMMON_SRCS = 

SERVER_SRCS = bank_server.c
CLIENT_SRCS = bank_client.c
TELLER_SRCS = teller.c

SERVER_BIN = bank_server
CLIENT_BIN = bank_client

TEST_SCRIPTS = test_suite.sh \
               test_cases/test_basic.sh \
               test_cases/test_concurrent.sh \
               test_cases/test_error_handling.sh \
               test_cases/test_recovery.sh \
               test_cases/test_signal_handling.sh \
               test_cases/test_stress.sh \
               test_memory_leaks.sh

.PHONY: all clean test setup_tests

all: $(SERVER_BIN) $(CLIENT_BIN) setup_tests

$(SERVER_BIN): $(SERVER_SRCS) $(TELLER_SRCS) $(COMMON_DEPS)
	$(CC) $(CFLAGS) -o $@ $(SERVER_SRCS) $(TELLER_SRCS) $(COMMON_SRCS) $(LIBS)

$(CLIENT_BIN): $(CLIENT_SRCS) $(COMMON_DEPS)
	$(CC) $(CFLAGS) -o $@ $(CLIENT_SRCS) $(COMMON_SRCS) $(LIBS)

# Create test directory structure if it doesn't exist
setup_tests:
	@mkdir -p test_cases
	@echo "Making test scripts executable..."
	@chmod +x $(TEST_SCRIPTS) 2>/dev/null || true
	@echo "Test scripts are ready for execution."

# Clean up compiled binaries, log files, and temporary files
clean:
	rm -f $(SERVER_BIN) $(CLIENT_BIN) *.o *.bankLog core *.file *.log
	rm -f /dev/shm/adabank_shm 2>/dev/null || true
	rm -f /tmp/bank_*_req /tmp/bank_*_res 2>/dev/null || true
	killall $(SERVER_BIN) 2>/dev/null || true
	killall $(CLIENT_BIN) 2>/dev/null || true

# Run all tests
test: all
	@echo "Running all tests..."
	@./test_suite.sh

# Helper target to run memory leak test
memory_leak_test: all
	@echo "Running memory leak test..."
	@./test_memory_leaks.sh

# Helper target to run basic test only
basic_test: all
	@echo "Running basic test..."
	@./test_cases/test_basic.sh

# Helper target to run concurrent test only
concurrent_test: all
	@echo "Running concurrent test..."
	@./test_cases/test_concurrent.sh

# Run only the stress test
stress_test: all
	@echo "Running stress test..."
	@./test_cases/test_stress.sh

# Install dependencies (for systems that might need them)
install_deps:
	@echo "Checking for required packages..."
	@if command -v apt-get >/dev/null; then \
		echo "Debian/Ubuntu system detected"; \
		echo "Installing dependencies (may require sudo password)..."; \
		sudo apt-get update && sudo apt-get install -y build-essential librt-dev valgrind; \
	elif command -v yum >/dev/null; then \
		echo "RHEL/CentOS/Fedora system detected"; \
		echo "Installing dependencies (may require sudo password)..."; \
		sudo yum install -y gcc make glibc-devel valgrind; \
	else \
		echo "Package manager not recognized. Please install manually:"; \
		echo "- GCC compiler"; \
		echo "- Make utility"; \
		echo "- POSIX threads library"; \
		echo "- Real-time library"; \
		echo "- Valgrind (for memory testing)"; \
	fi

# Help target explaining available make commands
help:
	@echo "Available make targets:"
	@echo "  all            - Build the server and client programs (default)"
	@echo "  clean          - Remove compiled binaries and temporary files"
	@echo "  test           - Run all tests"
	@echo "  memory_leak_test - Run memory leak test with Valgrind"
	@echo "  basic_test     - Run basic functionality test only"
	@echo "  concurrent_test - Run concurrent clients test only"
	@echo "  stress_test    - Run stress test only"
	@echo "  install_deps   - Try to install required dependencies"
	@echo "  setup_tests    - Ensure test scripts are executable"
	@echo "  help           - Show this help message"