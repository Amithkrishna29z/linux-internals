/* httpd.c -- a concurrent static-file HTTP server on a single-thread epoll
 * event loop. Serves files from a document root to many clients at once.
 * The capstone of Modules 3, 9, and 10.
 *
 * Compile:  gcc -Wall -Wextra -O2 -o httpd httpd.c
 * Run:      ./httpd 8080 ./www          (serve ./www on port 8080)
 *   then:   curl http://127.0.0.1:8080/index.html
 *           or open http://127.0.0.1:8080/ in a browser
 *
 * Teaching scope: handles the common case correctly (GET, Content-Length,
 * path-traversal protection, many concurrent connections). Noted limitations
 * (large-file write buffering via EPOLLOUT, request pipelining) are left as
 * extensions -- see the module's "Extensions" section.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/stat.h>
#include <arpa/inet.h>

#define MAX_EVENTS 128
#define REQ_MAX    8192

static const char *g_docroot = ".";

/* Per-connection state: accumulate the request until we see the end of headers
 * (\r\n\r\n). This is the "per-connection state machine" an event loop needs
 * because a request may arrive across several reads (TCP is a byte stream). */
struct conn {
    int  fd;
    char req[REQ_MAX];
    size_t len;
};

/* Make an fd non-blocking (Module 10) -- required for a correct event loop. */
static int set_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL);
    return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

/* Write all `n` bytes, looping past short writes (Module 9 gotcha). Blocking-
 * style for simplicity; a production server would buffer and watch EPOLLOUT. */
static int write_all(int fd, const char *buf, size_t n)
{
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(fd, buf + off, n - off);
        if (w < 0) {
            if (errno == EINTR) continue;              /* Module 3: retry EINTR */
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue; /* spin briefly */
            return -1;                                  /* real error (e.g. EPIPE) */
        }
        off += (size_t)w;
    }
    return 0;
}

/* Send a small status-only response (errors). */
static void send_status(int fd, int code, const char *reason)
{
    char hdr[256];
    int n = snprintf(hdr, sizeof hdr,
        "HTTP/1.1 %d %s\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%s\n",
        code, reason, strlen(reason) + 1, reason);
    write_all(fd, hdr, (size_t)n);
}

/* Serve the file at docroot+path. Sends headers with Content-Length (so the
 * client knows where the body ends -- the Module 9 framing lesson) then body. */
static void serve_file(int fd, const char *path)
{
    /* Path-traversal protection: reject any ".." so a client can't escape the
     * docroot and read /etc/passwd. A real security requirement, not optional. */
    if (strstr(path, "..")) { send_status(fd, 403, "Forbidden"); return; }

    char full[1024];
    const char *rel = (strcmp(path, "/") == 0) ? "/index.html" : path;
    snprintf(full, sizeof full, "%s%s", g_docroot, rel);

    int ffd = open(full, O_RDONLY);                     /* Module 3 */
    if (ffd < 0) { send_status(fd, 404, "Not Found"); return; }

    struct stat st;
    if (fstat(ffd, &st) < 0 || !S_ISREG(st.st_mode)) {
        close(ffd); send_status(fd, 404, "Not Found"); return;
    }

    /* Headers: Content-Length tells the client exactly how many body bytes
     * follow -- without it, over a keep-alive stream the client can't tell
     * where the response ends (TCP has no message boundaries, Module 9). */
    char hdr[256];
    int hn = snprintf(hdr, sizeof hdr,
        "HTTP/1.1 200 OK\r\nContent-Length: %lld\r\nConnection: close\r\n\r\n",
        (long long)st.st_size);
    if (write_all(fd, hdr, (size_t)hn) < 0) { close(ffd); return; }

    /* Body: read the file in chunks and write each to the socket. Same read/
     * write loop as copying a file (Module 3) -- the socket is just an fd. */
    char buf[65536];
    ssize_t r;
    while ((r = read(ffd, buf, sizeof buf)) > 0)
        if (write_all(fd, buf, (size_t)r) < 0) break;
    close(ffd);
}

/* Parse the request line "GET /path HTTP/1.1" and dispatch. */
static void handle_request(struct conn *c)
{
    char method[16], path[1024];
    if (sscanf(c->req, "%15s %1023s", method, path) != 2) {
        send_status(c->fd, 400, "Bad Request"); return;
    }
    if (strcmp(method, "GET") != 0) {
        send_status(c->fd, 501, "Not Implemented"); return;
    }
    printf("%s %s\n", method, path);
    serve_file(c->fd, path);
}

int main(int argc, char **argv)
{
    int port = (argc > 1) ? atoi(argv[1]) : 8080;
    if (argc > 2) g_docroot = argv[2];

    signal(SIGPIPE, SIG_IGN);   /* Module 9: don't die when a client vanishes */

    /* --- Listening socket (Module 9) --- */
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
    set_nonblock(lfd);

    /* --- epoll event loop (Module 10) --- */
    int ep = epoll_create1(0);
    struct epoll_event ev = { .events = EPOLLIN, .data.ptr = NULL };  /* NULL = listener */
    epoll_ctl(ep, EPOLL_CTL_ADD, lfd, &ev);
    printf("httpd serving %s on http://127.0.0.1:%d/\n", g_docroot, port);

    struct epoll_event events[MAX_EVENTS];
    for (;;) {
        int n = epoll_wait(ep, events, MAX_EVENTS, -1);
        if (n < 0) { if (errno == EINTR) continue; perror("epoll_wait"); break; }

        for (int i = 0; i < n; i++) {
            struct conn *c = events[i].data.ptr;

            if (c == NULL) {
                /* Listener ready: accept ALL pending connections (Module 10). */
                for (;;) {
                    int cfd = accept(lfd, NULL, NULL);
                    if (cfd < 0) break;                 /* EAGAIN: drained */
                    set_nonblock(cfd);
                    struct conn *nc = calloc(1, sizeof *nc);
                    if (!nc) { close(cfd); continue; }   /* OOM: drop this client */
                    nc->fd = cfd;
                    struct epoll_event cev = { .events = EPOLLIN, .data.ptr = nc };
                    epoll_ctl(ep, EPOLL_CTL_ADD, cfd, &cev);
                }
                continue;
            }

            /* A client fd is readable: accumulate request bytes. */
            ssize_t r = read(c->fd, c->req + c->len, REQ_MAX - 1 - c->len);
            if (r <= 0) {                               /* 0 = closed; <0 = error */
                epoll_ctl(ep, EPOLL_CTL_DEL, c->fd, NULL);
                close(c->fd); free(c);
                continue;
            }
            c->len += (size_t)r;
            c->req[c->len] = '\0';

            /* Have we received the full header block yet? (\r\n\r\n) */
            if (strstr(c->req, "\r\n\r\n") || c->len >= REQ_MAX - 1) {
                handle_request(c);                       /* parse + serve */
                epoll_ctl(ep, EPOLL_CTL_DEL, c->fd, NULL);
                close(c->fd); free(c);                   /* Connection: close */
            }
            /* else: partial request -- stay registered, read more next event */
        }
    }
    close(ep);
    return 0;
}
