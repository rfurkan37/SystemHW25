CC = gcc
CFLAGS = -Wall -Wextra -g
TARGET = ipc_program

all: $(TARGET)

$(TARGET): main.c
	$(CC) $(CFLAGS) -o $(TARGET) main.c

clean:
	sudo rm -f $(TARGET)
	sudo rm -f /tmp/fifo1 /tmp/fifo2 /tmp/log_fifo
	sudo rm -f /tmp/daemon.log
	-pkill -f $(TARGET) || true

# Key functionality tests based on PDF requirements
test: clean $(TARGET)
	@echo "========== TESTING IPC PROGRAM REQUIREMENTS =========="
	
	@echo "\n[Test 1] Basic functionality with command line arguments:"
	@./$(TARGET) 42 17 & \
	pid=$$!; \
	echo "Program started with PID: $$pid"; \
	echo "Waiting for completion (30 seconds max)..."; \
	for i in $$(seq 1 30); do \
		sleep 1; \
		if ! kill -0 $$pid 2>/dev/null; then \
			echo "Program completed after $$i seconds"; \
			break; \
		fi; \
		if [ $$i -eq 30 ]; then \
			echo "WARNING: Program did not complete within 30 seconds"; \
			kill -TERM $$pid; \
		fi; \
	done
	
	@echo "\n[Test 2] Verify all processes exited properly (no zombies):"
	@echo "Running program and checking for zombies..."
	@./$(TARGET) 55 66 & \
	pid=$$!; \
	sleep 15; \
	zombie_count=$$(ps aux | grep defunct | grep -v grep | wc -l); \
	echo "Current zombie count: $$zombie_count"; \
	if [ "$$zombie_count" -eq 0 ]; then \
		echo "✓ No zombies found - Zombie protection works!"; \
	else \
		echo "✗ WARNING: $$zombie_count zombie processes found"; \
	fi; \
	wait $$pid || echo "Process already exited"
	
	@echo "\n[Test 3] FIFO creation and communication:"
	@./$(TARGET) 100 50 & \
	pid=$$!; \
	sleep 5; \
	if [ -p /tmp/fifo1 ] && [ -p /tmp/fifo2 ] && [ -p /tmp/log_fifo ]; then \
		echo "✓ All FIFOs created successfully"; \
	else \
		echo "✗ FIFO creation failed"; \
		[ -p /tmp/fifo1 ] || echo "  - fifo1 missing"; \
		[ -p /tmp/fifo2 ] || echo "  - fifo2 missing"; \
		[ -p /tmp/log_fifo ] || echo "  - log_fifo missing"; \
	fi; \
	wait $$pid || echo "Process already exited"
	
	@echo "\n[Test 4] Daemon process and logging:"
	@echo "Checking if daemon creates log file..."
	@./$(TARGET) 33 77 & \
	pid=$$!; \
	sleep 15; \
	if [ -f /tmp/daemon.log ]; then \
		echo "✓ Daemon log file created"; \
		echo "Log file content (last 10 lines):"; \
		tail -10 /tmp/daemon.log; \
	else \
		echo "✗ Daemon log file not found"; \
	fi; \
	wait $$pid || echo "Process already exited"
	
	@echo "\n[Test 5] Verify correct calculation of larger number:"
	@echo "Testing with first number larger..."
	@./$(TARGET) 200 100 > test_output.txt & \
	pid=$!; \
	sleep 25; \
	grep -q "The larger number is 200" test_output.txt && \
		echo "✓ Correctly identified 200 as larger" || \
		echo "✗ Failed to identify correct larger number"; \
	wait $pid 2>/dev/null || true
	
	@echo "Testing with second number larger..."
	@./$(TARGET) 50 150 > test_output.txt & \
	pid=$!; \
	sleep 25; \
	grep -q "The larger number is 150" test_output.txt && \
		echo "✓ Correctly identified 150 as larger" || \
		echo "✗ Failed to identify correct larger number"; \
	wait $pid || echo "Process already exited"
	@rm -f test_output.txt
	
	@echo "\n[Test 6] Verify child process sleep and execution:"
	@echo "Running program and monitoring child processes..."
	@./$(TARGET) 88 99 & \
	main_pid=$$!; \
	sleep 3; \
	child_pids=$$(pgrep -P $$main_pid || echo ""); \
	echo "Child PIDs: $$child_pids"; \
	if [ -n "$$child_pids" ]; then \
		echo "✓ Child processes created"; \
		echo "Waiting for child processes to finish after sleep..."; \
		sleep 15; \
		remaining=$$(pgrep -P $$main_pid || echo ""); \
		if [ -z "$$remaining" ]; then \
			echo "✓ All child processes completed after sleep period"; \
		else \
			echo "✗ Some child processes remain after expected completion"; \
		fi; \
	else \
		echo "✗ No child processes found"; \
	fi; \
	wait $$main_pid || echo "Process already exited"
	
	@echo "\n[Test 7] SIGCHLD handling and counter increment:"
	@echo "Running program and checking if it exits after children complete..."
	@./$(TARGET) 111 222 & \
	pid=$$!; \
	sleep 5; \
	echo "Program started, waiting for completion..."; \
	sleep 25; \
	if kill -0 $$pid 2>/dev/null; then \
		echo "✗ Program still running after children should have completed"; \
		kill -TERM $$pid; \
	else \
		echo "✓ Program exited properly after children completed"; \
	fi
	
	@echo "\n========== TEST SUMMARY =========="
	@echo "All tests completed. Please review the results above."
	@echo "For the bonus requirements:"
	@echo "1. Zombie protection: Test 2 verifies this functionality"
	@echo "2. Exit status printing: Check the program output for exit status of child processes"

# Individual test targets for specific components
test-basic: clean $(TARGET)
	@echo "Testing basic functionality..."
	./$(TARGET) 10 5

# Run memory test with valgrind if available
test-memory: clean $(TARGET)
	@which valgrind > /dev/null; \
	if [ $$? -eq 0 ]; then \
		echo "Running memory test with valgrind..."; \
		valgrind --leak-check=full --show-leak-kinds=all ./$(TARGET) 10 20; \
	else \
		echo "Valgrind not found. Install it to run memory tests."; \
	fi