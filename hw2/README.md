# IPC Program - Inter-Process Communication with FIFOs

A comprehensive C program demonstrating inter-process communication using named pipes (FIFOs), signal handling, and daemon processes. This program showcases advanced system programming concepts including process management, IPC mechanisms, and proper resource cleanup.

## Overview

This program creates a multi-process system that:
1. Takes two integers as command-line arguments
2. Determines which number is larger using child processes
3. Implements communication between processes via FIFOs
4. Includes a daemon process for logging and monitoring
5. Demonstrates proper signal handling and zombie process prevention

## Architecture

The program creates four main processes:

### 1. Parent Process
- Parses command-line arguments
- Creates and manages child processes and daemon
- Handles FIFO communication
- Implements signal handlers for proper cleanup

### 2. Child Process 1
- Reads two integers from FIFO1
- Sleeps for 10 seconds (simulating processing time)
- Determines the larger number
- Writes result to FIFO2

### 3. Child Process 2  
- Reads the result from FIFO2
- Sleeps for 10 seconds
- Displays the final result

### 4. Daemon Process
- Runs as a background daemon
- Logs system events to `/tmp/daemon.log`
- Handles termination and reconfiguration signals
- Monitors other processes through a log FIFO

## Features

- **FIFO Communication**: Uses three named pipes for inter-process communication
- **Signal Handling**: Proper handling of SIGCHLD, SIGTERM, SIGHUP, and SIGALRM
- **Daemon Process**: Background process with logging capabilities
- **Zombie Prevention**: Automatic cleanup of terminated child processes
- **Resource Management**: Proper cleanup of FIFOs and file descriptors
- **Error Handling**: Comprehensive error checking and recovery

## Requirements

- GCC compiler
- POSIX-compliant Unix/Linux system
- Root privileges may be required for cleanup operations

## Building

```bash
# Build the program
make

# Clean build artifacts and temporary files
make clean
```

## Usage

```bash
./ipc_program <number1> <number2>
```

### Example
```bash
./ipc_program 42 17
```

This will:
1. Create the necessary processes and FIFOs
2. Child processes will determine that 42 is the larger number
3. Display progress messages and results
4. Clean up all resources upon completion

## Testing

The project includes comprehensive tests to verify all functionality:

```bash
# Run all tests
make test

# Run basic functionality test
make test-basic

# Run memory leak detection (requires valgrind)
make test-memory
```

### Test Coverage

The test suite verifies:
- Basic functionality with different input combinations
- Zombie process prevention
- FIFO creation and communication
- Daemon process creation and logging
- Correct mathematical calculations
- Child process execution timing
- Signal handling and cleanup

## File Structure

```
.
├── main.c          # Main program source code
├── makefile        # Build configuration and tests
└── README.md       # This file
```

## Temporary Files

The program creates several temporary files during execution:
- `/tmp/fifo1` - Communication pipe from parent to child1
- `/tmp/fifo2` - Communication pipe from child1 to child2  
- `/tmp/log_fifo` - Logging pipe to daemon
- `/tmp/daemon.log` - Daemon log file

All temporary files are automatically cleaned up upon program termination.

## Signal Handling

The program implements robust signal handling:

- **SIGCHLD**: Prevents zombie processes by reaping terminated children
- **SIGTERM**: Graceful shutdown of daemon and cleanup
- **SIGHUP**: Daemon reconfiguration (placeholder for future features)
- **SIGALRM**: Timeout mechanism for daemon safety

## Error Handling

The program includes comprehensive error handling for:
- FIFO creation and access failures
- Process creation errors
- Signal handler installation issues
- File descriptor management problems
- Resource cleanup failures

## Implementation Details

### Process Synchronization
- Child processes sleep for 10 seconds to simulate processing time
- Parent process uses a counter mechanism to track child completion
- FIFO operations provide natural synchronization points

### Memory Management
- All dynamically allocated resources are properly freed
- File descriptors are closed when no longer needed
- FIFOs are unlinked during cleanup

### Daemon Implementation
- Proper daemonization with `setsid()`
- Output redirection to log files
- Non-blocking I/O for responsive signal handling
- Timeout mechanisms for safety

## Troubleshooting

### Common Issues

1. **Permission Denied**: Ensure you have write access to `/tmp/` directory
2. **FIFO Exists**: Run `make clean` to remove existing FIFOs
3. **Process Hanging**: Check for zombie processes with `ps aux | grep defunct`
4. **Daemon Not Starting**: Verify `/tmp/daemon.log` for error messages

### Debugging

Enable verbose output by examining the daemon log:
```bash
tail -f /tmp/daemon.log
```

## Educational Value

This program demonstrates key systems programming concepts:

- **Process Management**: Fork, exec, wait, and process lifecycle
- **Inter-Process Communication**: Named pipes (FIFOs) for data exchange
- **Signal Handling**: Asynchronous event processing
- **Daemon Programming**: Background service implementation
- **Resource Management**: Proper cleanup and error handling
- **System Calls**: Low-level Unix system programming

## License

This project is intended for educational purposes as part of a systems programming course.

## Contributing

This is an educational project. For improvements or bug fixes, please ensure all tests pass and follow the existing code style.
