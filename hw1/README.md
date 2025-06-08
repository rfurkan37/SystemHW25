# File Manager - HW1

A comprehensive file management utility written in C that provides essential file and directory operations with built-in logging capabilities and file locking mechanisms.

## Features

- **Directory Operations**: Create and delete directories
- **File Operations**: Create, read, append, and delete files  
- **Directory Listing**: List directory contents with file type indicators
- **Extension Filtering**: List files by specific file extensions
- **Logging System**: Automatic operation logging with timestamps
- **File Locking**: Thread-safe file operations using `flock()` 
- **Error Handling**: Comprehensive error reporting and validation
- **Process Management**: Uses child processes for directory operations

## Prerequisites

- GCC compiler
- UNIX-like operating system (Linux, macOS)
- Make utility

## Installation

1. Clone the repository:
```bash
git clone <repository-url>
cd hw1
```

2. Compile the project:
```bash
make
```

This will create the `fileManager` executable.

## Usage

```bash
./fileManager <command> [arguments]
```

### Available Commands

| Command | Arguments | Description |
|---------|-----------|-------------|
| `createDir` | `<directory_name>` | Create a new directory |
| `createFile` | `<file_name>` | Create a new file with timestamp |
| `listDir` | `<directory_name>` | List contents of a directory |
| `listFilesByExtension` | `<directory_name> <extension>` | List files with specific extension |
| `readFile` | `<file_name>` | Display file contents |
| `appendToFile` | `<file_name> <content>` | Append content to a file |
| `deleteFile` | `<file_name>` | Delete a file |
| `deleteDir` | `<directory_name>` | Delete a directory |
| `showLogs` | | Display operation logs |

### Examples

```bash
# Create a directory
./fileManager createDir myProject

# Create a file
./fileManager createFile myProject/readme.txt

# Add content to file
./fileManager appendToFile myProject/readme.txt "Hello, World!"

# Read file contents
./fileManager readFile myProject/readme.txt

# List directory contents
./fileManager listDir myProject

# List only .txt files
./fileManager listFilesByExtension myProject .txt

# View operation logs
./fileManager showLogs

# Clean up
./fileManager deleteFile myProject/readme.txt
./fileManager deleteDir myProject
```

## File Structure

```
hw1/
├── main.c              # Main program entry point
├── fileManager.c       # Core file management functions
├── fileManager.h       # File manager header file
├── utils.c             # Utility functions (logging, timestamps)
├── utils.h             # Utility functions header
├── Makefile           # Build configuration
├── README.md          # This file
└── hw1_report.pdf     # Project report
```

## Key Features

### File Locking
The application implements file locking using `flock()` to ensure thread-safe operations:
- **Shared locks** for read operations (multiple concurrent reads allowed)
- **Exclusive locks** for write operations (blocking other reads/writes)

### Logging System
All operations are automatically logged with timestamps to `log.txt`:
- Operation type and target
- Timestamp of execution
- Success/failure status

### Error Handling
Comprehensive error checking for:
- File/directory existence validation
- Permission errors
- System call failures
- Invalid command arguments

## Testing

The project includes comprehensive test suites:

```bash
# Run simple test scenario
make test

# Run comprehensive tests (success cases, error cases, file locking)
make comprehensive_test

# Test only success scenarios
make test_success

# Test error conditions
make test_errors

# Test file locking mechanisms
make test_file_locking
```

## Build Targets

- `make` or `make all` - Build the main executable
- `make clean` - Remove build artifacts and test files
- `make test` - Run basic functionality tests
- `make comprehensive_test` - Run all test suites

## Technical Implementation

- **Language**: C (C99 standard)
- **System Calls**: Uses low-level system calls (`open`, `read`, `write`, `mkdir`, etc.)
- **Process Management**: Fork/exec for directory operations
- **File I/O**: Direct file descriptor operations
- **Memory Management**: Stack-allocated buffers with bounds checking
- **Concurrency**: File locking with `flock()` for safe concurrent access

## Error Codes

The program uses standard UNIX exit codes:
- `0` - Success
- `1` - General errors (file not found, permission denied, etc.)

## Contributing

1. Fork the repository
2. Create a feature branch
3. Make your changes
4. Add tests for new functionality
5. Submit a pull request

## License

This project is part of a system programming homework assignment.
