/* echo_client.c
 *
 * A minimal TCP client: socket -> connect, then send stdin lines to the
 * server and print whatever it echoes back. Ctrl-D (EOF) ends it.
 *
 * Compile:  gcc -Wall -Wextra -o echo_client echo_client.c
 * Run:      ./echo_client 127.0.0.1 8080
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s <ip> <port>\n", argv[0]);
        return 2;
    }
    const char *ip   = argv[1];
    int         port = atoi(argv[2]);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
        fprintf(stderr, "bad IP: %s\n", ip); return 1;
    }

    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        perror("connect"); return 1;             /* server down? wrong port? */
    }
    printf("connected to %s:%d -- type lines, Ctrl-D to quit\n", ip, port);

    char line[1024];
    while (fgets(line, sizeof line, stdin) != NULL) {
        size_t len = strlen(line);
        if (write(fd, line, len) < 0) { perror("write"); break; }

        /* read the echo back and print it */
        ssize_t n = read(fd, line, sizeof line - 1);
        if (n <= 0) { printf("server closed\n"); break; }
        line[n] = '\0';
        printf("echo: %s", line);
    }

    close(fd);
    return 0;
}
