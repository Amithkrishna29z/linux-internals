/* epoll_server.c
 *
 * The scalable answer to C10K: a single-process, single-thread echo server
 * using epoll. Register the listening socket once; epoll_wait returns ONLY the
 * ready fds (O(ready), not O(watched)), so this scales to tens of thousands of
 * connections. This is, in miniature, how Nginx/Redis/Node handle I/O.
 *
 * Compile:  gcc -Wall -Wextra -o epoll_server epoll_server.c
 * Run:      ./epoll_server 8080   (connect many echo_clients at once)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <arpa/inet.h>

#define MAX_EVENTS 64

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
    if (listen(lfd, 128) < 0) { perror("listen"); return 1; }

    /* Create the epoll instance and register the listening socket. */
    int ep = epoll_create1(0);
    if (ep < 0) { perror("epoll_create1"); return 1; }

    struct epoll_event ev;
    ev.events = EPOLLIN;                 /* level-triggered: notify when readable */
    ev.data.fd = lfd;
    epoll_ctl(ep, EPOLL_CTL_ADD, lfd, &ev);      /* register the listener ONCE */
    printf("epoll echo server on port %d\n", port);

    struct epoll_event events[MAX_EVENTS];
    for (;;) {
        int n = epoll_wait(ep, events, MAX_EVENTS, -1);   /* block; returns READY fds */
        if (n < 0) { perror("epoll_wait"); break; }

        for (int i = 0; i < n; i++) {         /* loop over the n READY events only */
            int fd = events[i].data.fd;

            if (fd == lfd) {
                /* listener ready => accept the new client and register it */
                int cfd = accept(lfd, NULL, NULL);
                if (cfd < 0) { perror("accept"); continue; }
                struct epoll_event cev;
                cev.events = EPOLLIN;
                cev.data.fd = cfd;
                epoll_ctl(ep, EPOLL_CTL_ADD, cfd, &cev);   /* register client ONCE */
                printf("client %d connected\n", cfd);
            } else {
                /* client ready => echo, or clean up on disconnect */
                char buf[1024];
                ssize_t r = read(fd, buf, sizeof buf);
                if (r <= 0) {
                    printf("client %d disconnected\n", fd);
                    epoll_ctl(ep, EPOLL_CTL_DEL, fd, NULL);  /* deregister */
                    close(fd);
                } else {
                    write(fd, buf, (size_t)r);
                }
            }
        }
    }
    close(ep);
    return 0;
}
