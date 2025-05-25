# Multi-threaded Distributed Chat and File Server

A robust, multi-threaded chat server implementation in C with file transfer capabilities, room management, and comprehensive logging.

## Features

### Core Functionality
- **Multi-threaded Architecture**: Handles up to 15 concurrent clients
- **Room-based Chat**: Dynamic room creation and management
- **Private Messaging**: Direct whisper functionality between users
- **File Transfer**: Secure file sharing with queue management (max 3MB)
- **Thread-safe Logging**: Comprehensive logging with timestamps
- **Graceful Shutdown**: SIGINT handling with client notification

### Technical Specifications
- **Threading Model**: One thread per client + dedicated file transfer worker
- **Synchronization**: Mutexes, semaphores, and condition variables
- **Supported File Types**: .txt, .pdf, .jpg, .png
- **Maximum Clients**: 15 concurrent connections
- **File Queue**: Up to 5 concurrent file transfers
- **Username Validation**: Alphanumeric characters only (max 16 chars)
- **Room Names**: Alphanumeric characters only (max 32 chars)

## Project Structure

```
.
├── Makefile                 # Build configuration
├── README.md               # This file
├── test_runner.sh          # Automated test suite
├── server/                 # Server implementation
│   ├── common.h           # Shared data structures and declarations
│   ├── main.c             # Main server entry point
│   ├── client_handler.c   # Client connection management
│   ├── room_manager.c     # Room operations and messaging
│   ├── file_transfer.c    # File transfer queue and processing
│   ├── logging.c          # Thread-safe logging system
│   └── utils.c            # Utility functions and validation
└── client/                # Client implementation
    ├── common.h           # Client-side data structures
    ├── main.c             # Main client entry point
    ├── network.c          # Network connection handling
    ├── commands.c         # Command processing and execution
    └── utils.c            # Client utility functions
```

## Building the Project

### Prerequisites
- GCC compiler with C99 support
- POSIX threads library (pthread)
- Linux/Unix environment

### Compilation
```bash
# Build both server and client
make

# Build only server
make chatserver

# Build only client
make chatclient

# Clean build artifacts
make clean

# Install to system (requires sudo)
make install
```

## Usage

### Starting the Server
```bash
./chatserver <port>

# Example
./chatserver 5000
```

The server will:
- Listen on the specified port
- Create a `server.log` file for logging
- Accept up to 15 concurrent client connections
- Handle graceful shutdown with Ctrl+C

### Connecting Clients
```bash
./chatclient <server_ip> <port>

# Example - connect to local server
./chatclient 127.0.0.1 5000

# Example - connect to remote server
./chatclient 192.168.1.100 5000
```

### Client Commands

Once connected, use these commands:

#### Basic Commands
- `/help` - Show available commands
- `/exit` - Disconnect from server

#### Room Management
- `/join <room_name>` - Join or create a chat room
- `/leave` - Leave current room

#### Messaging
- `/broadcast <message>` - Send message to all users in current room
- `/whisper <username> <message>` - Send private message to specific user

#### File Transfer
- `/sendfile <username> <filename>` - Send file to specific user

### Example Session

**Server Terminal:**
```bash
$ ./chatserver 5000
2024-01-15 10:30:00 - [INIT] Logging system initialized
2024-01-15 10:30:00 - [INIT] Server state initialized
2024-01-15 10:30:00 - [INIT] File transfer queue initialized
2024-01-15 10:30:00 - [INIT] Server listening on port 5000
2024-01-15 10:30:00 - [START] Chat server started successfully
```

**Client Terminal:**
```bash
$ ./chatclient 127.0.0.1 5000
Connected to server 127.0.0.1:5000
=== Chat Client ===
Enter username (alphanumeric, max 16 chars): alice
Login successful
Welcome, alice! Type /help for available commands.

You are now connected! Type /help for commands.
> /join general
Joining room 'general'...
[SERVER] Joined room successfully
> /broadcast Hello everyone!
[general] alice: Hello everyone!
> /whisper bob How are you?
[WHISPER to bob] How are you?
> /sendfile bob document.txt
File 'document.txt' sent to bob (1024 bytes)
> /exit
Disconnecting from server...
Goodbye!
```

## Testing

The project includes comprehensive automated testing with multiple test suites:

### Quick Demonstration
```bash
# Quick demo of key functionality (2-3 minutes)
./quick_demo.sh
```

### Comprehensive Test Suite
```bash
# Full test suite covering all 10 specified scenarios (5-10 minutes)
./comprehensive_test_suite.sh
```

### Improved Automated Tests
```bash
# Enhanced automated tests with persistent connections
./test_runner_improved.sh
```

### Manual Interactive Testing
```bash
# Visual demonstration with multiple terminal windows
./manual_test_demo.sh
```

### Test Coverage
The test suites validate all 10 specified scenarios:

1. **Concurrent User Load** - 30 clients connecting simultaneously
2. **Duplicate Usernames** - Username uniqueness enforcement
3. **File Upload Queue Limit** - Queue management with 5 concurrent uploads
4. **Unexpected Disconnection** - Graceful handling of client crashes
5. **Room Switching** - Dynamic room membership management
6. **Oversized File Rejection** - 3MB file size limit enforcement
7. **SIGINT Server Shutdown** - Graceful shutdown with client notification
8. **Rejoining Rooms** - Room re-entry behavior (ephemeral message history)
9. **Same Filename Collision** - Filename conflict resolution
10. **File Queue Wait Duration** - Queue wait time tracking

### Additional Testing Features
- Server startup and client connections
- Concurrent client handling (up to 15 clients)
- Room functionality and message routing
- Message broadcasting and private whispers
- File transfer capabilities with queue management
- Input validation and error handling
- Thread safety and resource management
- Memory leak detection support
- Graceful shutdown behavior
- Comprehensive logging validation

### Manual Testing
1. **Start the server:**
   ```bash
   ./chatserver 5000
   ```

2. **Connect multiple clients:**
   ```bash
   # Terminal 1
   ./chatclient 127.0.0.1 5000
   
   # Terminal 2
   ./chatclient 127.0.0.1 5000
   
   # Terminal 3
   ./chatclient 127.0.0.1 5000
   ```

3. **Test scenarios:**
   - Join same room from multiple clients
   - Send broadcast messages
   - Send whisper messages
   - Transfer files between users
   - Test graceful shutdown (Ctrl+C on server)

### Memory Testing
For memory leak detection:
```bash
valgrind --leak-check=full --track-origins=yes ./chatserver 5000
```

## Architecture Details

### Threading Model
- **Main Thread**: Accepts incoming connections and handles signals
- **Client Handler Threads**: One per connected client (detached)
- **File Transfer Worker**: Dedicated thread for processing file queue
- **Logging**: Thread-safe with mutex protection

### Synchronization Mechanisms
- **Client List Mutex**: Protects global client array
- **Room Mutexes**: Individual mutex per room for member management
- **File Queue Mutex**: Protects file transfer queue
- **Log Mutex**: Ensures thread-safe logging
- **Semaphore**: Controls concurrent file transfers (max 5)
- **Condition Variables**: Signals file transfer worker

### Message Protocol
All communication uses a structured message format:
```c
typedef struct {
    message_type_t type;        // Message type enum
    char sender[17];            // Sender username
    char receiver[17];          // Receiver username (for whispers/files)
    char room[33];              // Room name
    char content[1024];         // Message content
    size_t content_length;      // Content length
    char filename[256];         // Filename (for file transfers)
    size_t file_size;          // File size (for file transfers)
} message_t;
```

### Error Handling
- Input validation for all user inputs
- Network error detection and recovery
- Memory allocation failure handling
- File I/O error management
- Graceful degradation on resource limits

## Security Considerations

- **Input Validation**: All user inputs are validated
- **Buffer Overflow Protection**: Proper bounds checking
- **Resource Limits**: Maximum clients, file sizes, queue lengths
- **File Type Restrictions**: Only specific file types allowed
- **Username Sanitization**: Alphanumeric characters only

## Limitations

- Maximum 15 concurrent clients
- File size limited to 3MB
- Supported file types: .txt, .pdf, .jpg, .png
- Room names and usernames: alphanumeric only
- No persistent storage (rooms/messages lost on restart)
- No encryption (plain text communication)

## Troubleshooting

### Common Issues

**Server won't start:**
- Check if port is already in use: `netstat -ln | grep :5000`
- Ensure port number is valid (1-65535)
- Check permissions for log file creation

**Client can't connect:**
- Verify server is running: `ps aux | grep chatserver`
- Check firewall settings
- Ensure correct IP address and port

**File transfer fails:**
- Check file exists and is readable
- Verify file type is supported
- Ensure file size is under 3MB
- Check receiver username is valid

**Memory issues:**
- Monitor with: `top -p $(pgrep chatserver)`
- Use valgrind for detailed analysis
- Check for file descriptor leaks: `lsof -p $(pgrep chatserver)`

### Debug Mode
For verbose logging, modify the logging level in `server/logging.c` or run with:
```bash
strace -e trace=network ./chatserver 5000
```

## Contributing

1. Follow the existing code style
2. Add appropriate error handling
3. Update documentation for new features
4. Test thoroughly with the test suite
5. Ensure thread safety for any shared resources

## License

This project is provided as-is for educational purposes. 