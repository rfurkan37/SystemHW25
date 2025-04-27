#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h> // pid_t

#include "common.h"

static void usage(const char *prog)
{
    fprintf(stderr, "Usage: %s <command_file> [server_fifo_name]\n", prog);
    fprintf(stderr, "  server_fifo_name defaults to %s if not provided.\n", DEFAULT_SERVER_FIFO_NAME);
    exit(1);
}

int main(int argc, char *argv[])
{
    if (argc < 2 || argc > 3)
        usage(argv[0]);

    const char *cmdfile = argv[1];
    const char *server_fifo_name = (argc == 3) ? argv[2] : DEFAULT_SERVER_FIFO_NAME;

    FILE *fp = fopen(cmdfile, "r");
    if (!fp)
    {
        perror("Client: Error opening command file");
        exit(1);
    }

    pid_t pid = getpid();
    printf("Client (PID %d): Reading commands from '%s'\n", pid, cmdfile);
    printf("Client (PID %d): Contacting server via FIFO '%s'...\n", pid, server_fifo_name);

    int srv_fd = open(server_fifo_name, O_WRONLY);
    if (srv_fd == -1)
    {
        fprintf(stderr, "Client (PID %d): Failed to open server FIFO '%s'. Is the server running? Error: %s\n",
                pid, server_fifo_name, strerror(errno));
        fclose(fp);
        exit(1);
    }

    printf("Client (PID %d): Sending PID to server.\n", pid);
    // Send PID followed by a newline for server parsing
    if (dprintf(srv_fd, "%d\n", pid) < 0)
    {
        perror("Client: Failed to write PID to server FIFO");
        close(srv_fd);
        fclose(fp);
        exit(1);
    }
    // fsync is likely unnecessary for FIFO, but doesn't hurt much
    // fsync(srv_fd);
    close(srv_fd); // Close FIFO after sending PID

    /* Create client-specific FIFOs */
    char req_path[128], res_path[128]; // Increased size slightly
    snprintf(req_path, sizeof(req_path), "/tmp/bank_%d_req", pid);
    snprintf(res_path, sizeof(res_path), "/tmp/bank_%d_res", pid);

    // Clean up potentially stale FIFOs first
    unlink(req_path);
    unlink(res_path);

    if (mkfifo(req_path, 0600) == -1)
    {
        perror("Client: mkfifo request pipe failed");
        fclose(fp);
        exit(1);
    }
    if (mkfifo(res_path, 0600) == -1)
    {
        perror("Client: mkfifo response pipe failed");
        unlink(req_path); // Clean up first FIFO
        fclose(fp);
        exit(1);
    }

    printf("Client (PID %d): Created FIFOs: %s (request), %s (response)\n", pid, req_path, res_path);
    printf("Client (PID %d): Waiting for Teller to open FIFOs...\n", pid);

    // Open request pipe (Write-Only). This will block until the Teller opens it for reading.
    int req_fd = open(req_path, O_WRONLY);
    if (req_fd == -1)
    {
        perror("Client: Error opening request FIFO for writing");
        unlink(req_path);
        unlink(res_path);
        fclose(fp);
        exit(1);
    }
    printf("Client (PID %d): Request FIFO opened by Teller.\n", pid);


    // Open response pipe (Read-Only). This will block until the Teller opens it for writing.
    int res_fd = open(res_path, O_RDONLY);
    if (res_fd == -1)
    {
        perror("Client: Error opening response FIFO for reading");
        close(req_fd); // Close the one that succeeded
        unlink(req_path);
        unlink(res_path);
        fclose(fp);
        exit(1);
    }
    printf("Client (PID %d): Response FIFO opened by Teller. Processing commands...\n", pid);


    char line[128];
    char resp[256]; // Slightly larger buffer for responses
    int line_num = 0;

    while (fgets(line, sizeof(line), fp))
    {
        line_num++;
        // Remove trailing newline if present
        line[strcspn(line, "\n\r")] = '\0';

        // Skip empty lines or lines starting with # (comments)
        if (strlen(line) == 0 || line[0] == '#')
            continue;

        printf("Client (PID %d): Sending Cmd #%d: '%s'\n", pid, line_num, line);

        // Send command WITH a newline for Teller's fgets
        char write_buf[130];
        snprintf(write_buf, sizeof(write_buf), "%s\n", line);
        ssize_t write_len = strlen(write_buf);

        if (write(req_fd, write_buf, write_len) != write_len)
        {
            if (errno == EPIPE) {
                 fprintf(stderr, "Client (PID %d): Error writing command #%d: Teller closed the pipe (EPIPE).\n", pid, line_num);
            } else {
                 perror("Client: Error writing command to request FIFO");
            }
            break; // Exit loop if write fails
        }

        /* Wait for response */
        // printf("Client (PID %d): Waiting for response #%d...\n", pid, line_num); // Less verbose
        ssize_t n = read(res_fd, resp, sizeof(resp) - 1);
        if (n > 0)
        {
            resp[n] = '\0'; // Null-terminate the received data
            // Teller's response should already include a newline
            printf("Client Response: %s", resp);
            fflush(stdout); // Ensure output is visible immediately
        }
        else if (n == 0)
        {
            // End Of File - Teller closed the write end of the response pipe
            printf("Client (PID %d): Received EOF from response pipe. Teller likely finished or closed connection.\n", pid);
            break;
        }
        else
        {
            // Read error
            perror("Client: Error reading response from response FIFO");
            break;
        }
    }

    printf("Client (PID %d): Command file '%s' processed. Cleaning up.\n", pid, cmdfile);
    fclose(fp);
    close(req_fd);
    close(res_fd);

    // Unlink FIFOs after closing them
    // Give Teller a moment? Usually not necessary, OS handles cleanup.
    // usleep(50000);
    if (unlink(req_path) == -1) {
        perror("Client: Warning: Failed to unlink request FIFO");
    }
    if (unlink(res_path) == -1) {
        perror("Client: Warning: Failed to unlink response FIFO");
    }

    printf("Client (PID %d): Finished.\n", pid);
    return 0;
}