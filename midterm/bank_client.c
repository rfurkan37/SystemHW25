#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <string.h>
#include <errno.h>

#include "common.h"

static void usage(const char *prog) {
    fprintf(stderr, "Usage: %s <command_file>\n", prog);
    exit(1);
}

int main(int argc, char *argv[]) {
    if (argc != 2) usage(argv[0]);

    const char *cmdfile = argv[1];
    FILE *fp = fopen(cmdfile, "r");
    if (!fp) { perror("fopen cmdfile"); exit(1); }

    /* inform server */
    pid_t pid = getpid();
    int srv_fd = open(SERVER_FIFO, O_WRONLY);
    if (srv_fd == -1) { perror("open server fifo"); exit(1); }
    dprintf(srv_fd, "%d\n", pid);
    close(srv_fd);

    /* create pipes */
    char req_path[64], res_path[64];
    snprintf(req_path, sizeof(req_path), "/tmp/bank_%d_req", pid);
    snprintf(res_path, sizeof(res_path), "/tmp/bank_%d_res", pid);
    mkfifo(req_path, 0600);
    mkfifo(res_path, 0600);

    int req_fd = open(req_path, O_WRONLY);
    int res_fd = open(res_path, O_RDONLY);

    if (req_fd == -1 || res_fd == -1) { perror("open fifos"); exit(1); }

    char line[128];
    while (fgets(line, sizeof(line), fp)) {
        /* write original line to teller */
        write(req_fd, line, strlen(line));
        /* wait for response line */
        char resp[128];
        ssize_t n = read(res_fd, resp, sizeof(resp)-1);
        if (n > 0) {
            resp[n] = 0;
            printf("%s", resp);
        }
    }

    fclose(fp);
    close(req_fd); close(res_fd);
    unlink(req_path); unlink(res_path);
    return 0;
}
