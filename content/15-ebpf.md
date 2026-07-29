# Module 15 — Berkeley Packet Filter (BPF) and eBPF

> **Estimated time:** ~3 hours · **Core path:** Concepts 1–3 (what eBPF is, the verifier/safety, hooks and attach points) and the two `bpftrace` programs are core. Maps (Concept 4) and the toolchain/use-case survey (Concept 5) are core-but-lighter; writing raw `libbpf`/C is beyond scope (we use `bpftrace`, the accessible on-ramp).
>
> **Prerequisites:** Modules 0–14. This is the direct counterpoint to Module 14 — the *safe* way to run code in the kernel, motivated by exactly the panic risk you just learned to fear. You need syscalls and the user/kernel boundary (Module 13), the driver/`file_operations` idea of kernel hooks (Module 14), and the "one process, thousands of events" mindset (Module 10). Running the examples wants a modern Linux kernel (5.x+) and `bpftrace`.

---

## The Big Picture

Module 14 gave you the power to run code in the kernel — and the terror that comes with it: one bad pointer and the whole machine panics, one leaked allocation and you reboot, no safety net anywhere. For decades that was the deal: kernel-level insight and control meant kernel-module risk. **eBPF** dissolves that tradeoff, and it's the most important thing to happen to Linux in twenty years. The idea sounds impossible at first: let *unprivileged-ish* userspace load small programs that run **inside the kernel**, at hooks all over it — on every syscall, every network packet, every function entry — but make it so those programs *provably cannot* crash, hang, or corrupt the kernel. The trick is a **verifier**: before the kernel will run your eBPF program, it mathematically analyzes it and *rejects* anything that could misbehave — no unbounded loops (so it can't hang), no arbitrary memory access (so it can't corrupt), no uninitialized reads, bounded stack. If it doesn't pass, it doesn't load. You get kernel-level power with a seatbelt the hardware-privileged world of Module 14 never had.

What began as the humble **Berkeley Packet Filter** — a tiny virtual machine `tcpdump` used to decide which packets to capture, in the kernel, without copying every packet to userspace — was generalized around 2014 into **extended BPF (eBPF)**: a general-purpose, verified, in-kernel execution engine that can attach to almost anything. Today eBPF is the engine behind a stunning range of production systems. **Observability:** `bpftrace`, `bcc`, and tools like `opensnoop`/`execsnoop` let you ask arbitrary questions about a running kernel ("which processes are opening which files, right now?") with near-zero overhead and no reboot — the modern replacement for the printf-debugging and `strace`-everything approaches you've used all course. **Networking:** Cilium, Katran, and XDP (eXpress Data Path) process packets at the earliest possible point — in the driver, before the kernel even builds a socket buffer — powering Kubernetes networking and DDoS mitigation at millions of packets per second. **Security:** eBPF LSM hooks and tools like Falco and Tetragon enforce policy and detect intrusions by observing kernel events in real time. In every case, eBPF is doing what previously required a risky custom kernel module — but safely, dynamically loadable, and removable without a reboot.

The mechanics are elegant. You write a small program (in a restricted C, or a `bpftrace` one-liner), it's compiled to **eBPF bytecode**, the kernel **verifies** it, then a **JIT** compiles it to native machine code so it runs at full speed. It attaches to a **hook** — a **kprobe** (any kernel function entry), a **tracepoint** (a stable, curated kernel event), an **XDP** hook (raw packets in the driver), a **socket filter**, an **LSM** hook (security decisions). When that event fires, your program runs, examines the context (the syscall arguments, the packet, the function's registers), and does something: record data, drop a packet, allow/deny an action. To *keep* state across invocations and to hand results back to userspace, eBPF programs use **maps** — kernel-resident key/value data structures (hash maps, arrays, ring buffers, histograms) that both the eBPF program and a userspace controller can read and write. A tracing tool is, in essence: attach a program to a hook, have it tally events into a map, and have userspace periodically read the map and print. That's `bpftrace` in three sentences.

This module is a shorter, capstone-adjacent one: you won't write raw eBPF C (that needs `clang`, `libbpf`, and a chapter of boilerplate), but you'll use **`bpftrace`** — a high-level tracing language that compiles to eBPF — to write real, useful kernel programs in a line or two: count which programs are being executed system-wide, trace every file open with the process and filename, build a latency histogram. In doing so you'll directly *observe* concepts from earlier modules — watch `execve` fire for every command (Module 5), see `openat` with real paths (Module 3), measure syscall latency (Module 13) — using the very mechanism (verified in-kernel programs) that represents the state of the art. It's a fitting penultimate stop: the kernel, which you've spent fourteen modules learning to *use* and one module learning to *extend dangerously*, is now something you can extend **safely and programmably** — before Module 16 asks you to build something real with everything you know.

---

## Concepts

### 1. What eBPF is: verified programs running inside the kernel

**What it is:** **eBPF** is a mechanism to load small programs from userspace into the **kernel**, where they run — safely — at defined hook points. You write the program, it compiles to eBPF **bytecode** (a small RISC-like instruction set), the kernel **verifies** it's safe, a **JIT** turns it into native code, and it attaches to an event; when the event fires, the program runs in kernel context with access to that event's data.

```
   userspace:  write program → compile to eBPF bytecode
                                     │  load (bpf syscall)
                                     ▼
   kernel:     VERIFIER checks it's safe ──reject──▶ (won't load)
                     │ accept
                     ▼
               JIT → native code, ATTACHED to a hook (syscall/packet/kprobe)
                     │  event fires
                     ▼
               your code runs IN-KERNEL, reads event data, updates a MAP
```

**Why it exists:** It resolves the central dilemma of Module 14: kernel-level power vs kernel-level danger. Before eBPF, to observe or influence kernel behavior you wrote a kernel module — full privilege, zero safety, panic-on-bug. eBPF lets you run custom logic *in the kernel* with a *guarantee of safety* (Concept 2), dynamically loaded and unloaded without rebooting. That combination — programmable, safe, dynamic, fast — is why it displaced kernel modules for whole categories (tracing, packet processing, security) and why it's considered revolutionary.

**Java analogy:** The closest analogy is the **JVM itself**: eBPF bytecode running on an in-kernel virtual machine, verified before execution, JIT-compiled to native code, is *strikingly* like Java bytecode verified by the JVM's bytecode verifier and JIT-compiled by HotSpot. The eBPF verifier is the kernel's bytecode verifier; the eBPF JIT is HotSpot. If you understand *why* Java can safely run untrusted bytecode (verification + a restricted instruction set + a managed VM), you already understand *why* the kernel can safely run eBPF. It's the JVM's safety model brought inside the Linux kernel.

### 2. The verifier: why eBPF can't crash the kernel

**What it is:** The **verifier** is the kernel component that statically analyzes an eBPF program *before* loading it and rejects anything unsafe. It enforces, among other rules:
- **No unbounded loops** — every loop must provably terminate (historically loops were banned entirely; modern kernels allow bounded ones), so a program *cannot hang the kernel*.
- **No arbitrary memory access** — the program may only touch its own stack, its maps, and the specific context passed to it, with every pointer access bounds-checked, so it *cannot corrupt kernel memory* or read secrets.
- **No uninitialized reads, bounded stack, limited instruction count** — the program is finite, deterministic, and analyzable.

```
   your eBPF program  ──▶  VERIFIER: simulate ALL paths, prove:
                             • terminates (bounded)         → can't hang
                             • every memory access in-bounds → can't corrupt
                             • no uninitialized data         → deterministic
                           ┌─────────────┴─────────────┐
                         PASS: JIT + load           FAIL: rejected, error msg
```

**Why it exists:** This is the whole reason eBPF is safe to expose more broadly than kernel-module loading. By *proving* (not merely hoping) that a program halts and stays in bounds, the verifier removes the two catastrophic failure modes of kernel code from Module 14 — the infinite loop that locks a CPU and the wild pointer that panics the machine. The guarantee is *ex ante*: unsafe programs never run at all, so there's no runtime crash to recover from. The cost is expressiveness (you can't write arbitrary code — the verifier will reject anything it can't prove safe), which is a deliberate, worthwhile trade.

**Java analogy:** Precisely the **JVM bytecode verifier**, which rejects malformed/unsafe class files at load time (stack overflows, type violations, illegal jumps) so the JVM never executes them. Both prove safety statically before running a single instruction. The eBPF verifier is stricter in some ways (it must prove *termination*, which the JVM doesn't) because kernel context is less forgiving than a user-mode JVM — a hung kernel is worse than a hung thread. Same philosophy, higher stakes.

### 3. Hooks and attach points: where eBPF programs run

**What it is:** An eBPF program is attached to a **hook** — a point in the kernel where it will be invoked when a relevant event occurs. The major families:
- **kprobes / kretprobes** — attach to (almost) *any* kernel function's entry or return; dynamic, powerful, but tied to internal function names that can change.
- **tracepoints** — attach to *stable, curated* kernel events (e.g. `syscalls:sys_enter_openat`, `sched:sched_switch`); a maintained API, preferred for tracing.
- **XDP (eXpress Data Path)** — attach to the network driver's earliest RX point, before the kernel builds a socket buffer; used for ultra-fast packet filtering/forwarding (DDoS drop, load balancing).
- **socket filters / tc** — attach to network processing for per-packet decisions higher up the stack.
- **LSM hooks / uprobes** — attach to security decision points (allow/deny actions) or to *userspace* function entries.

```
   syscall enter/exit ──▶ [tracepoint]  → count/trace syscalls (observability)
   any kernel function ──▶ [kprobe]     → inspect internals dynamically
   packet in the driver ─▶ [XDP]        → drop/redirect at line rate (networking)
   security decision ────▶ [LSM]        → allow/deny (security)
```

**Why it exists:** eBPF's usefulness comes from *where* it can run. Different problems need different vantage points: observability wants syscall/function hooks (to see what programs do), high-performance networking wants the earliest packet hook (to act before expensive processing), security wants the decision hooks (to enforce policy). Providing a rich menu of attach points — from stable tracepoints to dynamic kprobes to line-rate XDP — is what lets one mechanism serve tracing, networking, *and* security. The hook determines what data your program sees and what it can affect.

**Java analogy:** Hooks are like **instrumentation agents / AOP join points**: a Java agent (via `java.lang.instrument`) attaches bytecode to method entry/exit to trace or modify behavior without changing source — exactly what a kprobe/uprobe does at the kernel/binary level. Tracepoints are like well-defined logging/metrics hooks a framework exposes deliberately (stable), vs kprobes which are like instrumenting arbitrary private methods (powerful but brittle). If you've used a profiler or APM agent that "hooks into" your app, eBPF hooks are that idea, for the kernel.

### 4. Maps: state and the userspace bridge

**What it is:** **Maps** are kernel-resident data structures that eBPF programs use to (a) keep state across invocations (an eBPF program is otherwise stateless per event) and (b) communicate with userspace. Common types: **hash maps** and **arrays** (key/value tables), **per-CPU** variants (lock-free per-core counters), **ring buffers / perf buffers** (stream events to userspace efficiently), and specialized ones (LRU, stack-trace, histogram-friendly).

```
   eBPF program (in kernel)                userspace controller
   ──────────────────────                  ────────────────────
   on each event:                          periodically:
     count[pid]++    ───writes───▶ [ MAP ] ◀───reads─── print the table
                                (shared kernel memory)
   (a ring buffer map streams individual events to userspace instead)
```

**Why it exists:** An eBPF program fires per-event and has no persistent memory of its own, yet almost every use needs to *accumulate* (count syscalls, build a histogram, remember a flow) and *report* (get results to a userspace tool). Maps provide both: shared, typed, kernel-managed storage that survives across program invocations and is readable/writable from userspace via the `bpf` syscall. They're the memory and the I/O channel of the eBPF world — the reason a one-line `bpftrace` counter can tally millions of events in-kernel and then print a tidy table.

**Java analogy:** A map is like a **`ConcurrentHashMap` shared between the instrumented code and the reporting thread** — the agent increments counters as events happen, and a reporter thread periodically reads and emits them. The per-CPU map is the striped/`LongAdder` pattern (per-core counters summed on read) to avoid contention (Module 6). The ring buffer is a `BlockingQueue` streaming events from producer (kernel program) to consumer (userspace). You've built this exact "instrument writes to a shared structure, reporter reads it" pattern in application monitoring; maps are its kernel form.

### 5. The toolchain and what it's used for: `bpftrace`, BCC, `libbpf`

**What it is:** The ecosystem for writing and running eBPF, from highest-level to lowest:
- **`bpftrace`** — a high-level tracing language (awk-like) for one-liners and short scripts; compiles to eBPF for you. The fastest way to answer ad-hoc kernel questions. *(What this module uses.)*
- **BCC (BPF Compiler Collection)** — Python/C++ framework with many ready tools (`opensnoop`, `execsnoop`, `biolatency`, `tcplife`); good for richer tools.
- **`libbpf` + CO-RE** — the production C path: write eBPF in restricted C, compile with `clang`, load with `libbpf`; "compile once, run everywhere" via BTF type info. What Cilium, Falco, and serious projects use.

Use cases, concretely: **observability** (trace/aggregate kernel & app events with negligible overhead — the modern `strace`/profiler), **networking** (XDP/tc packet processing: Cilium's Kubernetes networking, Katran load balancing, DDoS mitigation), **security** (runtime detection and enforcement: Falco, Tetragon, seccomp-bpf).

**Why it exists:** eBPF bytecode is tedious to write by hand, so a ladder of tools trades power for convenience: `bpftrace` for instant insight, BCC for packaged tools, `libbpf`/CO-RE for portable production software. The layering mirrors every mature platform (a REPL/one-liner tier, a library tier, a framework tier). The breadth of use cases — one mechanism spanning observability, networking, and security — is *why* eBPF matters: it's not a niche tracing gadget but a general in-kernel programmability layer that major infrastructure now depends on.

**Java analogy:** The tool ladder mirrors, say, the JVM observability stack: `bpftrace` ≈ a `jshell`/one-liner or an ad-hoc JFR event; BCC ≈ a packaged profiler/tool suite (async-profiler, VisualVM); `libbpf`/CO-RE ≈ a full instrumentation agent shipped with your product (an APM agent). And "compile once, run everywhere" (CO-RE/BTF) is *literally* Java's original promise — write once, run on any kernel — achieved for eBPF via embedded type information, the same problem the JVM solved with bytecode + a portable runtime.

---

## Code

> **Environment:** these are **`bpftrace`** scripts — the accessible on-ramp to eBPF. Install (`sudo apt install bpftrace`, kernel 5.x+) and run with **root** (eBPF loading is privileged). They compile to eBPF, get verified, and run in-kernel. Production eBPF uses `libbpf`/C, but `bpftrace` is how everyone starts and how most ad-hoc kernel questions get answered.

### Program 1 — `execsnoop.bt`: watch every program the system runs

```
#!/usr/bin/env bpftrace
/*
 * execsnoop.bt -- print every program executed system-wide, live.
 * Attaches an eBPF program to the execve tracepoint (Module 5's exec!): each
 * time ANY process calls execve, our in-kernel program fires and prints the
 * calling PID, the parent's name, and the command being run.
 *
 * Run:  sudo bpftrace execsnoop.bt
 *   then in another terminal run some commands (ls, date, grep ...) and watch.
 * Stop: Ctrl-C.
 */

BEGIN
{
    printf("%-8s %-16s %-16s %s\n", "PID", "PARENT", "COMM", "ARGS");
}

/* Hook: the enter of the execve syscall -- a stable tracepoint. */
tracepoint:syscalls:sys_enter_execve
{
    /* args->filename is the program path; comm/pid are the current process. */
    printf("%-8d %-16s %-16s %s\n",
           pid, comm, str(args->filename), str(args->argv[0]));
}
```

**Expected output:**
```
$ sudo bpftrace execsnoop.bt
PID      PARENT           COMM             ARGS
20481    bash             bash             /usr/bin/ls
20482    bash             bash             /usr/bin/date
20483    bash             bash             /usr/bin/grep
^C
```

**Walkthrough of the non-obvious parts:**
- **This is a real eBPF program in ~4 lines.** `bpftrace` compiles the `tracepoint:...{ ... }` block to eBPF bytecode, the kernel verifies it, JITs it, and attaches it to the `sys_enter_execve` tracepoint. Every `execve` on the *entire system* now runs your printf — with negligible overhead, no kernel module, no reboot. Compare Module 14's pages of module boilerplate and panic risk.
- **You are watching Module 5 live.** Every command your shell runs is a `fork`+`execve`; this traces the `execve` half, so you literally see the mechanism you learned abstractly — `bash` exec'ing `/usr/bin/ls` — as it happens. `bpftrace` turned a syscall you *called* into an event you *observe*.
- **`tracepoint:syscalls:sys_enter_execve`** is a **stable** hook (Concept 3) — a maintained kernel API for "a process is entering execve" — which is why this script works across kernel versions. The `args->` fields are the syscall's arguments, exposed by the tracepoint. `comm`, `pid` are built-in `bpftrace` variables for the current process.
- **`str(args->filename)`** — the argument is a *user-space* pointer (Module 13); `str()` safely reads the string from it (the eBPF equivalent of `copy_from_user`, which the verifier requires you route through safe helpers). You can't just dereference it, exactly as in Module 14's driver.
- The overhead is so low you can run this on a busy production box — which is the entire point: `strace` (Module 13) attaches to *one* process with heavy `ptrace` overhead; this observes *every* process at near-zero cost, because the filtering/printing happens in-kernel.

### Program 2 — `readsize.bt`: a latency/size histogram using a map

```
#!/usr/bin/env bpftrace
/*
 * readsize.bt -- histogram of the SIZE returned by read() syscalls, per second,
 * aggregated IN-KERNEL using an eBPF map. Demonstrates maps (Concept 4): the
 * kernel program tallies into a histogram map; bpftrace prints it on exit.
 * Also counts read() calls per process name.
 *
 * Run:  sudo bpftrace readsize.bt      (Ctrl-C after some activity)
 */

/* On every return from a read() syscall, record the returned byte count. */
tracepoint:syscalls:sys_exit_read
/args->ret > 0/                     /* only successful reads (ret = bytes read) */
{
    @bytes = hist(args->ret);        /* @bytes is a HISTOGRAM MAP, updated in-kernel */
    @reads[comm] = count();          /* @reads is a per-command COUNTER MAP */
}

END
{
    printf("\n-- read() sizes (log2 histogram) --\n");
    print(@bytes);
    printf("\n-- read() count by process --\n");
    print(@reads);
}
```

**Expected output:**
```
$ sudo bpftrace readsize.bt
^C
-- read() sizes (log2 histogram) --
@bytes:
[1]         12 |@@@                                     |
[2, 4)       3 |@                                       |
[4, 8)       8 |@@                                      |
[16, 32)    41 |@@@@@@@@@@                               |
[64, 128)  190 |@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@|
[1K, 2K)    77 |@@@@@@@@@@@@@@@@                         |

-- read() count by process --
@reads[sshd]:      14
@reads[bash]:      52
@reads[bpftrace]: 118
```

**Walkthrough of the non-obvious parts:**
- **`@bytes = hist(args->ret)` is a map, updated entirely in-kernel.** The `@name` syntax declares a BPF map (Concept 4); `hist()` buckets values into a log2 histogram *in the kernel*, so millions of `read`s are aggregated with no per-event userspace cost. `bpftrace` only reads the finished map when you Ctrl-C and prints it. This "aggregate in-kernel, report on exit" is the core eBPF observability pattern.
- **`@reads[comm] = count()`** is a **hash map keyed by process name** — one counter per command, incremented in-kernel. Reading the map at the end gives you a per-process breakdown "for free." This is exactly the "instrument writes to a shared map, reporter reads it" pattern from Concept 4, in two tokens.
- **The `/args->ret > 0/` filter** is a *predicate* — the block only runs when the read returned a positive byte count (Module 3: `read` returns bytes read, 0 at EOF, -1 on error). Filtering in-kernel means uninteresting events cost almost nothing; they never reach userspace.
- **`sys_exit_read`** (not `enter`) is used because the *return value* (bytes read) is what we want, and that's only known at syscall exit — mirroring Module 13's understanding of a syscall's return. The tracepoint gives us `args->ret`, the value the syscall put in `rax`.
- **The histogram directly visualizes a lesson from Module 3/10:** you'll typically see clusters at common buffer sizes — evidence of buffering (a spike at, say, 4096 or the stdio buffer size) vs unbuffered tiny reads (a spike at small sizes). You're *measuring* the buffering behavior the course has been preaching, using in-kernel aggregation.

---

## Under the Hood

You can watch eBPF's load-verify-attach lifecycle with `strace` and `bpftool`. Run `sudo strace -e bpf,perf_event_open bpftrace execsnoop.bt`:

```
bpf(BPF_PROG_LOAD, {prog_type=BPF_PROG_TYPE_TRACEPOINT, insn_cnt=64, ...}, ...) = 5   ← [1]
bpf(BPF_MAP_CREATE, {map_type=BPF_MAP_TYPE_PERF_EVENT_ARRAY, ...}, ...) = 4           ← [2]
perf_event_open({type=PERF_TYPE_TRACEPOINT, config=sys_enter_execve, ...}) = 6        ← [3]
ioctl(6, PERF_EVENT_IOC_SET_BPF, 5)                                            = 0    ← [4] attach!
# ...program now runs in-kernel on every execve; userspace just reads the map...
```

And `sudo bpftool prog list` shows your loaded program:
```
92: tracepoint  name sys_enter_execve  tag a1b2...  gpl
    loaded_at 2026-07-29T08:00:00  uid 0
    xlated 512B  jited 389B  memlock 4096B                                            ← [5]
```

Annotated:
1. **`bpf(BPF_PROG_LOAD, ...)`** — the single syscall that submits your compiled eBPF **bytecode** to the kernel. *This* is where the **verifier runs*** — the kernel analyzes all 64 instructions, proves safety, and (on success) JIT-compiles and returns an fd (5) for the loaded program. If the verifier rejected it, this syscall returns `-EACCES`/`-EINVAL` with a verifier log explaining exactly why. Everything eBPF goes through this one `bpf()` syscall with different commands.
2. **`bpf(BPF_MAP_CREATE, ...)`** — creating a **map** (Concept 4), here a perf-event array to stream the printf events to userspace. Maps are also just `bpf()` syscall commands, returning an fd you read/write.
3–4. **`perf_event_open(... sys_enter_execve ...)` then `ioctl(..., PERF_EVENT_IOC_SET_BPF, 5)`** — this is the **attach**: open the tracepoint as a perf event (fd 6), then `ioctl` (Module 11!) to bind our loaded eBPF program (fd 5) to it. From this instant, every `execve` on the system runs the verified program. Notice the whole eBPF stack is built from primitives you already know — syscalls (Module 13), fds (the whole course), `ioctl` (Module 11).
5. **`xlated 512B  jited 389B`** — the program exists in two forms: `xlated` (verified eBPF bytecode) and `jited` (native machine code the JIT produced). It runs as native code at full speed — verification is a *load-time* cost, not a per-event one. `memlock 4096B` is its bounded memory. `bpftool` lets you inspect every loaded eBPF program on the system.

The headline: **eBPF is one `bpf()` syscall to load-and-verify bytecode, another to create maps, and a `perf_event_open`+`ioctl` to attach to a hook — after which your *verified, JIT-native* program runs in-kernel on every event, aggregating into maps that userspace reads.** The safety (verifier at `BPF_PROG_LOAD`) and the speed (JIT) are exactly what let you do, routinely and on production systems, what Module 14's kernel module could only do dangerously.

---

## Try This

Ordered easy → hard. (Linux 5.x+, `bpftrace` installed, run as root.)

1. **(Easy) One-liner syscall counts.** `sudo bpftrace -e 'tracepoint:raw_syscalls:sys_enter { @[comm] = count(); }'` — run it for a few seconds, Ctrl-C, and see a per-process syscall tally. Relate the counts to Module 13 (each is a real trap). *Hint: this one line is a complete verified in-kernel program — the whole "count events into a map, print on exit" pattern.*

2. **(Easy) Trace file opens with paths.** `sudo bpftrace -e 'tracepoint:syscalls:sys_enter_openat { printf("%s -> %s\n", comm, str(args->filename)); }'` and watch which processes open which files (Module 3, live). *Hint: this is `opensnoop` in one line — you're seeing every `open` the whole system makes.*

3. **(Medium) Run and read `execsnoop.bt` and `readsize.bt`.** Run each, generate activity (run commands; `cat` a big file), and interpret the output — the exec trace against Module 5, the read histogram against Module 3's buffering. Find the buffer-size spike. *Hint: unbuffered vs buffered I/O shows up as different clusters in the histogram.*

4. **(Medium) Measure syscall latency.** Write a `bpftrace` script that records a timestamp on `sys_enter_read` and, on `sys_exit_read`, adds `(nsecs - @start[tid])` to a `hist()` — a read-latency histogram. Relate the numbers to Module 13's ~hundreds-of-ns trap cost plus the actual I/O. *Hint: key the start time by `tid` (thread id) so concurrent reads don't collide; this is the `biolatency` idea.*

5. **(Hard) Inspect loaded programs, and read a BCC tool's source.** `sudo bpftool prog list` and `sudo bpftool map list` to see what's loaded. Then read the source of a BCC tool (`/usr/share/bcc/tools/execsnoop` or `opensnoop`) and map its structure onto this module: the eBPF program, the maps, the attach, the userspace read loop. Explain how it differs from your `bpftrace` version. *Hint: BCC exposes the map/attach/read machinery `bpftrace` hides — it's the next rung down the toolchain ladder.*

---

## Gotchas

- **The verifier will reject programs, sometimes cryptically.** eBPF is *not* general C — unbounded loops, unchecked pointer math, too-large programs, and reads the verifier can't prove safe are rejected at `BPF_PROG_LOAD` with a (long) verifier log. When a program won't load, read the log: it names the offending instruction and why. Fighting the verifier is a normal part of writing eBPF; it's protecting the kernel from you (Module 14's lesson, enforced automatically).

- **Root/privileges and kernel version matter.** Loading eBPF generally requires `CAP_BPF`/`CAP_SYS_ADMIN` (root in practice), and features/hooks vary by kernel — `bpftrace` needs a reasonably modern kernel (5.x+ for the good stuff), BTF for CO-RE, etc. "It works on my machine" is a real trap; check the kernel version and available tracepoints (`sudo bpftrace -l`).

- **kprobes are unstable; prefer tracepoints.** A `kprobe` on an internal kernel function works until that function is renamed/inlined/removed in a kernel update, then your tool silently breaks. **Tracepoints** are a maintained, stable API — prefer them for anything you'll rely on. Use kprobes only when no tracepoint exposes what you need, and expect maintenance.

- **Tracing has overhead — low, not zero.** eBPF is dramatically cheaper than `strace`/`ptrace`, but attaching to a very high-frequency event (every packet, every scheduler switch) and doing nontrivial work still costs measurable CPU. Aggregate in-kernel (maps/histograms), filter early with predicates, and avoid heavyweight per-event work. Measure the overhead of your tracing on hot paths.

- **`str()`/reading user or kernel pointers must go through helpers.** You can't dereference arbitrary pointers in eBPF (the verifier forbids it); use the provided helpers (`str()`, `bpf_probe_read_*`) which safely read memory — the eBPF form of `copy_from_user` (Module 13). Reading a user pointer that's since been freed or a bad address yields empty/garbage, not a crash (the helper handles the fault), but design for it.

- **eBPF isn't a general kernel-extension mechanism.** It's deliberately *restricted* — you can't do arbitrary things (allocate freely, sleep anywhere, call any kernel function, loop unboundedly). For observability, networking, and security policy it's ideal; for genuinely novel hardware support or unrestricted logic you still need a kernel module (Module 14). Know which tool the job needs; eBPF's safety comes precisely from what it *won't* let you do.

- **Maps have fixed sizes and eviction.** A hash map has a max entry count; exceed it and inserts fail (or, for LRU maps, old entries are evicted). A tool that keys a map by an unbounded dimension (e.g. per-connection with millions of connections) can silently drop data. Size maps for your cardinality and prefer LRU/per-CPU types where appropriate.

---

## Checkpoint

1. What is eBPF, and what fundamental dilemma from Module 14 does it resolve? Describe the load→verify→JIT→attach lifecycle at a high level.
2. What does the verifier do, and what two catastrophic kernel-code failure modes does it eliminate *before* a program ever runs? What's the cost of this safety?
3. Name three families of eBPF attach points (hooks) and a use case each serves. Why does the choice of hook matter, and why are tracepoints usually preferred over kprobes?
4. What are eBPF maps, what two problems do they solve, and how does a typical observability tool use them? Connect this to a pattern you'd recognize from application monitoring.
5. Why is eBPF often called "the JVM of the kernel"? Draw out at least three specific parallels to the Java bytecode/JVM model, and name one thing eBPF's verifier must prove that the JVM's doesn't.

---

<details>
<summary><b>Checkpoint Answers</b></summary>

1. **eBPF** is a mechanism to load small programs from userspace into the **kernel**, where they run at defined hook points — but only after the kernel **verifies** they're safe. It resolves Module 14's dilemma: previously, kernel-level power (observing/influencing kernel behavior) required a kernel module with *full privilege and zero safety* (a bug panics the machine); eBPF gives the same in-kernel power with a *guarantee of safety*, loadable and removable without rebooting. Lifecycle: **load** (submit compiled eBPF bytecode via the `bpf()` syscall) → **verify** (the kernel statically proves the program is safe, rejecting it otherwise) → **JIT** (compile the bytecode to native machine code for full-speed execution) → **attach** (bind it to a hook); thereafter it runs in-kernel whenever that event fires.

2. The **verifier** statically analyzes an eBPF program before loading and **rejects** anything it can't prove safe: it requires provable **termination** (no unbounded loops) and **in-bounds memory access only** (own stack, maps, and the passed context, all bounds-checked), plus no uninitialized reads and a bounded instruction count. This eliminates, *before the program runs at all*, the two catastrophic kernel failure modes from Module 14: the **infinite loop that hangs/locks the kernel** (prevented by proving termination) and the **wild pointer that corrupts/panics the kernel** (prevented by bounds-checking every access). The cost is **expressiveness** — you can't write arbitrary code; anything the verifier can't prove safe (unbounded loops, unchecked pointer math, too-large programs) is refused, so eBPF is a deliberately restricted environment.

3. Families: **tracepoints** (stable, curated kernel events like `sys_enter_execve`) — observability; **kprobes/kretprobes** (any kernel function entry/return, dynamic) — deep/ad-hoc introspection; **XDP** (earliest packet hook, in the driver) — high-performance networking (DDoS drop, load balancing); also **LSM hooks** — security enforcement, **uprobes** — userspace tracing. The hook matters because it determines *what data the program sees and what it can affect* — observability wants syscall/function hooks, fast networking wants the earliest packet hook, security wants decision hooks. **Tracepoints are preferred over kprobes** because they're a *stable, maintained API*; a kprobe attaches to an internal function name that can be renamed/inlined/removed by a kernel update, silently breaking the tool, whereas tracepoints are kept stable across versions.

4. **Maps** are kernel-resident data structures (hash maps, arrays, per-CPU counters, ring buffers, histograms) that solve two problems: (a) **state** — an eBPF program fires per-event and is otherwise stateless, so maps let it *accumulate* across invocations (counters, histograms, remembered flows); and (b) the **userspace bridge** — maps are readable/writable from userspace via the `bpf()` syscall, so results get *reported*. A typical observability tool has the in-kernel program **tally events into a map** (e.g. `count()` per process, or a `hist()` of sizes) and a userspace controller **periodically read the map and print** — aggregating millions of events in-kernel at near-zero cost. This is the same "instrument writes to a shared `ConcurrentHashMap`/`LongAdder`, a reporter thread reads and emits" pattern used in application monitoring/APM.

5. eBPF is "the JVM of the kernel" because it runs **verified bytecode on an in-kernel virtual machine, JIT-compiled to native code** — structurally identical to the JVM. Parallels: (1) **bytecode + a portable instruction set** (eBPF bytecode ≈ Java bytecode); (2) a **verifier that statically proves safety at load time and rejects unsafe programs** (eBPF verifier ≈ JVM bytecode verifier); (3) a **JIT compiler** turning verified bytecode into fast native code (eBPF JIT ≈ HotSpot); (4) **"compile once, run everywhere"** portability across kernels via BTF/CO-RE ≈ Java's write-once-run-anywhere via bytecode + a portable runtime. One thing eBPF's verifier must prove that the JVM's does **not**: **termination** (that the program halts / has no unbounded loops) — because a hung kernel is catastrophic, whereas the JVM tolerates a looping user thread.

</details>

---

*Next up: **Module 16 — Capstone.** Everything converges. You'll build one substantial, real program that exercises the whole stack — a choice of a tiny **container runtime** (namespaces + cgroups + `clone` + `chroot`, tying together processes, filesystems, and isolation), a **concurrent HTTP server** (sockets + `epoll` + the event loop + parsing), or a **key–value store** (file I/O + `mmap` + concurrency + a network protocol). One project, fourteen modules of foundations, and the confidence that there's no magic left — only syscalls, file descriptors, and the kernel you now understand. The finish line.*
