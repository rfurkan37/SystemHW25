# System Calls Conversion Summary

## 🔧 **CONVERSION FROM C LIBRARY TO POSIX SYSTEM CALLS**

### **Overview**
Converted all file operations from high-level C library functions (`fopen`, `fread`, `fwrite`, `fclose`) to low-level POSIX system calls (`open`, `read`, `write`, `close`) for better systems programming education and control.

---

## **📁 FILES MODIFIED**

### **1. Server Logging System (`server/logging.c`)**

#### **Before (C Library Functions)**
```c
FILE* log_file = NULL;

void init_logging(void) {
    log_file = fopen("server.log", "a");
    if (!log_file) {
        perror("Failed to open log file");
        exit(1);
    }
}

void log_message(const char* type, const char* message) {
    if (log_file) {
        fprintf(log_file, "%s - [%s] %s\n", timestamp, type, message);
        fflush(log_file);
    }
}

void cleanup_logging(void) {
    if (log_file) {
        fclose(log_file);
        log_file = NULL;
    }
}
```

#### **After (System Calls)**
```c
int log_fd = -1;

void init_logging(void) {
    log_fd = open("server.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (log_fd < 0) {
        perror("Failed to open log file");
        exit(1);
    }
}

void log_message(const char* type, const char* message) {
    char log_buffer[1024];
    int log_len = snprintf(log_buffer, sizeof(log_buffer), "%s - [%s] %s\n", timestamp, type, message);
    
    if (log_fd >= 0 && log_len > 0) {
        ssize_t written = write(log_fd, log_buffer, log_len);
        if (written > 0) {
            fsync(log_fd);  // Force write to disk
        }
    }
}

void cleanup_logging(void) {
    if (log_fd >= 0) {
        close(log_fd);
        log_fd = -1;
    }
}
```

### **2. Client File Utilities (`client/utils.c`)**

#### **Before (C Library Functions)**
```c
long get_file_size(FILE* file) {
    if (!file) return -1;
    
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    
    return size;
}
```

#### **After (System Calls)**
```c
long get_file_size(int fd) {
    if (fd < 0) return -1;
    
    off_t current_pos = lseek(fd, 0, SEEK_CUR);  // Save current position
    if (current_pos == -1) return -1;
    
    off_t size = lseek(fd, 0, SEEK_END);         // Seek to end
    if (size == -1) return -1;
    
    if (lseek(fd, current_pos, SEEK_SET) == -1) return -1;  // Restore position
    
    return (long)size;
}
```

### **3. Client File Sending (`client/commands.c`)**

#### **Before (C Library Functions)**
```c
FILE* file = fopen(filename, "rb");
if (!file) {
    printf("Failed to open file '%s': %s\n", filename, strerror(errno));
    return;
}

long file_size = get_file_size(file);
char* file_data = malloc(file_size);

if (fread(file_data, 1, file_size, file) != (size_t)file_size) {
    printf("Failed to read file data\n");
    free(file_data);
    fclose(file);
    return;
}
fclose(file);
```

#### **After (System Calls)**
```c
int fd = open(filename, O_RDONLY);
if (fd < 0) {
    printf("Failed to open file '%s': %s\n", filename, strerror(errno));
    return;
}

long file_size = get_file_size(fd);
char* file_data = malloc(file_size);

ssize_t bytes_read = read(fd, file_data, file_size);
if (bytes_read != file_size) {
    printf("Failed to read file data\n");
    free(file_data);
    close(fd);
    return;
}
close(fd);
```

### **4. Client File Receiving (`client/main.c`)**

#### **Before (C Library Functions)**
```c
FILE* file = NULL;
file = fopen(filename, "wb");
if (file) {
    fwrite(file_data, 1, msg.file_size, file);
    fclose(file);
    printf("File saved as '%s'\n", filename);
} else {
    printf("Failed to save file\n");
}
```

#### **After (System Calls)**
```c
int fd = -1;
fd = open(filename, O_WRONLY | O_CREAT | O_EXCL, 0644);
if (fd >= 0) {
    ssize_t bytes_written = write(fd, file_data, msg.file_size);
    close(fd);
    if (bytes_written == (ssize_t)msg.file_size) {
        printf("File saved as '%s'\n", filename);
    } else {
        printf("Failed to write complete file\n");
    }
} else {
    printf("Failed to create file: %s\n", strerror(errno));
}
```

### **5. Header Files Updated**

#### **Function Signature Change**
```c
// Before
long get_file_size(FILE* file);

// After  
long get_file_size(int fd);
```

#### **Added System Call Headers**
```c
#include <fcntl.h>      // For open() flags
#include <sys/stat.h>   // For file permissions
#include <sys/types.h>  // For data types
```

---

## **🎯 BENEFITS OF SYSTEM CALLS**

### **Educational Value**
- **Lower-level understanding**: Direct interaction with kernel
- **Systems programming**: Understanding file descriptors vs FILE*
- **Error handling**: More granular control over operations
- **Performance awareness**: Understanding buffering differences

### **Technical Advantages**
- **File descriptor control**: Direct manipulation of file descriptors
- **Atomic operations**: `O_EXCL` flag for collision-free file creation
- **Permission control**: Explicit file permission setting (0644)
- **Error granularity**: More specific error reporting

### **System Call Features Used**
- **`open()`**: File opening with flags and permissions
- **`read()`**: Direct data reading from file descriptor
- **`write()`**: Direct data writing to file descriptor
- **`close()`**: File descriptor cleanup
- **`lseek()`**: File position manipulation
- **`fsync()`**: Force data to disk (equivalent to `fflush()`)

---

## **🔍 KEY DIFFERENCES**

| Aspect | C Library Functions | System Calls |
|--------|-------------------|--------------|
| **Abstraction** | High-level, buffered | Low-level, direct |
| **Performance** | Buffered I/O | Unbuffered I/O |
| **Control** | Limited | Full control |
| **Error Handling** | errno + return codes | errno + return codes |
| **Portability** | ANSI C standard | POSIX standard |
| **File Handle** | FILE* pointer | int file descriptor |
| **Buffering** | Automatic | Manual (if needed) |

---

## **✅ COMPILATION SUCCESS**

All changes compiled successfully with:
```bash
make clean && make
```

**Result**: Both `chatserver` and `chatclient` binaries created without warnings or errors.

---

## **🎓 SYSTEMS PROGRAMMING EDUCATION**

This conversion demonstrates:

1. **File Descriptor Management**: Understanding how the kernel manages files
2. **System Call Interface**: Direct interaction with operating system
3. **Error Handling**: Lower-level error detection and reporting
4. **Resource Management**: Manual control over file operations
5. **Performance Considerations**: Understanding buffering vs direct I/O

**Perfect for systems programming coursework!** 🚀 