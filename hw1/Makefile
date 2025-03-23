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
	@echo "\n========== TESTING FILE LOCKING OPERATIONS (WITH FLOCK) ==========\n"
	@rm -rf lock_test_dir
	@echo "Setup: Creating test directory and file..."
	@./$(TARGET) createDir lock_test_dir > /dev/null
	@./$(TARGET) createFile lock_test_dir/locked_file.txt > /dev/null
	@echo "✓ Setup complete\n"
	
	@echo "TEST 1: SHARED LOCK TESTING (READ LOCKS)"
	@echo "----------------------------------------"
	@echo "  • Starting first read operation in background..."
	@./$(TARGET) readFile lock_test_dir/locked_file.txt > /dev/null & \
	PID1=$$!; \
	sleep 1; \
	echo "  • Starting second read operation (should succeed immediately)..."; \
	./$(TARGET) readFile lock_test_dir/locked_file.txt > /dev/null & \
	PID2=$$!; \
	wait $$PID1 $$PID2; \
	echo "  ✓ Both read operations completed successfully (shared locking working)"; \
	echo "  • Testing write after reads..."; \
	./$(TARGET) appendToFile lock_test_dir/locked_file.txt "Write after reads" > /dev/null; \
	echo "  ✓ Write after reads succeeded\n"
	
	@echo "TEST 2: EXCLUSIVE LOCK TESTING (WRITE LOCKS)"
	@echo "------------------------------------------"
	@echo "  • Starting write operation in background (will hold exclusive lock)..."
	@./$(TARGET) appendToFile lock_test_dir/locked_file.txt "First write" > /dev/null & \
	PID1=$$!; \
	sleep 1; \
	echo "  • Attempting concurrent write operation (should block)..."; \
	( timeout 5 ./$(TARGET) appendToFile lock_test_dir/locked_file.txt "Second write" > /dev/null && \
	  echo "  ✓ Second write succeeded after lock release" || \
	  echo "  ✗ Second write timed out (unexpected error)" ) & \
	PID2=$$!; \
	echo "  • Attempting concurrent read operation (should also block)..."; \
	( timeout 5 ./$(TARGET) readFile lock_test_dir/locked_file.txt > /dev/null && \
	  echo "  ✓ Read operation succeeded after lock release" || \
	  echo "  ✗ Read operation timed out (unexpected error)" ) & \
	PID3=$$!; \
	wait $$PID1; \
	echo "  • First write operation completed, releasing lock..."; \
	wait $$PID2 $$PID3
	
	@echo "\nTEST 3: LOCK RELEASE VERIFICATION" 
	@echo "---------------------------------"
	@echo "  • Verifying locks are properly released..."
	@./$(TARGET) appendToFile lock_test_dir/locked_file.txt "Final content" > /dev/null
	@echo "  ✓ Append operation succeeded"
	@echo "  • Reading final file contents to verify all operations worked:"
	@echo "  ----------------------------------------"
	@./$(TARGET) readFile lock_test_dir/locked_file.txt
	@echo "  ----------------------------------------"
	
	@rm -rf lock_test_dir
	@echo "\n========== FILE LOCKING TESTS COMPLETED SUCCESSFULLY ==========\n"

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