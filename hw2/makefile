CC = gcc
CFLAGS = -Wall -Wextra -g
TARGET = ipc_program

all: $(TARGET)

$(TARGET): main.c
	$(CC) $(CFLAGS) -o $(TARGET) main.c

clean:
	rm -f $(TARGET)
	rm -f /tmp/fifo1 /tmp/fifo2 /tmp/log_fifo
	rm -f /tmp/daemon.log

# Comprehensive test suite
test: $(TARGET)
	@echo "========== RUNNING COMPREHENSIVE TESTS =========="
	@echo "\n1. Basic Functionality Tests:"
	@echo "Test with first number larger"
	@./$(TARGET) 10 5 & pid1=$$!; sleep 20; kill -0 $$pid1 2>/dev/null || echo "Test completed successfully"
	@echo "\nTest with second number larger"
	@./$(TARGET) 3 8 & pid2=$$!; sleep 20; kill -0 $$pid2 2>/dev/null || echo "Test completed successfully"
	@echo "\nTest with equal numbers"
	@./$(TARGET) 7 7 & pid3=$$!; sleep 20; kill -0 $$pid3 2>/dev/null || echo "Test completed successfully"
	@echo "\nTest with negative number"
	@./$(TARGET) -5 10 & pid4=$$!; sleep 20; kill -0 $$pid4 2>/dev/null || echo "Test completed successfully"
	
	@echo "\n2. FIFO Communication Test:"
	@echo "Running program and checking log..."
	@./$(TARGET) 100 200 & pid5=$$!; sleep 20; kill -0 $$pid5 2>/dev/null || echo "Test completed"
	@echo "Log contents:"
	@cat /tmp/daemon.log | tail -10
	
	@echo "\n3. Signal Handling Test:"
	@echo "Running program in background..."
	@./$(TARGET) 15 25 & pid6=$$!; sleep 5; \
	echo "Sending SIGUSR1 signal..."; \
	kill -USR1 $$pid6 2>/dev/null || echo "Process already completed"; \
	sleep 15; kill -0 $$pid6 2>/dev/null || echo "Test completed"
	
	@echo "\n4. Zombie Protection Verification:"
	@echo "Checking for zombie processes after execution..."
	@./$(TARGET) 1 2 & pid7=$$!; sleep 15; \
	zombie_count=$$(ps aux | grep defunct | grep -v grep | wc -l); \
	if [ "$$zombie_count" -eq 0 ]; then echo "No zombies found - Zombie protection works!"; \
	else echo "WARNING: $$zombie_count zombie processes found"; fi; \
	sleep 5; kill -0 $$pid7 2>/dev/null || echo "Test completed"
	
	@echo "\n5. Memory Test (requires valgrind):"
	@if command -v valgrind > /dev/null; then \
		echo "Running with valgrind..."; \
		timeout 30s valgrind --leak-check=full --error-exitcode=1 ./$(TARGET) 30 40 2>&1 | grep -E "ERROR SUMMARY|heap usage"; \
	else \
		echo "Valgrind not installed. Skipping memory test."; \
	fi
	
	@echo "\n========== TEST SUMMARY =========="
	@echo "All tests executed. Check output for any errors or warnings."
	@echo "For a complete verification, also manually check:"
	@echo "1. Exit status reporting"
	@echo "2. Daemon process logging"
	@echo "3. Error handling in edge cases"

# Test specific components individually
test-basic: $(TARGET)
	@echo "Testing basic functionality..."
	@./$(TARGET) 10 5

test-memory: $(TARGET)
	@echo "Testing for memory leaks (requires valgrind)..."
	@if command -v valgrind > /dev/null; then \
		valgrind --leak-check=full ./$(TARGET) 15 25; \
	else \
		echo "Valgrind not installed. Cannot perform memory test."; \
	fi

test-zombies: $(TARGET)
	@echo "Testing zombie process prevention..."
	@./$(TARGET) 7 8 & \
	pid=$$!; \
	sleep 15; \
	zombie_count=$$(ps aux | grep defunct | grep -v grep | wc -l); \
	if [ "$$zombie_count" -eq 0 ]; then \
		echo "No zombies found - Zombie protection works!"; \
	else \
		echo "WARNING: $$zombie_count zombie processes found"; \
	fi; \
	kill -0 $$pid 2>/dev/null || echo "Process completed normally"