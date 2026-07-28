/* echo_server.c
 *
 * A minimal TCP echo server: socket -> bind -> listen -> accept loop.
 * Serves ONE client at a time (iterative), echoing back whatever it reads.
 * The read/write loop is identical to file/pipe I/O -- the network is just
 * another fd. (Concurrent version: see cserver.c.)
 *
 * Compile:  gcc -Wall -Wextra -o echo_server echo_server.c
 * Run:      ./echo_server 8080
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>     /* htons, inet_ntop, sockaddr_in */

int main(int argc, char **argv)
{
    int port = (argc > 1) ? atoi(argv[1]) : 8080;

    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) { perror("socket"); return 1; }

    /* SO_REUSEADDR: let us re-bind the port immediately after a restart
     * (otherwise it lingers in TIME_WAIT and bind fails). */
    int yes = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);   /* 0.0.0.0: all interfaces */
    addr.sin_port        = htons(port);          /* host->network byte order! */

    if (bind(lfd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        perror("bind"); return 1;
    }
    if (listen(lfd, 16) < 0) { perror("listen"); return 1; }   /* backlog 16 */
    printf("echo server listening on port %d\n", port);

    for (;;) {
        struct sockaddr_in cli;
        socklen_t clilen = sizeof cli;
        int cfd = accept(lfd, (struct sockaddr *)&cli, &clilen);  /* BLOCKS */
        if (cfd < 0) { perror("accept"); continue; }

        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &cli.sin_addr, ip, sizeof ip);
        printf("client connected: %s:%d\n", ip, ntohs(cli.sin_port));

        /* Echo loop: read from the client, write it straight back.
         * Same read/write as a file -- the fd just happens to be a socket. */
        char buf[1024];
        ssize_t n;
        while ((n = read(cfd, buf, sizeof buf)) > 0)
            write(cfd, buf, (size_t)n);          /* echo it back */
        /* read returns 0 when the client closes the connection (EOF) */

        printf("client disconnected\n");
        close(cfd);
    }
    /* not reached */
}
