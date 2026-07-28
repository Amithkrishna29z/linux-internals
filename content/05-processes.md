# Module 5 — Processes

> **Estimated time:** 4–5 hours · **Core path:** Concepts 1–4 (fork, exec, wait, zombies/orphans) and the `forkexec` program are core. Signals (Concept 5) and the mini-shell are core-but-meaty; the async-signal-safety deep-dive is a second-pass topic.
>
> **Prerequisites:** Modules 0–4. You need fds (they're inherited across fork/exec), the buffered-vs-unbuffered fork bug (Module 3), and `dup2` redirection (Module 3) — the mini-shell uses all of it.

---

## The Big Picture

This is the pivot module. Everything before was "one program, talking to the kernel." Now we learn how programs *make other programs* — and it turns out Unix does this with a design so strange and so elegant that it's worth the whole module to absorb. In most environments you'd expect a single "run this program" call: give it a path and arguments, get back a new process. Unix splits that into **two** calls that seem bizarre at first: `fork` (clone the current process into two identical copies) and `exec` (replace a process's program with a different one). You almost never want a clone of yourself, and you almost never `exec` without cloning first — so why two steps? The answer is the deepest idea in Unix process design, and once it clicks, `ProcessBuilder`, shell redirection, pipes, and containers all make sense.

Start with **`fork`**: the call that returns *twice*. You call it once, and execution continues in *two* processes — the original (parent) and a near-identical copy (child) — each getting a different return value so they can tell which one they are. The child inherits everything: the same code, the same variable values, the same open file descriptors, the same current directory. For a moment there are two processes running the same program at the same line. This is genuinely mind-bending coming from Java, where there is no equivalent — `new Thread()` shares memory, but `fork` creates a whole separate process with its *own copy* of memory. The kernel makes this cheap with **copy-on-write**: it doesn't actually duplicate the memory, it just marks it shared-and-read-only and copies individual pages only when one side writes. So `fork` is fast even for a process using gigabytes.

Then **`exec`**: it does *not* create a process — it *replaces* the program running in the current one. The code, heap, and stack are thrown away and the new program's are loaded in; the PID stays the same, and — crucially — the open file descriptors survive. This is the key that makes the two-step design pay off: *between* the `fork` and the `exec`, the child is still running the parent's code, so it can adjust the environment the new program will inherit — redirect stdout to a file (`dup2`, Module 3/4), close descriptors, change directory, drop privileges — and *then* `exec`. That gap between fork and exec is where the shell does all its I/O plumbing. One call couldn't offer that hook; two calls do. That's the whole answer to "why two steps."

After a child runs, it must be **reaped**. When a process exits, it doesn't fully vanish — it becomes a **zombie**, a tiny husk holding its exit status, waiting for its parent to call **`wait`** and collect that status. If the parent never does, zombies pile up (a resource leak). If the parent dies first, the child becomes an **orphan** and is adopted by PID 1 (`init`/systemd), which reaps it. This parent-child-reaping dance, plus the exit codes flowing back (the `$?` you met in Module 0), is how process lifecycles are managed. Get it wrong and you get the two classic process bugs every backend engineer should be able to explain in an interview: zombie accumulation and the orphan/adoption mechanism.

Finally, **signals** — the OS's way of poking a process asynchronously. Ctrl-C (Module 4) sends `SIGINT`; a child exiting sends the parent `SIGCHLD`; `kill -9` sends `SIGKILL`. Signals interrupt your program's normal flow to run a handler, and that asynchrony makes them subtly dangerous: a handler can fire *between any two instructions*, so it may only safely call a small set of "async-signal-safe" functions (not `printf`, not `malloc`). We'll install handlers with `sigaction`, reap children on `SIGCHLD`, and understand why signal handlers are a minefield. Then we tie it all together into a **working mini-shell** — prompt, read, fork, exec, wait, with redirection — which is the single best exercise for cementing this entire module. You'll have built, in ~150 lines, the thing you type into every day.

---

## Concepts

### 1. `fork`: one call, two returns, and copy-on-write

**What it is:** `fork()` creates a new process by duplicating the calling one. After it returns, *two* processes are executing the next line. They're nearly identical — same code, same variable values, same open fds, same working directory — but distinguished by `fork`'s return value:

```
   pid_t pid = fork();
   ┌───────────────────────────────────────────────┐
   │  fork() is called ONCE...                       │
   │  ...but returns in TWO processes:               │
   │                                                 │
   │   PARENT:  pid = <child's PID>  (a positive #)  │
   │   CHILD:   pid = 0                               │
   │   ERROR:   pid = -1  (no child made)            │
   └───────────────────────────────────────────────┘

   if (pid < 0)      { perror("fork"); }        // failure
   else if (pid == 0){ /* I am the CHILD */ }   // child sees 0
   else              { /* I am the PARENT, child is `pid` */ }
```

The child gets a **copy** of the parent's memory — not shared, *copied*. Change a variable in the child and the parent doesn't see it (unlike threads, Module 6). But copying gigabytes on every fork would be ruinous, so the kernel uses **copy-on-write (COW)**: parent and child initially share the same physical memory pages, marked read-only. The copy of a page happens lazily, only when one side *writes* to it. Read-heavy children (which then `exec` and throw the memory away anyway) cost almost nothing.

**Why it exists:** It's the Unix primitive for creating processes. Combined with `exec`, it gives the two-step model whose payoff is the fork/exec gap (Concept 2). COW makes it efficient enough to be the universal mechanism.

**Java analogy:** **There is no `fork` in Java, and understanding *why* is illuminating.** `fork` duplicates the entire process including all threads' state — but a JVM has many threads (GC, JIT, etc.), locks, and internal invariants, and duplicating a multi-threaded process leaves those in an undefined state in the child (only the forking thread survives in the child). So Java offers `ProcessBuilder`/`Runtime.exec` which do `fork`+`exec` together (via `posix_spawn`) and never expose the dangerous in-between. When people say "you can't safely fork a JVM," this is why. `fork` is a process concept; Java lives above it.

### 2. The `exec` family: replacing the process image

**What it is:** `exec` **replaces** the current process's program with a new one. It does *not* return on success — there's nothing to return to; the old code is gone. Same PID, same open fds (they're inherited), but new code/data/heap/stack:

```
   execvp("ls", argv);      // replace THIS process with /usr/bin/ls
   perror("execvp");        // only reached if exec FAILED (e.g. not found)
   _exit(127);              // convention: 127 = command not found
```

The family members differ in how you pass arguments and whether they search `$PATH`:
- `execv(path, argv)` / `execvp(file, argv)` — **v** = argv array; **p** = search `$PATH` (so `execvp("ls", ...)` finds `/usr/bin/ls`, using the Module 1 logic).
- `execl(path, arg0, arg1, ..., NULL)` — **l** = list the args inline.
- `execve(path, argv, envp)` — the actual **syscall**; all the others are library wrappers over it. Lets you set the environment explicitly.

The magic of the two-step design lives *between* fork and exec: in the child, after `fork` but before `exec`, you're still running the parent's code, so you can rewire the world the new program will wake up in — `dup2` to redirect its stdout, `close` unwanted fds, `chdir`, `setuid` to drop privileges. Then `exec`, and the new program inherits that carefully-arranged state without knowing anything happened.

**Why it exists:** Separating "make a process" (fork) from "choose its program" (exec) creates the configuration hook that single-step process creation can't offer. Redirection, pipes, and privilege-dropping all happen in that gap.

**Java analogy:** `Runtime.exec`/`ProcessBuilder` = fork+exec fused. Java's `ProcessBuilder.redirectOutput`, `.directory()`, `.environment()` are Java's way of exposing "the stuff you'd do in the fork/exec gap" as configuration methods — because Java can't give you the raw gap. Every option on `ProcessBuilder` is a thing a C programmer does by hand between `fork` and `execvp`.

### 3. `wait`/`waitpid`, exit codes, zombies, and orphans

**What it is:** When a child exits, the kernel keeps a small record (its exit status) until the parent collects it with `wait`/`waitpid`. That not-yet-collected record is a **zombie** (`Z` in `ps`) — the process is dead but its entry lingers.

```
   pid_t child = fork();
   if (child == 0) { /* ... */ _exit(3); }     // child exits with code 3
   int status;
   pid_t w = waitpid(child, &status, 0);        // parent BLOCKS until child exits
   if (WIFEXITED(status))
       printf("child %d exited with %d\n", w, WEXITSTATUS(status));  // -> 3
   else if (WIFSIGNALED(status))
       printf("child killed by signal %d\n", WTERMSIG(status));
```

- **`wait(&status)`** — wait for *any* child. **`waitpid(pid, &status, opts)`** — wait for a specific child; `WNOHANG` makes it non-blocking (poll).
- The `status` is decoded with macros: `WIFEXITED`/`WEXITSTATUS` (normal exit + code), `WIFSIGNALED`/`WTERMSIG` (killed by a signal).
- **Zombie**: child exited, parent hasn't `wait`ed. Harmless singly, but a parent that forks children and never reaps them **leaks** process-table entries until the system can't fork anymore.
- **Orphan**: parent exits *before* the child. The child is re-parented to **PID 1** (`init`/systemd), which reaps it automatically. So orphans are cleaned up; zombies-with-a-live-negligent-parent are the real leak.

**Why it exists:** The exit status must survive the process's death so the parent can learn *how* the child ended (this is `$?`, Module 0). `wait` is the collection mechanism. The zombie state is just "status is ready, waiting to be read."

**Java analogy:** `Process.waitFor()` returns the exit code — that's `waitpid` + `WEXITSTATUS`. `Process.exitValue()` (non-blocking, throws if still running) is `waitpid(..., WNOHANG)`. Java reaps the OS-level zombie for you inside `Process`, which is why you rarely see zombies from Java — but forget to consume a subprocess's output and drain/`waitFor` it and you get the Java version of the same resource leak (a stuck process).

### 4. The process tree, PIDs/PPIDs, and PID 1

**What it is:** Every process has a **PID** (its ID) and a **PPID** (its parent's PID). Since every process is forked by another, they form a **tree** rooted at PID 1:

```
   PID 1  systemd/init        (the root; started by the kernel at boot)
     ├── PID 843  sshd
     │     └── PID 1002 bash        (your shell)
     │            ├── PID 1050 ls        (fork+exec'd by bash, reaped by bash)
     │            └── PID 1051 ./minishell
     │                   └── PID 1077 grep   (fork+exec'd by YOUR shell)
     └── PID 500  cron
```

**PID 1** is special: started directly by the kernel at boot, it's the ancestor of everything and the adopter of all orphans. If PID 1 ever dies, the kernel panics. `pstree`, `ps -ef`, and `ps --forest` show the tree; `/proc/[pid]/status` (Module 12) shows a process's PPID.

**Why it exists:** The tree is the natural consequence of "every process is forked by a parent," and it gives structure to lifecycle management (who reaps whom) and signal delivery (process groups, job control). PID 1 as a universal reaper guarantees no orphan is left un-reaped.

**Java analogy:** `ProcessHandle` (Java 9+) exposes `.parent()`, `.children()`, `.pid()` — a view of exactly this tree. `ProcessHandle.current().pid()` is `getpid()`. The concept of a single root and orphan adoption has no in-JVM analogy (threads don't get "adopted"), but the tree structure maps directly onto `ProcessHandle`.

### 5. Signals: `kill`, `SIGCHLD`, `SIGINT`, and `sigaction`

**What it is:** A **signal** is an asynchronous notification delivered to a process — a software interrupt. The kernel (or another process via `kill`) can interrupt your program to deliver one, running a **handler** you installed, or taking a default action (often: terminate). Common ones:

```
   SIGINT  (2)   Ctrl-C from the terminal          default: terminate
   SIGTERM (15)  polite "please exit" (kill's default)  default: terminate
   SIGKILL (9)   forcible kill -- CANNOT be caught/ignored/handled
   SIGSEGV (11)  invalid memory access (the segfault)    default: terminate+core
   SIGCHLD (17)  a child stopped or exited          default: ignored
   SIGSTOP/SIGTSTP  suspend (Ctrl-Z)               SIGSTOP can't be caught
```

Install a handler with **`sigaction`** (not the older, unportable `signal()`):

```
   struct sigaction sa = {0};
   sa.sa_handler = my_handler;      // function to run on the signal
   sigemptyset(&sa.sa_mask);        // no extra signals blocked during handler
   sa.sa_flags = SA_RESTART;        // auto-restart interrupted syscalls
   sigaction(SIGINT, &sa, NULL);
```

**`SIGCHLD`** is the key one for process management: the kernel sends it to a parent when a child exits, so the parent can reap in a handler (instead of blocking in `wait`) — this is how servers avoid zombies while doing other work.

**Async-signal-safety** — the sharp edge. A handler can fire *between any two machine instructions*, including in the middle of `malloc` or `printf`. If the handler then calls `malloc`/`printf` too, it can corrupt internal state (non-reentrant). So handlers may call only **async-signal-safe** functions (a specific list: `write`, `_exit`, `waitpid`, `signal-safe` primitives — **not** `printf`, `malloc`, most of stdio). The safe pattern: in the handler, do the absolute minimum (set a `volatile sig_atomic_t` flag, or `write()` a byte, or `waitpid` in a loop) and do real work back in the main loop.

**Why it exists:** Signals are how the OS and other processes communicate asynchronous events — a key press, a child dying, a timer firing, a fault — without the target polling. It's the interrupt mechanism (Module 13's hardware interrupts) surfaced to user space.

**Java analogy:** JVM shutdown hooks (`Runtime.addShutdownHook`) catch `SIGTERM`/`SIGINT` — that's a signal handler in disguise. But Java gives you a *safe, cooked* version: the hook runs on a normal thread, not in the raw async context, so you can allocate and log freely. The raw async-signal-safety minefield is hidden. `SIGKILL` being uncatchable is why a shutdown hook *doesn't* run on `kill -9` — a great interview question ("why didn't my shutdown hook run?"). **The async-signal-safety constraint has no Java equivalent** because the JVM shields you from it.

---

## Code

### Program 1 — `forkexec.c`: fork, redirect in the gap, exec, wait, decode status

```c
/* forkexec.c
 *
 * The canonical fork+exec+wait cycle -- what your shell does for every
 * command. Demonstrates: the two returns of fork, using the fork/exec GAP
 * to redirect the child's stdout to a file (dup2), execvp with PATH search,
 * and decoding the child's exit status in the parent.
 *
 * Compile:  gcc -Wall -Wextra -o forkexec forkexec.c
 * Run:      ./forkexec              (runs `ls -l`, child's output -> out.txt)
 *           cat out.txt
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>     /* fork, execvp, dup2, close, _exit */
#include <fcntl.h>      /* open */
#include <sys/wait.h>   /* waitpid, W* macros */
#include <errno.h>

int main(void)
{
    printf("parent PID = %d, about to fork...\n", getpid());
    fflush(stdout);   /* FLUSH before fork: avoid the duplicated-buffer bug (Module 3) */

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return 1;
    }

    if (pid == 0) {
        /* ---- CHILD ----
         * We are still running THIS program's code. This is the GAP:
         * set up the environment the new program will inherit, THEN exec. */
        printf("  child PID = %d (fork returned 0 here)\n", getpid());
        fflush(stdout);

        /* Redirect the child's stdout to out.txt -- exactly like `ls > out.txt`.
         * The exec'd `ls` will inherit fd 1 pointing at the file. */
        int fd = open("out.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) { perror("open"); _exit(1); }
        if (dup2(fd, STDOUT_FILENO) < 0) { perror("dup2"); _exit(1); }
        close(fd);

        /* Replace this process with `ls -l`. execvp searches $PATH.
         * On success, NOTHING below runs -- the image is gone. */
        char *args[] = { "ls", "-l", NULL };
        execvp("ls", args);

        /* Only reached if exec FAILED. Use _exit (not exit) in a child
         * after fork to avoid flushing the parent's stdio buffers twice. */
        perror("execvp");
        _exit(127);   /* 127 = command not found, by convention */
    }

    /* ---- PARENT ----
     * fork returned the child's PID here. Wait for the child to finish
     * and decode HOW it ended. */
    int status;
    pid_t w = waitpid(pid, &status, 0);   /* blocks until child exits */
    if (w < 0) { perror("waitpid"); return 1; }

    if (WIFEXITED(status))
        printf("parent: child %d exited normally, code %d\n",
               w, WEXITSTATUS(status));
    else if (WIFSIGNALED(status))
        printf("parent: child %d killed by signal %d\n",
               w, WTERMSIG(status));

    printf("parent: child's output was redirected to out.txt\n");
    return 0;
}
```

**Expected output:**
```
$ ./forkexec
parent PID = 12345, about to fork...
  child PID = 12346 (fork returned 0 here)
parent: child 12346 exited normally, code 0
parent: child's output was redirected to out.txt
$ cat out.txt
total 48
-rwxr-xr-x 1 amith amith 17024 Jul 28 10:00 forkexec
-rw-r--r-- 1 amith amith  2891 Jul 28 10:00 forkexec.c
...
```

**Walkthrough of the non-obvious parts:**
- `fflush(stdout)` before `fork` — the Module 3 lesson applied: an unflushed stdout buffer gets *copied into the child*, so both would print the "about to fork" line. Flushing first empties the buffer so it's only printed once. This bug bites everyone who skips it.
- The `if (pid == 0)` block **is the child**, and everything in it before `execvp` runs *the parent's program in the child process* — that's the gap. We use it to `dup2` the child's stdout to a file, so the eventual `ls` writes there. `ls` has no idea; it just writes to fd 1.
- `execvp("ls", args)` — searches `$PATH` (the **p**), takes an argv array (the **v**). On success it never returns; the `perror`/`_exit` below only run if `ls` couldn't be found/executed.
- `_exit(127)` not `exit(127)` — in a child after fork, use `_exit` (or `_Exit`): it terminates immediately *without* running `atexit` handlers or flushing stdio buffers, which would re-flush the *parent's* inherited buffers and duplicate output. Subtle but correct.
- `waitpid(pid, &status, 0)` then `WIFEXITED`/`WEXITSTATUS` — the parent blocks until the child exits, then decodes the status. `ls` succeeding gives exit code 0.

### Program 2 — `signals.c`: handle SIGINT gracefully and reap children on SIGCHLD

```c
/* signals.c
 *
 * Installs signal handlers with sigaction: SIGINT (Ctrl-C) sets a flag for
 * a graceful shutdown, and SIGCHLD reaps children so they never become
 * zombies. Demonstrates async-signal-safety: handlers do the MINIMUM
 * (set a flag / call waitpid), real work happens in the main loop.
 *
 * Compile:  gcc -Wall -Wextra -o signals signals.c
 * Run:      ./signals        (spawns children; press Ctrl-C to stop)
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <errno.h>
#include <string.h>

/* volatile sig_atomic_t: the ONLY type safe to touch from a handler and
 * read from main. 'volatile' stops the compiler caching it in a register. */
static volatile sig_atomic_t g_stop = 0;

/* SIGINT handler: do the minimum. Just record that we should stop.
 * We do NOT printf here (not async-signal-safe). */
static void on_sigint(int signo)
{
    (void)signo;
    g_stop = 1;
    /* write() IS async-signal-safe, so a tiny note is allowed: */
    const char msg[] = "\n[SIGINT received: will shut down]\n";
    write(STDERR_FILENO, msg, sizeof msg - 1);
}

/* SIGCHLD handler: reap ALL finished children in a loop. waitpid with
 * WNOHANG is async-signal-safe. Looping handles multiple children exiting
 * "at once" (signals don't queue, so one SIGCHLD may cover several). */
static void on_sigchld(int signo)
{
    (void)signo;
    int saved = errno;                 /* preserve errno across the handler */
    while (waitpid(-1, NULL, WNOHANG) > 0)
        ;                               /* reap each; loop until none left */
    errno = saved;
}

static void install(int signo, void (*fn)(int), int flags)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = fn;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = flags;
    if (sigaction(signo, &sa, NULL) < 0) { perror("sigaction"); exit(1); }
}

int main(void)
{
    install(SIGINT,  on_sigint,  0);
    install(SIGCHLD, on_sigchld, SA_RESTART);   /* SA_RESTART: resume slept syscalls */

    printf("PID %d running. I spawn a child every 2s. Ctrl-C to stop.\n",
           getpid());

    while (!g_stop) {
        pid_t c = fork();
        if (c < 0) { perror("fork"); break; }
        if (c == 0) {
            /* child: do a little work, then exit with a code */
            printf("  child %d working...\n", getpid());
            fflush(stdout);
            sleep(1);
            _exit(0);           /* triggers SIGCHLD in the parent -> reaped */
        }
        /* parent: keep going; the SIGCHLD handler reaps children for us,
         * so NO zombies accumulate even though we never call wait() here. */
        sleep(2);
    }

    printf("Graceful shutdown. Reaping any stragglers...\n");
    while (waitpid(-1, NULL, 0) > 0)      /* reap remaining children */
        ;
    printf("Done.\n");
    return 0;
}
```

**Expected output (interactive; press Ctrl-C after a few children):**
```
$ ./signals
PID 20001 running. I spawn a child every 2s. Ctrl-C to stop.
  child 20002 working...
  child 20003 working...
^C
[SIGINT received: will shut down]
Graceful shutdown. Reaping any stragglers...
Done.
```

**Walkthrough of the non-obvious parts:**
- `volatile sig_atomic_t g_stop` — the *only* portable, safe way to share a flag between a handler and `main`. `sig_atomic_t` guarantees reads/writes are atomic w.r.t. signal delivery; `volatile` stops the compiler from optimizing away re-reads (it might otherwise cache `g_stop` in a register and loop forever). Using a plain `int` here is a real bug.
- `on_sigint` calls `write`, not `printf` — because a handler can interrupt `printf` mid-way; calling `printf` *again* from the handler can deadlock or corrupt stdio. `write` is on the async-signal-safe list. This is the discipline that keeps handlers safe.
- `on_sigchld` loops `waitpid(-1, NULL, WNOHANG)` — reaps *every* finished child. Signals **don't queue**: if three children exit while you're handling one SIGCHLD, you get *one* more SIGCHLD, not three. Looping until `waitpid` returns 0 catches them all. This exact idiom prevents zombie accumulation in servers.
- `errno` save/restore in the handler — the handler runs asynchronously and calls `waitpid`, which may change `errno`; if it fired mid-way through main's code that was about to check `errno`, you'd corrupt it. Saving and restoring is correct handler hygiene.
- `SA_RESTART` on SIGCHLD — without it, a blocking syscall (like `sleep`'s underlying `nanosleep`, or a `read`) interrupted by the signal would fail with `EINTR`; `SA_RESTART` transparently restarts it. (Note some syscalls don't restart even with the flag — hence Module 3's manual `EINTR` loops for critical I/O.)

### Project — `minishell.c`: a working shell (prompt → read → fork → exec → wait, with `>` redirection)

```c
/* minishell.c
 *
 * A minimal but REAL shell. It ties together the whole module: read a line,
 * parse it into argv, handle output redirection (`cmd > file`), fork, set up
 * redirection in the child (the fork/exec GAP), execvp, and wait in the
 * parent -- reporting the exit code as $?. Also handles built-in `cd` and
 * `exit`. This is bash's core loop in ~130 lines.
 *
 * Compile:  gcc -Wall -Wextra -o minishell minishell.c
 * Run:      ./minishell
 *           mysh$ ls -l
 *           mysh$ echo hello > greeting.txt
 *           mysh$ cat greeting.txt
 *           mysh$ cd /tmp
 *           mysh$ exit
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <errno.h>

#define MAX_ARGS 64

/* Split `line` in place into argv words on whitespace. Also detects a
 * `> filename` redirection, removing it from argv and returning the target
 * filename via *outfile (NULL if none). Returns the argument count. */
static int parse(char *line, char *argv[], char **outfile)
{
    *outfile = NULL;
    int argc = 0;
    char *tok = strtok(line, " \t\r\n");
    while (tok != NULL && argc < MAX_ARGS - 1) {
        if (strcmp(tok, ">") == 0) {
            /* next token is the redirection target */
            tok = strtok(NULL, " \t\r\n");
            if (tok == NULL) {
                fprintf(stderr, "mysh: syntax error: expected filename after >\n");
                return -1;
            }
            *outfile = tok;
        } else {
            argv[argc++] = tok;
        }
        tok = strtok(NULL, " \t\r\n");
    }
    argv[argc] = NULL;   /* execvp needs a NULL-terminated array */
    return argc;
}

int main(void)
{
    char line[1024];
    char *argv[MAX_ARGS];
    char *outfile;

    while (1) {
        /* PROMPT */
        printf("mysh$ ");
        fflush(stdout);

        /* READ a line. NULL => EOF (Ctrl-D): exit cleanly. */
        if (fgets(line, sizeof line, stdin) == NULL) {
            printf("\n");
            break;
        }

        int argc = parse(line, argv, &outfile);
        if (argc < 0) continue;      /* parse error already reported */
        if (argc == 0) continue;     /* empty line */

        /* BUILT-INS: cd and exit must run in the SHELL itself, not a child
         * (a child chdir wouldn't affect the shell). */
        if (strcmp(argv[0], "exit") == 0)
            break;
        if (strcmp(argv[0], "cd") == 0) {
            const char *dir = argv[1] ? argv[1] : getenv("HOME");
            if (chdir(dir) < 0) perror("cd");
            continue;
        }

        /* EXTERNAL command: fork + exec + wait. */
        pid_t pid = fork();
        if (pid < 0) { perror("fork"); continue; }

        if (pid == 0) {
            /* CHILD: the fork/exec GAP -- set up redirection, then exec. */
            if (outfile != NULL) {
                int fd = open(outfile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (fd < 0) { perror(outfile); _exit(1); }
                if (dup2(fd, STDOUT_FILENO) < 0) { perror("dup2"); _exit(1); }
                close(fd);
            }
            execvp(argv[0], argv);
            /* only if exec failed: */
            fprintf(stderr, "mysh: %s: %s\n", argv[0], strerror(errno));
            _exit(127);
        }

        /* PARENT: wait and report status (this is $?). */
        int status;
        if (waitpid(pid, &status, 0) < 0) { perror("waitpid"); continue; }
        if (WIFEXITED(status) && WEXITSTATUS(status) != 0)
            fprintf(stderr, "mysh: [exit %d]\n", WEXITSTATUS(status));
        else if (WIFSIGNALED(status))
            fprintf(stderr, "mysh: [killed by signal %d]\n", WTERMSIG(status));
    }
    return 0;
}
```

**Expected output:**
```
$ ./minishell
mysh$ echo hello world
hello world
mysh$ echo saved here > out.txt
mysh$ cat out.txt
saved here
mysh$ ls /nonexistent
mysh: ls: No such file or directory... 
mysh: [exit 2]
mysh$ nosuchcmd
mysh: nosuchcmd: No such file or directory
mysh$ cd /tmp
mysh$ pwd
/tmp
mysh$ exit
$
```

**Walkthrough of the non-obvious parts:**
- **Built-ins run in the parent** — `cd` and `exit` are handled *before* forking, in the shell process itself. Why? A `chdir` in a forked child changes only the child's directory; the child then exits and the shell is unaffected. For `cd` to actually change *the shell's* directory, it must run in the shell. Every real shell handles `cd`, `exit`, `export` as built-ins for this exact reason — a favorite interview question ("why is `cd` a shell built-in and not a program?").
- **Redirection happens in the child, in the gap** — after `fork`, before `execvp`, the child `open`s the file and `dup2`s it onto fd 1. The exec'd program inherits the redirected stdout. This is the whole payoff of the two-step model, now in your own shell.
- `execvp(argv[0], argv)` — PATH search + argv array. `argv` is already NULL-terminated by `parse`. On failure we report with `strerror(errno)` and `_exit(127)`.
- `fgets` returning NULL = **Ctrl-D (EOF)** — the line discipline (Module 4) sends EOF, `fgets` returns NULL, and we exit cleanly, exactly like bash quitting on Ctrl-D.
- The parent's `waitpid` + status decoding gives you `$?` — reporting non-zero exits and signal deaths, just like a real shell tracks the last command's status.

---

## Under the Hood

Run `strace -f ./forkexec` (`-f` follows the child) and watch the two-step creation:

```
write(1, "parent PID = 12345, about to fork...\n", 37) = 37
clone(child_stack=NULL, flags=CLONE_CHILD_CLEARTID|... SIGCHLD) = 12346   ← [1] fork!
                                                        ↑ returns child PID to parent
strace: Process 12346 attached
[pid 12346] openat(AT_FDCWD, "out.txt", O_WRONLY|O_CREAT|O_TRUNC, 0644) = 3  ← [2] child, in the gap
[pid 12346] dup2(3, 1)                                = 1                    ← [3] redirect stdout
[pid 12346] close(3)                                  = 0
[pid 12346] execve("/usr/bin/ls", ["ls", "-l"], 0x...) = 0                   ← [4] child becomes ls
[pid 12345] wait4(12346, ...                                                 ← [5] parent blocks in wait
[pid 12346] write(1, "total 48\n...", ...)            = ...                  ← ls's output -> the file
[pid 12346] exit_group(0)                             = ?                    ← [6] ls exits, code 0
[pid 12345] <... wait4 resumed> [{WIFEXITED(s) && WEXITSTATUS(s) == 0}], 0) = 12346  ← [7] reaped
[pid 12345] write(1, "parent: child 12346 exited...\n", ...) = ...
```

Annotated:
1. **`clone(... SIGCHLD) = 12346`** — `fork` is implemented via the **`clone`** syscall on Linux. It returns the child's PID (12346) to the parent. (In the child, the same call "returns" 0 — you see the child continue as a separate traced process.) The `SIGCHLD` flag means "send SIGCHLD to the parent when this child dies" — that's how Concept 5's SIGCHLD gets generated.
2–3. **`openat` + `dup2(3, 1)` in pid 12346** — the child, *still running forkexec's code*, sets up redirection **in the gap**. fd 1 now points at `out.txt`. This is the exact moment the two-step design pays off.
4. **`execve("/usr/bin/ls", ...) = 0`** — the child *becomes* `ls`. Note the PID is unchanged (still 12346) but the program is now `ls`; the inherited fd 1 still points at the file. Everything `ls` writes goes there.
5, 7. **`wait4(12346, ...)`** — the parent's `waitpid` is the `wait4` syscall, blocking until the child exits, then returning the decoded status. This is the reaping that prevents a zombie.
6. **`exit_group(0)`** — `ls` exits with code 0; the kernel turns the child into a (briefly) zombie and sends SIGCHLD to the parent, whose `wait4` collects the status and lets the zombie be freed.

The headline: **`fork` is `clone`, the child runs your code in the gap (openat/dup2) before `execve` replaces it, and `wait4` reaps the exit status.** You are watching, at the syscall level, exactly what your `minishell` — and real bash — does for every command. Run `strace -f ./minishell` and type `ls > x`; you'll see this identical clone→(dup2)→execve→wait4 sequence.

---

## Try This

Ordered easy → hard.

1. **(Easy) See both returns.** Write a 10-line program: `pid_t p = fork(); printf("pid=%d, fork returned %d, my getpid=%d\n", (int)getpid(), (int)p, (int)getpid());`. Run it. You'll see the line printed *twice* with different values — the parent (returns child PID) and child (returns 0). *Hint: `fflush(stdout)` before fork or you'll get the double-buffer bug too.*

2. **(Easy) Make a zombie, then a reaper.** Fork a child that `_exit(0)`s immediately; have the parent `sleep(30)` *without* calling `wait`. In another terminal, `ps aux | grep defunct` — you'll see a `<defunct>` zombie. Then add `wait(NULL)` and confirm the zombie disappears. *Hint: `Z`/`<defunct>` in ps = a zombie waiting to be reaped.*

3. **(Medium) Watch orphan adoption.** Fork a child that `sleep(10)`s and prints its PPID (`getppid()`) once per second. Have the *parent* exit immediately. Watch the child's PPID change from the parent's PID to **1** (systemd) the moment the parent dies. Explain who reaps the child now. *Hint: orphans are re-parented to PID 1, which reaps them.*

4. **(Medium) Add `>>` and `<` to minishell.** Extend `parse`/the child setup to handle append (`>>` → `O_APPEND` instead of `O_TRUNC`) and input redirection (`<` → open O_RDONLY, `dup2` onto fd 0). Test with `sort < unsorted.txt > sorted.txt`. *Hint: input redirection is `dup2(fd, STDIN_FILENO)`; it's the mirror image of output redirection.*

5. **(Hard) Add a single pipe (`cmd1 | cmd2`) to minishell.** Create a `pipe()` (Module 8 preview), fork twice, `dup2` the write end onto cmd1's stdout and the read end onto cmd2's stdin, close the unused ends in each child, and `wait` for both. Test `ls | wc -l`. Explain why you must close the pipe ends you don't use in each process (or the reader never sees EOF). *Hint: this is the bridge to Module 8; the "close unused ends" rule is the classic pipe gotcha.*

---

## Gotchas

- **Not reaping children → zombies.** A parent that forks and never `wait`s leaves zombies (`<defunct>`) that consume process-table slots. In a long-running server, this eventually makes `fork` fail. Fix: `wait`/`waitpid`, or reap in a `SIGCHLD` handler (Program 2). The #1 process interview question: "what's a zombie and how do you prevent it?"

- **`exit()` vs `_exit()` in a forked child.** After `fork`, a child that fails to `exec` should call **`_exit()`**, not `exit()`. `exit()` runs `atexit` handlers and *flushes stdio buffers* — but the child inherited a *copy* of the parent's buffers, so flushing them re-emits the parent's buffered output (the double-print bug again). `_exit()` skips all that.

- **Forgetting to flush before `fork`.** Unflushed stdout is copied into the child; both flush later → duplicated output. `fflush(stdout)` before `fork` (or use unbuffered `write`). Directly connected to Module 3's printf-before-fork bug.

- **`SIGKILL`/`SIGSTOP` can't be caught.** You cannot install a handler for signal 9 (`SIGKILL`) or `SIGSTOP` — the kernel enforces this so there's always a way to kill/stop a runaway process. This is *why* a JVM shutdown hook doesn't run on `kill -9`. Interview gold.

- **Doing unsafe things in a signal handler.** Calling `printf`, `malloc`, or most library functions from a handler is undefined behavior (non-reentrant). A handler can fire mid-`malloc` and re-enter it → heap corruption or deadlock. Only call async-signal-safe functions (`write`, `_exit`, `waitpid`, ...). Set a `volatile sig_atomic_t` flag and do the work in `main`.

- **Signals don't queue.** If three `SIGCHLD`s "arrive" while one is being handled, you get *one* pending SIGCHLD, not three. So a SIGCHLD handler must **loop** `waitpid(-1, ..., WNOHANG)` to reap *all* ready children, or some zombies survive. Same for any signal used as a "something happened" nudge.

- **Assuming `exec` returns on success.** It doesn't — on success there's no old program to return to. Code after `exec` runs *only* on failure. Beginners write `execvp(...); printf("done");` and are confused when "done" never prints on success (and always prints on failure).

- **Built-ins that must be in the shell.** `cd`, `exit`, `export`, `umask` must run in the shell process, not a forked child — a child's `chdir` dies with the child. If your shell forks `cd`, the directory never changes. This is why they're built-ins.

---

## Checkpoint

1. `fork()` "returns twice." Explain what that means and how the parent and child each tell which one they are. What does copy-on-write buy you?
2. Why does Unix split process creation into `fork` + `exec` (two calls) instead of one "spawn" call? What can you do in the gap between them, and give a concrete example.
3. What is a zombie process, what is an orphan, and what happens to each? Which one is the real resource leak, and how do you prevent it?
4. Your `SIGCHLD` handler calls `waitpid(-1, NULL, WNOHANG)` exactly once per signal, yet zombies still sometimes accumulate. Why, and what's the fix?
5. Why must `cd` be a shell built-in rather than an external program the shell forks and execs?

---

<details>
<summary><b>Checkpoint Answers</b></summary>

1. `fork()` is called once but execution continues in **two** processes — the original **parent** and a near-identical **child** (a copy of the parent's memory, fds, etc.). It returns **the child's PID in the parent** and **0 in the child** (and -1 on failure, in the parent only), so each branch (`if (pid == 0)` = child) knows its role. **Copy-on-write** means the kernel doesn't physically duplicate the parent's memory at fork; parent and child share pages read-only and a page is copied only when one side writes it — making fork cheap even for large processes (and essentially free when the child immediately `exec`s).

2. The two-step split creates a **gap** between "make the process" and "load the program," and in that gap the child (still running the parent's code) can **configure the environment the new program will inherit** — redirect fds with `dup2` (e.g. send stdout to a file for `cmd > out.txt`), close descriptors, `chdir`, drop privileges with `setuid`, set up pipes. A single "spawn" call couldn't offer that hook. Concrete example: to run `ls > out.txt`, the shell forks, and in the child `open`s the file and `dup2`s it onto fd 1 *before* `execvp("ls", ...)`.

3. A **zombie** is a process that has exited but whose exit status hasn't yet been collected by its parent via `wait` — it lingers as a husk in the process table (`<defunct>`). An **orphan** is a process whose parent exited first; it's re-parented to **PID 1** (init/systemd), which reaps it. The **real leak is zombies with a live but negligent parent** (orphans get auto-reaped by PID 1). Prevent zombies by calling `wait`/`waitpid`, or by reaping in a `SIGCHLD` handler.

4. Because **signals don't queue**: if several children exit close together, multiple SIGCHLDs collapse into (possibly) a single delivered signal, so handling one child per signal leaves the others un-reaped. The fix is to **loop** in the handler: `while (waitpid(-1, NULL, WNOHANG) > 0) ;` — reap every ready child on each SIGCHLD.

5. `cd` must change **the shell's own working directory**. If the shell forked a child to run `cd`, the child's `chdir` would change only the child's directory, and the child then exits — leaving the shell's directory unchanged. So `cd` (and `exit`, `export`, etc.) must execute **in the shell process itself**, which is why they're built-ins rather than external `fork`+`exec`'d programs.

</details>

---

*Next up: **Module 6 — Threads.** `pthread_create`/`join`, why threads share an address space (and processes don't), race conditions with a broken counter, mutexes/condvars/semaphores, thread-safety vs reentrancy, and a producer–consumer bounded buffer. The Java mapping is direct: `synchronized`→mutex, `wait/notify`→condvar. Continuing straight on.*
