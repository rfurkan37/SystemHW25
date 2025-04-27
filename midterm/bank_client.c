#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <string.h>
#include <errno.h>

#include "common.h"

static void usage(const char *prog)
{
    fprintf(stderr, "Usage: %s <command_file>\n", prog);
    exit(1);
}

int main(int argc, char *argv[])
{
    if (argc != 2)
        usage(argv[0]);

    const char *cmdfile = argv[1];
    FILE *fp = fopen(cmdfile, "r");
    // Assuming server FIFO is in CWD with default name if not passed as arg
    const char *server_fifo_name = DEFAULT_SERVER_FIFO_NAME;
    if (!fp)
    {
        perror("fopen cmdfile");
        exit(1);
    }

    /* inform server */
    pid_t pid = getpid();
    printf("Client (PID: %d): Reading commands from '%s'\n", pid, cmdfile);
    printf("Client (PID: %d): Contacting server via FIFO '%s'...\n", pid, server_fifo_name);
    int srv_fd = open(server_fifo_name, O_WRONLY);
    if (srv_fd == -1)
    {
        fprintf(stderr, "Client (PID: %d): Failed to open server FIFO '%s'. Is server running in this directory? Error: %s\n", pid, server_fifo_name, strerror(errno));
        fclose(fp); // Close file pointer before exiting
        exit(1);
    }
    printf("Client (PID: %d): Sending PID to server.\n", pid);
    // Ensure the write is atomic enough for the server to read cleanly
    // dprintf is generally fine. Adding a newline might help server parsing.
    if (dprintf(srv_fd, "%d\n", pid) < 0)
    {
        perror("Client failed to write PID to server FIFO");
        close(srv_fd);
        fclose(fp);
        exit(1);
    }
    fsync(srv_fd); // Try to ensure it's sent before closing
    close(srv_fd);

    /* create pipes */
    char req_path[64], res_path[64];
    snprintf(req_path, sizeof(req_path), "/tmp/bank_%d_req", pid);
    snprintf(res_path, sizeof(res_path), "/tmp/bank_%d_res", pid);
    // Clean up old FIFOs just in case
    unlink(req_path);
    unlink(res_path);
    if (mkfifo(req_path, 0600) == -1)
    {
        perror("Client mkfifo req_path failed");
        fclose(fp);
        exit(1);
    }
    if (mkfifo(res_path, 0600) == -1)
    {
        perror("Client mkfifo res_path failed");
        unlink(req_path); // Clean up first fifo
        fclose(fp);
        exit(1);
    }

    printf("Client (PID: %d): Created FIFOs: %s (req), %s (res)\n", pid, req_path, res_path);

    // Open request pipe blocking initially, but teller needs it O_RDONLY first.
    // Client opens O_WRONLY. Teller opens O_RDONLY. This is correct.
    int req_fd = open(req_path, O_WRONLY);
    // Open response pipe. Client needs O_RDONLY. Teller opens O_WRONLY.
    // Teller might not have opened its end yet, so O_RDONLY might block.
    // Consider O_RDONLY | O_NONBLOCK then fcntl back, or just let it block.
    int res_fd = open(res_path, O_RDONLY);

    if (req_fd == -1 || res_fd == -1)
    {
        if (req_fd == -1)
            perror("Client open req_fd failed");
        if (res_fd == -1)
            perror("Client open res_fd failed");
        // Clean up FIFOs if creation succeeded but open failed
        close(req_fd); // Close if open
        close(res_fd); // Close if open
        unlink(req_path);
        unlink(res_path);
        fclose(fp);
        exit(1);
    }

    printf("Client (PID: %d): Processing commands...\n", pid);
    char line[128];
    char resp[128];
    int line_num = 0;

    while (fgets(line, sizeof(line), fp))
    {
        line_num++;
        // Remove trailing newline if present, important for teller parsing
        line[strcspn(line, "\n")] = '\0';
        if (strlen(line) == 0)
            continue; // Skip empty lines

        printf("Client (PID: %d): Sending cmd #%d: '%s'\n", pid, line_num, line);
        // Need to add newline for teller's fgets
        ssize_t write_len = strlen(line) + 1; // +1 for null terminator? No, for newline!
        char write_buf[130];                  // line + newline + null
        snprintf(write_buf, sizeof(write_buf), "%s\n", line);
        write_len = strlen(write_buf);

        if (write(req_fd, write_buf, write_len) != write_len)
        {
            perror("Client write request error");
            break;
        }
        // Consider fsync(req_fd);

        /* wait for response line */
        printf("Client (PID: %d): Waiting for response #%d...\n", pid, line_num);
        ssize_t n = read(res_fd, resp, sizeof(resp) - 1);
        if (n > 0)
        {
            resp[n] = 0; // Null-terminate the response
            // Print the raw response from the teller
            printf("Client Response: %s", resp); // Teller includes newline usually
                                                 // Ensure output is visible immediately
            fflush(stdout);
        }
        else if (n == 0)
        {
            printf("Client (PID: %d): Server/Teller closed the response pipe unexpectedly (EOF).\n", pid);
            break;
        }
        else
        {
            perror("Client read response error");
            break;
        }
    }

    printf("Client (PID: %d): Command file processed. Cleaning up.\n", pid);
    fclose(fp);
    close(req_fd);
    close(res_fd);
    // Give teller a moment to potentially see EOF before unlinking
    // usleep(100000); // Optional small delay
    unlink(req_path);
    unlink(res_path);
    printf("Client (PID: %d): Finished.\n", pid);
    return 0;
}