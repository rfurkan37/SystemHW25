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
int global_log_fifo_fd = -1;
volatile sig_atomic_t daemon_running = 1;

// Signal handler for SIGCHLD
void sigchld_handler(int sig __attribute__((unused)))
{
    pid_t pid;
    int status;

    // Use waitpid with WNOHANG to avoid blocking
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0)
    {
        // Skip counting the daemon to avoid early termination
        if (pid == daemon_pid)
        {
            printf("Daemon process %d has exited with status %d\n", pid, WEXITSTATUS(status));
            daemon_pid = 0; // Mark daemon as handled
            continue;
        }

        printf("Child process %d has exited with status %d\n", pid, WEXITSTATUS(status));
        child_counter += 2;
    }
}

// Signal handler for daemon termination
void daemon_sigterm_handler(int sig __attribute__((unused)))
{
    // Log termination
    time_t now;
    char time_str[100];
    time(&now);
    strftime(time_str, sizeof(time_str), "[%Y-%m-%d %H:%M:%S]", localtime(&now));
    fprintf(stderr, "%s Daemon received SIGTERM, shutting down gracefully\n", time_str);

    // Close resources
    if (global_log_fifo_fd >= 0)
    {
        close(global_log_fifo_fd);
    }

    // Set flag to exit the main loop
    daemon_running = 0;

    // Forcefully exit to ensure termination
    _exit(EXIT_SUCCESS);
}

// Signal handler for daemon reconfiguration
void daemon_sighup_handler(int sig __attribute__((unused)))
{
    time_t now;
    char time_str[100];
    time(&now);
    strftime(time_str, sizeof(time_str), "[%Y-%m-%d %H:%M:%S]", localtime(&now));
    fprintf(stderr, "%s Daemon received SIGHUP, reconfiguring\n", time_str);

    // In a real implementation, this would reload configuration files
}

// Daemon process code
void run_daemon()
{
    time_t now;
    char time_str[100];
    int log_fifo_fd;

    // Create and open the log FIFO - make sure it exists
    unlink(LOG_FIFO); // Remove if exists
    if (mkfifo(LOG_FIFO, 0666) < 0)
    {
        fprintf(stderr, "Daemon: Failed to create log FIFO: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }

    fprintf(stderr, "Daemon: Opening log FIFO for reading\n");

    // Open the log FIFO - blocking open to ensure synchronization with parent
    if ((log_fifo_fd = open(LOG_FIFO, O_RDONLY)) < 0)
    {
        fprintf(stderr, "Daemon: Failed to open log FIFO: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }

    // Save to global for signal handlers
    global_log_fifo_fd = log_fifo_fd;

    // Set to non-blocking after opening
    int flags = fcntl(log_fifo_fd, F_GETFL, 0);
    fcntl(log_fifo_fd, F_SETFL, flags | O_NONBLOCK);
    // Non-blocking mode is used to prevent the daemon from hanging indefinitely
    // while waiting for input. This allows the daemon to perform other tasks
    // and check for termination signals between read attempts.

    // Set up signal handlers using sigaction for more reliability
    struct sigaction sa_term, sa_hup, sa_alrm;

    // SIGTERM handler
    memset(&sa_term, 0, sizeof(sa_term));
    sa_term.sa_handler = daemon_sigterm_handler;
    sigaction(SIGTERM, &sa_term, NULL);

    // SIGHUP handler
    memset(&sa_hup, 0, sizeof(sa_hup));
    sa_hup.sa_handler = daemon_sighup_handler;
    sigaction(SIGHUP, &sa_hup, NULL);

    // SIGALRM handler - use same as SIGTERM
    memset(&sa_alrm, 0, sizeof(sa_alrm));
    sa_alrm.sa_handler = daemon_sigterm_handler;
    sigaction(SIGALRM, &sa_alrm, NULL);

    // Other signals
    signal(SIGUSR1, SIG_IGN);               // Can be implemented later if needed
    signal(SIGINT, daemon_sigterm_handler); // Handle Ctrl+C as well

    // Initialize daemon monitoring structures
    time(&now);
    strftime(time_str, sizeof(time_str), "[%Y-%m-%d %H:%M:%S]", localtime(&now));
    fprintf(stderr, "%s Daemon started with PID: %d\n", time_str, getpid());

    // Main daemon loop
    char buffer[1024];
    // Set up an alarm as a safety mechanism
    alarm(60); // Force termination after 60 seconds if other mechanisms fail

    while (daemon_running)
    {
        time(&now);
        strftime(time_str, sizeof(time_str), "[%Y-%m-%d %H:%M:%S]", localtime(&now));

        // Non-blocking read from the log FIFO
        int n = read(log_fifo_fd, buffer, sizeof(buffer) - 1);
        if (n > 0)
        {
            buffer[n] = '\0';
            fprintf(stderr, "%s %s", time_str, buffer);
        }
        else if (n == 0)
        {
            // EOF on FIFO - all writers have closed, we can exit
            fprintf(stderr, "%s All writers closed FIFO, daemon exiting\n", time_str);
            daemon_running = 0;
            break;
        }

        // Check for any inactive child processes (timeout mechanism)
        // This is a simple implementation - a real one might be more complex

        // Sleep for a bit before checking again
        usleep(500000); // 500 ms
    }

    // Final cleanup
    time(&now);
    strftime(time_str, sizeof(time_str), "[%Y-%m-%d %H:%M:%S]", localtime(&now));
    fprintf(stderr, "%s Daemon exiting cleanly\n", time_str);

    close(log_fifo_fd);
    exit(EXIT_SUCCESS);
}

int main(int argc, char *argv[])
{
    int n1, n2;
    // Initialize result to zero as required
    volatile int result = 0;
    pid_t child1_pid = 0, child2_pid = 0;

    // Check command line arguments
    if (argc != 3)
    {
        fprintf(stderr, "Usage: %s <num1> <num2>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    // Parse integers from command line
    n1 = atoi(argv[1]);
    n2 = atoi(argv[2]);

    // Calculate the larger number and store in result
    result = (n1 > n2) ? n1 : n2;

    printf("Parent process calculated larger number: %d\n", result);

    // Create FIFOs - clean up any existing ones first
    unlink(FIFO1);
    unlink(FIFO2);
    unlink(LOG_FIFO);

    if (mkfifo(FIFO1, 0666) < 0)
    {
        perror("Failed to create FIFO1");
        exit(EXIT_FAILURE);
    }

    if (mkfifo(FIFO2, 0666) < 0)
    {
        perror("Failed to create FIFO2");
        unlink(FIFO1);
        exit(EXIT_FAILURE);
    }

    // Set up signal handler for SIGCHLD
    struct sigaction sa;
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    if (sigaction(SIGCHLD, &sa, NULL) < 0)
    {
        perror("sigaction failed");
        unlink(FIFO1);
        unlink(FIFO2);
        exit(EXIT_FAILURE);
    }

    // Create daemon process
    daemon_pid = fork();
    if (daemon_pid < 0)
    {
        perror("Failed to create daemon");
        unlink(FIFO1);
        unlink(FIFO2);
        exit(EXIT_FAILURE);
    }

    // If this is the daemon process
    if (daemon_pid == 0)
    {
        // Daemonize properly
        if (setsid() < 0)
        {
            perror("setsid failed");
            exit(EXIT_FAILURE);
        }

        // Redirect standard file descriptors to log file
        int log_fd = open(DAEMON_LOG, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (log_fd < 0)
        {
            perror("Failed to open daemon log file");
            exit(EXIT_FAILURE);
        }

        dup2(log_fd, STDOUT_FILENO);
        dup2(log_fd, STDERR_FILENO);

        if (log_fd > STDERR_FILENO)
        {
            close(log_fd);
        }

        // Close the unnecessary file descriptors
        close(STDIN_FILENO);

        // Run the daemon
        run_daemon();
        exit(EXIT_SUCCESS); // Should never reach here
    }

    printf("Daemon process created with PID: %d\n", daemon_pid);

    // Wait longer for the daemon to start and open its FIFO
    // sleep(3); // Increased from 1 to 3 seconds to ensure proper initialization

    // Log file for writing messages to daemon - try multiple times
    int log_fifo_fd = -1;
    int retries = 5;

    while (retries > 0 && log_fifo_fd < 0)
    {
        log_fifo_fd = open(LOG_FIFO, O_WRONLY | O_TRUNC);
        if (log_fifo_fd < 0)
        {
            printf("Waiting for daemon to open log FIFO (%d retries left)...\n", retries);
            sleep(1);
            retries--;
        }
    }

    if (log_fifo_fd < 0)
    {
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
    if (child1_pid < 0)
    {
        perror("Fork for child1 failed");
        snprintf(log_msg, sizeof(log_msg), "Fork for child1 failed: %s\n", strerror(errno));
        write(log_fifo_fd, log_msg, strlen(log_msg));

        // Kill daemon and clean up
        if (daemon_pid > 0)
        {
            kill(daemon_pid, SIGTERM);
        }
        unlink(FIFO1);
        unlink(FIFO2);
        unlink(LOG_FIFO);
        close(log_fifo_fd);
        exit(EXIT_FAILURE);
    }

    if (child1_pid == 0)
    { // Child process 1
        // Open the FIFOs
        int fd1, fd2;
        int num1, num2, larger;

        // Open FIFO1 for reading
        if ((fd1 = open(FIFO1, O_RDONLY)) < 0)
        {
            perror("Child 1: Cannot open FIFO1 for reading");
            exit(EXIT_FAILURE);
        }
        
        // Sleep for 10 seconds as specified
        sleep(10);

        // Read the integers
        if (read(fd1, &num1, sizeof(int)) < 0 || read(fd1, &num2, sizeof(int)) < 0)
        {
            perror("Child 1: Failed to read from FIFO1");
            close(fd1);
            exit(EXIT_FAILURE);
        }
        close(fd1);

        // Determine the larger number
        larger = (num1 > num2) ? num1 : num2;

        // Open FIFO2 for writing
        if ((fd2 = open(FIFO2, O_WRONLY)) < 0)
        {
            perror("Child 1: Cannot open FIFO2 for writing");
            exit(EXIT_FAILURE);
        }

        // Write the larger number
        if (write(fd2, &larger, sizeof(int)) < 0)
        {
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
    if (child2_pid < 0)
    {
        perror("Fork for child2 failed");
        snprintf(log_msg, sizeof(log_msg), "Fork for child2 failed: %s\n", strerror(errno));
        write(log_fifo_fd, log_msg, strlen(log_msg));

        // Kill the first child and daemon, then clean up
        if (child1_pid > 0)
        {
            kill(child1_pid, SIGTERM);
        }
        if (daemon_pid > 0)
        {
            kill(daemon_pid, SIGTERM);
        }
        unlink(FIFO1);
        unlink(FIFO2);
        unlink(LOG_FIFO);
        close(log_fifo_fd);
        exit(EXIT_FAILURE);
    }

    if (child2_pid == 0)
    { // Child process 2
        // Open the FIFOs
        int fd2;
        int larger;
        
        // Sleep for 10 seconds as specified
        sleep(10);

        // Open FIFO2 for reading
        if ((fd2 = open(FIFO2, O_RDONLY)) < 0)
        {
            perror("Child 2: Cannot open FIFO2 for reading");
            exit(EXIT_FAILURE);
        }

        // Read the larger number
        if (read(fd2, &larger, sizeof(int)) < 0)
        {
            perror("Child 2: Failed to read from FIFO2");
            close(fd2);
            exit(EXIT_FAILURE);
        }
        close(fd2);

        printf("Child 2: The larger number is %d\n", larger);
        exit(EXIT_SUCCESS);
    }


    // Open FIFO1 for writing
    int fd1 = open(FIFO1, O_WRONLY);
    if (fd1 < 0)
    {
        perror("Parent: Cannot open FIFO1 for writing");
        snprintf(log_msg, sizeof(log_msg), "Cannot open FIFO1 for writing: %s\n", strerror(errno));
        write(log_fifo_fd, log_msg, strlen(log_msg));

        // Kill children and daemon, then clean up
        if (child1_pid > 0)
        {
            kill(child1_pid, SIGTERM);
        }
        if (child2_pid > 0)
        {
            kill(child2_pid, SIGTERM);
        }
        if (daemon_pid > 0)
        {
            kill(daemon_pid, SIGTERM);
        }
        unlink(FIFO1);
        unlink(FIFO2);
        unlink(LOG_FIFO);
        close(log_fifo_fd);
        exit(EXIT_FAILURE);
    }

    // Parent process continues here

    // Log child creation
    snprintf(log_msg, sizeof(log_msg), "Created Child 1 with PID: %d\n", child1_pid);
    write(log_fifo_fd, log_msg, strlen(log_msg));

    snprintf(log_msg, sizeof(log_msg), "Created Child 2 with PID: %d\n", child2_pid);
    write(log_fifo_fd, log_msg, strlen(log_msg));

    // Write the integers to FIFO1
    if (write(fd1, &n1, sizeof(int)) < 0 || write(fd1, &n2, sizeof(int)) < 0)
    {
        perror("Parent: Failed to write to FIFO1");
        snprintf(log_msg, sizeof(log_msg), "Failed to write to FIFO1: %s\n", strerror(errno));
        write(log_fifo_fd, log_msg, strlen(log_msg));
        close(fd1);

        // Kill children and daemon, then clean up
        if (child1_pid > 0)
        {
            kill(child1_pid, SIGTERM);
        }
        if (child2_pid > 0)
        {
            kill(child2_pid, SIGTERM);
        }
        if (daemon_pid > 0)
        {
            kill(daemon_pid, SIGTERM);
        }
        unlink(FIFO1);
        unlink(FIFO2);
        unlink(LOG_FIFO);
        close(log_fifo_fd);
        exit(EXIT_FAILURE);
    }

    close(fd1);

    // Main loop
    while (child_counter < num_children * 2)
    {
        printf("Proceeding... (counter: %d)\n", child_counter);

        snprintf(log_msg, sizeof(log_msg), "Parent: Proceeding... (counter: %d)\n", child_counter);
        write(log_fifo_fd, log_msg, strlen(log_msg));

        sleep(2); // Print message every 2 seconds
    }

    // All children have exited, cleanup
    printf("All children have exited, cleaning up...\n");

    snprintf(log_msg, sizeof(log_msg), "All children have exited, cleaning up...\n");
    write(log_fifo_fd, log_msg, strlen(log_msg));

    // Wait a moment for messages to be processed
    // sleep(1);

    // Send SIGTERM to daemon process and wait to ensure it exits
    // In the parent process, where daemon termination is handled
    if (daemon_pid > 0)
    {
        snprintf(log_msg, sizeof(log_msg), "Sending SIGTERM to daemon (PID: %d)\n", daemon_pid);
        write(log_fifo_fd, log_msg, strlen(log_msg));

        // Close the log FIFO from parent side - this helps daemon detect EOF
        close(log_fifo_fd);
        log_fifo_fd = -1;

        // Send termination signal
        if (kill(daemon_pid, SIGTERM) == 0)
        {
            printf("Waiting for daemon to exit...\n");

            // Wait with timeout for daemon to terminate
            for (int i = 0; i < 3; i++)
            {
                if (waitpid(daemon_pid, NULL, WNOHANG) == daemon_pid ||
                    (kill(daemon_pid, 0) < 0 && errno == ESRCH))
                {
                    printf("Daemon has exited\n");
                    daemon_pid = 0;
                    break;
                }
                sleep(1);
            }

            // Force termination if still running
            if (daemon_pid > 0)
            {
                printf("Daemon didn't exit with SIGTERM, sending SIGKILL\n");
                kill(daemon_pid, SIGKILL);
                waitpid(daemon_pid, NULL, 0);
            }
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