#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <string.h> // For strdup, strstr, strncmp
#include <errno.h>
#include <sys/types.h> // pid_t
#include <libgen.h>    // basename
#include <stdbool.h>   // For bool type

#include "common.h"

static void usage(const char *prog)
{
    char *pcopy = strdup(prog);
    if (pcopy)
    {
        fprintf(stderr, "Usage: %s <cmdfile> [fifo]\n", basename(pcopy));
        free(pcopy);
    }
    else
        fprintf(stderr, "Usage: <prog> <cmdfile> [fifo]\n");
    fprintf(stderr, "  fifo defaults to %s\n", DEFAULT_SERVER_FIFO_NAME);
    exit(1);
}

int count_commands(const char *filename)
{
    FILE *f = fopen(filename, "r");
    if (!f)
        return 0;
    int count = 0;
    char line[128];
    while (fgets(line, sizeof(line), f))
    {
        line[strcspn(line, "\n\r")] = 0;
        if (strlen(line) > 0 && line[0] != '#')
            count++;
    }
    fclose(f);
    return count;
}

int main(int argc, char *argv[])
{
    if (argc < 2 || argc > 3)
        usage(argv[0]);
    const char *cmdfile = argv[1];
    char server_fifo_path[256];
    snprintf(server_fifo_path, sizeof(server_fifo_path), "%s", (argc == 3) ? argv[2] : DEFAULT_SERVER_FIFO_NAME);
    int num_commands = count_commands(cmdfile);
    FILE *fp = fopen(cmdfile, "r");
    if (!fp)
    {
        fprintf(stderr, "Client: Error opening '%s': %s\n", cmdfile, strerror(errno));
        exit(1);
    }

    pid_t pid = getpid(); // Get client's own PID
    printf("Reading %s..\n", cmdfile);
    printf("%d clients to connect.. creating clients..\n", num_commands);

    int srv_fd = open(server_fifo_path, O_WRONLY);
    if (srv_fd == -1)
    {
        fprintf(stderr, "Cannot connect %s...\n", server_fifo_path);
        fprintf(stderr, "exiting..\n");
        fclose(fp);
        exit(1);
    }
    printf("Connected to Adabank..\n");
    if (dprintf(srv_fd, "%d\n", pid) < 0)
    {
        perror("Client write PID");
        close(srv_fd);
        fclose(fp);
        exit(1);
    }
    close(srv_fd);

    char req_path[128], res_path[128];
    snprintf(req_path, sizeof(req_path), "/tmp/bank_%d_req", pid);
    snprintf(res_path, sizeof(res_path), "/tmp/bank_%d_res", pid);
    unlink(req_path);
    unlink(res_path);
    if (mkfifo(req_path, 0600) == -1)
    {
        perror("mkfifo req");
        fclose(fp);
        exit(1);
    }
    if (mkfifo(res_path, 0600) == -1)
    {
        perror("mkfifo res");
        unlink(req_path);
        fclose(fp);
        exit(1);
    }

    int req_fd = open(req_path, O_WRONLY);
    if (req_fd == -1)
    {
        perror("open req");
        unlink(req_path);
        unlink(res_path);
        fclose(fp);
        exit(1);
    }
    int res_fd = open(res_path, O_RDONLY);
    if (res_fd == -1)
    {
        perror("open res");
        close(req_fd);
        unlink(req_path);
        unlink(res_path);
        fclose(fp);
        exit(1);
    }

    char line[128], resp[256];
    int line_num = 0;
    int client_command_counter = 0;

    // --- Pre-calculate expected message prefixes using client's PID ---
    char expected_wrong_prefix[64];
    char expected_closed_prefix[64];
    char expected_served_prefix[64]; // For BankID_ part

    snprintf(expected_wrong_prefix, sizeof(expected_wrong_prefix), "Client%d something went WRONG", pid);
    snprintf(expected_closed_prefix, sizeof(expected_closed_prefix), "Client%d served.. account closed", pid);
    snprintf(expected_served_prefix, sizeof(expected_served_prefix), "Client%d served.. BankID_", pid);


    while (fgets(line, sizeof(line), fp))
    {
        line_num++;
        line[strcspn(line, "\n\r")] = '\0';
        if (strlen(line) == 0 || line[0] == '#')
            continue;

        client_command_counter++;

        char b_id_str[64], op_str[32], am_str[32];
        sscanf(line, "%63s %31s %31s", b_id_str, op_str, am_str);
        const char *action_str = "unknown action";
        if (strcmp(op_str, "deposit") == 0) action_str = "depositing";
        else if (strcmp(op_str, "withdraw") == 0) action_str = "withdrawing";
        printf("Client%d connected..%s %s credits\n", client_command_counter, action_str, am_str);
        fflush(stdout);

        char write_buf[130];
        snprintf(write_buf, sizeof(write_buf), "%s\n", line);
        ssize_t write_len = strlen(write_buf);
        if (write(req_fd, write_buf, write_len) != write_len)
        {
            if (errno == EPIPE) fprintf(stderr, "Client%d: Teller closed pipe.\n", client_command_counter);
            else perror("Client write cmd");
            break;
        }

        // --- Read and Parse Response ---
        memset(resp, 0, sizeof(resp)); // Clear buffer before read
        ssize_t n = read(res_fd, resp, sizeof(resp) - 1);
        if (n > 0)
        {
            // Note: No need to explicitly null-terminate if buffer was cleared,
            // but it doesn't hurt: resp[n] = '\0';

            // --- Use strncmp for parsing ---
            bool matched = false;

            // 1. Check for "something went WRONG" (allow for trailing newline)
            if (strncmp(resp, expected_wrong_prefix, strlen(expected_wrong_prefix)) == 0) {
                printf("Client%d something went WRONG\n", client_command_counter);
                matched = true;
            }
            // 2. Check for "account closed" (allow for trailing newline)
            else if (strncmp(resp, expected_closed_prefix, strlen(expected_closed_prefix)) == 0) {
                 printf("Client%d served.. account closed\n", client_command_counter);
                 matched = true;
            }
            // 3. Check for "served.. BankID_" (allow for trailing newline and ID)
            else if (strncmp(resp, expected_served_prefix, strlen(expected_served_prefix)) == 0) {
                 int parsed_bank_id = -1;
                 // Point id_ptr to where the ID number should start
                 char *id_ptr = resp + strlen(expected_served_prefix);
                 // Use sscanf specifically to parse the integer ID from that point
                 if (sscanf(id_ptr, "%d", &parsed_bank_id) == 1) {
                      printf("Client%d served.. BankID_%d\n", client_command_counter, parsed_bank_id);
                      matched = true;
                 } else {
                      // Prefix matched, but couldn't parse ID number - treat as error
                      fprintf(stderr, "Client%d Warning: Matched BankID prefix but failed to parse ID in response: [%s]\n", client_command_counter, resp);
                      printf("Client%d something went WRONG\n", client_command_counter);
                      matched = true; // Treat as handled error case
                 }
            }

            // 4. Fallback if no specific pattern matched
            if (!matched) {
                // Trim potential trailing newline before printing warning
                resp[strcspn(resp, "\n\r")] = 0;
                fprintf(stderr, "Client%d Warning: Unparsed response format: [%s]\n", client_command_counter, resp);
                printf("Client%d something went WRONG\n", client_command_counter); // Default to WRONG
            }

             printf("..\n"); // Print delimiter
             fflush(stdout);
        }
        else if (n == 0)
        {
            printf("Client (PID %d): Teller closed connection unexpectedly.\n", pid);
            break;
        }
        else // n < 0
        {
            if (errno == EINTR) {
                client_command_counter--;
                continue;
            }
            perror("Client read response");
            break;
        }
    } // End while(fgets...)

    fclose(fp);
    close(req_fd);
    close(res_fd);

    if (unlink(req_path) == -1 && errno != ENOENT) perror("Client unlink req");
    if (unlink(res_path) == -1 && errno != ENOENT) perror("Client unlink res");

    printf("exiting..\n");
    return 0;
}