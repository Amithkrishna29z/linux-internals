# Module 13 — Kernel Integration: Syscalls to Kernel Internals

> **Estimated time:** 3–4 hours · **Core path:** Concepts 1–3 (user vs kernel mode, the syscall trap mechanism, the vDSO) and the `rawsyscall` + `syscall_cost` programs are core. The kernel-side dispatch/`copy_*_user` details and the kernel-data-structures tour (Concepts 4–5) are core-but-deep — read for the mental model, they're the on-ramp to Modules 14–15.
>
> **Prerequisites:** Modules 0–12. You've *called* syscalls (`read`, `write`, `fork`, `mmap`) since Module 3 and *watched* them in `strace` throughout; now you learn what actually happens when one fires. You saw `[vdso]` in `/proc/self/maps` (Module 12) — this module explains it. User/kernel mode ties back to the privilege that made processes isolated (Module 5) and memory protected (Module 7).

---

## The Big Picture

For twelve modules you've stood at a boundary and passed messages across it without seeing the other side. Every `read`, `write`, `fork`, `socket`, `mmap` was a **system call** — a request to the kernel — and `strace` showed you them crossing, but the crossing itself was a black box. This module opens it. The single most important fact about a modern operating system is that the CPU runs in (at least) two **privilege modes**: **user mode**, where your program runs with restricted powers — it *cannot* touch hardware, other processes' memory, or the kernel's data directly — and **kernel mode**, where code has total control of the machine. This hardware-enforced split is *the* mechanism behind everything you've learned: process isolation (Module 5), memory protection (Module 7), the fact that a buggy program crashes itself and not the whole system. Your code lives in the restricted world; the kernel lives in the privileged one; and a system call is the *only* sanctioned doorway between them.

How does that doorway work? When you call `write`, you're really calling a tiny glibc wrapper that puts the **syscall number** (write = 1 on x86-64) in a register, the arguments in other registers, and executes a special CPU instruction — **`syscall`** — that does something no ordinary instruction can: it atomically switches the CPU to kernel mode and jumps to a fixed kernel entry point. This is a **trap**, a deliberate, controlled transfer of control into the kernel. The kernel's entry handler reads the syscall number, looks it up in the **syscall table** (an array mapping numbers to kernel function pointers — `write` → `sys_write`), calls that function, and when it returns, executes another instruction that switches back to user mode and resumes your program right after the `syscall`. That round trip — user → kernel → user — is the heartbeat of every program's interaction with the world, and it isn't free: switching modes, saving/restoring registers, and the kernel's own bookkeeping cost hundreds of nanoseconds, which is *why* buffering (Module 3), batching, and `epoll` (Module 10) matter so much — they're all strategies to make *fewer* of these crossings.

Some syscalls are so hot and so harmless that the kernel cheats to avoid the trap entirely. That `[vdso]` region you saw mapped into every process in Module 12 is the **virtual dynamic shared object** — a little bit of kernel code the kernel maps into *user* space, containing fast implementations of a few read-only syscalls like `clock_gettime` and `gettimeofday`. Because reading the clock doesn't need kernel privileges (it just reads a shared memory page the kernel updates), the vDSO lets you call it as an ordinary function — *no mode switch, no trap* — turning a ~hundreds-of-nanoseconds syscall into a handful of nanoseconds. We'll *measure* this: benchmark a real trapping syscall against a vDSO call against a plain function call, and see the user/kernel transition cost with your own eyes. Making that cost concrete is one of the most clarifying things in systems programming — suddenly every performance lesson in this course (buffer, batch, multiplex) has a number attached.

Finally, we go one step below and take a first, honest look at the kernel *as code*: the syscall table, the `copy_from_user`/`copy_to_user` functions that safely move data across the boundary (you can't just dereference a user pointer in the kernel — it might be malicious or invalid), the per-process `task_struct` the kernel keeps for every process (the real thing behind `/proc/[pid]/`), and how a raw syscall reaches its handler. This is not a kernel-development module — you won't write kernel code here — but it demystifies the thing on the other side of the boundary and sets up the finale: Module 14 (device drivers — kernel code that *is* the other side of an fd) and Module 15 (eBPF — running your own verified code *inside* the kernel). After thirteen modules of using the kernel, you finally see it.

---

## Concepts

### 1. User mode vs kernel mode: the hardware privilege split

**What it is:** The CPU runs code at one of several **privilege levels** (x86 calls them "rings"; practically, two matter): **user mode** (ring 3) and **kernel mode** (ring 0). In **user mode**, privileged instructions are forbidden and memory access is confined to the process's own mapped pages — attempts to touch hardware, other memory, or execute privileged instructions **fault** (trap to the kernel). In **kernel mode**, code may execute any instruction and access all memory and hardware. The mode is a hardware state the CPU enforces on every instruction.

```
   RING 3  USER MODE          your program, libraries
     • restricted: no direct hardware, no other process's memory
     • a forbidden action → CPU faults into the kernel
                    │  syscall / trap ▲  │ return
                    ▼                 │  ▼
   RING 0  KERNEL MODE         the Linux kernel, drivers
     • unrestricted: all instructions, all memory, all hardware
```

**Why it exists:** This split is the foundation of every OS guarantee. Because user code *physically cannot* touch hardware or other processes' memory (the CPU blocks it), the kernel can enforce **isolation** (Module 5 — one process can't corrupt another), **protection** (Module 7 — you can't read arbitrary RAM), and **stability** (a user-mode crash is contained; only kernel-mode bugs can take down the machine). Without hardware-enforced privilege levels, "operating system" as we know it — multiple mutually-distrusting programs sharing one machine safely — would be impossible. The mode bit is the root of all security.

**Java analogy:** The JVM adds a *software* privilege layer on top (bytecode verification, the old SecurityManager, module boundaries), but that's enforced by the JVM, not hardware — a JVM bug or JNI call escapes it. The hardware user/kernel split is *below* and *stronger* than anything Java does: even native code the JVM runs is still in user mode, still unable to touch the kernel except through syscalls. When a Java program "does I/O," the JVM's native code executes the same `syscall` instruction your C will — the ring boundary is universal, language-independent.

### 2. The syscall mechanism: trapping into the kernel

**What it is:** A system call is how user code *requests* a privileged operation. The sequence on x86-64:

```
   USER SIDE (glibc wrapper for write):
     mov rax, 1          ; syscall NUMBER (write = 1) into rax
     mov rdi, fd         ; arg1 → rdi
     mov rsi, buf        ; arg2 → rsi
     mov rdx, count      ; arg3 → rdx
     syscall             ; ← the magic instruction: trap to kernel mode
     ; ...execution resumes HERE after the kernel returns, result in rax

   KERNEL SIDE:
     entry_SYSCALL_64:   ; fixed entry point the `syscall` instr jumps to
       look up rax (=1) in the SYSCALL TABLE  → sys_write
       call sys_write(rdi, rsi, rdx)
       put return value in rax
       sysret             ; switch back to user mode, resume after `syscall`
```

The **`syscall` instruction** atomically switches to kernel mode and jumps to a fixed, kernel-controlled entry point (you can't jump *anywhere* in the kernel — only that one door). The kernel reads the syscall number from `rax`, indexes the **syscall table** (an array of function pointers), and calls the matching handler. On return, `sysret` restores user mode.

**Why it exists:** The kernel must let user code request privileged services *without* letting it run arbitrary privileged code. The trap mechanism threads that needle: user code can only enter the kernel at *one controlled address*, with the *operation selected by a number* the kernel validates — so the kernel fully controls what user code can ask for and how. The syscall table is the menu; the `syscall` instruction is the only way to place an order. This is why there are exactly ~350 syscalls, not arbitrary kernel calls — the interface is deliberately narrow and guarded.

**Java analogy:** You never see this in Java, but it's happening constantly beneath you: `FileOutputStream.write` → JVM native method → glibc `write` → the `syscall` instruction → `sys_write`. Every I/O, every `Thread.sleep`, every `new` that grows the heap eventually traps into the kernel this way. The `strace -f java YourApp` command reveals your Java program's syscall stream — the same `write`/`read`/`futex`/`mmap` calls, because under every abstraction the boundary is crossed identically.

### 3. The vDSO: syscalls without the trap

**What it is:** The **vDSO** (virtual dynamic shared object) is a small shared library the *kernel* maps into every process's address space (the `[vdso]` line in `/proc/self/maps`, Module 12). It contains user-mode implementations of a handful of **read-only, non-privileged** syscalls — chiefly `clock_gettime`, `gettimeofday`, `time`, and `getcpu` — that can run entirely in user mode by reading a shared page of data the kernel keeps updated. Calling them is a *plain function call*: **no `syscall` instruction, no mode switch, no trap.**

```
   normal syscall (e.g. getpid):
     your code → syscall instr → KERNEL → sysret → back    (~hundreds of ns)

   vDSO call (e.g. clock_gettime):
     your code → call vdso_clock_gettime → reads a kernel-updated page → return
                 (all in USER mode, ~few ns — NO trap)
```

**Why it exists:** Some syscalls are called *enormously* often (reading the clock — every log line, every timeout, every benchmark) and need *no* kernel privilege (they only read data the kernel already exposes). Paying the full trap cost hundreds of times per millisecond for such calls is pure waste. The vDSO eliminates it: the kernel publishes the clock (and a few other read-only facts) in a shared memory page it updates, and maps read-only code into user space to read it — so the "syscall" becomes an ordinary function call. It's a targeted optimization for the hottest, safest syscalls, and it's why `clock_gettime` is ~100× faster than a trapping syscall.

**Java analogy:** `System.nanoTime()` / `System.currentTimeMillis()` are backed by `clock_gettime`/`gettimeofday`, so on Linux they ride the vDSO — which is *why* they're cheap enough to sprinkle through hot code (a full syscall per `nanoTime()` would wreck tight-loop profiling). When people say Java's time calls are "fast," the vDSO is a big part of the reason. You've been benefiting from this module's optimization for years without knowing it had a name.

### 4. Inside the boundary: dispatch, `copy_*_user`, and the return path

**What it is:** What the kernel actually does between trap and return. After `entry_SYSCALL_64` and the table lookup, the handler (`sys_write`, etc.) runs — but it must handle user-supplied pointers *carefully*. It **cannot** simply dereference a pointer the user passed (`buf` in `write`): that address might be invalid, unmapped, or point at kernel memory the user is trying to trick the kernel into leaking. So the kernel uses **`copy_from_user(dst, user_src, n)`** and **`copy_to_user(user_dst, src, n)`** — special functions that safely move data across the boundary, validating that the user address really belongs to the calling process and handling faults gracefully.

```
   sys_write(fd, user_buf, count):
     • validate fd (is it open? writable?)
     • copy_from_user(kernel_buf, user_buf, count)   ← SAFE copy, checks the pointer
         (if user_buf is bogus → returns -EFAULT, not a kernel crash)
     • do the actual write (to the file/pipe/socket behind fd)
     • return bytes written (or -errno) in rax
   → sysret: kernel→user; glibc wrapper turns a negative return into errno + -1
```

**Why it exists:** The boundary is a *trust boundary* — everything from user space is potentially hostile or buggy. `copy_from_user`/`copy_to_user` are the disciplined chokepoints for crossing it: they prevent the classic attacks (passing a kernel address to read/write privileged memory) and the classic crashes (dereferencing a wild user pointer would panic the kernel if done naively). This careful validation is *why* syscalls have overhead beyond the mode switch, and it's the reason kernel code is written so defensively. Every byte crossing the boundary is checked.

**Java analogy:** There's no direct analogue because Java never crosses this boundary itself — but the *principle* (never trust data from a less-privileged domain; validate at the chokepoint) is exactly input validation at a security boundary: sanitizing untrusted user input before your server acts on it, or a deserialization allowlist. `copy_from_user` is the kernel's "never trust the client" — the same instinct you apply to HTTP request bodies, applied to syscall arguments.

### 5. The kernel as code: the syscall table, `task_struct`, and raw syscalls

**What it is:** A first concrete look at kernel internals you can now name:
- **The syscall table** (`sys_call_table`) — an array indexed by syscall number holding function pointers to handlers. Number 1 → `sys_write`, 0 → `sys_read`, 57 → `sys_fork`, etc. (`/usr/include/asm/unistd_64.h` lists the numbers.)
- **`task_struct`** — the kernel's per-process structure: PID, state, memory map (`mm_struct`), open files (`files_struct`), parent pointer, scheduling info. *This is the real thing* behind every `/proc/[pid]/` file (Module 12) and behind `fork` (Module 5 duplicates it) — the kernel's ground-truth record of a process.
- **Raw syscalls** — you can bypass the glibc wrapper with `syscall(number, args...)`, invoking a syscall that may have no wrapper (new/obscure ones) or to see the mechanism nakedly.

```
   glibc wrapper:   write(fd, buf, n)   → convenient, sets errno
   raw:             syscall(SYS_write, fd, buf, n)   → same trap, no wrapper

   the number SYS_write (=1) indexes sys_call_table → sys_write → operates on
   the task_struct's files_struct to find fd's backing object, then writes it.
```

**Why it exists:** The syscall table is the kernel's dispatch mechanism (Concept 2's "menu"); `task_struct` is how the kernel *represents* the abstractions you've used all course (a process is a `task_struct`; an open fd is an index into its `files_struct`; a memory segment is a VMA in its `mm_struct`). Raw syscalls exist for the gap where the kernel has added a syscall glibc hasn't wrapped yet, and as a teaching window onto the bare mechanism. Naming these makes the kernel concrete: it's not magic, it's data structures and function pointers, and you now know the main ones.

**Java analogy:** `task_struct` is the OS-level analogue of the JVM's internal `Thread`/`Klass` bookkeeping objects — the runtime's private record of each managed entity. `syscall(number, ...)` has a loose cousin in reflection or `MethodHandle`s (invoking by identifier rather than a compiled call site), and a closer one in the JVM's own use of raw syscalls via JNI when no higher-level API exists. The lesson mirrors Module 12's: the tidy abstractions (a "process," a "thread") are, one level down, concrete structures the runtime maintains.

---

## Code

### Program 1 — `rawsyscall.c`: make a syscall with no glibc wrapper

```c
/* rawsyscall.c
 *
 * Invokes syscalls THREE ways to make the mechanism visible: the normal glibc
 * wrapper write(), the raw syscall(SYS_write, ...) that skips the wrapper but
 * makes the same trap, and getpid via both the wrapper and raw. All of them
 * end at the SAME `syscall` instruction and the SAME kernel handler.
 *
 * Compile:  gcc -Wall -Wextra -o rawsyscall rawsyscall.c
 * Run:      ./rawsyscall
 *   Inspect:  strace ./rawsyscall   (see identical write/getpid syscalls)
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <unistd.h>
#include <sys/syscall.h>   /* SYS_write, SYS_getpid -- the syscall NUMBERS */
#include <string.h>

int main(void)
{
    const char *msg = "hello via ";

    /* 1) The normal way: glibc's write() wrapper. */
    write(STDOUT_FILENO, msg, strlen(msg));
    write(STDOUT_FILENO, "glibc write()\n", 14);

    /* 2) The raw way: syscall(SYS_write, ...). SAME trap, SAME sys_write,
     *    just without glibc's wrapper (and it does NOT set errno for you). */
    const char *raw = "hello via ";
    syscall(SYS_write, STDOUT_FILENO, raw, strlen(raw));
    syscall(SYS_write, STDOUT_FILENO, "raw syscall()\n", 14);

    /* 3) getpid two ways -- prove they agree (same kernel handler). */
    pid_t a = getpid();                     /* glibc wrapper */
    pid_t b = (pid_t)syscall(SYS_getpid);   /* raw trap */
    printf("getpid(): wrapper=%d  raw=%d  %s\n",
           a, b, a == b ? "(identical)" : "(MISMATCH?!)");

    /* Show the syscall NUMBERS -- these index the kernel's sys_call_table. */
    printf("SYS_write = %d, SYS_getpid = %d (indices into sys_call_table)\n",
           SYS_write, SYS_getpid);
    return 0;
}
```

**Expected output:**
```
$ ./rawsyscall
hello via glibc write()
hello via raw syscall()
getpid(): wrapper=20933  raw=20933  (identical)
SYS_write = 1, SYS_getpid = 39 (indices into sys_call_table)
```

**Walkthrough of the non-obvious parts:**
- **`write()` and `syscall(SYS_write, ...)` produce identical `strace` output** — because they *are* the same syscall. The glibc `write` wrapper does almost nothing except put `1` in `rax`, the args in registers, execute `syscall`, and translate a negative return into `errno`. `syscall(SYS_write, ...)` skips the wrapper but hits the exact same instruction and kernel handler. Run `strace ./rawsyscall` and you can't tell which write is which.
- **The syscall *number* is the whole interface** — `SYS_write` is `1`, and that `1` is what the kernel looks up in `sys_call_table` to find `sys_write`. The number, not a symbol or address, selects the operation; that's why it's a stable ABI (write is 1 forever on x86-64).
- **`syscall()` doesn't set `errno`** the way wrappers do — on error it returns the negative error code directly (or -1 with you responsible for reading it), which is why raw syscalls are less convenient. The wrapper's job is exactly this ergonomics layer.
- **getpid wrapper == raw** confirms there's no hidden difference: both reach `sys_getpid`, which reads the current `task_struct`'s PID. The glibc convenience is just packaging around an identical trap.
- `SYS_getpid = 39` (not sequential with write=1) reflects the historical order syscalls were added — the numbers are an ABI, frozen once assigned.

### Program 2 (Project) — `syscall_cost.c`: measure the user/kernel crossing

```c
/* syscall_cost.c
 *
 * Makes the cost of crossing the user/kernel boundary CONCRETE by timing:
 *   (a) a plain function call        -- no boundary
 *   (b) a vDSO call (clock_gettime)  -- runs in user mode, NO trap
 *   (c) a real trapping syscall (getpid via raw syscall, uncached) -- full trap
 * You should see (a) < (b) << (c): the trap is ~50-150x a function call.
 *
 * Compile:  gcc -Wall -Wextra -O2 -o syscall_cost syscall_cost.c
 * Run:      ./syscall_cost
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <sys/syscall.h>

#define N 2000000

/* A trivial function the compiler can't optimize away (volatile sink). */
static volatile long sink = 0;
static long plain_call(long x) { return x + 1; }

/* Return nanoseconds elapsed for a loop, using clock_gettime (vDSO) to time. */
static double time_loop(const char *label, int which)
{
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    for (long i = 0; i < N; i++) {
        switch (which) {
        case 0: sink += plain_call(i);                        break; /* function */
        case 1: { struct timespec ts;
                  clock_gettime(CLOCK_MONOTONIC, &ts);              /* vDSO */
                  sink += ts.tv_nsec; }                       break;
        case 2: sink += syscall(SYS_getpid);                   break; /* real trap */
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ns = (t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec);
    double per = ns / N;
    printf("%-28s %8.2f ns/call\n", label, per);
    return per;
}

int main(void)
{
    printf("timing %d iterations each:\n", N);
    double f = time_loop("(a) plain function call", 0);
    double v = time_loop("(b) vDSO clock_gettime", 1);
    double s = time_loop("(c) real syscall getpid", 2);

    printf("\nratios:  vDSO/func = %.1fx   syscall/func = %.1fx   syscall/vDSO = %.1fx\n",
           v / f, s / f, s / v);
    printf("=> the user/kernel TRAP is the expensive part; the vDSO avoids it.\n");
    return 0;
}
```

**Expected output (numbers vary by CPU; the *ratios* are the lesson):**
```
$ ./syscall_cost
timing 2000000 iterations each:
(a) plain function call          1.10 ns/call
(b) vDSO clock_gettime          18.40 ns/call
(c) real syscall getpid        320.75 ns/call

ratios:  vDSO/func = 16.7x   syscall/func = 291.6x   syscall/vDSO = 17.4x
=> the user/kernel TRAP is the expensive part; the vDSO avoids it.
```

**Walkthrough of the non-obvious parts:**
- **The three tiers make the boundary's cost visceral.** A plain function call is ~1 ns (no boundary). `clock_gettime` via the vDSO is ~15–20 ns — more than a function call (it reads a shared page, does some math) but still **in user mode, no trap**. A real trapping syscall (`getpid`) is ~200–400 ns — *hundreds* of times a function call. That gap *is* the user/kernel mode switch, register save/restore, and kernel entry/exit overhead.
- **We use raw `syscall(SYS_getpid)` for the trap, not glibc `getpid()`** — because modern glibc *caches* the PID after the first call and returns it without trapping (an optimization!), which would hide the very cost we're measuring. The raw syscall forces a real trap every iteration. (This caching is itself a great example of "avoid the crossing.")
- **`clock_gettime` is doing double duty** — it's both the thing we *measure* in case (b) and the *timer* for all three loops (in `time_loop`). That's fine and even fitting: the vDSO makes it cheap enough to call as a timing primitive without the measurement overhead dominating.
- **This one benchmark retroactively justifies half the course.** Buffering (Module 3: one `write` of 4 KB beats 4096 one-byte writes → 4095 fewer traps), `epoll` over per-fd polling (Module 10: one `epoll_wait` beats N `read` attempts), stdio over raw syscalls — every "do fewer syscalls" lesson now has a *number*: each avoided crossing saves ~300 ns. At millions of ops/sec, that's the whole performance budget.
- **`-O2` matters here** — without optimization the loop overhead swamps the signal; with it, the compiler tightens the loop so the syscall/vDSO/function cost dominates. (The `volatile sink` stops the optimizer from deleting the "useless" work entirely.)

---

## Under the Hood

Two lenses: `strace` shows the syscalls that *trap*, and `ltrace`/disassembly show what the vDSO does *without* trapping. Run `strace ./syscall_cost`:

```
clock_gettime(CLOCK_MONOTONIC, {tv_sec=..., tv_nsec=...}) = 0    ← [1] appears ONCE (or never!)
...
# the 2,000,000 clock_gettime calls in loop (b) DO NOT APPEAR in strace  ← [2]
...
getpid()                                                         ← [3] but these...
getpid()                                                         ←     ...DO appear
getpid()                                                         ←     (2,000,000 of them)
```

And disassemble to see the `syscall` instruction itself (`objdump -d` on a tiny static binary, or `gdb`):

```
   # glibc getpid wrapper (roughly):
   mov    $0x27, %eax        ; 0x27 = 39 = SYS_getpid into eax
   syscall                   ; ← THE trap instruction (user → kernel)
   ret
```

Annotated:
1. **`clock_gettime` in strace appears only for the timing calls that happen *before* the loop measurement stabilizes** — or, on a vDSO-enabled system, the ones inside the tight loop **don't appear at all**.
2. **The 2,000,000 loop `clock_gettime` calls are invisible to `strace`** — this is the vDSO caught red-handed. `strace` works by intercepting *traps into the kernel* (via `ptrace`); a vDSO call never traps, so `strace` literally cannot see it. **"It's a syscall that strace can't trace" is the definition of a vDSO call.** That absence is the proof it ran entirely in user mode.
3. **`getpid()` (raw) *does* show up 2,000,000 times** — because it genuinely traps every iteration. The contrast in the same strace — invisible `clock_gettime`, visible `getpid` — is the vDSO's value made observable: same "syscall" API, but one crosses the boundary and one doesn't.
4. **The `syscall` instruction in the disassembly** is the actual door: `mov $number, %eax; syscall`. Every trapping syscall in the entire course bottoms out in those two instructions. Above them is glibc's wrapper; below them is `entry_SYSCALL_64`, the table lookup, and the handler.

The headline: **a trapping syscall is `mov number→rax; syscall` → kernel entry → table lookup → handler → `sysret`, and `strace` sees it because it traps; a vDSO call is a plain function reading a kernel-maintained page, so it never traps and `strace` can't see it — which is exactly why it's ~100× cheaper.** You are now looking at both sides of the boundary you've used since Module 3.

---

## Try This

Ordered easy → hard.

1. **(Easy) Prove the wrapper and raw syscall are identical.** `strace ./rawsyscall` and confirm the two `write`s (glibc and raw) and the two `getpid`s look the same in the trace. Explain why. *Hint: both compile to `mov number→rax; syscall` — the wrapper is a thin shell over the same instruction.*

2. **(Easy) Catch the vDSO in the act.** `strace ./syscall_cost 2>&1 | grep -c clock_gettime` vs `grep -c getpid`. The `getpid` count is ~2,000,000; `clock_gettime` is tiny. Explain what that count difference proves about where each call ran. *Hint: strace only sees traps; the missing clock_gettimes ran in user mode via the vDSO.*

3. **(Medium) Measure buffering as saved syscalls.** Write a program that writes 100,000 bytes one byte at a time with raw `write` (100,000 traps), time it; then with a 64 KB buffer flushed once (2 traps), time it. Relate the speedup to Module 3 and to this module's ~300 ns/trap number. *Hint: 100,000 traps × ~300 ns ≈ 30 ms of pure boundary-crossing you eliminated.*

4. **(Medium) Find and read the syscall table numbers.** `cat /usr/include/asm/unistd_64.h` (or `ausyscall --dump`) and find the numbers for `read`, `write`, `openat`, `mmap`, `clone`, `epoll_wait`. Cross-check a few against `strace` output. Explain what the number *is* to the kernel. *Hint: it's the index into `sys_call_table` — the number literally selects the function pointer.*

5. **(Hard) Call a syscall with no glibc wrapper.** Pick a syscall glibc doesn't wrap (historically `gettid` before glibc 2.30, or `getrandom` on very old glibc, or `pidfd_open`) and invoke it via `syscall(SYS_xxx, ...)`. Verify it works and explain why raw `syscall()` is the only way to reach it. *Hint: when the kernel adds a syscall faster than glibc adds a wrapper, `syscall(number, ...)` is the escape hatch — the number exists even when the C function doesn't.*

---

## Gotchas

- **`syscall()` doesn't set `errno` like wrappers do.** A raw `syscall()` returns the negative error code (e.g. `-EBADF`) as its result on some paths, and the `errno` convention is glibc's wrapper behavior. When using `syscall()` directly, check the return value's sign yourself and don't assume `errno` was set the same way. This is a real portability/correctness trap in raw-syscall code.

- **glibc caches some syscalls (so they don't trap).** `getpid()` (post-glibc-2.25) and others may return a cached value without trapping — great for performance, but it means "call `getpid()` in a loop to benchmark syscall cost" measures *nothing*. Use raw `syscall(SYS_getpid)` to force a real trap when you actually want to measure or observe the crossing.

- **You can't `strace` a vDSO call — that's not a bug.** If a "syscall" doesn't appear in `strace`, it may be a vDSO call (`clock_gettime`, `gettimeofday`, `time`, `getcpu`) that never trapped. Don't conclude "it wasn't called"; it ran in user mode. To see time-related calls trap, you'd have to defeat the vDSO (rarely wanted).

- **Syscall numbers are architecture-specific.** `SYS_write` is 1 on x86-64 but a *different* number on ARM64, x86-32, etc. Never hard-code the integer; use the `SYS_*` / `__NR_*` constants from `<sys/syscall.h>`, which resolve to the right number per arch. Code with literal syscall numbers breaks when ported.

- **Never assume a user pointer is valid — in kernel/driver code.** (Relevant as you head into Modules 14–15.) Kernel code must use `copy_from_user`/`copy_to_user`, never bare dereferences of user pointers — a raw dereference of a bad/hostile user address is a security hole or kernel panic. This is the #1 rule of the boundary and the source of many CVEs when violated.

- **Minimizing syscalls is a real optimization, not premature.** Because each trap is ~hundreds of ns, "reduce the number of syscalls" (buffer, batch, use `writev`/`sendmmsg`, `epoll` instead of per-fd polling, `io_uring` to batch) is a first-order performance lever in I/O-heavy code — not micro-optimization. Profile syscall counts (`strace -c`) when a program is slow; a surprising count is often the cause.

- **The mode switch cost is fixed overhead, independent of the work.** A syscall that does almost nothing (`getpid`) still costs the full trap; a syscall that does a lot (`read` of 1 MB) amortizes the trap over real work. This is *why* batching helps: you're spreading one fixed ~300 ns crossing over more useful bytes/operations. Tiny frequent syscalls are the worst case.

---

## Checkpoint

1. What are user mode and kernel mode, who enforces the distinction, and why is this split the foundation of process isolation and memory protection (Modules 5 and 7)?
2. Walk through what happens, step by step, when your program calls `write` — from the glibc wrapper to the kernel handler and back. What role do the syscall number, the `syscall` instruction, and the syscall table play?
3. What is the vDSO, which kinds of syscalls does it accelerate and why *those*, and why can't `strace` see a vDSO call? Roughly how much cheaper is it than a trapping syscall?
4. Why can't kernel code simply dereference a pointer passed from user space? What does it use instead, and what two categories of disaster does that prevent?
5. Why is "make fewer syscalls" a genuine performance strategy? Connect it to buffering (Module 3), `epoll` (Module 10), and the measured cost of a trap. What fixed cost does every syscall pay regardless of how much work it does?

---

<details>
<summary><b>Checkpoint Answers</b></summary>

1. **User mode** (ring 3) is the restricted CPU privilege level your programs run in: they cannot execute privileged instructions or access hardware/other processes' memory/kernel memory — such attempts fault. **Kernel mode** (ring 0) is unrestricted: the kernel may run any instruction and touch all memory and hardware. The **CPU (hardware)** enforces the distinction on every instruction. This split is the foundation of isolation and protection because user code *physically cannot* reach outside its own mapped memory or touch hardware — so the kernel can guarantee one process can't corrupt another (Module 5) or read arbitrary RAM (Module 7), and a user-mode crash is contained. The privilege bit is the hardware root of every OS security guarantee.

2. `write(fd, buf, n)` calls glibc's wrapper, which puts the **syscall number** (write = 1) in `rax` and the arguments in `rdi`/`rsi`/`rdx`, then executes the **`syscall` instruction** — which atomically switches the CPU to kernel mode and jumps to the fixed kernel entry point (`entry_SYSCALL_64`). The kernel reads `rax`, indexes the **syscall table** (an array of function pointers) at 1 to find `sys_write`, and calls it; `sys_write` validates the fd, safely copies the data with `copy_from_user`, performs the write to the object behind the fd, and puts the result in `rax`. Then `sysret` switches back to user mode and resumes right after the `syscall` instruction; the glibc wrapper converts a negative return into `errno` + -1. The number selects the operation, the instruction is the controlled door, the table is the dispatch.

3. The **vDSO** is a small shared library the kernel maps into every process's address space containing user-mode implementations of a few **read-only, non-privileged** syscalls — `clock_gettime`, `gettimeofday`, `time`, `getcpu`. It accelerates *those* because they're called extremely often and need no kernel privilege (they only read data — like the current time — that the kernel already publishes in a shared page), so they can run entirely in user mode as a plain function call with **no trap**. `strace` can't see a vDSO call because `strace` intercepts *traps into the kernel* (via `ptrace`), and a vDSO call never traps — it stays in user mode. It's roughly **~100×** cheaper (single-digit-to-tens of ns vs hundreds of ns for a trapping syscall).

4. Because a pointer from user space is **untrusted** — it might be invalid/unmapped, or deliberately point at kernel memory to trick the kernel into reading or writing privileged data. A naive dereference would either **panic the kernel** (wild/unmapped address) or **leak/corrupt kernel memory** (a security hole). So kernel code uses **`copy_from_user`/`copy_to_user`**, which validate that the address genuinely belongs to the calling process and handle faults gracefully (returning `-EFAULT` rather than crashing). This disciplined chokepoint prevents both the crash category and the privilege-escalation/info-leak category — it's the kernel's "never trust the client."

5. Every syscall pays a **fixed cost** — the user↔kernel mode switch, register save/restore, and kernel entry/exit bookkeeping (~hundreds of ns) — *regardless* of how much work it does. So doing the same work in **fewer** syscalls avoids paying that fixed cost repeatedly: buffering (Module 3) turns 4096 one-byte `write`s (4096 traps) into one 4 KB `write` (1 trap); `epoll` (Module 10) replaces polling each of N fds with one `epoll_wait` that returns only the ready ones; stdio, `writev`, and `io_uring` batch similarly. With each trap ~300 ns, eliminating a million of them saves ~300 ms of pure overhead — a first-order performance lever in I/O-heavy code, not micro-optimization. Tiny, frequent syscalls are the worst case because the fixed trap cost isn't amortized over any real work.

</details>

---

*Next up: **Module 14 — Device Drivers, Controllers, and Hardware I/O.** We cross fully to the other side: a device driver is kernel code that *is* the thing behind an fd. How `read`/`write`/`ioctl` on `/dev/something` dispatch into driver functions, character vs block devices, the `file_operations` table, interrupts and DMA, and (conceptually) writing a minimal kernel module. The `read` you've called since Module 3 finally meets the code that answers it. Continuing straight on.*
