# Module 10 — I/O Models and Asynchronous I/O

> **Estimated time:** 3–4 hours · **Core path:** Concepts 1–4 (blocking vs non-blocking, the multiplexing idea, `select`/`poll`, `epoll`) and the `select_server` + `epoll_server` programs are core. The event-loop architecture discussion and the `io_uring` preview (Concept 5) are core-but-forward-looking; edge- vs level-triggered subtleties are a second-pass topic.
>
> **Prerequisites:** Modules 0–9. This module directly resolves the **C10K** cliffhanger from Module 9 (fork-per-connection doesn't scale). You need sockets and `accept`/`read`/`write` (Module 9), blocking I/O and `EINTR` (Module 3), and the "everything is an fd" spine — because multiplexing works uniformly across sockets, pipes, and files.

---

## The Big Picture

Module 9 left you with a working server and a problem: fork-per-connection spends a whole process on each client, so ten thousand idle-but-open connections cost ten thousand processes — gigabytes of RAM and a scheduler drowning in context switches. This is the famous **C10K problem**, and its solution reshaped how the entire internet is built. The insight is simple to state and profound in consequence: **most connections are idle most of the time.** A chat server with 10,000 users has maybe 50 actively sending bytes at any instant; the other 9,950 sockets are just *waiting*. Dedicating a process (or thread) to each waiting socket is enormous waste. What if *one* thread could watch all 10,000 sockets at once, and do work only for the handful that are actually ready? That is **I/O multiplexing**, and it's the foundation of Nginx, Redis, Node.js, HAProxy, and Netty — the highest-performance servers on Earth run on a single (or few) thread(s) juggling tens of thousands of connections.

To get there you first need to unlearn a hidden assumption. Every `read`/`accept`/`recv` you've written **blocks**: if there's no data, the calling thread sleeps until there is. That's why fork/thread-per-connection exists at all — a blocked thread can't do anything else, so you need one per connection. The alternative is a **non-blocking** fd (`O_NONBLOCK`): now `read` returns *immediately* — with data if there's data, or with the error `EAGAIN`/`EWOULDBLOCK` meaning "nothing ready right now, try later." A non-blocking fd lets one thread poke many sockets without getting stuck on any one. But naively looping over thousands of non-blocking fds asking "ready? ready? ready?" (a *busy-poll*) burns 100% CPU doing nothing — the same trap as busy-waiting on a condition variable (Module 6). We need the kernel to tell us *which* fds are ready, so we can sleep until at least one is, then handle exactly those.

That's what the multiplexing syscalls do. **`select`** and **`poll`** are the classic answer: hand the kernel a set of fds, and the call blocks until one or more become ready (or a timeout), then tells you which. One thread, one blocking call, many fds — no process-per-connection. But `select`/`poll` have a fatal flaw at scale: they're **O(n)** — every call passes the *entire* fd set to the kernel and the kernel scans *all* of them, so watching 10,000 fds means copying and scanning 10,000 entries on every single wait, even if only one is ready. **`epoll`** (Linux's answer, and the star of this module) fixes this: you register fds *once* with the kernel, and each wait returns *only the ready ones* in **O(ready)**, not O(total). This is the difference between a server that melts at 1,000 connections and one that hums at 100,000. We'll build an `epoll`-based echo server — a single process, no forking, handling arbitrarily many clients — and it *is*, in miniature, how Nginx works.

Finally, the frontier. Even `epoll` has a cost: it tells you *when* an fd is ready, but you still call `read`/`write` yourself (this is "readiness" notification). The newest model, **`io_uring`**, goes further to true **asynchronous I/O**: you submit operations ("read these bytes into this buffer") to a shared ring buffer, the kernel performs them in the background, and posts completions back — you're notified when the work is *done*, not merely *possible*, and with far fewer syscalls. It's the model async runtimes are racing to adopt. Understanding the progression — blocking → non-blocking → multiplexed (`select`→`poll`→`epoll`) → fully async (`io_uring`) — is understanding the last forty years of the fight to make servers fast, and exactly why the code behind your favorite framework looks the way it does.

---

## Concepts

### 1. Blocking vs non-blocking file descriptors

**What it is:** By default an fd is **blocking**: a `read` with no data available *sleeps the calling thread* until data arrives (and `write` sleeps if the buffer is full, `accept` sleeps until a client connects). Set the `O_NONBLOCK` flag (via `fcntl` or at open/accept time) and the fd becomes **non-blocking**: the same calls return *immediately* — either with whatever is available, or with the error `EAGAIN` (a.k.a. `EWOULDBLOCK`) meaning "no data/space right now."

```
   BLOCKING read on empty socket:
     read(fd, buf, n)  ── thread SLEEPS here until bytes arrive ──> returns n

   NON-BLOCKING read on empty socket:
     read(fd, buf, n)  ── returns -1 IMMEDIATELY, errno == EAGAIN
                          "nothing ready; go do something else, try later"

   fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);   // make fd non-blocking
```

**Why it exists:** Blocking is the simple, natural model — one thread, one task, sleep when there's nothing to do — but it *ties up a thread per in-flight operation*, which is the root of the process/thread-per-connection cost. Non-blocking I/O breaks that coupling: a single thread can attempt I/O on many fds without getting stuck on any one, which is the prerequisite for one thread serving thousands of connections. The cost is complexity — you must handle `EAGAIN` everywhere and manage the "what do I do while waiting" logic yourself.

**Java analogy:** Blocking is `InputStream.read()` / classic `Socket` — the thread parks until data comes, the model behind thread-per-request servlets. Non-blocking is NIO's `SocketChannel.configureBlocking(false)`, where `read()` returns `0` immediately when nothing's available (Java's `EAGAIN`). The whole `java.nio` package exists for the same reason `O_NONBLOCK` does: to let one thread manage many channels. Project Loom's virtual threads are a *third* path — keep the simple blocking API but make the "threads" cheap enough to have millions.

### 2. The multiplexing idea: watch many fds, sleep until one is ready

**What it is:** **I/O multiplexing** is a syscall that takes a *set* of file descriptors and blocks until *at least one* of them is ready for I/O (readable, writable, or errored) — then returns and tells you which. One thread makes one blocking call and is woken precisely when there's work to do, on whichever fd(s) became ready.

```
   fds you care about:  [ listen_fd, client3, client7, client42, ... 9997 more ]
                                 │
                                 ▼
      select/poll/epoll_wait(all of them)  ── ONE thread SLEEPS here ──
                                 │
                                 ▼  (kernel wakes it when any are ready)
   returns:  "client7 and client42 are readable"
                                 │
                                 ▼
   handle ONLY client7 and client42 (read/echo), then wait again.
```

**Why it exists:** It's the resolution of the tension in Concept 1: non-blocking fds let one thread touch many sockets, but busy-polling them all burns CPU. Multiplexing gives you the efficiency of *sleeping* (no CPU used while idle) *and* the scalability of *one thread for many fds* — the kernel does the waiting for all of them at once and reports only the ready ones. This is the single most important idea for scalable servers: **decouple the number of connections from the number of threads.** One event loop, thousands of connections.

**Java analogy:** `java.nio.channels.Selector` **is** this: you `register` channels with a `Selector`, call `selector.select()` (blocks until channels are ready), then iterate the `selectedKeys()` to handle exactly the ready ones. The `Selector` is implemented on top of `epoll` (Linux), `kqueue` (macOS/BSD), or equivalents. When you write a Netty server or an async framework, this select-loop is the engine underneath — you're about to build its C original.

### 3. `select` and `poll`: the classic (O(n)) multiplexers

**What it is:** The two portable, long-standing multiplexing syscalls:
- **`select(nfds, &readset, &writeset, &exceptset, timeout)`** — you fill bitmask sets of fds to watch; it blocks until some are ready, then *modifies the sets in place* to contain only the ready ones. Limited to `FD_SETSIZE` (usually **1024**) fds.
- **`poll(fds[], nfds, timeout)`** — you pass an array of `struct pollfd { fd; events; revents; }`; it fills each `revents` with what's ready. No hard fd-count limit, and a cleaner API than `select`.

```
   struct pollfd fds[N];
   fds[0].fd = listen_fd;  fds[0].events = POLLIN;   // want "readable"
   ... fill in client fds ...
   int ready = poll(fds, N, -1);                     // block until something ready
   for (i = 0; i < N; i++)
       if (fds[i].revents & POLLIN) handle(fds[i].fd);   // scan ALL N to find ready
```

**Why it exists (and their flaw):** They were the original answer to "watch many fds," and they're **portable** (POSIX, work everywhere) — still the right choice for small fd counts or cross-platform code. But both are **O(n)** in the number of watched fds: on *every* call you pass the whole set to the kernel (a copy), the kernel scans *all* of them, and on return you scan *all* of them to find the ready few. At 10,000 fds where only 1 is ready, you copy and scan 10,000 entries to do 1 unit of work — the overhead dominates. `select` additionally caps out at 1024 fds. This O(n)-per-call cost is exactly what `epoll` was invented to eliminate.

**Java analogy:** Old JDK `Selector` implementations used `poll` under the hood; the `Selector` API hides which syscall it maps to. You rarely call `select`/`poll` semantics directly in Java, but the "iterate all keys to find the ready ones" shape of a naive `Selector` loop mirrors `poll`'s O(n) scan — and Java's `Selector` on Linux moved to `epoll` for the same scalability reason described next.

### 4. `epoll`: scalable (O(ready)) event notification

**What it is:** Linux's high-performance multiplexer, built around a kernel-side data structure you interact with through three calls:
- **`epoll_create1(0)`** → returns an **epoll fd** (yes, the multiplexer is itself an fd).
- **`epoll_ctl(epfd, EPOLL_CTL_ADD/MOD/DEL, fd, &event)`** → register/modify/remove an fd *once*; the kernel remembers it.
- **`epoll_wait(epfd, events[], maxevents, timeout)`** → block until some registered fds are ready, and return *only the ready ones*.

```
   epoll_ctl(ep, ADD, client_fd, ...)   ← register each fd ONCE (kernel keeps a table)
   ...
   n = epoll_wait(ep, evs, MAX, -1);     ← blocks; returns ONLY the ready fds
   for (i = 0; i < n; i++)               ← loop over n READY fds, not all 10000
       handle(evs[i].data.fd);

   Cost per wait:  O(number ready)  — NOT O(number watched).
```

**Why it exists:** To kill `select`/`poll`'s O(n) tax. Because fds are registered *once* (not re-passed every call) and the kernel maintains a ready-list internally, `epoll_wait` returns in time proportional to the number of *ready* fds, independent of how many total are being watched. Watching 100,000 mostly-idle connections and getting back the 12 that are ready costs ~12 units of work, not 100,000. This O(ready) scaling is *the* enabling technology for the modern high-concurrency server; it's Linux-specific (`kqueue` is the BSD/macOS equivalent, IOCP the Windows one).

**Edge- vs level-triggered** — the one subtlety: by default epoll is **level-triggered** (like `poll`) — it keeps reporting an fd as ready as long as data remains, so you can read *some* now and be told again later. **Edge-triggered** (`EPOLLET`) reports readiness only on the *transition* (new data arrived), once — so you *must* drain the fd completely (read until `EAGAIN`) or you'll miss data. Edge-triggered is faster (fewer wakeups) but demands non-blocking fds and careful draining; level-triggered is easier and the right default while learning.

**Java analogy:** On Linux, `Selector` is `epoll` — `register()` is `epoll_ctl(ADD)`, `select()` is `epoll_wait`, `selectedKeys()` are the ready events. Netty exposes this directly with its `EpollEventLoopGroup` (vs the portable `NioEventLoopGroup`) precisely to use edge-triggered epoll for maximum throughput. Every time you've deployed a Netty/Vert.x/async service, `epoll_wait` was its beating heart.

### 5. The event loop, and the async frontier (`io_uring`)

**What it is:** Multiplexing gives rise to the **event loop** architecture: a single thread runs `while (1) { events = epoll_wait(...); for each ready fd: do a small non-blocking chunk of work; }`. Each connection is a little state machine advanced a step whenever its fd is ready. There's no blocking, no thread-per-connection — one loop drives everything. This is the architecture of **Nginx, Redis, Node.js (libuv), HAProxy**.

But `epoll` is **readiness-based**: it tells you an fd *can* be read, then *you* call `read`. The frontier is **completion-based async I/O**, epitomized by Linux's **`io_uring`**: you submit operations (read/write/accept/…) into a shared **submission ring**, the kernel executes them asynchronously, and posts results into a **completion ring** — you learn when the operation is *done*, having copied fewer times and made far fewer syscalls (you can batch dozens of operations per syscall, or none).

```
   READINESS (epoll):   "fd is readable"  → you call read()      → then handle
   COMPLETION (io_uring): submit read(fd,buf) → kernel does it → "read done, here it is"

   event loop:  while (1) { wait for events; advance each ready connection; }
                └─ one thread, thousands of connections, no blocking ─┘
```

**Why it exists:** The event loop exists because it's the *only* model that scales a single thread to massive concurrency with low, predictable latency — no per-connection memory, no context-switch storm. `io_uring` exists because even `epoll`'s "notify then syscall" has overhead (a syscall per operation), and at millions of ops/sec that overhead dominates; batching submissions/completions through shared rings cuts syscalls dramatically and enables true async for *disk* I/O too (which `epoll` never handled well). It's the current endpoint of the forty-year march toward doing more I/O with less CPU.

**Java analogy:** The event loop is Netty's `EventLoop`, Vert.x's reactor, and Node's libuv (not Java, but the same design). The "small non-blocking chunk per ready fd, never block the loop" rule is *the* cardinal law of async programming you may know as "don't block the event loop" (or Node's "don't block the main thread"). `io_uring` is being adopted under Java async libraries now; conceptually it's the completion-based cousin of `CompletableFuture`/`AsynchronousChannel` — you're told when the work *finished*, not when you *may* start it.

---

## Code

> **Try it:** compile `epoll_server` (or `select_server`), run it, then connect several `./echo_client 127.0.0.1 8080` (from Module 9) at once. A *single* server process serves them all concurrently — no forking.

### Program 1 — `nonblock.c`: see `EAGAIN`, the heart of non-blocking I/O

```c
/* nonblock.c
 *
 * Demonstrates the difference a non-blocking fd makes. We make stdin
 * non-blocking and read in a loop: instead of the thread sleeping until you
 * type, read() returns -1 with errno==EAGAIN when there's nothing yet, so the
 * loop keeps spinning (and could do other work). Type something to see it read.
 *
 * Compile:  gcc -Wall -Wextra -o nonblock nonblock.c
 * Run:      ./nonblock          (prints "waiting..." until you type a line)
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>

int main(void)
{
    /* Make stdin (fd 0) non-blocking. */
    int flags = fcntl(STDIN_FILENO, F_GETFL);
    fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);

    char buf[256];
    int spins = 0;
    for (;;) {
        ssize_t n = read(STDIN_FILENO, buf, sizeof buf - 1);
        if (n > 0) {
            buf[n] = '\0';
            printf("read %zd bytes: %s", n, buf);
            if (buf[0] == 'q') break;          /* type 'q' to quit */
        } else if (n == 0) {
            printf("EOF\n"); break;
        } else {
            /* n < 0: check WHY. EAGAIN just means "nothing ready right now." */
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (++spins % 5000000 == 0)     /* throttle the demo print */
                    printf("waiting... (read returned EAGAIN, thread NOT blocked)\n");
                continue;                        /* a real program does other work here */
            }
            perror("read"); break;
        }
    }
    return 0;
}
```

**Expected output:**
```
$ ./nonblock
waiting... (read returned EAGAIN, thread NOT blocked)
waiting... (read returned EAGAIN, thread NOT blocked)
hello
read 6 bytes: hello
q
read 2 bytes: q
$
```

**Walkthrough of the non-obvious parts:**
- `fcntl(fd, F_GETFL)` then `F_SETFL` with `| O_NONBLOCK` is the standard "add a flag" dance — get the current flags, OR in the new one, set them back. Clobbering the flags (setting `O_NONBLOCK` alone without OR-ing the existing flags) is a common bug.
- **`EAGAIN` is not an error** — it's the defining *normal* return of non-blocking I/O, meaning "would have blocked; nothing available." You treat it as "try again later," not as a failure. Every non-blocking program is built around handling `EAGAIN` gracefully. (`EWOULDBLOCK` is the same value on Linux; check both for portability.)
- The busy-spin here (looping on `EAGAIN`) is deliberately showing the *problem*: a non-blocking fd alone tempts you into a CPU-burning poll. The `throttle` counter hides how furiously it spins. The **fix** is multiplexing (Programs 2/3) — sleep in `select`/`epoll` until ready, instead of spinning.
- This is why non-blocking fds and multiplexing are a *pair*: non-blocking makes the I/O calls not stall, and multiplexing provides the efficient "sleep until ready" so you don't busy-wait. One without the other is incomplete.

### Program 2 — `select_server.c`: many clients, one thread, via `select`

```c
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
```

**Expected output (server):**
```
$ ./select_server 8080
select echo server on port 8080
client 4 connected
client 5 connected
client 4 disconnected
```

**Walkthrough of the non-obvious parts:**
- **One process, no fork, many clients.** The listening socket and every client fd live in the `master` set; a single `select` watches them all and blocks until *any* is ready. When two clients connect, they're both in the set; whichever sends data first shows up ready. This is the C10K answer's core idea, minus epoll's scalability.
- **`select` destroys the set it's given** — it modifies the passed `fd_set` in place to contain only the ready fds. So we keep a `master` copy and pass a fresh `readset = master` each iteration. Passing `master` directly would corrupt your list of watched fds — a classic `select` bug.
- **The `maxfd + 1` and the full scan are `select`'s O(n)**: `select` needs the highest fd number, and after it returns we loop `fd` from 0 to `maxfd` calling `FD_ISSET` to find the ready ones. With 10,000 clients that's a 10,000-iteration scan every wakeup even if one client is ready — the inefficiency `epoll` removes.
- **The listener readable = a pending `accept`.** Multiplexing treats "a new connection is waiting" uniformly as "the listening fd is readable," so `accept` itself never blocks here — you only call it when `select` says there's a client. New clients get `FD_SET` into `master`; disconnected ones (`read` returns 0) get `FD_CLR`.
- `FD_SETSIZE` (usually 1024) caps how many fds `select` can watch — the hard limit that, along with the O(n) scan, pushes real servers to `epoll`.

### Project — `epoll_server.c`: the scalable event-loop echo server

```c
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
```

**Expected output (server):**
```
$ ./epoll_server 8080
epoll echo server on port 8080
client 5 connected
client 6 connected
client 7 connected
client 6 disconnected
```

**Walkthrough of the non-obvious parts:**
- **Register once, wait many times.** Each fd is added to the epoll instance a *single* time with `epoll_ctl(ADD)`; thereafter `epoll_wait` returns it whenever it's ready without you re-passing the whole set. Contrast `select_server`, which rebuilds and re-scans the entire fd set on every iteration — *that's* the O(n)→O(ready) improvement, visible in the code structure itself.
- **`epoll_wait` returns only the ready fds** into the `events[]` array, and we loop `i` from 0 to `n` — where `n` is the number *ready*, not the number *watched*. With 50,000 idle connections and 3 active, `n` is 3. This is why one process handles C10K+: the work per wakeup scales with activity, not connection count.
- **The epoll instance is itself an fd** (`ep`) — you could even register one epoll fd inside another. This "everything is an fd" consistency is what lets epoll integrate uniformly with sockets, pipes, timerfds, signalfds, and eventfds.
- **Level-triggered** (default, no `EPOLLET`): if a client sends more than our 1024-byte `read` grabs, epoll will simply report the fd readable *again* next `epoll_wait`, so we don't lose data even though we read only once per event. Edge-triggered (`EPOLLET`) would require us to loop `read` until `EAGAIN` right here — more efficient but easier to get wrong (see Gotchas). Level-triggered is the correct default while learning.
- **Deregister on disconnect** (`epoll_ctl(DEL)`) before `close` — good hygiene, though closing an fd auto-removes it from epoll. Forgetting to remove a *still-open* fd you no longer care about leaks epoll registrations.
- This ~70-line program, with a real protocol swapped in for "echo," is the skeleton of Nginx and Redis. Add a request parser and a response writer per connection state machine, and you have a production web server's I/O core.

---

## Under the Hood

Run `strace ./epoll_server 8080`, connect two clients, send one line from each, and watch the event loop:

```
socket(AF_INET, SOCK_STREAM, ...)      = 3                     ← listening fd
bind(3, ...); listen(3, 128)           = 0
epoll_create1(0)                       = 4                     ← [1] the epoll fd
epoll_ctl(4, EPOLL_CTL_ADD, 3, {EPOLLIN, {u32=3}}) = 0         ← [2] register listener ONCE
epoll_wait(4, [{EPOLLIN, {u32=3}}], 64, -1) = 1               ← [3] wakes: listener ready
accept(3, NULL, NULL)                  = 5                     ←     new client fd 5
epoll_ctl(4, EPOLL_CTL_ADD, 5, {EPOLLIN, {u32=5}}) = 0         ←     register client 5
epoll_wait(4, [{EPOLLIN, {u32=5}}], 64, -1) = 1               ← [4] wakes: ONLY fd 5 ready
read(5, "hi\n", 1024)                  = 3
write(5, "hi\n", 3)                    = 3                     ←     echo
epoll_wait(4, [{EPOLLIN,{u32=6}},{EPOLLIN,{u32=5}}], 64, -1) = 2 ← [5] TWO ready at once
...
epoll_wait(4, ...                                              ← back to sleep, no CPU used
```

Annotated:
1. **`epoll_create1(0) = 4`** — the multiplexer is created and *is a file descriptor* (fd 4). Everything, including the thing that watches fds, is an fd.
2. **`epoll_ctl(4, ADD, 3, ...)`** — the listening socket (fd 3) is registered with epoll **once**. This is the key difference from `select`/`poll`: registration is separate from waiting, so the kernel keeps the interest list and you never re-submit it.
3–4. **`epoll_wait(4, [...], 64, -1)`** — the single blocking call the whole server sleeps in. It returns an array of **only the ready** events (`{u32=3}` = listener ready → `accept`; next time `{u32=5}` = only client 5 ready → read/echo). The thread uses **zero CPU** while blocked here; the kernel wakes it exactly when there's work.
5. **`... = 2`** — two clients ready in one wakeup: `epoll_wait` returns both events and the loop handles each. The return value is the count of *ready* fds; scale that mental picture to 100,000 watched connections returning the 40 that are active, and you see why this is O(ready), not O(watched).

The headline: **`epoll_create1` makes an fd, `epoll_ctl` registers each socket once, and `epoll_wait` is the single call the server sleeps in — waking with only the ready fds, so one thread and zero idle CPU serve unbounded connections.** Compare `strace ./select_server`: you'll see the *entire* fd set copied into every `select` call and back out — the O(n) traffic epoll eliminates. That difference, at scale, is the whole ballgame.

---

## Try This

Ordered easy → hard.

1. **(Easy) Feel blocking vs non-blocking.** Run `./nonblock` and watch it spin printing "waiting..." (non-blocking: the thread never sleeps). Then remove the `O_NONBLOCK` line, rebuild, and run — now it silently *blocks* until you type. Contrast the CPU usage of the two (`top` in another terminal). *Hint: non-blocking without multiplexing burns a core; that's the problem select/epoll solves.*

2. **(Easy) One server, many clients, no fork.** Start `./epoll_server 8080` and connect three `./echo_client` (Module 9) at once. Type in all three — all echo, served by a *single* process (check `ps`: no children). Compare to Module 9's `cserver` which spawned a process per client. *Hint: same external behavior, radically different resource cost — that's the point of this module.*

3. **(Medium) Count the fds in `select`'s syscalls.** `strace -e trace=select ./select_server 8080` with a few clients connected, and look at the size of the fd sets passed on each call. Then `strace -e trace=epoll_wait ./epoll_server 8080` and note epoll passes nothing but gets back only ready fds. Explain the O(n) vs O(ready) difference from what you see. *Hint: select re-sends the whole set every call; epoll registered once.*

4. **(Medium) Add a write-buffering path.** Real servers can't always `write` everything at once (short writes, Module 9). Extend `epoll_server` so that when `write` returns `EAGAIN` (make the client fd non-blocking first), you register `EPOLLOUT` for that fd and finish the write when it's writable. Explain why a server must handle "socket not ready for writing." *Hint: a slow client whose receive buffer is full makes your write block/EAGAIN; the event loop must not stall on it — watch EPOLLOUT instead.*

5. **(Hard) Go edge-triggered.** Switch the client registrations to `EPOLLIN | EPOLLET` and make every client fd non-blocking. Now you *must* loop `read` until `EAGAIN` on each ready event (edge-triggered notifies only on new data, once). Verify with a client that sends more than 1024 bytes in a burst that you don't lose the tail. Explain the trap you just avoided. *Hint: with EPOLLET, if you read once and stop, the leftover bytes sit unread and you're never notified again — the classic edge-triggered bug.*

---

## Gotchas

- **Non-blocking without multiplexing = busy-wait.** Setting `O_NONBLOCK` and looping on `EAGAIN` burns 100% CPU spinning. Non-blocking fds are only half the solution; you *must* pair them with `select`/`poll`/`epoll` to sleep efficiently until an fd is ready. One without the other is either a stall (blocking) or a spin (non-blocking alone).

- **Passing your master `fd_set` directly to `select`.** `select` *modifies* the sets in place to hold only the ready fds, destroying your list of watched fds. Always pass a *copy* (`readset = master;`) each iteration and keep `master` intact. Forgetting this makes clients mysteriously "disappear" from the watch set.

- **Treating `EAGAIN` as an error.** `EAGAIN`/`EWOULDBLOCK` on a non-blocking fd is the *normal* "nothing ready right now" signal, not a failure — handle it by waiting/retrying, never by closing the connection or aborting. Also check *both* names; they're equal on Linux but not guaranteed everywhere.

- **Edge-triggered without draining to `EAGAIN`.** With `EPOLLET`, epoll notifies only on the *transition* to ready, once. If you `read` a single buffer and stop while more data remains, you won't be notified again and the leftover bytes are stuck forever. Edge-triggered *requires* looping `read`/`accept` until `EAGAIN`, and requires non-blocking fds. Level-triggered (the default) forgives a single read — use it until you specifically need ET.

- **Forgetting to remove closed fds from the watch set.** In `select`, a `close`d fd left in the set causes `EBADF`; in epoll, closing an fd auto-removes it, but a *dup*'d fd or one you stop caring about must be `epoll_ctl(DEL)`'d or you accumulate stale registrations. Match every add with an eventual remove/close.

- **Blocking the event loop.** The cardinal sin of the single-threaded event-loop model: doing anything slow (a blocking `read` on a *file*, a big `sleep`, heavy computation, a synchronous DB call) inside the loop freezes *every* connection, because there's one thread. All work in the loop must be non-blocking and quick; offload slow/CPU-heavy work to a thread pool. (This is Node's "don't block the event loop" and Netty's "don't block the EventLoop thread.")

- **`select`'s 1024 fd limit.** `FD_SETSIZE` caps `select` at ~1024 fds; fd numbers ≥ 1024 corrupt memory when `FD_SET`. If you need more, use `poll` (no hard limit) or `epoll` (scalable). Don't try to raise `FD_SETSIZE`; switch mechanisms.

- **Assuming one `epoll_wait`/`read` gets a whole message.** Same TCP-is-a-stream rule as Module 9: readiness means *some* bytes, not a complete message. Each connection needs its own buffer and parse state so a partial message is held until the rest arrives across multiple events. This per-connection state machine is the real work of an event-loop server.

---

## Checkpoint

1. What is the C10K problem, and why does the fork/thread-per-connection model (Module 9) fail to solve it? What key observation about real connections makes a different model possible?
2. Explain blocking vs non-blocking file descriptors and the meaning of `EAGAIN`. Why is a non-blocking fd *by itself* not enough — what problem does naive use of it create?
3. What does I/O multiplexing do, and how does it let a single thread serve thousands of connections efficiently (getting both the benefit of sleeping *and* of one-thread-many-fds)?
4. Why is `epoll` more scalable than `select`/`poll`? Describe the three `epoll` calls and explain the O(ready) vs O(n) difference concretely. What's the difference between level- and edge-triggered?
5. What is an event loop, which famous servers use it, and what is the one cardinal rule you must never break inside it? How does `io_uring`'s model differ from `epoll`'s?

---

<details>
<summary><b>Checkpoint Answers</b></summary>

1. **C10K** is handling ~10,000 simultaneous connections on one server. Fork/thread-per-connection fails because it spends a whole process or thread — megabytes of memory plus scheduler/context-switch overhead — on *each* connection, so 10,000 connections mean 10,000 processes/threads, exhausting RAM and drowning the scheduler, even though most connections are idle. The key observation is exactly that: **most connections are idle most of the time** (only a small fraction are actively transferring data at any instant), so dedicating a thread to each *waiting* connection is enormous waste — one thread could instead watch them all and work only on the few that are ready.

2. A **blocking** fd makes I/O calls (`read`/`accept`/`write`) sleep the calling thread until the operation can proceed; a **non-blocking** fd (`O_NONBLOCK`) makes them return immediately — with available data/space, or with `errno == EAGAIN` (`EWOULDBLOCK`) meaning "would have blocked; nothing ready now." Non-blocking alone isn't enough because, to make progress across many fds, you'd loop over them retrying — a **busy-wait** that pins the CPU at 100% doing nothing while everything is idle. You need a way to *sleep* until at least one fd is ready, which is what multiplexing (`select`/`poll`/`epoll`) adds.

3. **I/O multiplexing** is a syscall that watches a *set* of fds and blocks until one or more become ready (readable/writable/error), then reports which. It gives a single thread both properties at once: it **sleeps** (zero CPU) while all watched fds are idle — the kernel does the waiting for the whole set — and it wakes to handle **only the ready fds**, so one thread drives many connections. This decouples the number of connections from the number of threads: an *event loop* (`while(1){ wait; handle ready; }`) on one thread serves thousands of sockets, using memory/CPU proportional to *activity*, not to connection count.

4. `select`/`poll` are **O(n)**: on every call you pass the *entire* fd set to the kernel (a copy), the kernel scans all of them, and you scan all of them on return — so watching 10,000 fds costs ~10,000 units of work per wait even if one is ready (and `select` caps at ~1024 fds). `epoll` splits registration from waiting: **`epoll_create1`** makes an epoll fd; **`epoll_ctl(ADD/MOD/DEL)`** registers each fd *once* (the kernel keeps the interest list); **`epoll_wait`** blocks and returns *only the ready* fds. So work per wait is **O(number ready)**, independent of the total watched — the difference between melting at 1,000 connections and humming at 100,000. **Level-triggered** (default) keeps reporting an fd while data remains (a single read per event is safe); **edge-triggered** (`EPOLLET`) reports only on the transition to ready, once, so you must drain the fd (loop until `EAGAIN`) or lose data — faster but trickier, and it requires non-blocking fds.

5. An **event loop** is a single thread running `while (1) { events = wait_for_ready_fds(); for each ready fd: do a small non-blocking chunk of work; }` — each connection is a state machine advanced whenever its fd is ready. **Nginx, Redis, Node.js (libuv), and HAProxy** use it. The cardinal rule: **never block the event loop** — no slow/blocking calls (blocking file reads, `sleep`, heavy computation, synchronous DB calls) inside it, because the single thread would freeze *every* connection; offload slow work to a thread pool. **`io_uring`** differs from `epoll` by being **completion-based** rather than **readiness-based**: instead of "fd is ready, now *you* call `read`," you submit operations into a shared submission ring, the kernel performs them asynchronously, and posts results into a completion ring — you're told when the work is *done*, with far fewer syscalls (batched) and support for async *disk* I/O that epoll never handled well.

</details>

---

*Next up: **Module 11 — ioctl and the Many Kinds of I/O.** Not everything fits `read`/`write`: how do you set a terminal's size, a socket's options, or a device's mode? `ioctl` is the catch-all "device control" syscall, the escape hatch for operations that don't map to a byte stream. We'll revisit the terminal (Module 4), poke network interfaces, and see why `ioctl` is both indispensable and famously ugly. A shorter module — the fd spine bends to control, not just data. Continuing straight on.*
