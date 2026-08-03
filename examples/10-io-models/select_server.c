/* select_server.c
 *
 * A single-threaded, single-process echo server that handles MANY clients at
 * once using select() -- no fork, no threads. One select() call watches the
 * listening socket plus every connected client; we handle only the fds that
 * are ready. This is the classic pre-epoll multiplexed server.
 *
 * Compile:  gcc -Wall -Wextra -o select_server select_server.c
 * Run:      ./select_server 8080   (connect several echo_clients at once)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/select.h>
#include <arpa/inet.h>

int main(int argc, char **argv)
{
    int port = (argc > 1) ? atoi(argv[1]) : 8080;

    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    int yes = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(lfd, (struct sockaddr *)&addr, sizeof addr) < 0) { perror("bind"); return 1; }
    if (listen(lfd, 16) < 0) { perror("listen"); return 1; }
    printf("select echo server on port %d\n", port);

    fd_set master;          /* the set of all fds we care about */
    FD_ZERO(&master);
    FD_SET(lfd, &master);
    int maxfd = lfd;        /* select needs the highest fd number + 1 */

    for (;;) {
        fd_set readset = master;             /* select MODIFIES the set: copy it */
        if (select(maxfd + 1, &readset, NULL, NULL, NULL) < 0) {
            perror("select"); break;
        }

        /* Scan every fd up to maxfd to see which are ready (this is the O(n)). */
        for (int fd = 0; fd <= maxfd; fd++) {
            if (!FD_ISSET(fd, &readset)) continue;

            if (fd == lfd) {
                /* the listener is readable => a new client is waiting */
                int cfd = accept(lfd, NULL, NULL);
                if (cfd < 0) { perror("accept"); continue; }
                FD_SET(cfd, &master);         /* watch the new client too */
                if (cfd > maxfd) maxfd = cfd;
                printf("client %d connected\n", cfd);
            } else {
                /* an existing client is readable => echo, or handle disconnect */
                char buf[1024];
                ssize_t n = read(fd, buf, sizeof buf);
                if (n <= 0) {                 /* 0 = client closed; <0 = error */
                    printf("client %d disconnected\n", fd);
                    close(fd);
                    FD_CLR(fd, &master);      /* stop watching it */
                } else {
                    write(fd, buf, (size_t)n);   /* echo back */
                }
            }
        }
    }
    return 0;
}
