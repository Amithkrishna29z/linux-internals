# POSIX & Linux Systems Programming — From First Principles

A complete, self-contained systems programming course built for a Java/Spring Boot
backend developer learning Linux/POSIX from the ground up. Every concept is anchored
to a Java equivalent where one exists — and when there isn't one, that's called out
explicitly, because those are usually the concepts that matter most.

- **Written explanations:** [`content/`](content/) — one markdown file per module.
- **Runnable C code:** [`examples/`](examples/) — every program compiles clean with `-Wall -Wextra`.
- **Build everything:** `make` at the repo root.

The spine of the whole course is the **file descriptor**. Files, pipes, sockets,
epoll, devices, and `/proc` are all the same idea wearing different clothes. Watch
for the `fd` thread in every module.

---

## How to study

Each module file follows the same shape:

1. **The Big Picture** — where this fits and why it exists.
2. **Concepts** — what / why / the Java analogy / ASCII diagrams.
3. **Code** — complete, compilable C programs with compile commands, expected output, and walkthroughs.
4. **Under the Hood** — what the kernel actually does (with an annotated `strace`).
5. **Try This** — hands-on exercises, easy → hard.
6. **Gotchas** — bugs, misconceptions, interview traps.
7. **Checkpoint** — 5 self-test questions with answers.

Work through modules in order. Compile and run *everything* — you learn this subject
with your hands, not your eyes. `strace` the examples and watch them cross the
user/kernel boundary.

### Two paths
- **Core 20-hour path** — the fastest route to competence (Josh Kaufman's "20 hours"
  rule). Follow the sections each module flags as **core**.
- **Full 40–50 hour path** — core plus every deep-dive section. Do this if you want
  to write drivers and eBPF, not just use the syscalls.

---

## Module index

| # | Module | Status | Est. time |
|---|--------|--------|-----------|
| 0 | [The Mental Model](content/00-the-mental-model.md) | ✅ Done | 3–4 h |
| 1 | [The User, the Shell, and the Filesystem](content/01-the-user-the-shell-the-filesystem.md) | ✅ Done | 3–4 h |
| 2 | [C for Systems Programming](content/02-c-for-systems-programming.md) | ✅ Done | 3–4 h |
| 3 | [File I/O: open, read, write, close](content/03-file-io.md) | ✅ Done | 3–4 h |
| 4 | [Console I/O and the Terminal](content/04-console-io-and-the-terminal.md) | ✅ Done | 2–3 h |
| 5 | Processes (fork, exec, wait, signals) | ⏳ Planned | 4–5 h |
| 6 | Threads | ⏳ Planned | 3–4 h |
| 7 | Memory Management | ⏳ Planned | 4–5 h |
| 8 | Interprocess Communication (IPC) | ⏳ Planned | 3–4 h |
| 9 | Network I/O and Socket Programming | ⏳ Planned | 3–4 h |
| 10 | I/O Models and Asynchronous I/O | ⏳ Planned | 3–4 h |
| 11 | ioctl and the Many Kinds of I/O | ⏳ Planned | 2 h |
| 12 | The /proc and /sys Filesystems | ⏳ Planned | 2–3 h |
| 13 | Kernel Integration: Syscalls to Kernel Internals | ⏳ Planned | 3–4 h |
| 14 | Device Drivers, Controllers, and Hardware I/O | ⏳ Planned | 4–5 h |
| 15 | Berkeley Packet Filter (BPF) and eBPF | ⏳ Planned | 3 h |
| 16 | Capstone (container runtime / HTTP server / KV store) | ⏳ Planned | 6–8 h |

---

## Building the examples

```sh
make            # build every example in the course
make clean      # remove all built binaries
```

Individual modules build on their own too:

```sh
cd examples/00-mental-model
gcc -Wall -Wextra -o hello_syscall hello_syscall.c
./hello_syscall
strace -e trace=write ./hello_syscall
```

> **Note:** The C code targets **Linux** (glibc, POSIX syscalls). On Windows use WSL2;
> on macOS a Linux VM/container — some syscalls (`epoll`, `/proc`, kernel modules) are
> Linux-only by design, and that's part of the lesson.

---

## Requirements

- A Linux environment (native, WSL2, or a VM)
- `gcc`, `make`, `strace`, `ltrace`, `gdb`, `objdump` — on Debian/Ubuntu:
  ```sh
  sudo apt install build-essential strace ltrace gdb binutils
  ```
- Later modules add: `linux-headers-$(uname -r)` (Module 13/14), `bpftrace` (Module 15).
