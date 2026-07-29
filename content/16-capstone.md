# Module 16 — Capstone

> **Estimated time:** 6–8 hours · **Core path:** Build **one** of the three projects end to end. The worked example here is the **concurrent HTTP server** (it exercises the most of the course); the **container runtime** and **key–value store** are given as full design blueprints to build the same way. There's no new syscall to learn — the capstone is about *composing* what you already know into something real.
>
> **Prerequisites:** All of Modules 0–15. This is where the whole course converges: file I/O (3), processes (5), threads (6), memory (7), IPC (8), sockets (9), the epoll event loop (10), and the kernel-boundary understanding (13) that tells you *why* your design choices matter.

---

## The Big Picture

Fifteen modules ago, a program printing "hello" through `write` was a mystery worth a whole module. Now you understand the file descriptor that carried it, the syscall that trapped into the kernel, the buffering that batched it, the process that ran it, and the driver that ultimately moved the bytes. The capstone is where that understanding stops being a collection of facts and becomes an *engineering capability*: you build one substantial, real program — the kind that, at the start of this course, would have looked like magic — and discover there's no magic left in it. Just file descriptors, syscalls, memory, and the kernel you now know from both sides.

The rule of the capstone is **build one thing completely**. Three projects are offered, each a real category of infrastructure software, each exercising a different cross-section of the course. Pick the one that pulls hardest at you and build it end to end — running, tested, understood — rather than sampling all three. The goal isn't feature-completeness (these are teaching versions, not production replacements); it's the moment where you trace a request through *your own* code and can name what every line is doing at the syscall level, because you built all fifteen layers underneath it.

**The three paths:**

1. **A concurrent HTTP server** *(worked in full below)* — the most broadly useful, and the one that most directly rewards the socket + epoll modules. You'll accept many simultaneous connections in a single-threaded **event loop** (Module 10), parse HTTP requests, serve files from disk (Module 3), and speak a real protocol correctly (Content-Length, the TCP-is-a-stream framing lesson from Module 9). At the end you point a browser at it and it *works* — and you understand Nginx.

2. **A tiny container runtime** — the most mind-expanding. Using `clone` with **namespace** flags (`CLONE_NEWPID`, `CLONE_NEWNS`, `CLONE_NEWNET`, `CLONE_NEWUTS`) you create a process that has its *own* PID space (it thinks it's PID 1), its own mount tree, its own hostname — then `chroot`/`pivot_root` into a root filesystem and `exec` a shell inside it. Add **cgroups** (writing to `/sys/fs/cgroup`, Module 12) to cap its memory/CPU. In ~200 lines you build the core of Docker, and namespaces stop being jargon.

3. **A persistent key–value store** — the most classic systems project. A network server (Modules 9–10) speaking a simple `GET`/`SET` protocol over TCP, storing data in a **memory-mapped** file (Module 7's `mmap`) for persistence, with a hash index, concurrency control (Module 6), and a write-ahead log for durability. It's Redis's skeleton, and it forces you to confront the storage/durability/concurrency triangle every database lives in.

Each project ends the same way: with a program you can `strace` and *narrate*, a design you can defend, and the quiet confidence that the software running the internet is made of the same parts you now command. This module is lighter on new concepts (there are none) and heavier on synthesis — the payoff of the whole journey.

---

## Project 1 (worked): A Concurrent HTTP Server

### What you're building

A single-process, single-threaded HTTP/1.1 server that serves static files from a document root to many simultaneous clients using an **epoll event loop** — the Nginx architecture, in miniature. It ties together: `socket`/`bind`/`listen`/`accept` (Module 9), non-blocking fds + `epoll` (Module 10), `open`/`read` of files (Module 3), correct HTTP framing with `Content-Length` (the Module 9 "TCP has no message boundaries" lesson), and per-connection state machines (the real work of an event-loop server, Module 10).

### How it exercises the course

| Module | What the server uses it for |
|---|---|
| 3 (File I/O) | `open`/`read`/`close` to serve files; `fstat` for size/Content-Length |
| 4 (Console/fds) | stdout/stderr logging; fds as the universal handle |
| 5 (Processes) | (extension) fork worker processes; `SIGPIPE`/`SIGINT` handling |
| 9 (Sockets) | the listening socket, `accept`, `read`/`write` on connections, byte order |
| 10 (epoll) | the event loop: one thread, many clients, non-blocking, O(ready) |
| 13 (Syscalls) | *why* it batches reads/writes — every syscall is a ~300 ns trap |
| 12 (/proc) | (debugging) watch its fds in `/proc/self/fd`, connections in `/proc/net/tcp` |

### The code — `httpd.c`

```c
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
```

**Expected session:**
```
$ mkdir -p www && echo '<h1>It works!</h1>' > www/index.html
$ ./httpd 8080 ./www
httpd serving ./www on http://127.0.0.1:8080/
GET /                          ← logged when a browser/curl hits it
GET /index.html
GET /missing.html              ← returns 404

# another terminal:
$ curl -s http://127.0.0.1:8080/
<h1>It works!</h1>
$ curl -si http://127.0.0.1:8080/missing.html | head -1
HTTP/1.1 404 Not Found
```

**Walkthrough of the non-obvious parts:**
- **The per-connection `struct conn` is the heart of an event-loop server** and the thing Module 10 promised was "the real work." Because TCP is a byte stream (Module 9), a request may arrive in pieces across multiple `read`s; we accumulate into `c->req` and only parse once we've seen the end-of-headers marker `\r\n\r\n`. A naive server that assumed one `read` = one request would break under real network timing. Storing state *per connection* is what lets one thread juggle many half-finished requests.
- **`data.ptr` carries the connection object**, not just an fd. `epoll_event.data` is a union — we stash a pointer to the `struct conn` (or `NULL` for the listener) so that when `epoll_wait` returns an event, we immediately have that connection's state with no lookup. This is the idiomatic way to associate kernel readiness events with userspace state.
- **`accept` in a loop until `EAGAIN`** — when the listener is readable, there may be *several* pending connections; we accept all of them before returning to `epoll_wait` (especially important if you later switch to edge-triggered, Module 10). Accepting only one per event would leave connections queued.
- **`Content-Length` is load-bearing, not decoration** — it tells the client exactly how many body bytes follow, so it knows when the response is complete. This is the Module 9 framing lesson made concrete: over a raw TCP stream there are no message boundaries, so HTTP *declares* the length. (We also send `Connection: close` and close after one response, sidestepping keep-alive framing for simplicity.)
- **Path-traversal protection (`strstr(path, "..")`)** is a real security control: without it, `GET /../../etc/passwd` would escape the document root and serve arbitrary files. Every file-serving server needs this; it's the kind of thing that becomes a CVE when forgotten (and connects to Module 7/14's "never trust input crossing a boundary").
- **`SIGPIPE` ignored** (Module 9 gotcha) — a client disconnecting mid-response would otherwise deliver `SIGPIPE` and *kill the server*; ignoring it turns the failed `write` into an `EPIPE` error we handle. One line that separates a toy from something that survives real clients.
- **The honest limitation** (`write_all` spinning on `EAGAIN`): a fully correct server would, on a partial write to a slow client, register `EPOLLOUT` and resume the write when the socket is writable again (Module 10, Try This #4) rather than spin — otherwise one slow client can briefly stall the loop. We note it rather than hide it; fixing it is the natural next extension and the difference between this and Nginx.

### Extensions (make it yours)

- **Write buffering with `EPOLLOUT`** — the correct fix for the `write_all` limitation: per-connection output buffer, register `EPOLLOUT`, drain when writable. This is the last piece that makes it truly non-blocking (Module 10).
- **HTTP keep-alive** — don't close after one response; reset the connection's parse state and serve multiple requests per connection (requires careful framing with `Content-Length` on both sides).
- **A thread pool for blocking work** — offload slow file reads (or add CGI/proxying) to worker threads (Module 6) so the event loop never blocks — the hybrid model real servers use.
- **`fork` per-CPU workers** — `SO_REUSEPORT` + one epoll loop per core (Module 5 + 10), the Nginx multi-worker model, to use all cores.
- **Observe it** — `strace -f ./httpd` and narrate every syscall; watch its connections in `/proc/net/tcp` and fds in `/proc/self/fd` (Module 12); trace it with `bpftrace` (Module 15).

---

## Project 2 (blueprint): A Tiny Container Runtime

### What you're building
A program that launches a process in **isolation** — its own PID namespace (it's PID 1 inside), mount namespace, network namespace, and hostname — inside a root filesystem, optionally resource-capped with cgroups. The core of Docker in ~200 lines. **Requires root and a Linux host; do it in a VM.**

### The architecture
```
   parent:  clone(child_fn, stack,
                  CLONE_NEWPID|CLONE_NEWNS|CLONE_NEWUTS|CLONE_NEWNET|SIGCHLD, arg)
                     │  (like fork/Module 5, but the child gets NEW namespaces)
                     ▼
   child (in new namespaces):
     sethostname("container")            // own hostname (CLONE_NEWUTS)
     mount root fs; pivot_root / chroot  // own filesystem view (CLONE_NEWNS)
     mount -t proc proc /proc            // its own /proc (shows only its PIDs!)
     exec("/bin/sh")                     // becomes the container's init
   parent:
     write PID to /sys/fs/cgroup/.../cgroup.procs   // cap memory/CPU (Module 12)
     waitpid(child)                                  // reap it (Module 5)
```

### How it exercises the course
- **Module 5** — `clone` is `fork`'s powerful sibling; the child/parent split, `waitpid` reaping, exec into the containerized program. The child being "PID 1" makes Module 5's PID-1/init discussion visceral.
- **Module 1/3** — `chroot`/`pivot_root`, mounting filesystems, the filesystem tree as a per-container view.
- **Module 12** — cgroups are configured by *writing files* under `/sys/fs/cgroup` (`memory.max`, `cpu.max`, `cgroup.procs`) — Module 12's "control the kernel through files" applied to resource limits.
- **Module 8/9** — network namespaces and `veth` pairs for container networking (advanced extension).

### Key milestones to build toward
1. `clone` with `CLONE_NEWUTS` only; `sethostname` in the child; prove the parent's hostname is unchanged. *(Namespaces are per-resource.)*
2. Add `CLONE_NEWPID` + mount a private `/proc`; run `ps` inside and see *only* the container's processes (child is PID 1). *(The "aha" moment.)*
3. Add `CLONE_NEWNS` + `pivot_root` into a minimal root fs (e.g. a busybox/alpine rootfs) and `exec /bin/sh`. *(A real container shell.)*
4. Add a cgroup: create `/sys/fs/cgroup/mycontainer`, set `memory.max`, write the child PID to `cgroup.procs`; run a memory hog inside and watch it get OOM-killed at the limit. *(Resource isolation.)*

---

## Project 3 (blueprint): A Persistent Key–Value Store

### What you're building
A networked `GET`/`SET` server that persists data to disk via a memory-mapped file, with a hash index and durability via a write-ahead log — Redis's skeleton. **Confronts the storage + concurrency + durability triangle head-on.**

### The architecture
```
   clients ──TCP GET/SET──▶  server (epoll event loop, Modules 9+10)
                               │
                               ├── in-memory hash index  (key → offset)
                               ├── mmap'd data file       (Module 7: persistence
                               │      [records: klen|key|vlen|value ...]  at mem speed)
                               ├── append-only WAL         (durability: fsync on SET)
                               └── a mutex / RW-lock        (Module 6: concurrent access)
```

### How it exercises the course
- **Modules 9 + 10** — the TCP server and event loop (reuse the HTTP server's skeleton); a simple line protocol (`SET key value\r\n`, `GET key\r\n`).
- **Module 7** — `mmap` the data file so reads/writes are memory-speed and the OS handles paging to disk; store offsets (not pointers) into the mapped region (the Module 8 "pointers don't travel" rule).
- **Module 3** — the write-ahead log: append each mutation and `fsync` (Module 3's durability discussion) before acknowledging a `SET`, so a crash can't lose an acked write.
- **Module 6** — concurrency control: a mutex or reader/writer lock protecting the index and file (or shard the keyspace to reduce contention).

### Key milestones to build toward
1. In-memory only: TCP server, `GET`/`SET` against a hash map. *(Protocol + networking.)*
2. Persist: append every `SET` to a log file, replay it on startup. *(Durability via WAL — crash-safe.)*
3. `mmap` the data file for the value store; index holds offsets. *(Memory-mapped storage, Module 7.)*
4. Concurrency: handle simultaneous clients safely (event loop is single-threaded, so start there; add threads + locking as an extension). *(The concurrency triangle.)*

---

## Closing: There Is No Magic Left

Whichever project you built, step back and look at what it's made of. The HTTP server is `socket` + `epoll` + `read`/`write` + a state machine. The container is `clone` + namespaces + a few file writes. The key–value store is `mmap` + a hash table + `fsync` + a socket. Every one of those is a syscall or a data structure you now understand from the userspace call all the way down to the kernel trap, the `file_operations` dispatch, and (for a few) the driver and hardware beneath. The software that runs the internet — web servers, container orchestrators, databases, load balancers — is built from *exactly these parts*. You have not learned about that software; you have learned the material it is made of, and built a working member of each family with your own hands.

That is the real capstone: not the program, but the disappearance of magic. When you now read "Nginx uses an event loop," "Docker uses namespaces and cgroups," "Redis mmaps its data and fsyncs a log," "eBPF runs verified programs in the kernel" — none of it is a mysterious incantation. It's a specific arrangement of file descriptors, syscalls, memory mappings, and processes, each of which you can explain, `strace`, and reimplement. The file descriptor that carried your first `write` in Module 0 turned out to be the spine of the entire system: files, terminals, pipes, sockets, epoll instances, devices, even the kernel's own introspection — all the same idea wearing different clothes, all reachable with `open`/`read`/`write`/`close`, all crossing the one guarded boundary between your code and the machine.

You started as a Java developer for whom the OS was a black box beneath the JVM. You end able to build the black box. Go read real source — Nginx, redis, runc, the Linux kernel's `fs/` directory — and you'll find it *legible*, made of the concepts in these sixteen modules. There's more to learn (there always is: io_uring in depth, TCP internals, filesystem implementation, the scheduler), but you now have the foundation that makes all of it approachable, because you know the one secret the whole field is built on: **it's just files, memory, processes, and the kernel — and none of it is magic.**

---

## Final Checklist

Whichever path you chose, you've completed the capstone when you can:

1. **Run it for real** — point a browser/`curl` at your server, get a shell in your container, or `SET`/`GET` against your store, and see it work.
2. **`strace` and narrate it** — trace your program and explain what every syscall is doing and which module taught it to you.
3. **Defend the design** — explain *why* an event loop (not thread-per-connection), *why* namespaces (not just `chroot`), *why* a WAL (not just `mmap`) — the tradeoffs, in the vocabulary of the course.
4. **Name a real system it mirrors** — Nginx, Docker/runc, Redis — and identify what your teaching version omits and why that matters at scale.
5. **Extend it once** — add `EPOLLOUT` write buffering, a network namespace, or a reader/writer lock — and watch your understanding deepen from "I read about this" to "I built this."

*You've reached the end of the course. There's no "next up" — there's the whole open field of systems software, and you're now equipped to walk into any of it. Congratulations.*
