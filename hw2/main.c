#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <string.h>
#include <errno.h>
#include <time.h>

#define FIFO1 "/tmp/fifo1"
#define FIFO2 "/tmp/fifo2"
#define DAEMON_LOG "/tmp/daemon.log"
#define LOG_FIFO "/tmp/log_fifo"

// Global variables
int num_children = 2;
int child_counter = 0;
pid_t daemon_pid = 0;

// Signal handler for SIGCHLD
void sigchld_handler(int sig __attribute__((unused))) {
    pid_t pid;
    int status;
    
    // Use waitpid with WNOHANG to avoid blocking
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        printf("Child process %d has exited with status %d\n", pid, WEXITSTATUS(status));
        child_counter += 2;
    }
}

// This function is no longer used - daemon implementation simplified
// Keeping the stub for reference
int daemonize() {
    return -1;
}

// Daemon process code
void run_daemon() {
    time_t now;
    char time_str[100];
    int log_fifo_fd;
    
    // Create and open the log FIFO - make sure it exists
    unlink(LOG_FIFO); // Remove if exists
    if (mkfifo(LOG_FIFO, 0666) < 0) {
        fprintf(stderr, "Daemon: Failed to create log FIFO: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }
    
    fprintf(stderr, "Daemon: Opening log FIFO for reading\n");
    
    // Open the log FIFO - blocking open to ensure synchronization with parent
    if ((log_fifo_fd = open(LOG_FIFO, O_RDONLY)) < 0) {
        fprintf(stderr, "Daemon: Failed to open log FIFO: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }
    
    // Set to non-blocking after opening
    int flags = fcntl(log_fifo_fd, F_GETFL, 0);
    fcntl(log_fifo_fd, F_SETFL, flags | O_NONBLOCK);
    
    // Set up signal handlers
    signal(SIGTERM, SIG_IGN);
    signal(SIGHUP, SIG_IGN);
    signal(SIGUSR1, SIG_IGN);
    
    fprintf(stderr, "Daemon started with PID: %d\n", getpid());
    
    // Main daemon loop
    char buffer[1024];
    while (1) {
        time(&now);
        strftime(time_str, sizeof(time_str), "[%Y-%m-%d %H:%M:%S]", localtime(&now));
        
        // Non-blocking read from the log FIFO
        int n = read(log_fifo_fd, buffer, sizeof(buffer) - 1);
        if (n > 0) {
            buffer[n] = '\0';
            fprintf(stderr, "%s %s", time_str, buffer);
        }
        
        // Check for any inactive child processes (timeout mechanism)
        // This is a simple implementation - a real one might be more complex
        
        // Sleep for a bit before checking again
        usleep(500000);  // 500 ms
    }
    
    close(log_fifo_fd);
}

int main(int argc, char *argv[]) {
    int n1, n2;
    // Initialize result to zero as required
    volatile int result = 0;
    pid_t child1_pid, child2_pid;
    
    // Check command line arguments
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <num1> <num2>\n", argv[0]);
        exit(EXIT_FAILURE);
    }
    
    // Parse integers from command line
    n1 = atoi(argv[1]);
    n2 = atoi(argv[2]);
    
    // Calculate the larger number and store in result
    result = (n1 > n2) ? n1 : n2;
    
    printf("Parent process calculated larger number: %d\n", result);
    
    // Create FIFOs
    if (mkfifo(FIFO1, 0666) < 0 && errno != EEXIST) {
        perror("Failed to create FIFO1");
        exit(EXIT_FAILURE);
    }
    
    if (mkfifo(FIFO2, 0666) < 0 && errno != EEXIST) {
        perror("Failed to create FIFO2");
        unlink(FIFO1);
        exit(EXIT_FAILURE);
    }
    
    if (mkfifo(LOG_FIFO, 0666) < 0 && errno != EEXIST) {
        perror("Failed to create LOG_FIFO");
        unlink(FIFO1);
        unlink(FIFO2);
        exit(EXIT_FAILURE);
    }
    
    // Set up signal handler for SIGCHLD
    struct sigaction sa;
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    if (sigaction(SIGCHLD, &sa, NULL) < 0) {
        perror("sigaction failed");
        exit(EXIT_FAILURE);
    }
    
    // Create daemon process
    daemon_pid = fork();
    if (daemon_pid < 0) {
        perror("Failed to create daemon");
        exit(EXIT_FAILURE);
    }
    
    // If this is the daemon process
    if (daemon_pid == 0) {
        // Daemonize properly
        if (setsid() < 0) {
            perror("setsid failed");
            exit(EXIT_FAILURE);
        }
        
        // Redirect standard file descriptors to log file
        int log_fd = open(DAEMON_LOG, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (log_fd < 0) {
            perror("Failed to open daemon log file");
            exit(EXIT_FAILURE);
        }
        
        dup2(log_fd, STDOUT_FILENO);
        dup2(log_fd, STDERR_FILENO);
        
        if (log_fd > STDERR_FILENO) {
            close(log_fd);
        }
        
        // Run the daemon
        run_daemon();
        exit(EXIT_SUCCESS);  // Should never reach here
    }
    
    printf("Daemon process created with PID: %d\n", daemon_pid);
    
    // Wait a moment for the daemon to start and open its FIFO
    sleep(1);
    
    // Log file for writing messages to daemon - try multiple times
    int log_fifo_fd = -1;
    int retries = 5;
    
    while (retries > 0 && log_fifo_fd < 0) {
        log_fifo_fd = open(LOG_FIFO, O_WRONLY);
        if (log_fifo_fd < 0) {
            printf("Waiting for daemon to open log FIFO (%d retries left)...\n", retries);
            sleep(1);
            retries--;
        }
    }
    
    if (log_fifo_fd < 0) {
        perror("Failed to open log FIFO for writing after retries");
        // Continue without logging rather than exiting
        log_fifo_fd = open("/dev/null", O_WRONLY);
    }
    
    // Write a message to the log
    char log_msg[256];
    snprintf(log_msg, sizeof(log_msg), "Parent process started with PID: %d\n", getpid());
    write(log_fifo_fd, log_msg, strlen(log_msg));
    
    // Create first child process
    child1_pid = fork();
    if (child1_pid < 0) {
        perror("Fork for child1 failed");
        snprintf(log_msg, sizeof(log_msg), "Fork for child1 failed: %s\n", strerror(errno));
        write(log_fifo_fd, log_msg, strlen(log_msg));
        exit(EXIT_FAILURE);
    }
    
    if (child1_pid == 0) {  // Child process 1
        // Open the FIFOs
        int fd1, fd2;
        int num1, num2, larger;
        
        // Sleep for 10 seconds as specified
        sleep(10);
        
        // Open FIFO1 for reading
        if ((fd1 = open(FIFO1, O_RDONLY)) < 0) {
            perror("Child 1: Cannot open FIFO1 for reading");
            exit(EXIT_FAILURE);
        }
        
        // Read the integers
        if (read(fd1, &num1, sizeof(int)) < 0 || read(fd1, &num2, sizeof(int)) < 0) {
            perror("Child 1: Failed to read from FIFO1");
            close(fd1);
            exit(EXIT_FAILURE);
        }
        close(fd1);
        
        // Determine the larger number
        larger = (num1 > num2) ? num1 : num2;
        
        // Open FIFO2 for writing
        if ((fd2 = open(FIFO2, O_WRONLY)) < 0) {
            perror("Child 1: Cannot open FIFO2 for writing");
            exit(EXIT_FAILURE);
        }
        
        // Write the larger number
        if (write(fd2, &larger, sizeof(int)) < 0) {
            perror("Child 1: Failed to write to FIFO2");
            close(fd2);
            exit(EXIT_FAILURE);
        }
        close(fd2);
        
        printf("Child 1: The larger number is %d\n", larger);
        exit(EXIT_SUCCESS);
    }
    
    // Create second child process
    child2_pid = fork();
    if (child2_pid < 0) {
        perror("Fork for child2 failed");
        snprintf(log_msg, sizeof(log_msg), "Fork for child2 failed: %s\n", strerror(errno));
        write(log_fifo_fd, log_msg, strlen(log_msg));
        exit(EXIT_FAILURE);
    }
    
    if (child2_pid == 0) {  // Child process 2
        // Open the FIFOs
        int fd2;
        int larger;
        
        // Sleep for 10 seconds as specified
        sleep(10);
        
        // Open FIFO2 for reading
        if ((fd2 = open(FIFO2, O_RDONLY)) < 0) {
            perror("Child 2: Cannot open FIFO2 for reading");
            exit(EXIT_FAILURE);
        }
        
        // Read the larger number
        if (read(fd2, &larger, sizeof(int)) < 0) {
            perror("Child 2: Failed to read from FIFO2");
            close(fd2);
            exit(EXIT_FAILURE);
        }
        close(fd2);
        
        printf("Child 2: The larger number is %d\n", larger);
        exit(EXIT_SUCCESS);
    }
    
    // Parent process continues here
    
    // Log child creation
    snprintf(log_msg, sizeof(log_msg), "Created Child 1 with PID: %d\n", child1_pid);
    write(log_fifo_fd, log_msg, strlen(log_msg));
    
    snprintf(log_msg, sizeof(log_msg), "Created Child 2 with PID: %d\n", child2_pid);
    write(log_fifo_fd, log_msg, strlen(log_msg));
    
    // Open FIFO1 for writing
    int fd1 = open(FIFO1, O_WRONLY);
    if (fd1 < 0) {
        perror("Parent: Cannot open FIFO1 for writing");
        snprintf(log_msg, sizeof(log_msg), "Cannot open FIFO1 for writing: %s\n", strerror(errno));
        write(log_fifo_fd, log_msg, strlen(log_msg));
        exit(EXIT_FAILURE);
    }
    
    // Write the integers to FIFO1
    if (write(fd1, &n1, sizeof(int)) < 0 || write(fd1, &n2, sizeof(int)) < 0) {
        perror("Parent: Failed to write to FIFO1");
        snprintf(log_msg, sizeof(log_msg), "Failed to write to FIFO1: %s\n", strerror(errno));
        write(log_fifo_fd, log_msg, strlen(log_msg));
        close(fd1);
        exit(EXIT_FAILURE);
    }
    
    close(fd1);
    
    // Main loop
    while (child_counter < num_children * 2) {
        printf("Proceeding... (counter: %d)\n", child_counter);
        
        snprintf(log_msg, sizeof(log_msg), "Parent: Proceeding... (counter: %d)\n", child_counter);
        write(log_fifo_fd, log_msg, strlen(log_msg));
        
        sleep(2);  // Print message every 2 seconds
    }
    
    // All children have exited, cleanup
    printf("All children have exited, cleaning up...\n");
    
    snprintf(log_msg, sizeof(log_msg), "All children have exited, cleaning up...\n");
    write(log_fifo_fd, log_msg, strlen(log_msg));
    
    // Send SIGTERM to daemon process
    if (daemon_pid > 0 && kill(daemon_pid, SIGTERM) < 0) {
        // Don't show error if process doesn't exist - it might have already exited
        if (errno != ESRCH) {
            perror("Failed to terminate daemon process");
        }
    }
    
    // Clean up FIFOs
    unlink(FIFO1);
    unlink(FIFO2);
    unlink(LOG_FIFO);
    
    close(log_fifo_fd);
    
    printf("Done.\n");
    return 0;
}