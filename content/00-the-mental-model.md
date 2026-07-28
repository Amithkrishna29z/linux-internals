# Module 0 — The Mental Model

> **Estimated time:** 3–4 hours · **Core path:** this whole module is core. Do not skip it. Everything else in the course hangs off the mental model you build here.
>
> **Prerequisites:** You can write and run a "Hello, World" in *some* language. You've used a terminal at least a little. That's it.

---

## The Big Picture

You've spent years writing Spring Boot services. You call `repository.save(user)` and a row appears in Postgres. You return a `ResponseEntity` and bytes land on someone's browser. Between your code and the metal there is a tower of abstractions, and for most of your career you've been standing near the top of it, comfortably. This course walks you down the tower, floor by floor, until you're standing on the ground looking at the CPU. Module 0 is the elevator ride down where I show you the shape of the building before we start touching wires.

Here is the single most important idea in this entire course: **your program does not run the computer.** The operating system runs the computer. Your program is a guest. When your Java process wants to read a file, send a network packet, or even just print to the screen, it cannot do those things itself — it does not have permission to touch the disk controller or the network card. It has to *ask* the operating system to do it on its behalf. That request is called a **system call**, and understanding what happens when you make one is the spine of everything that follows. Files, sockets, threads, memory, devices — every single one is just a different flavor of "ask the kernel to do a thing."

Why does it work this way? Two reasons, and they're worth sitting with. First, **abstraction**: there are hundreds of different disk models, network cards, and GPUs. If every program had to know how to talk to each one, no software would ever work on more than one machine. The OS provides one uniform interface — `read()`, `write()`, `open()` — and translates it to whatever hardware is actually present. You already love this idea; it's exactly why you code against a `DataSource` interface instead of Postgres's wire protocol. Second, **protection**: your machine runs dozens of processes owned by different people and programs. If any one of them could scribble directly on RAM or disk, a single bug (or a single piece of malware) would take down everything. So the hardware itself enforces a wall between "programs" and "the OS," and only the OS is allowed on the privileged side.

That wall is not a metaphor — it is a physical feature of the CPU called **privilege rings**, and the OS lives in the most privileged one (the *kernel*). Your code lives in the least privileged one (*user space*). Crossing that wall is a deliberate, controlled, slightly expensive event. Learning systems programming is, more than anything, learning exactly *when* and *how* your code crosses that wall, what it costs, and what the kernel does on the other side. Once you can see the wall, you can never unsee it — and a huge amount of "mysterious" backend behavior (why is my service slow, why did it get OOM-killed, why is this fd leaking) suddenly becomes obvious.

The last big idea for today: **the process is the atom.** In the Java world you think in objects, beans, and threads. Down here the fundamental unit is the *process* — a running program with its own private view of memory, its own list of open files, its own identity. Your entire Spring Boot app is one process (well, one process with many threads inside it). The shell is a process. `ls` is a process. `systemd` is a process. Everything that runs is a process, processes make system calls, and system calls are how the OS does its job. Hold those three sentences in your head and the rest of this course is just filling in detail.

---

## Concepts

### 1. What an operating system actually is

**What it is:** Two things wearing one coat.
1. A **resource manager** — it decides which process gets the CPU right now, how RAM is divided up, whose turn it is to use the disk. It's an arbitrator handing out scarce resources to competing programs.
2. An **abstraction layer** — it hides the ugly, hardware-specific reality behind clean, uniform interfaces. "A file" is not a real thing; it's a fiction the OS maintains on top of spinning platters or flash cells. "A process having 4GB of memory" is a lie the OS tells every process simultaneously (Module 7).

**Why it exists:** Because hardware is hostile to share and hostile to program directly. The OS makes one machine safely usable by many programs at once, and makes many different machines look the same to your code.

**Java analogy:** The JVM is *itself* a mini-operating-system for your bytecode. It manages memory (GC = its resource manager for RAM), it abstracts the real OS (`File`, `Socket` work the same on Linux and Windows), and it schedules threads. So you already understand the *pattern* of "a manager that both allocates resources and hides the layer below." The kernel is that pattern, one level deeper — and the JVM is one of its guests.

### 2. User space vs kernel space, and privilege rings

**What it is:** x86 CPUs have four privilege levels called rings, numbered 0 (most privileged) to 3 (least). In practice Linux uses only two:
- **Ring 0 = kernel space.** Code here can execute any CPU instruction, touch any memory, talk to any device.
- **Ring 3 = user space.** Code here is fenced in. Try to execute a privileged instruction or read memory you don't own, and the CPU traps — it interrupts you and hands control to the kernel, which usually kills you (that's a segfault).

```
   PRIVILEGE RINGS (Linux uses only 0 and 3)

        +-----------------------------------+
        |            Ring 3                 |
        |   USER SPACE                      |
        |   your Java app, bash, ls, gcc    |
        |   ┌─────────────────────────────┐ |
        |   │        Ring 0               │ |
        |   │   KERNEL SPACE              │ |
        |   │   scheduler, drivers,       │ |
        |   │   filesystem, TCP stack     │ |
        |   │   >>> can touch hardware <<< │ |
        |   └─────────────────────────────┘ |
        +-----------------------------------+

   The only legal door from Ring 3 into Ring 0
   is a SYSTEM CALL. Everything else is a trap = death.
```

**Why it exists:** Protection. If any user program could run in ring 0, one buggy pointer would corrupt the kernel and crash the whole machine. The hardware enforces the wall so that software bugs stay contained to the process that made them.

**Java analogy:** There isn't a perfect one, and *that's the point* — this is a hardware feature the JVM cannot give you or take away. The closest cousin is the (now-removed) Java `SecurityManager`, which fenced off "untrusted" code from dangerous operations. But `SecurityManager` was enforced in *software* and could be misconfigured away. Rings are enforced by silicon. **No Java equivalent, and that's exactly why Java can't, for instance, write a device driver.**

### 3. What a system call *really* is

**What it is:** The one legal doorway from ring 3 to ring 0. When you call `write()`, here's the mechanism (x86-64):

```
  USER SPACE (ring 3)                 KERNEL SPACE (ring 0)
  -------------------                 ---------------------
  1. put syscall number in  register RAX  (write = 1)
  2. put arguments in       RDI, RSI, RDX ...
  3. execute the CPU instruction:  syscall
        │
        │   ← CPU switches to ring 0, jumps to a fixed
        │     kernel entry point (the syscall handler)
        ▼
                                 4. kernel reads RAX = 1
                                 5. indexes the SYSCALL TABLE:
                                       table[1] = sys_write
                                 6. runs sys_write in the kernel
                                    (validates args, does the I/O)
                                 7. puts return value back in RAX
        ┌──────────────────────────────┘
        ▼
  8. CPU switches back to ring 3, execution resumes
     right after the syscall instruction
```

The key players:
- **syscall number** — an integer naming which service you want. `write` is 1, `read` is 0, `openat` is 257 on x86-64.
- **the `syscall` instruction** — the hardware trap that flips privilege and jumps to the kernel's fixed entry point. Your program cannot jump *anywhere* it likes into the kernel; it can only knock on this one door.
- **the syscall table** — a big array of function pointers inside the kernel. Your syscall number indexes it. This is why numbers matter and are stable forever (breaking them breaks every compiled binary).

**Why it exists:** It's the controlled, auditable, single entry point that makes the protection wall usable. "Come in through this one door, tell me exactly what you want by number, and I'll decide whether to do it."

**Java analogy:** A system call is a *remote procedure call to the kernel*. You (the caller) marshal arguments, transfer control across a boundary you can't cross yourself, the other side does privileged work, and hands you a result. If you've ever called a Spring `@RestController` from a client, you know the shape: request in, boundary crossing, response out. The difference is the "network" here is the CPU's privilege mechanism, and the round trip is measured in tens-to-hundreds of nanoseconds, not milliseconds.

### 4. "Every program is a command and every command is a program"

**What it is:** `ls` is not magic built into the shell. It is a compiled C program living at `/usr/bin/ls`. When you type `ls -l /tmp`, the shell:
1. Splits your line into words: `["ls", "-l", "/tmp"]`.
2. Looks for a file named `ls` by walking each directory in your `$PATH` (`/usr/local/bin:/usr/bin:/bin:...`) until it finds `/usr/bin/ls`.
3. `fork()`s a copy of itself (Module 5), then `exec()`s `/usr/bin/ls`, handing it the argument list.
4. Waits for it to finish and collects its **exit code** (0 = success, non-zero = failure).

```
   argv:  argv[0]="ls"   argv[1]="-l"   argv[2]="/tmp"   argv[3]=NULL
                │
   $PATH search: /usr/local/bin  /usr/bin ✔ found /usr/bin/ls  /bin ...
                │
   shell: fork() ──► child ──► exec("/usr/bin/ls", argv) ──► runs, exit(0)
                │                                                  │
                └──────────── wait() ◄─────── exit code 0 ─────────┘
```

**Why it exists:** It's the Unix philosophy made concrete — the system is composed of small programs you can combine, not a monolith with hardcoded commands. This is *why* you can write your own command (Module 1) and drop it into `$PATH`, and the shell will treat it exactly like `ls`.

**Java analogy:** `argv` is your `public static void main(String[] args)` — `args` *is* `argv[1..]` (Java drops `argv[0]`, the program name; C keeps it). The exit code is `System.exit(int)`. `$PATH` resolution is conceptually the JVM's classpath search for a class to run. The `fork`+`exec` step, though, is `ProcessBuilder`/`Runtime.exec` under the hood — and *how* it works has no Java equivalent worth the name, which is why Module 5 exists.

### 5. The process as the core abstraction

**What it is:** A process is a running instance of a program plus everything the OS tracks about it: its memory (code, heap, stack), its **file descriptor table** (the numbered list of things it has open — remember this phrase, it's the spine of the course), its process ID (PID), its parent (PPID), its user/permissions, and its current CPU register state. The program on disk is dead text; the process is that text *breathed into life* and given resources.

**Why it exists:** It's the unit of isolation and the unit of scheduling. Each process gets its own private memory view so they can't corrupt each other, and the scheduler hands out CPU time process-by-process (thread-by-thread, really — Module 6).

**Java analogy:** A whole JVM instance is one process. The `.class`/`.jar` file is the dead program; `java -jar app.jar` breathes it into a process. Inside, Java threads map to OS threads that share that one process's memory — which is precisely why two Java threads can see the same `static` field, and two separate processes cannot. That sharing-vs-isolation line is the entire story of Modules 5, 6, and 8.

### 6. The tools you'll live in

You will not learn systems programming by reading. You learn it by running a program and *watching* what it does at the syscall boundary. These six tools are your instruments:

| Tool | What it does | Java-world analogy |
|---|---|---|
| `man` | The manual. Section matters (see below). | Javadoc, but for the OS |
| `strace` | Traces every **system call** a program makes. | Your window through the ring 3/0 wall |
| `ltrace` | Traces every **library call** (e.g. `malloc`, `printf`). | One layer up from `strace` |
| `gcc` | The compiler: C source → runnable ELF binary. | `javac`, but produces native code |
| `gdb` | The debugger: step, breakpoint, inspect memory. | IntelliJ debugger / `jdb` |
| `objdump` | Disassembles a binary back to assembly. | `javap -c` for native code |

**`man` sections — memorize this, it trips up everyone:**
- **Section 1** — user commands (`man 1 ls`). Programs you run in the shell.
- **Section 2** — **system calls** (`man 2 write`). The kernel's direct interface. *This is your section.*
- **Section 3** — **library functions** (`man 3 printf`). glibc/C-library functions that usually call section-2 syscalls underneath.

The same name can live in multiple sections and mean different things. `man 2 write` is the raw syscall; there is a `write` in other contexts too. When in doubt, specify the number: `man 2 open`, not just `man open`. Getting fluent at "which section?" is half of reading Unix documentation.

---

## Code

Systems programming is a hands-on sport, so even in this conceptual module you're going to compile and run real C and *see the wall* with `strace`. Two tiny programs.

### Program 1 — `write()` directly vs `printf()`: your first look at the boundary

```c
/* hello_syscall.c
 *
 * Prints a message TWO ways:
 *   (a) via write(2)  -- a raw system call, straight through the wall
 *   (b) via printf(3) -- a C-library function that buffers, then calls write(2)
 *
 * The point is not the output (it looks identical). The point is what
 * strace shows underneath: printf and write are NOT the same thing.
 *
 * Compile:  gcc -Wall -Wextra -o hello_syscall hello_syscall.c
 * Run:      ./hello_syscall
 * Inspect:  strace -e trace=write ./hello_syscall
 */

#include <unistd.h>     /* write(), STDOUT_FILENO */
#include <stdio.h>      /* printf(), perror()     */
#include <string.h>     /* strlen()               */
#include <errno.h>      /* errno                  */

int main(void)
{
    const char *msg = "Hello via write() syscall\n";

    /* write(fd, buffer, count) returns the number of bytes actually
     * written, or -1 on error. fd 1 is standard output (STDOUT_FILENO).
     * RULE OF THE COURSE: check every syscall's return value. */
    ssize_t n = write(STDOUT_FILENO, msg, strlen(msg));
    if (n == -1) {
        perror("write");   /* prints "write: <reason>" using errno */
        return 1;
    }

    /* printf lives in the C library. It formats into an internal buffer
     * and (for a terminal) flushes on the newline, eventually calling
     * write() itself. You never invoked write() here -- glibc did. */
    printf("Hello via printf() library call\n");

    return 0;   /* exit code 0 = success, like System.exit(0) */
}
```

**Expected output:**
```
Hello via write() syscall
Hello via printf() library call
```

**What `strace -e trace=write ./hello_syscall` shows (the interesting part):**
```
write(1, "Hello via write() syscall\n", 26) = 26
write(1, "Hello via printf() library call\n", 32) = 32
+++ exited with 0 +++
```

**Line-by-line walkthrough of the non-obvious bits:**
- `#include <unistd.h>` — the POSIX header that declares `write`, `read`, `close`, `fork`, and friends. "unistd" = *UNIx STanDard*. This is your section-2 toolbox.
- `ssize_t n` — `write` returns a *signed* size (`ssize_t`) because it must be able to return `-1`. `size_t` (unsigned) couldn't. This tiny detail is a real interview gotcha.
- `write(STDOUT_FILENO, msg, strlen(msg))` — three args: the **file descriptor** (1), the buffer, the byte count. Notice it's byte-oriented and knows nothing about strings or newlines. That's the raw kernel interface: bytes in, bytes out.
- `if (n == -1) perror("write")` — checking the return value is not defensive noise; a "short write" (n less than the length) is a real thing you'll handle in Module 3. Here we only guard the `-1` error case.
- The `strace` output proves the lesson: **two** `write` syscalls, one from *your* code and one that glibc made *for* you inside `printf`. You crossed the ring 3/0 wall twice, but only wrote the syscall once yourself.

### Program 2 — a program *is* a command: reading `argv` and returning an exit code

```c
/* mycmd.c
 *
 * A minimal Unix "command". It reads its arguments (argv), does something
 * with them, and returns an exit code -- exactly what /usr/bin/ls does.
 * Drop this into your PATH (Module 1) and the shell treats it like any
 * built-in tool.
 *
 * Compile:  gcc -Wall -Wextra -o mycmd mycmd.c
 * Run:      ./mycmd hello world
 *           ./mycmd            ; echo "exit code was $?"
 */

#include <stdio.h>

int main(int argc, char *argv[])
{
    /* argc = argument COUNT, including the program name.
     * argv = argument VECTOR, an array of C strings, argv[argc] == NULL.
     *
     *   ./mycmd hello world
     *   argc = 3
     *   argv[0] = "./mycmd"   <-- program name (Java's args does NOT include this)
     *   argv[1] = "hello"
     *   argv[2] = "world"
     *   argv[3] = NULL         <-- the terminator you can loop until */
    printf("I was invoked as: %s\n", argv[0]);
    printf("I received %d argument(s):\n", argc - 1);

    for (int i = 1; i < argc; i++) {
        printf("  argv[%d] = \"%s\"\n", i, argv[i]);
    }

    /* Convention: 0 = success. Non-zero = a specific failure.
     * Here: fail (exit 1) if the user gave us no arguments.
     * The shell reads this as $? -- how `&&`, `||`, and scripts branch. */
    if (argc == 1) {
        fprintf(stderr, "usage: %s ARG...\n", argv[0]);  /* errors go to stderr, fd 2 */
        return 1;
    }

    return 0;
}
```

**Expected output:**
```
$ ./mycmd hello world
I was invoked as: ./mycmd
I received 2 argument(s):
  argv[1] = "hello"
  argv[2] = "world"

$ ./mycmd ; echo "exit code was $?"
I was invoked as: ./mycmd
I received 0 argument(s):
usage: ./mycmd ARG...
exit code was 1
```

**Line-by-line walkthrough:**
- `int main(int argc, char *argv[])` — this is the *real* signature of `main`. Java hides `argc` (you use `args.length`) and drops `argv[0]`. C hands you both. `argv` is a raw array of pointers-to-char, `NULL`-terminated.
- `argc - 1` — we subtract the program name to report "user-supplied" args, matching how you'd think in Java.
- `fprintf(stderr, ...)` — error messages go to **standard error** (fd 2), not stdout (fd 1). This is why `./mycmd 2>errors.log` can separate errors from real output — a Module 4 idea you're meeting early because it matters *now*.
- `return 1` from `main` becomes the process's exit code, which the shell exposes as `$?`. That's the plumbing behind `mycmd && echo ok` — `&&` only runs the second command if the first returned 0.

---

## Under the Hood

Let's take Program 1 and watch it cross the wall. Run:

```
$ strace -f ./hello_syscall
```

You'll see a *lot* of lines before your output — that's the dynamic linker loading glibc (Module 2). Scroll to the meaningful tail:

```
execve("./hello_syscall", ["./hello_syscall"], 0x7 ...) = 0   ← [1] the shell exec'd you
brk(NULL)                               = 0x55e2c1a3f000       ← [2] glibc probing the heap
...
write(1, "Hello via write() syscall\n", 26) = 26               ← [3] YOUR write() call
write(1, "Hello via printf() library call\n", 32) = 32         ← [4] printf's write()
exit_group(0)                           = ?                    ← [5] process exits, code 0
+++ exited with 0 +++
```

Annotated:

1. **`execve(...)`** — this is the syscall that *started your program*. The shell called it to replace its forked child's memory with your binary. Every program's life begins with an `execve` (Module 5). It returns 0 into the *new* program, which is a beautiful oddity: the caller of `execve` never sees the return, because it's been overwritten.
2. **`brk(NULL)`** — glibc feeling out where the heap currently ends so it can manage `malloc` (Module 7). This happens before your `main` even runs. It's the C runtime setting up shop.
3. **`write(1, "...", 26) = 26`** — *this is your line of code.* Trace the path: your `write()` call put syscall number 1 in RAX, the fd/buffer/count in registers, executed the `syscall` instruction → CPU flipped to ring 0 → kernel indexed `sys_write` in the syscall table → the VFS layer routed fd 1 to the terminal driver (Module 4/13) → characters appeared → return value 26 (bytes written) came back in RAX → CPU flipped back to ring 3. All of that, tens of nanoseconds, one line.
4. **`write(1, "...", 32) = 32`** — you did *not* write this call. `printf` did. It formatted your string into glibc's internal stdio buffer, saw the `\n`, and flushed the buffer by calling `write` itself. Same wall-crossing mechanism, invoked on your behalf. This is the entire "syscall vs library function" distinction (Module 2/3) made visible.
5. **`exit_group(0)`** — returning 0 from `main` doesn't just "stop"; glibc calls the `exit_group` syscall to ask the kernel to tear down the process and record exit code 0. The kernel reclaims your memory, closes your file descriptors, and notifies your parent (the shell), which stores 0 in `$?`.

The headline: **`strace` is an x-ray of the ring 3/0 boundary.** Every line is a moment your program knocked on the kernel's door. For the rest of this course, when something behaves mysteriously, the first move is *always* `strace it and look at the wall*.

*(How does `strace` itself see all this? It uses the `ptrace` system call to make the kernel stop your process on every syscall and report it — a debugger's superpower, and itself a great example of "the kernel exposes its own boundary as a service." You'll meet `ptrace` again in Module 5.)*

---

## Try This

Ordered easy → hard. Hints included, but wrestle first.

1. **(Easy) Find the number.** Run `man 2 syscall` and then `strace ./mycmd hi`. Identify which syscall printed each line of output. Now run `strace -e trace=write ./mycmd hi` to filter to just writes. *Hint: everything you see on screen went through `write(1, ...)`.*

2. **(Easy) Prove the section matters.** Run `man 2 write` and `man 3 printf`. Read the "SYNOPSIS" of each. Note that `write` takes a raw fd and returns `ssize_t`, while `printf` takes a format string and returns an `int`. Write one sentence explaining which one is "closer to the kernel." *Hint: which section is which?*

3. **(Medium) Watch the exit code travel.** Run `./mycmd` (no args) then immediately `echo $?`. Then run `./mycmd x` then `echo $?`. Then chain them: `./mycmd && echo "SUCCESS"` vs `./mycmd x && echo "SUCCESS"`. Explain why `&&` behaves differently. *Hint: `&&` runs the right side only if the left side's exit code is 0.*

4. **(Medium) See the two writes yourself.** Modify `hello_syscall.c` to print five lines via `printf` and five via `write`, then `strace -e trace=write` it. Count the `write` syscalls. Are there ten? Fewer? *Hint: stdio buffering may coalesce printf output into fewer, bigger writes — a Module 3 preview. Try piping the output to a file (`./a.out > out.txt`) and re-strace; the count may change because buffering behaves differently when stdout isn't a terminal.*

5. **(Hard) Trace a real command.** Run `strace -c ls /` (the `-c` gives a summary table). Look at which syscalls `ls` makes and how many times. Find `openat`, `read`, `write`, `close`, `statx`. Sketch, in one paragraph, the story of what `ls` does at the syscall level: open the directory, read its entries, stat each one, write the formatted result. *Hint: you just reverse-engineered a Unix command purely from its wall-crossings. That skill never stops being useful.*

---

## Gotchas

- **"The OS runs my program."** No — the CPU runs your program directly, in ring 3. The OS only gets involved when you make a syscall, take an interrupt, or your time slice expires. Most of the time your code runs *on the bare CPU* with the kernel not even watching. This surprises people who imagine the OS "interpreting" their code like a JVM. It doesn't.

- **Confusing library calls with system calls.** `printf`, `malloc`, `fopen`, `strlen` are **library** functions (section 3). `write`, `read`, `mmap`, `open`, `fork` are **system calls** (section 2). Only syscalls cross into the kernel. `strlen` never touches the kernel at all. Mixing these up is the single most common beginner confusion, and a frequent interview probe: *"Does `printf` make a system call?"* Answer: not directly — it eventually calls `write`, but it may buffer and not call it every time.

- **Assuming a syscall is cheap like a function call.** A normal function call is a few nanoseconds. A syscall crosses the privilege wall — saving state, switching mode, running kernel code, switching back — costing tens to hundreds of nanoseconds, sometimes more. This is *why* buffering exists (batch many small logical writes into one syscall) and why high-performance servers obsess over syscall count (Module 10, epoll). "Just call `write` per byte" is a performance trap.

- **`man ls` vs `man 2 write` confusion.** If you `man open` you might get section 2, but if a shell builtin shadows a name you can get surprised. Always pass the section number for syscalls: `man 2 open`. Interviewers love asking "how do you find the exact semantics of the `read` system call?" — the answer is `man 2 read`, and knowing the section signals you actually work at this level.

- **Thinking exit code 0 means "false."** In the shell, **0 = success/true**, non-zero = failure. This is inverted from C/Java booleans and bites everyone once. `if ./mycmd; then ...` runs the `then` branch when `mycmd` returns **0**. Remember: "zero problems = success."

- **`argv[0]` amnesia.** In C, `argv[0]` is the program's own name and `argc` counts it. Java's `args` does neither. Off-by-one bugs when porting mental models between the two are common. Also: `argv[0]` is whatever the *caller* passed — a program can be lied to about its own name, which some tools exploit (e.g. `busybox` behaves differently based on `argv[0]`).

---

## Checkpoint

Answer from memory, then check below.

1. What are the *two* jobs an operating system does, in one phrase each?
2. Which CPU privilege ring does your Java app run in, and which does the Linux kernel run in? What is the *only* legal way to get from the first to the second?
3. When you call `printf("hi\n")`, name the section-3 function and the section-2 system call involved, and say which one actually crosses into the kernel.
4. Your program runs `./mycmd` with no arguments and returns 1. Where does that `1` go, and how would a shell script react to it with `&&`?
5. You suspect a program is slow because it makes too many system calls. Name the tool you'd reach for and the one flag that gives you a summary count per syscall.

---

<details>
<summary><b>Checkpoint Answers</b></summary>

1. **Resource manager** (arbitrates CPU, RAM, disk, devices among competing processes) and **abstraction layer** (hides hardware behind uniform interfaces like files and sockets).

2. Your Java app runs in **ring 3 (user space)**; the kernel runs in **ring 0 (kernel space)**. The only legal way to cross from ring 3 to ring 0 is a **system call** (the `syscall` instruction trapping into the kernel's fixed entry point). Any other attempt to enter ring 0 is a trap/fault that typically kills the process.

3. The section-3 library function is **`printf`**; underneath it eventually calls the section-2 system call **`write`**. Only **`write`** crosses into the kernel — `printf` just formats and buffers, then hands bytes to `write`. (And it may not call `write` on every `printf` because of buffering.)

4. The `1` becomes the process's **exit code**, delivered to the parent (the shell) and exposed as **`$?`**. With `&&`, since the exit code is non-zero (failure), the right-hand command **does not run** (`&&` runs the right side only when the left side exits 0).

5. **`strace`**, and the **`-c`** flag (`strace -c ./prog`) prints a summary table of syscall counts, time, and errors per syscall.

</details>

---

*Next up: **Module 1 — The User, the Shell, and the Filesystem.** We'll turn `mycmd` into a real installed command, and finally answer why `passwd` is allowed to edit a file you can't. Say **"next"** when you're ready.*
