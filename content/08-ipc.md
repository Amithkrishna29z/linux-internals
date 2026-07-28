# Module 8 — Interprocess Communication (IPC)

> **Estimated time:** 3–4 hours · **Core path:** Concepts 1–3 (anonymous pipes, the `pipe`+`fork`+`dup2` pattern, FIFOs) and the `pipe_basic` + `pipeline` programs are core. Shared memory (Concept 4) and the mechanism-comparison tour (Concept 5) are core-but-meaty; the shared-memory-with-semaphore project is a second-pass consolidation of Modules 5–7.
>
> **Prerequisites:** Modules 0–7. This is where three earlier threads converge: `fork`'s separate address spaces (Module 5) create the *need* to communicate, `dup2` fd-redirection (Modules 3–4) is the *mechanism* for wiring pipes, and `mmap` (Module 7) is how shared memory works. The producer–consumer intuition from threads (Module 6) reappears — now *between processes*.

---

## The Big Picture

Module 5 gave processes their great virtue — **isolation**. Each has its own address space; one can't corrupt another. But isolation creates a problem: real systems are made of *cooperating* processes. Your shell runs `ls | wc -l` and the output of one program becomes the input of another. A web server hands connections to worker processes. A database's backends coordinate through shared state. None of that works if processes can't talk. **Interprocess communication** is the set of mechanisms the kernel provides to let isolated processes exchange data — and the beautiful thing is that the spine of this whole course, the **file descriptor**, is the star again. The most important IPC mechanism, the pipe, is *just a pair of file descriptors*, and you already know how to `read`, `write`, and `dup2` those.

Start with the **anonymous pipe**. `pipe(fds)` hands you two file descriptors: `fds[0]` you read from, `fds[1]` you write to. Bytes written to the write end come out the read end, in order — a kernel-buffered, unidirectional byte stream between the two ends. On its own that's just talking to yourself; the magic is combining it with `fork`. Because a child inherits the parent's open file descriptors (Module 5), a pipe created *before* `fork` becomes a channel *between* parent and child — one holds the write end, the other the read end, and now two separate processes share a byte stream. This `pipe`+`fork` pattern is the foundation, and when you add `dup2` to wire a pipe end onto stdin/stdout, you've built exactly the shell's `|` operator. We'll implement `ls | wc -l` from scratch and you'll see there's no magic in it — just two `fork`s, a `pipe`, and four `dup2`/`close` calls.

Then the variations. An anonymous pipe only connects *related* processes (parent/child, because the fd must be inherited). A **named pipe (FIFO)** — created with `mkfifo` — gives the same byte-stream semantics but appears as a *file in the filesystem*, so two *unrelated* programs can `open` it by path and talk. (`mkfifo mypipe; cmd1 > mypipe & cmd2 < mypipe` — pipe semantics without a shared parent.) Pipes are wonderful for streaming bytes, but they have a cost: every byte is *copied* through the kernel, twice (write in, read out). When two processes need to share a *large* region or exchange data at memory speed, the answer is **shared memory**: `mmap` the *same* physical pages into both processes' address spaces (Module 7's `mmap`, now with `MAP_SHARED`), and they can read and write a common region with zero copying — a write by one is instantly visible to the other. It's the fastest IPC, but it hands you back the concurrency problem from Module 6: shared mutable memory between processes needs synchronization (a semaphore), because now *two processes* can race on the same bytes.

Finally, a map of the territory. Beyond pipes and shared memory, POSIX offers **message queues** (kernel-managed queues of discrete messages with priorities) and **Unix-domain sockets** (the socket API of Module 9, but local — the standard way modern daemons talk: Docker, systemd, X11, database clients all use them). Each mechanism trades off differently along a few axes: does it stream bytes or discrete messages? Related processes only, or any? Copy through the kernel, or share memory directly? Built-in synchronization, or roll your own? Knowing the axes lets you pick the right tool instead of reaching for the one you happen to remember — and it reveals that "everything is a file descriptor" carries you through almost all of it.

---

## Concepts

### 1. Anonymous pipes: a byte stream that is a pair of fds

**What it is:** `pipe(int fds[2])` creates a **unidirectional** in-kernel byte buffer and returns two file descriptors for its ends: `fds[0]` is the **read** end, `fds[1]` is the **write** end. Bytes `write`-n to `fds[1]` are `read` from `fds[0]` in FIFO order. It's a fixed-size kernel buffer (typically 64 KB); writing to a full pipe *blocks* until a reader drains it, and reading an empty pipe *blocks* until a writer adds data — automatic flow control.

```
        write(fds[1], ...)                 read(fds[0], ...)
             │                                    ▲
             ▼                                    │
        ┌──────────────── kernel pipe buffer ─────────────┐
        │  [ b y t e s   i n   F I F O   o r d e r ] ...   │
        └─────────────────────────────────────────────────┘
        fds[1] = WRITE end                 fds[0] = READ end
```

**Why it exists:** It's the simplest kernel-provided channel for streaming bytes between two flows of execution, with built-in buffering and blocking flow control (no busy-waiting, no manual synchronization for the stream itself). It's unidirectional by design — for two-way you make *two* pipes — which keeps the semantics simple. Being a *file descriptor* means it plugs into the entire `read`/`write`/`dup2`/`select` machinery you already know.

**Java analogy:** `java.io.PipedInputStream` / `PipedOutputStream` connect two threads with exactly this byte-stream-with-blocking behavior. But the crucial difference is scope: Java's piped streams connect **threads in one JVM** (shared heap); a POSIX pipe connects **separate processes**. When you build a process pipeline in Java you use `ProcessBuilder`'s `redirectOutput`/`redirectInput` or `pipeTo` — and under the hood the JVM is creating OS pipes exactly like this one.

### 2. `pipe` + `fork` + `dup2`: how the shell's `|` works

**What it is:** A pipe alone talks to itself; combined with `fork` it becomes inter-*process* communication, because the child inherits the pipe's fds. The recipe for `cmd1 | cmd2`: create a pipe, `fork` twice; in the first child, `dup2` the pipe's **write** end onto **stdout** and `exec cmd1`; in the second child, `dup2` the pipe's **read** end onto **stdin** and `exec cmd2`. `cmd1` writes to "stdout" (really the pipe), `cmd2` reads from "stdin" (really the pipe).

```
   pipe(p);  fork twice.

     cmd1 (child A)                        cmd2 (child B)
     dup2(p[1], STDOUT_FILENO)             dup2(p[0], STDIN_FILENO)
     close p[0], p[1]                      close p[0], p[1]
     exec cmd1                             exec cmd2
          │ writes to stdout                     ▲ reads from stdin
          ▼                                       │
        p[1] ───────── kernel pipe ─────────── p[0]

   ⚠ EVERY process must close the pipe ends it doesn't use, or the reader
     never sees EOF (a pipe reports EOF only when ALL write ends are closed).
```

**Why it exists:** This is the payoff of the fork/exec gap (Module 5) plus fd inheritance plus `dup2` redirection (Modules 3–4). Because a program writes to fd 1 without knowing or caring what it's connected to, the shell can transparently connect one program's output to another's input — the *composability* that makes Unix's "small tools connected by pipes" philosophy work. `ls | grep foo | wc -l` is three `fork`s and two pipes, each program blissfully unaware it's in a pipeline.

**Java analogy:** `ProcessBuilder.startPipeline(List.of(pb1, pb2, pb3))` (Java 9+) builds exactly this — a pipeline of external processes wired output-to-input. Before that, you manually connected `p1.getInputStream()` to `p2`'s stdin. Either way, the JVM is doing this `pipe`+`fork`+`dup2` dance for you natively; `startPipeline` is Java naming the pattern you're about to implement by hand.

### 3. Named pipes (FIFOs): pipes for unrelated processes

**What it is:** A **FIFO** (named pipe) is a pipe that exists as a **filename** in the filesystem, created with `mkfifo("path", mode)` (or the `mkfifo` shell command). Any process can `open` it by path — one opens it for writing, another for reading — and they get the same byte-stream-with-blocking semantics as an anonymous pipe. Unlike an anonymous pipe, the two processes need **no common ancestor**; the shared *name* is the rendezvous point.

```
   $ mkfifo /tmp/myfifo          # a special file: prw-r--r-- (the 'p' = pipe)
   $ wc -l < /tmp/myfifo &       # reader blocks, waiting for a writer
   $ ls -l > /tmp/myfifo         # writer connects; bytes flow; reader unblocks

   open("/tmp/myfifo") blocks until BOTH a reader and a writer have opened it.
```

**Why it exists:** Anonymous pipes can only connect processes that share the fd via inheritance (parent/child). A FIFO lifts that restriction using the filesystem namespace as a meeting point, so *completely unrelated* programs — started separately, by different users, at different times — can stream data to each other. It's the simplest persistent named channel, useful for shell scripting, logging fan-in, and simple service interfaces.

**Java analogy:** No direct built-in — Java doesn't wrap `mkfifo` — but you'd open a FIFO as a normal `FileInputStream`/`FileOutputStream` on its path and get pipe semantics. The conceptual cousin is any *named* rendezvous: a well-known socket address, a named message queue, or a JMS queue name. The idea "unrelated parties meet at a shared name" is the through-line.

### 4. Shared memory: `mmap` the same pages into two processes

**What it is:** **Shared memory** maps the *same physical pages* into multiple processes' address spaces, so they share a region of memory directly — a write by one process is immediately visible to the others, with **no copying** through the kernel. Two common routes: `mmap` with `MAP_SHARED | MAP_ANONYMOUS` shared across a `fork` (related processes), or `shm_open` + `mmap` on a named POSIX shared-memory object (unrelated processes). Because the memory is genuinely shared and mutable, you must **synchronize** access — a POSIX semaphore (`sem_t` in shared memory, or a named `sem_open`) is the usual tool.

```
   Process A                         Process B
   ┌──────────────┐                  ┌──────────────┐
   │ virtual addr │──┐            ┌──│ virtual addr │
   └──────────────┘  │            │  └──────────────┘
                     ▼            ▼
              ┌───────────────────────────┐
              │  ONE physical region       │  ← both mappings point here
              │  [ shared bytes ]          │     write by A == visible to B
              └───────────────────────────┘        (needs a semaphore/mutex!)
```

**Why it exists:** Pipes and sockets copy every byte through the kernel (write-in, read-out) — fine for streams, but a bottleneck for large or high-frequency data. Shared memory is the **fastest** IPC because after setup there's no kernel involvement per access — it's just memory reads and writes. The price is that you inherit the full concurrency problem (Module 6) *across processes*: races, the need for mutual exclusion, and the fact that a pointer in the region is meaningless to the other process unless the region is mapped at compatible addresses (so you store offsets, not pointers).

**Java analogy:** `FileChannel.map()` returns a `MappedByteBuffer` — that's `mmap`, and mapping the same file `MAP_SHARED` from two JVMs gives you cross-process shared memory. `ByteBuffer.allocateDirect` is off-heap native memory in the same spirit. The synchronization burden is the same one you know from Java threads, now spanning processes — and since there's no shared JVM, you can't use `synchronized`; you need an OS primitive (a file lock, or a semaphore in the shared region). This is the layer under memory-mapped databases (LMDB, and the mmap in Kafka/Lucene).

### 5. The IPC menu: message queues, Unix-domain sockets, and how to choose

**What it is:** Two more mechanisms complete the toolkit, and a few axes let you choose among all of them:

- **POSIX message queues** (`mq_open`/`mq_send`/`mq_receive`): the kernel maintains a queue of **discrete messages** (not a byte stream), each with a **priority**; readers get whole messages, highest priority first. Good when you want message boundaries and prioritization for free.
- **Unix-domain sockets** (`socket(AF_UNIX, ...)`): the full socket API (Module 9) but *local* to one machine, addressed by a filesystem path (or abstract name). Bidirectional, support both stream and datagram semantics, and can even **pass file descriptors between processes** (`SCM_RIGHTS`) — a superpower nothing else has. This is what modern daemons actually use: Docker's `/var/run/docker.sock`, systemd, X11, PostgreSQL's local connections, `.sock` files everywhere.

The choosing axes:

```
   mechanism        | stream/msg | related-only? | copies? | built-in sync?
   -----------------|------------|---------------|---------|---------------
   anon pipe        | stream     | yes (inherit) | yes     | flow control
   FIFO             | stream     | no (by name)  | yes     | flow control
   shared memory    | raw bytes  | either        | NO      | you add it
   message queue    | messages   | no (by name)  | yes     | queue + priority
   unix socket      | either     | no (by name)  | yes     | flow control (+fd passing)
```

**Why it exists:** Different communication patterns want different guarantees. Streaming logs wants a pipe; a request/reply local service wants a Unix socket; sharing a big buffer at memory speed wants shared memory; prioritized discrete tasks want a message queue. There's no single "best" — the kernel offers a family so you can match the mechanism to the pattern (byte stream vs message, related vs unrelated, copy vs share, sync included vs DIY).

**Java analogy:** Unix-domain sockets got first-class Java support in Java 16 (`SocketChannel.open(StandardProtocolFamily.UNIX)`) precisely because so many services (Docker, databases) expose them. Message queues map to JMS / in-JVM `BlockingQueue` conceptually (discrete messages, priorities). The general lesson — "pick the IPC that matches your communication shape" — is the same architectural decision you make choosing between a Kafka topic, a REST call, and a shared cache, just one layer down.

---

## Code

### Program 1 — `pipe_basic.c`: parent and child talk over a pipe

```c
/* pipe_basic.c
 *
 * The simplest IPC: create a pipe BEFORE fork, so parent and child share it.
 * The child writes a message into the pipe; the parent reads it out. Shows
 * the must-close-the-unused-end rule (or the reader never sees EOF).
 *
 * Compile:  gcc -Wall -Wextra -o pipe_basic pipe_basic.c
 * Run:      ./pipe_basic
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/wait.h>

int main(void)
{
    int fds[2];
    if (pipe(fds) < 0) { perror("pipe"); return 1; }   /* fds[0]=read, fds[1]=write */

    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 1; }

    if (pid == 0) {
        /* CHILD: writer. Close the read end we don't use, then write. */
        close(fds[0]);
        const char *msg = "hello from the child";
        write(fds[1], msg, strlen(msg));
        close(fds[1]);        /* closing the write end lets the reader see EOF */
        _exit(0);
    }

    /* PARENT: reader. Close the write end we don't use, then read to EOF. */
    close(fds[1]);            /* CRUCIAL: if we leave OUR write end open, read never EOFs */
    char buf[128];
    ssize_t n;
    printf("parent received: ");
    while ((n = read(fds[0], buf, sizeof buf)) > 0)
        fwrite(buf, 1, (size_t)n, stdout);   /* read returns 0 (EOF) when all write ends closed */
    printf("\n");
    close(fds[0]);
    wait(NULL);
    return 0;
}
```

**Expected output:**
```
$ ./pipe_basic
parent received: hello from the child
```

**Walkthrough of the non-obvious parts:**
- The `pipe()` call happens **before** `fork()` — that's what makes both processes share it. After fork, *both* have both ends open (4 open fds total across the two processes); each must close the end it doesn't use.
- **The close discipline is not optional.** A pipe's read end returns EOF (`read` returns 0) only when *every* write-end fd is closed. The parent holds a copy of the write end (`fds[1]`) after fork; if the parent doesn't `close(fds[1])`, then even after the child closes *its* write end, a write end is still open (the parent's own), so the parent's `read` blocks **forever** waiting for more data. Closing the unused end is the #1 pipe gotcha.
- The child uses `_exit` (not `exit`) after `fork` — the Module 5 discipline (don't flush the parent's inherited stdio buffers).
- `read` in a loop until it returns 0 — the correct way to drain a stream; one `read` isn't guaranteed to return the whole message (though for a tiny message it usually does).

### Program 2 — `pipeline.c`: implement `ls | wc -l` from scratch

```c
/* pipeline.c
 *
 * Implements the shell pipeline `ls | wc -l` by hand: one pipe, two forks,
 * and dup2 to wire ls's stdout to the pipe and wc's stdin from the pipe.
 * This is EXACTLY what your shell does for the `|` operator.
 *
 * Compile:  gcc -Wall -Wextra -o pipeline pipeline.c
 * Run:      ./pipeline        (prints the number of entries, like `ls | wc -l`)
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>

int main(void)
{
    int p[2];
    if (pipe(p) < 0) { perror("pipe"); return 1; }

    /* --- Child 1: `ls`, stdout -> pipe write end --- */
    pid_t c1 = fork();
    if (c1 < 0) { perror("fork"); return 1; }
    if (c1 == 0) {
        dup2(p[1], STDOUT_FILENO);   /* ls writes to the pipe instead of the terminal */
        close(p[0]); close(p[1]);    /* close BOTH original pipe fds after dup2 */
        execlp("ls", "ls", (char *)NULL);
        perror("execlp ls"); _exit(127);
    }

    /* --- Child 2: `wc -l`, stdin <- pipe read end --- */
    pid_t c2 = fork();
    if (c2 < 0) { perror("fork"); return 1; }
    if (c2 == 0) {
        dup2(p[0], STDIN_FILENO);    /* wc reads from the pipe instead of the keyboard */
        close(p[0]); close(p[1]);
        execlp("wc", "wc", "-l", (char *)NULL);
        perror("execlp wc"); _exit(127);
    }

    /* --- Parent: MUST close both pipe ends, or wc never sees EOF --- */
    close(p[0]);
    close(p[1]);
    waitpid(c1, NULL, 0);
    waitpid(c2, NULL, 0);
    return 0;
}
```

**Expected output (the count depends on the directory):**
```
$ ./pipeline
7
$ ls | wc -l        # same result -- we reimplemented the pipe
7
```

**Walkthrough of the non-obvious parts:**
- **Every process closes the pipe fds it isn't actively using — including the parent.** After two forks, the pipe's two ends exist in *three* processes. `wc` sees EOF (and thus finishes counting) only when *all* write ends are closed: `ls`'s, and the parent's copy. If the parent forgets `close(p[1])`, `wc` hangs forever. This is the same close-discipline as Program 1, now with three processes — the classic pipeline deadlock.
- **`dup2` then close the originals.** After `dup2(p[1], STDOUT_FILENO)`, fd 1 and `p[1]` both point at the pipe's write end; we close `p[1]` (and `p[0]`) because the exec'd program only needs the standard fd (1 or 0), and leaving extra copies open would (a) keep a write end alive, breaking EOF, and (b) leak fds.
- `execlp` (list form, PATH search) is just the `execl` cousin of Module 5's `execvp` — fine here because the argument lists are fixed and short.
- The parent `waitpid`s **both** children so neither becomes a zombie (Module 5). Order doesn't matter much; both must be reaped.
- There is genuinely *nothing else* to the shell's `|`. Chain N commands with N-1 pipes and 2(N-1) dup2s and you have arbitrary pipelines.

### Project — `shm_counter.c`: two processes share a counter via `mmap` + a semaphore

```c
/* shm_counter.c
 *
 * Shared-memory IPC: parent and child both increment a counter that lives in
 * memory mmap'd MAP_SHARED (so it's the SAME physical memory in both). A POSIX
 * semaphore placed IN that shared memory synchronizes them -- because shared
 * mutable memory across processes races exactly like threads do (Module 6).
 *
 * Compile:  gcc -Wall -Wextra -pthread -o shm_counter shm_counter.c
 * Run:      ./shm_counter        (final count == 2 * ITERS, every time)
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <semaphore.h>

#define ITERS 100000

/* The shared region's layout: a semaphore for mutual exclusion + the counter. */
typedef struct {
    sem_t sem;
    long  counter;
} Shared;

int main(void)
{
    /* mmap an anonymous, SHARED region: survives fork and is common to both. */
    Shared *s = mmap(NULL, sizeof *s, PROT_READ | PROT_WRITE,
                     MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (s == MAP_FAILED) { perror("mmap"); return 1; }

    /* sem_init with pshared=1: a semaphore usable BETWEEN processes, initial value 1. */
    if (sem_init(&s->sem, 1, 1) < 0) { perror("sem_init"); return 1; }
    s->counter = 0;

    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 1; }

    /* BOTH parent and child run this loop, on the SAME s->counter. */
    for (int i = 0; i < ITERS; i++) {
        sem_wait(&s->sem);       /* lock: like pthread_mutex_lock, across processes */
        s->counter++;            /* the shared increment -- race without the semaphore */
        sem_post(&s->sem);       /* unlock */
    }

    if (pid == 0) {
        _exit(0);                /* child done */
    }

    wait(NULL);                  /* parent waits for child */
    printf("counter = %ld  (expected %d)  %s\n",
           s->counter, 2 * ITERS,
           s->counter == 2 * ITERS ? "OK" : "<-- RACE (lost updates)");

    sem_destroy(&s->sem);
    munmap(s, sizeof *s);
    return 0;
}
```

**Expected output:**
```
$ ./shm_counter
counter = 200000  (expected 200000)  OK
```
(Remove the `sem_wait`/`sem_post` and it prints a smaller, varying number — the Module 6 race, now *between processes*.)

**Walkthrough of the non-obvious parts:**
- **`MAP_SHARED | MAP_ANONYMOUS`** is the key. `MAP_ANONYMOUS` = memory not backed by a file (just RAM); `MAP_SHARED` = changes are visible to other processes mapping the same region. Because we `mmap` *before* `fork`, the child inherits the mapping and both point at the **same physical pages** — a normal `MAP_PRIVATE` mapping (Module 7) would instead be copy-on-write, and the child's increments wouldn't be seen by the parent.
- **The semaphore lives *inside* the shared region** (`s->sem`), and `sem_init(&s->sem, 1, 1)` — the middle `1` is `pshared`, meaning "shared between processes." A semaphore on a private stack wouldn't be visible to both processes; it must sit in the shared memory itself. This is the cross-process analogue of Module 6's mutex.
- **The race is identical to Module 6's**, just across processes instead of threads: `s->counter++` is load-add-store, and two processes interleaving lose updates. Shared memory gives you thread-style concurrency *without* threads — same hazard, same fix (mutual exclusion), different primitive (`sem_t` with `pshared`, because there's no shared JVM/mutex-in-one-address-space to lean on).
- `munmap` and `sem_destroy` clean up — though, like Module 7, the OS reclaims the mapping at exit anyway. Doing it explicitly is correct hygiene and matters for long-running processes that map and unmap repeatedly.
- This is the skeleton of every high-performance IPC system: a shared region for the data, a semaphore (or futex, or lock-free structure) for coordination. Databases, message brokers, and the disruptor pattern are elaborations of exactly this.

---

## Under the Hood

Run `strace -f ./pipeline` and watch a shell pipeline built from syscalls:

```
pipe2([3, 4], 0)                         = 0                 ← [1] fds 3(read) 4(write)
clone(... SIGCHLD)                       = 4501              ← [2] fork child 1 (ls)
[pid 4501] dup2(4, 1)                    = 1                 ← [3] ls: pipe-write -> stdout
[pid 4501] close(3)                      = 0                 ← [4] close unused ends
[pid 4501] close(4)                      = 0
[pid 4501] execve("/usr/bin/ls", ...)    = 0                 ← [5] become ls
clone(... SIGCHLD)                       = 4502              ← [6] fork child 2 (wc)
[pid 4502] dup2(3, 0)                    = 0                 ← [7] wc: pipe-read -> stdin
[pid 4502] close(3)                      = 0
[pid 4502] close(4)                      = 0
[pid 4502] execve("/usr/bin/wc", ...)    = 0                 ← [8] become wc
close(3)                                 = 0                 ← [9] PARENT closes both ends
close(4)                                 = 0                 ←     (or wc never EOFs!)
wait4(4501, ...)                         = 4501              ← [10] reap ls
[pid 4502] read(0, "...", ...)           = N                 ←     wc reads ls's output
[pid 4502] read(0, "", ...)              = 0                 ← [11] EOF: all write ends closed
wait4(4502, ...)                         = 4502              ←     reap wc
```

Annotated:
1. **`pipe2([3, 4], 0)`** — the kernel creates the pipe buffer and returns the two fds (3 = read, 4 = write). (glibc uses `pipe2`, the modern variant that can atomically set flags; plain `pipe` is equivalent here.)
2, 6. **`clone(... SIGCHLD)`** — two `fork`s (Module 5's `clone` with no sharing flags), creating the two child processes that will become `ls` and `wc`.
3–5. **`dup2(4, 1)` in `ls`** — redirect `ls`'s stdout onto the pipe's write end, close both original pipe fds, then `execve` `ls`. `ls` now writes to the pipe without knowing it.
7–8. **`dup2(3, 0)` in `wc`** — redirect `wc`'s stdin from the pipe's read end, close the originals, `execve` `wc`. Mirror image of `ls`'s setup.
9. **Parent `close(3)` and `close(4)`** — the linchpin. The parent inherited copies of *both* pipe ends; it uses neither, so it must close both. If it skips `close(4)`, a write end stays open and step 11 never happens.
11. **`read(0, "", ...) = 0`** — `wc` gets EOF because now *all* write ends are closed (`ls`'s via exit, the parent's via `close(4)`). Only then does `wc` finish counting and print. **This single `= 0` is why the close discipline matters** — it's the moment the whole pipeline can complete.

The headline: **a shell pipeline is `pipe2` + two `clone`s + `dup2`s to wire the ends onto stdin/stdout + disciplined `close`s so EOF can propagate.** You are watching, syscall by syscall, exactly what bash does for `ls | wc -l`. Run `strace -f` on your own shell running that command and you'll see the identical sequence.

---

## Try This

Ordered easy → hard.

1. **(Easy) Prove the close rule.** In `pipe_basic.c`, comment out the parent's `close(fds[1])`. Run it — the program **hangs** (the parent's `read` never sees EOF because the parent's own write end is still open). Uncomment it; it completes. Internalize: EOF on a pipe requires *all* write ends closed. *Hint: this one experiment teaches the single most important pipe fact.*

2. **(Easy) Talk over a FIFO from two terminals.** `mkfifo /tmp/f`. In terminal 1: `cat < /tmp/f` (it blocks). In terminal 2: `echo "hello unrelated process" > /tmp/f`. Watch terminal 1 print it. `ls -l /tmp/f` and note the `p` file type. *Hint: two unrelated `cat`/`echo` processes just did IPC with no shared parent — that's what "named" buys you.*

3. **(Medium) Extend the pipeline to three stages.** Modify `pipeline.c` to run `ls | grep . | wc -l` — two pipes, three forks, careful closing in all four processes (three children + parent). Confirm it matches the shell. *Hint: each pipe needs its own `close` discipline; a leaked write end anywhere hangs the stage after it.*

4. **(Medium) Make the shared counter race.** In `shm_counter.c`, remove the `sem_wait`/`sem_post` and run it several times. Watch the final count fall short of 200000, differently each run — the Module 6 data race reproduced *between processes*. Then restore the semaphore. *Hint: shared memory = shared mutable state = the same hazard as threads, minus the shared address space.*

5. **(Hard) Pass a file descriptor between processes over a Unix socket.** Use `socketpair(AF_UNIX, SOCK_STREAM, 0, sv)`, `fork`, and `sendmsg`/`recvmsg` with an `SCM_RIGHTS` control message to send an open fd (e.g. an opened file) from parent to child. Have the child `read` from the received fd. Explain why this is impossible with pipes or shared memory. *Hint: an fd is an index into a per-process table; only the kernel (via `SCM_RIGHTS`) can install a working copy in another process's table — this is how servers hand connections to workers.*

---

## Gotchas

- **Not closing unused pipe ends → the reader never sees EOF (deadlock).** A pipe reports EOF only when *every* write-end fd is closed. Every process (including the parent that forked the pipeline) must close the ends it doesn't use. A single leaked write end anywhere hangs the reader forever. This is *the* pipe bug; when a pipeline hangs, count your open write ends first.

- **`SIGPIPE` when writing to a pipe with no readers.** If you `write` to a pipe whose read end is fully closed, the kernel sends `SIGPIPE`, which by default *kills your process* (this is why `yes | head` terminates cleanly — `yes` gets SIGPIPE'd when `head` exits). Handle or ignore `SIGPIPE` (Module 5) if you want the `write` to fail with `EPIPE` instead of dying.

- **Pipes are unidirectional.** One pipe carries data one way. For bidirectional parent/child communication you need **two** pipes (and careful closing on both), or a `socketpair` (which is bidirectional). Trying to read and write the same pipe from both processes gets you garbage — one process reads its own bytes.

- **`MAP_PRIVATE` vs `MAP_SHARED` for shared memory.** `MAP_PRIVATE` is copy-on-write — after `fork`, each process gets its *own* copy on first write, so changes are **not** shared. You must use `MAP_SHARED` for the mapping to actually be common memory. Mixing these up gives you a "shared" counter that silently doesn't share.

- **Semaphore/mutex must live *in* the shared region.** A `sem_t` (or pthread mutex) on a process's private stack or heap isn't visible to the other process. For cross-process sync, place the primitive inside the shared memory and initialize it with the process-shared flag (`sem_init(..., pshared=1, ...)`, or `PTHREAD_PROCESS_SHARED` for a mutex). Otherwise each process locks its own private copy and there's no mutual exclusion at all.

- **Pointers don't travel through shared memory.** A pointer is a virtual address meaningful only in the process that made it; the other process may map the region at a different base. Store **offsets** into the region, not raw pointers, for any data structure that lives in shared memory.

- **Partial reads/writes on pipes.** `read`/`write` on a pipe can transfer *fewer* bytes than requested (especially past the ~64 KB buffer, or with large messages). Don't assume one `read` gets a whole "message" — pipes are byte streams with no message boundaries. If you need messages, frame them yourself (length prefix) or use a message queue / datagram socket.

- **Leftover FIFO files.** A `mkfifo`'d file persists in the filesystem after the processes exit — clean it up (`unlink`/`rm`) or reuse it deliberately. Also, `open`ing a FIFO for reading *blocks* until a writer opens it (and vice versa) unless you pass `O_NONBLOCK` — a common "my program hangs on open" surprise.

---

## Checkpoint

1. What does `pipe()` give you, and why must you combine it with `fork` to get inter-*process* communication? What's the direction and buffering behavior of a pipe?
2. Walk through exactly how a shell implements `cmd1 | cmd2` using `pipe`, `fork`, and `dup2`. Which fds get duplicated onto what, and which ends must each process close — and why?
3. Why does a pipe's reader only see EOF when *all* write ends are closed, and what practical bug does forgetting this cause? Include why the *parent* process (not just the writing child) must close its copy.
4. What is shared memory, why is it the fastest IPC, and what problem does it hand back to you? For the `mmap` route, what do `MAP_SHARED` and `MAP_ANONYMOUS` each contribute, and where must the synchronizing semaphore live?
5. Compare anonymous pipes, FIFOs, shared memory, and Unix-domain sockets along the axes of stream-vs-message, related-vs-unrelated processes, and copy-vs-share. Give one scenario where each is the right choice.

---

<details>
<summary><b>Checkpoint Answers</b></summary>

1. `pipe()` returns two file descriptors — `fds[0]` (read end) and `fds[1]` (write end) — connected by an in-kernel **unidirectional** byte buffer: bytes written to `fds[1]` come out of `fds[0]` in FIFO order, with the kernel buffering (~64 KB) and blocking a writer when full / a reader when empty (built-in flow control). On its own a pipe just connects one process to itself; combined with `fork`, the **child inherits the pipe's fds**, so the two ends end up in two different processes — now it's inter-process communication. (The pipe must be created *before* the fork for the child to inherit it.)

2. Create a pipe `p`, then `fork` twice. In child 1: `dup2(p[1], STDOUT_FILENO)` so `cmd1`'s stdout is the pipe's write end, `close(p[0])` and `close(p[1])` (the originals, now redundant), then `exec cmd1`. In child 2: `dup2(p[0], STDIN_FILENO)` so `cmd2`'s stdin is the pipe's read end, close both originals, then `exec cmd2`. The **parent** closes *both* `p[0]` and `p[1]`. Every process closes the ends it doesn't use so that (a) no fds leak and (b) `cmd2` can see EOF once all write ends are closed. `cmd1` then writes to "stdout" (the pipe) and `cmd2` reads from "stdin" (the pipe), unaware they're connected.

3. A pipe's read end returns EOF (`read` → 0) only when the kernel sees that **no** write-end fd remains open anywhere — because as long as any process could still write, more data might arrive. Forgetting to close a write end means `read` **blocks forever**, deadlocking the pipeline. The parent must close its copy too: after `fork`, the parent inherited the pipe's write end; even after the writing child closes *its* write end, the parent's still-open copy keeps a write end alive, so the reader never EOFs. Closing every unused end — parent included — is mandatory.

4. Shared memory maps the **same physical pages** into multiple processes' address spaces, so a write by one is instantly visible to the others with **no copying through the kernel** — which is why it's the fastest IPC (after setup, it's just memory access, no syscalls per byte). The problem it hands back is **concurrency**: shared mutable memory across processes races exactly like threads (Module 6), so you must add synchronization. For `mmap`, `MAP_ANONYMOUS` means the region is backed by RAM (not a file) and `MAP_SHARED` means writes are shared with other mappers (vs `MAP_PRIVATE`'s copy-on-write); mapping before `fork` makes both processes share it. The synchronizing semaphore (or mutex) must live **inside the shared region** and be initialized process-shared (`sem_init(..., 1, ...)`), or each process would lock a private copy and get no mutual exclusion.

5. **Anonymous pipe** — byte *stream*, *related* processes only (fd inheritance), *copies* through kernel: right for a parent feeding a child, or a shell pipeline stage. **FIFO** — byte *stream*, *unrelated* processes (by filesystem name), *copies*: right for two independently-started programs streaming (e.g. a logging fan-in via `mkfifo`). **Shared memory** — raw *bytes*, either related or unrelated (anon+fork or `shm_open`), *no copy* (shared): right for high-throughput/large-data sharing like a memory-mapped database or ring buffer. **Unix-domain socket** — *stream or message*, *unrelated* (by path), *copies* but bidirectional and can pass fds: right for a local request/reply service (Docker's socket, a database's local connections). Choose by matching the axis to your need: message boundaries? relatedness? data size/speed? built-in sync?

</details>

---

*Next up: **Module 9 — Network I/O and Socket Programming.** The Unix-domain sockets you just met go global: the Berkeley sockets API (`socket`/`bind`/`listen`/`accept`/`connect`), TCP vs UDP, `struct sockaddr` and byte order, building an echo server and client, and why "everything is a file descriptor" means a socket reads and writes just like a pipe or a file. The fd spine reaches the network. Continuing straight on.*
