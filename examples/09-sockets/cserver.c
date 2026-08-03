/* cserver.c
 *
 * A CONCURRENT TCP echo server: fork a child per accepted client so many
 * clients are served at once. Reaps finished children with a SIGCHLD handler
 * (Module 5) so no zombies accumulate. This is the classic pre-epoll server
 * model -- simple, robust, but one process per connection (see Module 10).
 *
 * Compile:  gcc -Wall -Wextra -o cserver cserver.c
 * Run:      ./cserver 8080
 *   then connect several ./echo_client 127.0.0.1 8080 at once -- all work.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <errno.h>
#include <arpa/inet.h>

/* Reap all finished children (Module 5's SIGCHLD idiom): loop with WNOHANG. */
static void on_sigchld(int signo)
{
    (void)signo;
    int saved = errno;
    while (waitpid(-1, NULL, WNOHANG) > 0)
        ;
    errno = saved;
}

/* Serve one client: echo until it disconnects. Runs in the child. */
static void serve(int cfd)
{
    char buf[1024];
    ssize_t n;
    while ((n = read(cfd, buf, sizeof buf)) > 0)
        write(cfd, buf, (size_t)n);
}

int main(int argc, char **argv)
{
    int port = (argc > 1) ? atoi(argv[1]) : 8080;

    /* Install the child reaper. */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigchld;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;               /* restart accept() across SIGCHLD */
    sigaction(SIGCHLD, &sa, NULL);

    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    int yes = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(port);
    if (bind(lfd, (struct sockaddr *)&addr, sizeof addr) < 0) { perror("bind"); return 1; }
    if (listen(lfd, 16) < 0) { perror("listen"); return 1; }
    printf("concurrent echo server on port %d\n", port);

    for (;;) {
        int cfd = accept(lfd, NULL, NULL);
        if (cfd < 0) {
            if (errno == EINTR) continue;   /* SIGCHLD interrupted accept: retry */
            perror("accept"); continue;
        }

        pid_t pid = fork();
        if (pid < 0) { perror("fork"); close(cfd); continue; }

        if (pid == 0) {
            /* CHILD: handle this one client; doesn't need the listener. */
            close(lfd);
            serve(cfd);
            close(cfd);
            _exit(0);
        }

        /* PARENT: doesn't need this client's fd; loop back to accept at once. */
        close(cfd);
    }
    /* not reached */
}
