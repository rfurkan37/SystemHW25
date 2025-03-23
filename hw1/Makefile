CC = gcc
CFLAGS = -Wall -Wextra -g

SRCS = main.c fileManager.c utils.c
OBJS = $(SRCS:.c=.o)
TARGET = fileManager

.PHONY: all clean test

all: $(TARGET)

$(TARGET): $(OBJS)
	@$(CC) $(CFLAGS) -o $@ $^

main.o: main.c fileManager.h
	@$(CC) $(CFLAGS) -c $< -o $@

fileManager.o: fileManager.c fileManager.h utils.h
	@$(CC) $(CFLAGS) -c $< -o $@

utils.o: utils.c utils.h
	@$(CC) $(CFLAGS) -c $< -o $@

clean:
	@rm -f $(OBJS) $(TARGET) log.txt
	@rm -rf test_dir
	@echo "Cleaned up!"

comprehensive_test: clean $(TARGET) test_success test_errors test_file_locking
	@echo "\n===== ALL TESTS COMPLETED SUCCESSFULLY ====="
	@rm -rf log.txt

test_success: $(TARGET)
	@echo "\n===== TESTING SUCCESSFUL OPERATIONS ====="
	@echo "1. Creating test directory structure"
	@rm -rf test_dir
	./$(TARGET) createDir test_dir
	./$(TARGET) createDir test_dir/subdir
	
	@echo "\n2. Creating various files"
	./$(TARGET) createFile test_dir/file1.txt
	./$(TARGET) createFile test_dir/file2.txt
	./$(TARGET) createFile test_dir/document.doc
	./$(TARGET) createFile test_dir/program.c
	./$(TARGET) createFile test_dir/subdir/nested.txt
	
	@echo "\n3. Testing listing operations"
	./$(TARGET) listDir test_dir
	./$(TARGET) listDir test_dir/subdir
	./$(TARGET) listFilesByExtension test_dir .txt
	./$(TARGET) listFilesByExtension test_dir .c
	
	@echo "\n4. Testing file content operations"
	./$(TARGET) appendToFile test_dir/file1.txt "First line of content"
	./$(TARGET) appendToFile test_dir/file1.txt "Second line of content"
	./$(TARGET) readFile test_dir/file1.txt
	
	@echo "\n5. Testing file deletion"
	./$(TARGET) deleteFile test_dir/program.c
	./$(TARGET) listDir test_dir
	
	@echo "\n6. Testing logs"
	./$(TARGET) showLogs
	
	@echo "\n===== SUCCESS TESTS COMPLETED ====="

test_errors: $(TARGET)
	@echo "\n===== TESTING ERROR CONDITIONS ====="
	@rm -rf test_error_dir
	./$(TARGET) createDir test_error_dir
	
	@echo "\n1. Testing directory errors"
	@echo "   a. Creating directory that already exists"
	./$(TARGET) createDir test_error_dir
	@echo "   b. Listing non-existent directory"
	./$(TARGET) listDir non_existent_dir
	@echo "   c. Listing directory with wrong params"
	./$(TARGET) listDir
	
	@echo "\n2. Testing file errors"
	@echo "   a. Creating file in non-existent directory"
	./$(TARGET) createFile non_existent_dir/file.txt
	@echo "   b. Creating file that already exists"
	./$(TARGET) createFile test_error_dir/duplicate.txt
	./$(TARGET) createFile test_error_dir/duplicate.txt
	@echo "   c. Reading non-existent file"
	./$(TARGET) readFile test_error_dir/missing.txt
	@echo "   d. Reading with wrong params"
	./$(TARGET) readFile
	
	@echo "\n3. Testing append errors"
	@echo "   a. Appending to non-existent file"
	./$(TARGET) appendToFile non_existent_file.txt "Some content"
	@echo "   b. Appending with wrong params"
	./$(TARGET) appendToFile
	./$(TARGET) appendToFile test_error_dir/some_file.txt
	
	@echo "\n4. Testing delete errors"
	@echo "   a. Deleting non-existent file"
	./$(TARGET) deleteFile test_error_dir/non_existent.txt
	@echo "   b. Deleting with wrong params"
	./$(TARGET) deleteFile
	
	@echo "\n5. Testing extension filtering errors"
	@echo "   a. Extension filtering with non-existent dir"
	./$(TARGET) listFilesByExtension non_existent_dir .txt
	@echo "   b. Extension filtering with wrong params"
	./$(TARGET) listFilesByExtension
	./$(TARGET) listFilesByExtension test_error_dir
	
	@echo "\n6. Testing invalid commands"
	./$(TARGET) invalidCommand test_error_dir

	@echo "\n===== ERROR TESTS COMPLETED ====="
	@rm -rf test_error_dir
	@rm -rf test_dir

test_file_locking: $(TARGET)
	@echo "\n===== TESTING FILE LOCKING OPERATIONS (WITH F_SETLKW) ====="
	@rm -rf lock_test_dir
	./$(TARGET) createDir lock_test_dir
	./$(TARGET) createFile lock_test_dir/locked_file.txt
	
	@echo "\n1. Testing read locking (shared access)"
	@echo "   a. Starting read operation in background..."
	./$(TARGET) readFile lock_test_dir/locked_file.txt > /dev/null & \
	PID1=$$!; \
	sleep 1; \
	echo "   b. Starting second read (should succeed immediately)..."; \
	./$(TARGET) readFile lock_test_dir/locked_file.txt > /dev/null & \
	PID2=$$!; \
	wait $$PID1 $$PID2; \
	echo "   c. Both reads completed. Testing write lock contention..."; \
	./$(TARGET) appendToFile lock_test_dir/locked_file.txt "Write after reads" 
	
	@echo "\n2. Testing write locking (exclusive access)"
	@echo "   a. Starting write operation in background (will block others)..."
	./$(TARGET) appendToFile lock_test_dir/locked_file.txt "First write" > /dev/null & \
	PID1=$$!; \
	sleep 1; \
	echo "   b. Attempting concurrent write (should block until first completes)..."; \
	( timeout 5 ./$(TARGET) appendToFile lock_test_dir/locked_file.txt "Second write" > /dev/null && echo "   ✓ Write succeeded after lock release" || echo "   ✗ Write timed out (still locked)" ) & \
	PID2=$$!; \
	echo "   c. Attempting concurrent read (should block)..."; \
	( timeout 5 ./$(TARGET) readFile lock_test_dir/locked_file.txt > /dev/null && echo "   ✓ Read succeeded after lock release" || echo "   ✗ Read timed out (still locked)" ) & \
	PID3=$$!; \
	wait $$PID1; \
	echo "   d. First write completed. Waiting for pending operations..."; \
	wait $$PID2 $$PID3; 
	
	@echo "\n3. Testing lock release after completion"
	./$(TARGET) appendToFile lock_test_dir/locked_file.txt "Final content"
	./$(TARGET) readFile lock_test_dir/locked_file.txt
	
	@rm -rf lock_test_dir
	@echo "\n===== FILE LOCKING TESTS COMPLETED ====="

test_non_blocking_locks: $(TARGET)
	@echo "\n===== TESTING NON-BLOCKING LOCK ATTEMPTS (F_SETLK) ====="
	@rm -rf non_block_test
	./$(TARGET) createDir non_block_test
	./$(TARGET) createFile non_block_test/file.txt
	
	@echo "\n1. Acquire a write lock in the background..."
	./$(TARGET) appendToFile non_block_test/file.txt "Locked content" > /dev/null & \
	PID1=$$!; \
	sleep 1; \
	@echo "2. Attempt non-blocking read (should fail immediately)..."; \
	./$(TARGET) readFile non_block_test/file.txt > /dev/null 2>&1 && echo "   ✗ Read succeeded (unexpected)" || echo "   ✓ Read failed (expected)"; \
	wait $$PID1; \
	
	@rm -rf non_block_test
	@echo "\n===== NON-BLOCKING LOCK TESTS COMPLETED ====="

test_lock_release_on_kill: $(TARGET)
	@echo "\n===== TESTING LOCK RELEASE AFTER PROCESS KILL ====="
	@rm -rf kill_test
	./$(TARGET) createDir kill_test
	./$(TARGET) createFile kill_test/file.txt
	
	@echo "\n1. Start a long-running write operation in the background..."
	./$(TARGET) appendToFile kill_test/file.txt "Locked content..." > /dev/null & \
	PID1=$$!; \
	sleep 1; \
	@echo "2. Kill the process holding the lock..."; \
	kill -9 $$PID1; \
	sleep 1; \
	@echo "3. Verify lock is released by attempting a new write..."; \
	./$(TARGET) appendToFile kill_test/file.txt "New content" > /dev/null && echo "   ✓ Write succeeded (lock released)" || echo "   ✗ Write failed (lock still held)"; \
	
	@rm -rf kill_test
	@echo "\n===== LOCK RELEASE ON KILL TESTS COMPLETED ====="

test_multiple_writers: $(TARGET)
	@echo "\n===== TESTING MULTIPLE CONCURRENT WRITERS ====="
	@rm -rf multi_writer_test
	./$(TARGET) createDir multi_writer_test
	./$(TARGET) createFile multi_writer_test/file.txt
	
	@echo "\n1. Start three concurrent writes (only one should proceed at a time)..."
	./$(TARGET) appendToFile multi_writer_test/file.txt "Write 1" > /dev/null & \
	PID1=$$!; \
	./$(TARGET) appendToFile multi_writer_test/file.txt "Write 2" > /dev/null & \
	PID2=$$!; \
	./$(TARGET) appendToFile multi_writer_test/file.txt "Write 3" > /dev/null & \
	PID3=$$!; \
	wait $$PID1 $$PID2 $$PID3; \
	@echo "2. Verify all writes completed sequentially..."; \
	./$(TARGET) readFile multi_writer_test/file.txt | grep -q "Write 1" && echo "   ✓ Write 1 found" || echo "   ✗ Write 1 missing"; \
	./$(TARGET) readFile multi_writer_test/file.txt | grep -q "Write 2" && echo "   ✓ Write 2 found" || echo "   ✗ Write 2 missing"; \
	./$(TARGET) readFile multi_writer_test/file.txt | grep -q "Write 3" && echo "   ✓ Write 3 found" || echo "   ✗ Write 3 missing"; \
	
	@rm -rf multi_writer_test
	@echo "\n===== MULTIPLE WRITER TESTS COMPLETED ====="

test_lock_after_deletion: $(TARGET)
	@echo "\n===== TESTING LOCKS ON DELETED FILES ====="
	@rm -rf delete_test
	./$(TARGET) createDir delete_test
	./$(TARGET) createFile delete_test/file.txt
	
	@echo "\n1. Open and lock the file in the background..."
	./$(TARGET) readFile delete_test/file.txt > /dev/null & \
	PID1=$$!; \
	sleep 1; \
	@echo "2. Delete the file while it's locked..."; \
	./$(TARGET) deleteFile delete_test/file.txt; \
	@echo "3. Verify the original process can still read the deleted file..."; \
	wait $$PID1 && echo "   ✓ Read succeeded (file still accessible to holder)" || echo "   ✗ Read failed"; \
	
	@rm -rf delete_test
	@echo "\n===== LOCK AFTER DELETION TESTS COMPLETED ====="

test_lock_logging: $(TARGET)
	@echo "\n===== TESTING LOCK ERROR LOGGING ====="
	@rm -rf log_test
	./$(TARGET) createDir log_test
	./$(TARGET) createFile log_test/file.txt
	
	@echo "\n1. Trigger a lock conflict and check logs..."
	./$(TARGET) appendToFile log_test/file.txt "Lock holder" > /dev/null & \
	PID1=$$!; \
	sleep 1; \
	./$(TARGET) appendToFile log_test/file.txt "Conflicting write" 2> /dev/null; \
	wait $$PID1; \
	@echo "2. Verify logs contain lock error..."; \
	./$(TARGET) showLogs | grep -q "File is currently being accessed" && echo "   ✓ Lock error logged" || echo "   ✗ Lock error missing from logs"; \
	
	@rm -rf log_test
	@echo "\n===== LOCK LOGGING TESTS COMPLETED ====="

test: $(TARGET)
	@echo "Running simple test scenario..."
	@echo "1. Creating directory testDir"
	./$(TARGET) createDir testDir
	@echo "\n2. Creating file testDir/example.txt"
	./$(TARGET) createFile testDir/example.txt
	@echo "\n3. Appending initial content to file"
	./$(TARGET) appendToFile testDir/example.txt "Hello, World!"
	@echo "\n4. Listing directory contents"
	./$(TARGET) listDir testDir
	@echo "\n5. Reading file content"
	./$(TARGET) readFile testDir/example.txt
	@echo "\n6. Appending more content to file"
	./$(TARGET) appendToFile testDir/example.txt "New Line"
	@echo "\n7. Reading updated file content"
	./$(TARGET) readFile testDir/example.txt
	@echo "\n8. Deleting file"
	./$(TARGET) deleteFile testDir/example.txt
	@echo "\n9. Showing logs"
	./$(TARGET) showLogs
	@rm -rf testDir
	@echo "\nSimple test scenario completed successfully!"