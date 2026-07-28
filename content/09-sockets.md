# Module 9 — Network I/O and Socket Programming

> **Estimated time:** 3–4 hours · **Core path:** Concepts 1–4 (sockets as fds, the server flow, the client flow, TCP vs UDP) and the `echo_server` + `echo_client` programs are core. Concurrency (Concept 5) and the fork-per-client server are core-but-meaty; the `select`/event-loop preview is deferred to Module 10.
>
> **Prerequisites:** Modules 0–8. A socket *is a file descriptor* (Module 3) — you'll `read`/`write` it exactly as you did files and pipes. The Unix-domain socket teaser (Module 8) generalizes here to the network. Concurrency comes from `fork` + `SIGCHLD` reaping (Module 5), and byte order echoes the integer-representation care from C (Module 2).

---

## The Big Picture

Everything in this course has been building toward a payoff: the file descriptor, which started as a handle on a file (Module 3), then a terminal (Module 4), then a pipe (Module 8), now becomes a handle on a **conversation with a computer across the world**. A **socket** is a file descriptor bound to a network endpoint, and — this is the whole beauty of Unix — once you have a connected socket, you `read` and `write` it with the *exact same syscalls* you used for files. The network is not a special world with its own I/O; it's the file abstraction stretched over TCP/IP. If you can read a file, you already know how to receive from a socket; the only new material is how to *establish* the connection.

That establishment is the **Berkeley sockets API**, a five-or-six-call sequence that has been essentially unchanged since 1983 and underlies every networked program on Earth — your browser, `curl`, Nginx, Postgres, the JVM's `Socket` class, all of it. The **server** side is a fixed ritual: `socket` (create the endpoint), `bind` (claim a port), `listen` (mark it as accepting connections), then `accept` in a loop (each call blocks until a client connects and returns a *new* fd for that one client). The **client** side is shorter: `socket`, then `connect` (reach out to a server's address and port). Once connected, both sides just `read`/`write` the fd and `close` it when done. We'll build a TCP **echo server** and **client** — the "hello world" of network programming — and you'll see there are maybe 30 lines of setup wrapping the same `read`/`write` loop you already know.

Two pieces of new vocabulary make the API make sense. First, **addresses and byte order**: a network endpoint is an IP address plus a port, packed into a `struct sockaddr_in`, and multi-byte numbers on the wire use **network byte order** (big-endian) regardless of your CPU — so you wrap ports in `htons()` ("host to network short") and parse IPs with `inet_pton()`. Forget the byte-order conversion and your server listens on a bizarre wrong port; it's the classic first bug. Second, **TCP vs UDP**: TCP (`SOCK_STREAM`) is a reliable, ordered, connection-oriented *byte stream* — what you want for almost everything (HTTP, SSH, databases); UDP (`SOCK_DGRAM`) is unreliable, unordered, connectionless *datagrams* — used where speed beats reliability (DNS, video, games). The API differs slightly (UDP skips `listen`/`accept`/`connect` and uses `sendto`/`recvfrom`), and understanding *why* they differ teaches you what TCP is quietly doing for you.

Finally, the problem that shapes all real servers: **concurrency**. A naive server `accept`s one client, talks to it, and only then `accept`s the next — so a second client waits while the first is served. Real servers handle many clients at once, and the first, oldest technique is **fork-per-connection**: `accept` a client, `fork` a child to handle it, and loop back to `accept` immediately (reaping dead children via `SIGCHLD`, straight out of Module 5). It's simple and robust, and it's how classic servers (inetd, early Apache) worked. It also doesn't scale to tens of thousands of connections — one process each is too heavy — which is *exactly* the cliffhanger that motivates Module 10 (I/O multiplexing: `select`/`poll`/`epoll`, one process juggling thousands of sockets). This module gets you a working, concurrent server; the next makes it scale.

---

## Concepts

### 1. A socket is a file descriptor for a network endpoint

**What it is:** `socket(domain, type, protocol)` creates a communication endpoint and returns a **file descriptor** for it. `domain` picks the address family (`AF_INET` = IPv4, `AF_INET6` = IPv6, `AF_UNIX` = local, Module 8); `type` picks the semantics (`SOCK_STREAM` = TCP, `SOCK_DGRAM` = UDP). Once connected, that fd is read and written with the *same* `read`/`write`/`close` you already know (plus socket-specific `send`/`recv` variants that add flags).

```
   int fd = socket(AF_INET, SOCK_STREAM, 0);   // a TCP/IPv4 endpoint

        ordinary file:   fd ── read/write ──> disk
        pipe:            fd ── read/write ──> another process
        SOCKET:          fd ── read/write ──> a program across the network
        └───────────────── same syscalls, different thing behind the fd ─────┘
```

**Why it exists:** The genius of Unix is representing wildly different things — files, devices, pipes, network connections — behind one uniform interface (the fd) so the same tools and syscalls compose across all of them. A socket brings the network into that scheme: after the connection is set up, network I/O *is* file I/O. This is why you can `cat` a file into a socket, why `select`/`epoll` (Module 10) work uniformly across files and sockets, and why the mental model "everything is a file descriptor" pays off so enormously.

**Java analogy:** `java.net.Socket` and `ServerSocket` wrap exactly this. `socket.getInputStream()`/`getOutputStream()` give you the read/write side of the fd as Java streams — the JVM holds an OS socket fd underneath and calls `read`/`write` on it. `SocketChannel` (NIO) is the same fd exposed for non-blocking/multiplexed use (Module 10). Everything you know about Java sockets is a thin object wrapper over the C API you're about to use raw.

### 2. The server flow: `socket` → `bind` → `listen` → `accept`

**What it is:** A TCP server follows a fixed four-call setup, then loops on `accept`:

```
   socket(AF_INET, SOCK_STREAM, 0)   → a listening-socket fd
   bind(fd, &addr, ...)              → claim IP:port (e.g. 0.0.0.0:8080)
   listen(fd, backlog)               → mark it passive; queue pending connections
   while (1) {
       int conn = accept(fd, ...);   → BLOCKS until a client connects;
                                       returns a NEW fd for THAT client
       ... read/write(conn) ...       (the listening fd keeps accepting)
       close(conn);
   }
```

The crucial subtlety: `accept` returns a **new** fd per client. The original `fd` stays open and keeps *listening*; each `accept` hands you a separate `conn` fd that is the private channel to one client. You talk to the client over `conn`, close `conn`, and `accept` again for the next.

**Why it exists:** The steps separate concerns that a single "start a server" call would conflate: `bind` claims the address (and can fail with "address already in use"), `listen` sets the connection backlog (how many pending clients the kernel queues before you `accept` them), and `accept` is the blocking rendezvous where a waiting client is handed to you. Splitting listening (one long-lived fd) from connected (one fd per client) is what lets one server serve many clients — and lets you decide *how* to handle each (serially, forked, threaded, or multiplexed).

**Java analogy:** `new ServerSocket(8080)` does `socket`+`bind`+`listen` in one constructor; `serverSocket.accept()` is `accept`, returning a `Socket` (your `conn` fd) per client. Java fused the setup into the constructor but kept `accept()` as the per-client blocking call — the shape is identical, and the "one listening socket, many connected sockets" model is exactly the same.

### 3. The client flow, addresses, and byte order

**What it is:** A TCP client is shorter: `socket`, then `connect` to a server's address. The address is a `struct sockaddr_in` — family, port, and IP — and multi-byte fields must be in **network byte order** (big-endian), so you convert:

```c
   struct sockaddr_in addr;
   addr.sin_family = AF_INET;
   addr.sin_port   = htons(8080);                 // host->network SHORT (port)
   inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr); // text IP -> packed network bytes

   int fd = socket(AF_INET, SOCK_STREAM, 0);
   connect(fd, (struct sockaddr *)&addr, sizeof addr);   // reach out; blocks until done
   // now read/write(fd) to talk to the server
```

`htons`/`htonl` ("host to network short/long") swap byte order on little-endian machines (a no-op on big-endian); `inet_pton` ("presentation to network") parses a dotted-quad or IPv6 string into the packed binary form.

**Why it exists:** The network is heterogeneous — machines with different native byte orders (Module 2's endianness) exchange packets — so the wire format is *fixed* at big-endian ("network byte order") and every host converts to/from its own order at the edges. The `htons`/`htonl`/`ntohs`/`ntohl` functions make this portable: write `htons(port)` and your code is correct on any CPU. Skipping them is the archetypal networking bug: `bind`ing port `8080` without `htons` actually binds `0x9020` = 36895, and nothing connects.

**Java analogy:** Java hides byte order entirely — `new Socket("127.0.0.1", 8080)` takes a plain `int` port and a hostname string, and the JVM does the `htons`/`inet_pton`/DNS work for you. The endianness resurfaces only when you handle raw binary protocols yourself: `ByteBuffer.order(ByteOrder.BIG_ENDIAN)` (the default, and it's *network* order for a reason) and `DataInputStream.readInt` (always big-endian) are Java quietly enforcing the same wire convention.

### 4. TCP vs UDP: stream vs datagram

**What it is:** The two dominant transport protocols, chosen via the socket `type`:

```
   TCP (SOCK_STREAM)                  UDP (SOCK_DGRAM)
   ─────────────────                  ────────────────
   connection-oriented (connect)      connectionless (just sendto)
   reliable: retransmits lost data    unreliable: lost packets stay lost
   ordered: bytes arrive in order     unordered: packets may reorder
   byte STREAM (no message bounds)    DATAGRAMs (each send = one recv)
   flow + congestion control          none (you send as fast as you like)
   API: listen/accept/connect,        API: bind + sendto/recvfrom
        read/write                         (no listen/accept/connect needed)
   HTTP, SSH, DB, TLS                  DNS, DHCP, video, games, QUIC-base
```

**Why it exists:** They embody a fundamental tradeoff. **TCP** does an enormous amount of work for you — acknowledgements, retransmission, reordering, flow control, congestion control — to present a clean, reliable, ordered byte pipe; the cost is latency (handshake, ack round-trips) and head-of-line blocking. **UDP** does almost nothing — it just fires datagrams and hopes — which is exactly right when you'd rather drop a late packet than wait for it (a video frame, a game position update) or when you're building your own reliability on top (QUIC, which powers HTTP/3). Understanding UDP's bareness is the best way to appreciate everything TCP silently guarantees.

**Java analogy:** `Socket`/`ServerSocket` are TCP; `DatagramSocket` and `DatagramPacket` are UDP. The `DatagramPacket` *is* the datagram — one `send` per packet, one `receive` per packet, message boundaries preserved — versus `Socket`'s `InputStream` which is a boundary-less byte stream you must frame yourself. The reliability difference is the same: with `DatagramSocket` a lost packet is simply never received, and it's on you to detect and handle it, exactly as in C.

### 5. Handling many clients: the concurrency problem

**What it is:** A server that reads/writes one client to completion before `accept`ing the next is **iterative** — it serves clients one at a time, and a slow client blocks everyone behind it. To serve clients concurrently, the classic technique is **fork-per-connection**: after `accept`, `fork` a child to handle that client while the parent immediately loops back to `accept`. Dead children are reaped with a `SIGCHLD` handler (Module 5) so they don't become zombies.

```
   while (1) {
       int conn = accept(listen_fd, ...);
       pid_t pid = fork();
       if (pid == 0) {              // CHILD: handle this one client
           close(listen_fd);        // child doesn't need the listener
           serve(conn);
           close(conn);
           _exit(0);
       }
       close(conn);                 // PARENT: doesn't need this client's fd
   }                                // loop back to accept immediately
   // + a SIGCHLD handler that reaps finished children (no zombies)
```

**Why it exists:** Concurrency is the whole point of a server — many clients, served without blocking each other. Fork-per-connection is the simplest correct answer: each client gets its own process (its own memory, its own crash isolation), and the OS scheduler interleaves them. It's robust and easy to reason about. Its limit is *scale*: a process per connection costs megabytes of memory and a context-switch to schedule, so ~10K simultaneous connections ("the C10K problem") overwhelm it — which is precisely why Module 10 introduces event-driven multiplexing, one process handling thousands of sockets.

**Java analogy:** The direct analogue is **thread-per-connection** (`new Thread(() -> handle(clientSocket)).start()` per `accept`) — the classic Java servlet/`Tomcat`-BIO model, using threads instead of processes (cheaper, shared heap, but shared-state hazards from Module 6). Both hit the same scaling wall, which is why Java grew NIO `Selector` (Module 10's `epoll`) and why Netty, and virtual threads (Project Loom), exist. Fork-per-connection is the process-based cousin of the thread pool you've deployed a hundred times.

---

## Code

> **Try it:** compile both programs below, run `./echo_server 8080` in one terminal, then `./echo_client 127.0.0.1 8080` in another and type lines — the server echoes them back. `Ctrl-D` (EOF, Module 4) ends the client.

### Program 1 — `echo_server.c`: an iterative TCP echo server

```c
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
```

**Expected output (server side):**
```
$ ./echo_server 8080
echo server listening on port 8080
client connected: 127.0.0.1:53124
client disconnected
```

**Walkthrough of the non-obvious parts:**
- **`SO_REUSEADDR`** — after a server exits, its port sits in `TIME_WAIT` for a couple minutes (a TCP correctness feature), during which `bind` fails with "Address already in use." Setting `SO_REUSEADDR` lets you re-bind immediately, which you *always* want during development. Forgetting it is the "why can't I restart my server?" head-scratcher.
- **`htons(port)` and `htonl(INADDR_ANY)`** — the byte-order conversions. `INADDR_ANY` (0.0.0.0) means "bind on all network interfaces." Both go through `hton*` to reach network byte order; the port bug (binding the wrong port) comes from skipping `htons`.
- **`accept` returns a *new* fd (`cfd`)** distinct from the listening fd (`lfd`). `lfd` keeps listening; `cfd` is this one client's channel. The `struct sockaddr_in cli` it fills in tells you *who* connected (their IP/port), which `inet_ntop` turns back into a readable string.
- **The echo loop is `read`/`write` — literally the file-I/O loop from Module 3.** `read` returns 0 when the client closes its end (EOF over the network), ending the loop. There is nothing network-specific about moving the bytes; the socket setup was the only new part.
- This server is **iterative**: it's stuck in one client's echo loop until that client disconnects, so a second client waits in the `listen` backlog. `cserver.c` (the project) fixes that with `fork`.

### Program 2 — `echo_client.c`: the TCP client side

```c
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
```

**Expected output (client side):**
```
$ ./echo_client 127.0.0.1 8080
connected to 127.0.0.1:8080 -- type lines, Ctrl-D to quit
hello
echo: hello
sockets are just fds
echo: sockets are just fds
^D
$
```

**Walkthrough of the non-obvious parts:**
- **`connect` is the client's whole setup** — no `bind`/`listen`/`accept`. It initiates the TCP three-way handshake to the server's address and blocks until it completes (or fails with `ECONNREFUSED` if nothing's listening — the "server down / wrong port" case).
- **`inet_pton` returns 1 on success**, 0 for a malformed address, -1 for an error — a three-way return worth checking (unlike the older `inet_addr`, which can't distinguish the valid address `255.255.255.255` from failure). It packs the text IP into `addr.sin_addr` in network byte order.
- **The write-then-read is request/response** over the same fd. Note this simple client assumes one `read` returns the whole echo — fine for a local echo of a short line, but a real protocol must loop `read` until it has a full message (TCP is a *stream*, no message boundaries — see Gotchas).
- `fgets` returning NULL on **Ctrl-D (EOF)** ends the loop and `close`s the socket, which the server sees as its `read` returning 0. The clean shutdown propagates across the network as EOF, just like a pipe (Module 8).

### Project — `cserver.c`: a concurrent (fork-per-client) echo server

```c
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
```

**Expected output (server, with two clients connecting):**
```
$ ./cserver 8080
concurrent echo server on port 8080
(each client is served by its own child process, concurrently)
```

**Walkthrough of the non-obvious parts:**
- **`fork` after `accept`** is the whole idea: the child runs `serve(cfd)` for this one client while the parent immediately loops back to `accept` the next — so N clients get N children running concurrently, no client blocking another. This is Module 5's `fork` applied to servers.
- **Both processes close the fd they don't need.** The child `close(lfd)` (it never accepts), the parent `close(cfd)` (it never serves this client). If the parent forgot `close(cfd)`, the fd would leak in the parent for every client (eventually exhausting the fd table), *and* the connection wouldn't fully close when the child finishes (a lingering fd keeps it half-open) — the same "close the end you don't use" discipline as pipes (Module 8).
- **The `SIGCHLD` handler reaps children** with the looping `waitpid(-1, WNOHANG)` idiom from Module 5 — without it, every finished client would leave a zombie, and a busy server would exhaust the process table. `SA_RESTART` asks the kernel to resume a syscall interrupted by the signal.
- **`accept` can fail with `EINTR`** when `SIGCHLD` interrupts it (even with `SA_RESTART`, `accept` is one of the calls that may still return `EINTR` on some systems). We check for it and simply retry the `accept` — the Module 3 `EINTR` lesson, now in a server loop.
- This server is correct and robust but **doesn't scale to C10K**: 10,000 clients = 10,000 processes = gigabytes of RAM and heavy scheduling. That limitation is the entire motivation for Module 10's `epoll` event loop, where *one* process handles thousands of sockets.

---

## Under the Hood

Run `strace ./echo_server 8080` in one terminal, connect a client, and watch the Berkeley API become syscalls:

```
socket(AF_INET, SOCK_STREAM, IPPROTO_IP) = 3                     ← [1] listening fd
setsockopt(3, SOL_SOCKET, SO_REUSEADDR, [1], 4) = 0
bind(3, {sa_family=AF_INET, sin_port=htons(8080),
         sin_addr=inet_addr("0.0.0.0")}, 16) = 0                 ← [2] claim the port
listen(3, 16)                            = 0                     ← [3] passive/backlog
accept(3, {sa_family=AF_INET, sin_port=htons(53124),
           sin_addr=inet_addr("127.0.0.1")}, [16]) = 4           ← [4] client! new fd 4
read(4, "hello\n", 1024)                 = 6                     ← [5] recv on the conn fd
write(4, "hello\n", 6)                   = 6                     ←     echo back
read(4, "", 1024)                        = 0                     ← [6] client closed: EOF
close(4)                                 = 0
accept(3, ...                                                    ← back to waiting
```

Annotated:
1. **`socket(...) = 3`** — the listening socket gets fd 3 (0/1/2 are stdin/out/err, Module 4). A socket is a numbered fd from the same per-process table as files and pipes.
2. **`bind(3, {... sin_port=htons(8080) ...}, 16)`** — notice strace shows `htons(8080)` explicitly: the kernel receives the port in network byte order. Binding to `0.0.0.0` claims the port on every interface.
3. **`listen(3, 16)`** — flips the socket from active to **passive** (it will now accept incoming connections) and sets the backlog queue to 16 pending connections.
4. **`accept(3, {... client addr ...}) = 4`** — blocks until a client connects, then returns a **brand-new fd (4)** for that connection and fills in the client's address. **fd 3 keeps listening; fd 4 is the conversation.** This one line is the heart of the server model.
5–6. **`read(4, ...)` / `write(4, ...)`** — ordinary file syscalls on the connection fd. `read(4, "", 1024) = 0` is the client's disconnect arriving as **EOF** — identical to a closed pipe or an end-of-file. The network connection's teardown surfaces through the same `= 0` you've seen since Module 3.

The headline: **`socket`/`bind`/`listen`/`accept` set up the fd, and then it's just `read`/`write`/`close` — the network is the file interface stretched across the wire, and `accept` returning a fresh fd per client is what makes one server serve many.** Run `strace -f ./cserver 8080` and you'll additionally see `clone` (the fork) after each `accept` — Module 5 and Module 9 fused into a working concurrent server.

---

## Try This

Ordered easy → hard.

1. **(Easy) Talk to your server with `nc` and `curl`.** Start `./echo_server 8080`, then `nc 127.0.0.1 8080` (netcat) and type lines — your C server echoes them, proving it speaks standard TCP. Then point a browser or `curl http://127.0.0.1:8080/` at it and read the raw HTTP request your server echoes back. *Hint: `curl` sends a real `GET / HTTP/1.1` request; your echo server bounces it back verbatim — that's the raw protocol.*

2. **(Easy) Trigger and fix "Address already in use."** Remove the `SO_REUSEADDR` block, run the server, stop it with Ctrl-C, and immediately restart — watch `bind` fail. Restore `SO_REUSEADDR` and confirm instant restart works. *Hint: the port is in TIME_WAIT; `SO_REUSEADDR` is why every real server sets it.*

3. **(Medium) Prove the iterative server blocks.** With the *iterative* `echo_server`, connect two clients at once. Type in the second — nothing echoes until the first client disconnects. Then repeat with `cserver` (forked) and watch both work simultaneously. *Hint: the iterative server is stuck in client 1's read/write loop; the forked one gave client 2 its own process.*

4. **(Medium) Write a UDP echo pair.** Build `udp_server.c` (`socket(AF_INET, SOCK_DGRAM, 0)` + `bind`, then loop `recvfrom`/`sendto`) and `udp_client.c` (`sendto`/`recvfrom`, no `connect`). Note there's no `listen`/`accept` and each `recvfrom` gives you a whole datagram plus the sender's address. *Hint: `recvfrom` fills in `struct sockaddr_in` of whoever sent the packet — that's how a connectionless server knows where to reply.*

5. **(Hard) Turn the echo server into a tiny HTTP server.** In `cserver`, replace `serve` so that instead of echoing, it reads the request and writes a valid HTTP response: `"HTTP/1.1 200 OK\r\nContent-Length: 13\r\n\r\nHello, world\n"`. Open `http://127.0.0.1:8080/` in a browser and see your page. Explain why `Content-Length` (or `Connection: close`) matters. *Hint: without a length or a close, the browser waits for more bytes — TCP has no message boundary, so HTTP must declare where the body ends. This is the Module 16 capstone in embryo.*

---

## Gotchas

- **Forgetting `htons`/`htonl` on the port/address.** Binding or connecting without converting to network byte order silently uses the wrong port (bytes swapped). The server "starts fine" but nothing can reach it, or it's on a nonsense port. Always `htons(port)`; it's the #1 first-time socket bug.

- **TCP has no message boundaries.** `read`/`recv` returns *whatever bytes have arrived*, which may be part of a message, one message, or several — TCP is a **byte stream**, not a message queue. Assuming "one `read` = one message" breaks under real network timing. Frame your messages yourself (length prefix, or a delimiter like `\n`, or a header like HTTP's `Content-Length`) and loop `read` until you have a complete one.

- **Short `write`s.** `write`/`send` on a socket may accept *fewer* bytes than you asked (the send buffer filled). You must loop, advancing past the bytes already sent, until the whole buffer is written. Assuming one `write` sends everything works locally and fails under load or with large payloads.

- **Not setting `SO_REUSEADDR`.** After a server stops, its port lingers in `TIME_WAIT`, and `bind` fails with "Address already in use" for a couple minutes. Set `SO_REUSEADDR` before `bind` so restarts work immediately. Nearly every real server does this.

- **`SIGPIPE` on writing to a closed connection.** If you `write` to a socket whose peer has closed, the kernel raises `SIGPIPE`, which by default kills your process — a client disconnecting can crash your server. Ignore `SIGPIPE` (`signal(SIGPIPE, SIG_IGN)`) or use `send(..., MSG_NOSIGNAL)` and handle the `EPIPE` error instead. (Same hazard as pipes, Module 8.)

- **Leaking the connection fd in the parent (forked server).** In `cserver`, the parent must `close(cfd)` after forking. Forget it and every client leaks an fd in the parent (fd-table exhaustion after ~1000 clients), *and* the connection doesn't fully close when the child ends (the parent's copy keeps a reference). Mirror image: the child must `close(lfd)`.

- **Blocking `accept`/`read` stalls an iterative server.** A single slow or malicious client that connects but sends nothing holds an iterative server hostage forever (it's blocked in that client's `read`). Concurrency (fork/thread/`epoll`) or timeouts (`SO_RCVTIMEO`) are the defenses — this is a real denial-of-service vector, not just a performance note.

- **Not checking `connect`/`accept` return values.** `connect` fails with `ECONNREFUSED` (nobody listening), `ETIMEDOUT` (unreachable host), etc.; `accept` can fail with `EINTR` or resource limits. Network calls fail *routinely* (unlike local file calls), so checking and handling every return is mandatory, not optional hygiene.

---

## Checkpoint

1. In what sense is a socket "just a file descriptor," and what does that buy you? Once a TCP connection is established, what syscalls move the data?
2. List the server-side call sequence for a TCP server and say what each call does. Why does `accept` return a *new* file descriptor, and what happens to the original listening fd?
3. What is network byte order, why does it exist, and which functions convert to/from it? What goes wrong if you skip the conversion on a port number?
4. Contrast TCP and UDP across connection, reliability, ordering, and message boundaries. Give one protocol that uses each and explain why that choice fits.
5. What is the fork-per-connection server model, why is it needed (what does an iterative server fail to do), and what is its scaling limitation that motivates I/O multiplexing?

---

<details>
<summary><b>Checkpoint Answers</b></summary>

1. A socket is an entry in the same per-process file-descriptor table as files and pipes, so once it's connected you use the **same syscalls** — `read`, `write`, `close`, and `select`/`epoll` — to move data over the network as you do for a file. That buys uniformity: the network becomes just another fd, tools and abstractions compose across files/pipes/sockets, and you already know 90% of the API. After a TCP connection is established, `read`/`recv` and `write`/`send` on the connection fd move the bytes (and `read` returning 0 signals the peer closed, i.e. EOF).

2. `socket()` creates the endpoint fd; `bind()` claims a local IP:port; `listen()` marks the socket passive and sets the backlog queue for pending connections; `accept()` blocks until a client connects and returns a **new** fd for that specific connection. `accept` returns a new fd because the original (listening) fd's job is to keep *accepting* — it must stay available for the next client — while each connection needs its own private channel. So the listening fd remains open and keeps accepting; each `accept` yields a separate connected fd you talk to that one client over, then close.

3. **Network byte order** is big-endian, the fixed byte ordering used for multi-byte integers on the wire, regardless of the host CPU's native (host) byte order. It exists because networked machines may have different endianness (Module 2), so the protocol standardizes one order and every host converts at the edges. Convert with `htons`/`htonl` (host→network short/long) when sending/binding and `ntohs`/`ntohl` when receiving. Skip it on a port and the bytes are swapped: e.g. `bind`ing `8080` without `htons` actually binds `0x9020` = 36895, so the server listens on the wrong port and nothing connects.

4. **TCP**: connection-oriented (`connect`/`accept`), reliable (retransmits lost data), ordered (in-order delivery), and a boundary-less **byte stream**. **UDP**: connectionless (just `sendto`), unreliable (lost datagrams stay lost), unordered, and preserves **message boundaries** (one `sendto` = one `recvfrom`). HTTP/SSH/databases use **TCP** because they need every byte, in order (a corrupted or reordered web page or SQL result is unacceptable). DNS/video/games use **UDP** because a fast, best-effort datagram beats waiting for a retransmit — a late DNS retry is cheap, a late video frame is useless, so dropping it and moving on is the right tradeoff.

5. **Fork-per-connection**: after `accept`, the server `fork`s a child to handle that client while the parent loops back to `accept` immediately (reaping children via `SIGCHLD`). It's needed because an **iterative** server serves one client to completion before accepting the next, so a slow (or idle) client blocks everyone behind it — no concurrency. Forking gives each client its own process, served in parallel. Its **scaling limit** is cost per connection: a whole process (megabytes of memory, scheduling/context-switch overhead) per client means ~10,000 simultaneous connections (the C10K problem) exhaust the machine — which is why Module 10 introduces `epoll`-style multiplexing, letting one process handle thousands of sockets without a process (or thread) each.

</details>

---

*Next up: **Module 10 — I/O Models and Asynchronous I/O.** The C10K cliffhanger gets resolved: blocking vs non-blocking fds, the `select`/`poll`/`epoll` progression, the event loop that lets one thread juggle thousands of sockets, edge- vs level-triggered, and a preview of `io_uring`. This is the model behind Nginx, Redis, Node.js, and Netty. The fd spine reaches its most powerful form: watching thousands of descriptors at once. Continuing straight on.*
