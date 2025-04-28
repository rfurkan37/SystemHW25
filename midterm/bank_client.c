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

/**
 * @brief Prints usage information and exits.
 * @param prog The program name (argv[0]).
 */
static void usage(const char *prog)
{
    char *pcopy = strdup(prog);
    fprintf(stderr, "Usage: %s <cmdfile> [fifo]\n", pcopy ? basename(pcopy) : "<prog>");
    if (pcopy) free(pcopy);
    fprintf(stderr, "  fifo defaults to %s\n", DEFAULT_SERVER_FIFO_NAME);
    exit(1);
}

/**
 * @brief Counts the number of valid commands (non-empty, non-comment lines) in a file.
 * @param filename Path to the command file.
 * @return The number of commands found.
 */
int count_commands(const char *filename)
{
    FILE *f = fopen(filename, "r");
    if (!f) return 0;
    int count = 0;
    char line[128];
    while (fgets(line, sizeof(line), f))
    {
        line[strcspn(line, "\n\r")] = 0; // Strip newline/cr
        // Count line if it's not empty and not a comment.
        if (strlen(line) > 0 && line[0] != '#')
            count++;
    }
    fclose(f);
    return count;
}

// --- Main Client Entry Point ---
int main(int argc, char *argv[])
{
    if (argc < 2 || argc > 3) usage(argv[0]);

    const char *cmdfile = argv[1];
    char server_fifo_path[256];
    // Use provided FIFO path or the default.
    snprintf(server_fifo_path, sizeof(server_fifo_path), "%s", (argc == 3) ? argv[2] : DEFAULT_SERVER_FIFO_NAME);

    int num_commands = count_commands(cmdfile);
    FILE *fp = fopen(cmdfile, "r");
    if (!fp)
    {
        fprintf(stderr, "Client: Error opening '%s': %s\n", cmdfile, strerror(errno));
        exit(1);
    }

    pid_t pid = getpid(); // Get client's own PID for identification and FIFO naming.
    printf("Reading %s..\n", cmdfile);
    printf("%d clients to connect.. creating clients..\n", num_commands); // Note: Reads commands sequentially, not spawning clients.

    // --- Connect to Server FIFO ---
    // Open the main server FIFO for writing the client's PID.
    int srv_fd = open(server_fifo_path, O_WRONLY);
    if (srv_fd == -1)
    {
        fprintf(stderr, "Cannot connect %s...\n", server_fifo_path);
        fprintf(stderr, "exiting..\n");
        fclose(fp);
        exit(1);
    }
    printf("Connected to Adabank..\n");

    // Send client PID to the server so it can spawn a dedicated Teller.
    if (dprintf(srv_fd, "%d\n", pid) < 0)
    {
        perror("Client write PID");
        close(srv_fd);
        fclose(fp);
        exit(1);
    }
    // Close the server FIFO fd; its purpose (sending PID) is done.
    close(srv_fd);

    // --- Create Client-Specific FIFOs ---
    // These FIFOs are used for communication between this client and its dedicated Teller.
    char req_path[128], res_path[128];
    snprintf(req_path, sizeof(req_path), "/tmp/bank_%d_req", pid); // Client -> Teller
    snprintf(res_path, sizeof(res_path), "/tmp/bank_%d_res", pid); // Teller -> Client

    // Remove any potential stale FIFOs from previous runs.
    unlink(req_path);
    unlink(res_path);

    // Create the FIFOs.
    if (mkfifo(req_path, 0600) == -1)
    {
        perror("mkfifo req");
        fclose(fp);
        exit(1);
    }
    if (mkfifo(res_path, 0600) == -1)
    {
        perror("mkfifo res");
        unlink(req_path); // Clean up partially created FIFOs.
        fclose(fp);
        exit(1);
    }

    // --- Open Client-Specific FIFOs ---
    // Open request FIFO for writing commands to the Teller.
    int req_fd = open(req_path, O_WRONLY);
    if (req_fd == -1)
    {
        perror("open req");
        unlink(req_path); unlink(res_path);
        fclose(fp);
        exit(1);
    }
    // Open response FIFO for reading results from the Teller.
    // Note: This open might block until the Teller opens the write end.
    int res_fd = open(res_path, O_RDONLY);
    if (res_fd == -1)
    {
        perror("open res");
        close(req_fd);
        unlink(req_path); unlink(res_path);
        fclose(fp);
        exit(1);
    }

    // --- Command Processing Loop ---
    char line[128], resp[256];
    int line_num = 0;
    int client_command_counter = 0; // Tracks which command this "client instance" is processing.

    // Pre-calculate expected response prefixes for efficient parsing.
    char expected_wrong_prefix[64];
    char expected_closed_prefix[64];
    char expected_served_prefix[64]; // Includes BankID_ part.
    snprintf(expected_wrong_prefix, sizeof(expected_wrong_prefix), "Client%d something went WRONG", pid);
    snprintf(expected_closed_prefix, sizeof(expected_closed_prefix), "Client%d served.. account closed", pid);
    snprintf(expected_served_prefix, sizeof(expected_served_prefix), "Client%d served.. BankID_", pid);


    while (fgets(line, sizeof(line), fp)) // Read commands from the file.
    {
        line_num++;
        line[strcspn(line, "\n\r")] = '\0'; // Strip newline/cr.
        if (strlen(line) == 0 || line[0] == '#') // Skip empty lines and comments.
            continue;

        client_command_counter++;

        // Parse command locally for printing status message.
        char b_id_str[64]="", op_str[32]="", am_str[32]=""; // Init to empty strings
        sscanf(line, "%63s %31s %31s", b_id_str, op_str, am_str);
        const char *action_str = "unknown action";
        if (strcmp(op_str, "deposit") == 0) action_str = "depositing";
        else if (strcmp(op_str, "withdraw") == 0) action_str = "withdrawing";
        printf("Client%d connected..%s %s credits\n", client_command_counter, action_str, am_str);
        fflush(stdout);

        // Send the command line to the Teller via the request FIFO.
        char write_buf[130];
        snprintf(write_buf, sizeof(write_buf), "%s\n", line); // Add newline delimiter.
        ssize_t write_len = strlen(write_buf);
        if (write(req_fd, write_buf, write_len) != write_len)
        {
            if (errno == EPIPE) fprintf(stderr, "Client%d: Teller closed connection (EPIPE).\n", client_command_counter);
            else perror("Client write cmd");
            break; // Stop processing if write fails.
        }

        // --- Read and Parse Response from Teller ---
        memset(resp, 0, sizeof(resp)); // Clear buffer before reading.
        ssize_t n = read(res_fd, resp, sizeof(resp) - 1); // Read response from Teller.
        if (n > 0)
        {
            resp[n] = '\0'; // Null-terminate the response.
            bool matched = false;

            // Use strncmp for robust prefix matching (handles potential trailing newline).
            // 1. Check for general error response.
            if (strncmp(resp, expected_wrong_prefix, strlen(expected_wrong_prefix)) == 0) {
                printf("Client%d something went WRONG\n", client_command_counter);
                matched = true;
            }
            // 2. Check for account closed response.
            else if (strncmp(resp, expected_closed_prefix, strlen(expected_closed_prefix)) == 0) {
                 printf("Client%d served.. account closed\n", client_command_counter);
                 matched = true;
            }
            // 3. Check for successful operation response (includes BankID).
            else if (strncmp(resp, expected_served_prefix, strlen(expected_served_prefix)) == 0) {
                 int parsed_bank_id = -1;
                 // Point to the expected start of the numeric ID after the prefix.
                 char *id_ptr = resp + strlen(expected_served_prefix);
                 // Attempt to parse the integer ID.
                 if (sscanf(id_ptr, "%d", &parsed_bank_id) == 1) {
                      printf("Client%d served.. BankID_%d\n", client_command_counter, parsed_bank_id);
                      matched = true;
                 } else {
                      // Prefix matched, but failed to parse the ID number - treat as an error.
                      fprintf(stderr, "Client%d Warning: Matched BankID prefix but failed to parse ID in response: [%s]\n", client_command_counter, resp);
                      printf("Client%d something went WRONG\n", client_command_counter); // Report as WRONG.
                      matched = true; // Indicate the response was handled (as an error).
                 }
            }

            // 4. Fallback if none of the expected patterns matched.
            if (!matched) {
                // Trim potential trailing newline from the unexpected response before printing.
                resp[strcspn(resp, "\n\r")] = 0;
                fprintf(stderr, "Client%d Warning: Unparsed response format from Teller: [%s]\n", client_command_counter, resp);
                printf("Client%d something went WRONG\n", client_command_counter); // Default to WRONG.
            }

             printf("..\n"); // Print delimiter as per example output.
             fflush(stdout);
        }
        else if (n == 0) // EOF on response pipe.
        {
            printf("Client (PID %d): Teller closed connection unexpectedly.\n", pid);
            break; // Stop processing.
        }
        else // n < 0 (Read error)
        {
            if (errno == EINTR) { // Interrupted by signal, retry reading command.
                 client_command_counter--; // Decrement counter as command wasn't fully processed.
                 continue;
            }
            perror("Client read response");
            break; // Stop processing on other read errors.
        }
    } // End while(fgets...)

    // --- Cleanup ---
    fclose(fp);    // Close command file.
    close(req_fd); // Close request FIFO.
    close(res_fd); // Close response FIFO.

    // Unlink the client-specific FIFOs.
    if (unlink(req_path) == -1 && errno != ENOENT) perror("Client unlink req");
    if (unlink(res_path) == -1 && errno != ENOENT) perror("Client unlink res");

    printf("exiting..\n");
    return 0;
}