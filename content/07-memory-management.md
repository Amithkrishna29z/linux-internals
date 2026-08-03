# Module 7 — Memory Management

> **Estimated time:** 4–5 hours · **Core path:** Concepts 1–4 (the address-space layout, virtual memory & page faults, `malloc`/`free`, `brk`/`mmap`) and the `layout` + `arena` programs are core. The allocator-internals and `valgrind`/ASan tooling (Concept 5) are core-but-meaty; writing your own `sbrk` allocator is a second-pass stretch.
>
> **Prerequisites:** Modules 0–6. You need the stack/heap distinction from C (Module 2), the copy-on-write memory that `fork` gives each process (Module 5), and the "threads share the address space" idea (Module 6) — this module explains *what that address space actually is*.

---

## The Big Picture

You've been allocating memory since Module 2 (`malloc`, `free`, the stack, the heap) without asking where any of it comes from. This module opens the box. The central revelation is **virtual memory**: every process believes it owns a vast, private, contiguous range of addresses — on a 64-bit machine, a preposterous 256 TB of it — starting at low addresses and running up. That belief is a *lie the kernel tells*, and it's the most productive lie in all of computing. No process actually has that RAM. Instead the hardware **MMU** (memory management unit) translates each virtual address your program uses into a physical RAM address on the fly, page by page (a **page** is 4 KB), using per-process tables the kernel maintains. Two processes can both use address `0x1000` and it maps to different physical RAM; a page can be shared read-only between processes (copy-on-write, the trick behind `fork`); a page can be *absent* from RAM entirely until you touch it. Coming from Java, you've had this handed to you invisibly — the JVM sits on top of exactly this machinery — but you never had to name it. Now you will.

Start with the **layout**. Your virtual address space is divided into regions, low to high: **text** (your compiled code, read-only), **data** (initialized globals), **bss** (zero-initialized globals, costing nothing on disk), the **heap** (grows *up*, where `malloc` lives), a big gap, and the **stack** (grows *down*, one frame per function call). Print the address of a global, a `static`, a local, and a `malloc`'d pointer and you can *see* this map with your own eyes — globals down low, heap above them, stack way up high. This isn't abstract: understanding which region an address lives in tells you instantly whether a pointer bug is a stack smash, a heap corruption, or a wild write.

Then **where memory comes from**. `malloc` is not a syscall — it's a *library* function that hands out chunks from a pool it manages, and only occasionally asks the kernel for *more* pool via two syscalls: **`brk`/`sbrk`** (move the top of the heap up) for small requests, and **`mmap`** (map a fresh region) for large ones. `free` returns a chunk to malloc's pool — usually *not* to the kernel; your process's memory footprint often only grows. Understanding that `malloc`/`free` are userspace bookkeeping over rare kernel calls explains why allocation is fast, why freed memory isn't "given back," and why a fragmented heap can waste RAM. And **page faults** tie it together: when you touch a virtual page that isn't yet backed by physical RAM, the CPU traps into the kernel, which finds or allocates a physical page and wires it up — *demand paging* means memory is only really allocated when first *used*, not when `malloc` returns.

Finally, the reckoning that every Java developer must face: **manual memory management and its bug classes.** You've lived your whole career under a garbage collector. Here, every `malloc` is a promise to `free`, exactly once, after the last use and never before. Break that promise four ways and you get the four horsemen of C: the **memory leak** (never freed — the GC's absence made visible), the **use-after-free** (freed too early, then read/written — a security catastrophe), the **double-free** (freed twice — heap corruption), and the **buffer overflow** (wrote past the end — the classic exploit). These are undefined behavior: sometimes a crash, sometimes silent corruption, sometimes a remote code execution CVE. The saving grace is tooling — **`valgrind`** and **AddressSanitizer (ASan)** catch these bugs mechanically, and learning to run every program under them is the single habit that separates competent C programmers from dangerous ones. We'll write buggy programs *on purpose* and watch the tools nail them.

---

## Concepts

### 1. The virtual address space and its segments

**What it is:** Every process gets its own **virtual address space** — a private, linear range of byte addresses from `0` up to a huge maximum — that has nothing to do with how much physical RAM exists. It's partitioned into **segments**, each a contiguous region with a purpose:

```
   HIGH addresses
   ┌───────────────────────────┐  0x7fff...
   │   STACK                    │  locals, return addrs, saved regs
   │      │ grows DOWN          │  (one frame per call)
   │      ▼                     │
   ├╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌┤
   │        (big unused gap)    │  ← mmap regions land in here too
   ├╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌┤
   │      ▲ grows UP            │
   │   HEAP                     │  malloc's pool (brk moves the top)
   ├───────────────────────────┤
   │   BSS                      │  zero-init globals/statics (no disk cost)
   │   DATA                     │  initialized globals/statics
   │   TEXT                     │  your machine code (read-only, shareable)
   └───────────────────────────┘  0x0  (a guard page: *0 -> SIGSEGV)
   LOW addresses
```

**Why it exists:** Segmentation gives each kind of data the properties it needs: **text** is read-only (so code can't be corrupted, and can be *shared* between processes running the same program), **data/bss** hold globals with static lifetime, the **heap** provides dynamic lifetime you control, and the **stack** provides automatic per-call lifetime. Virtual addressing on top means every process gets the same clean layout regardless of what else is running, processes can't see or corrupt each other's memory (isolation), and the kernel can place physical pages wherever convenient.

**Java analogy:** The JVM lives *inside* one of these address spaces and carves its own structure out of the heap segment: the Java heap (objects, GC-managed), the metaspace (class metadata), and per-thread Java stacks. When you get an `OutOfMemoryError`, the JVM failed to grow its slice of *this* heap; when you get a `StackOverflowError`, a Java thread's stack (a region like the one above) hit its limit. You never saw `text`/`data`/`bss` because the JVM and classloader abstracted them into "loaded classes," but they're right there under your running `java` process — look at `/proc/[pid]/maps` (Module 12) of a JVM and you'll see them.

### 2. Virtual memory, pages, and page faults

**What it is:** Physical RAM and virtual addresses are decoupled by the **MMU**, which translates addresses in fixed-size chunks called **pages** (4 KB on x86-64). The kernel keeps per-process **page tables** mapping virtual pages → physical **frames**. Crucially, a virtual page can map to *nothing yet*. When your code touches such a page, the CPU raises a **page fault** — a trap into the kernel, which then:

```
   your code:  int x = *p;         // p points into a not-yet-backed page
                    │
                    ▼  MMU: "no physical frame for this virtual page"
              ┌─────────────┐  PAGE FAULT (trap to kernel)
              │  kernel     │  1. find/allocate a physical frame
              │             │  2. (if needed) load contents (e.g. from file/swap)
              │             │  3. update the page table: vaddr -> frame
              └─────────────┘  4. resume your instruction -- it now succeeds
```

A fault that just needs a fresh zero page (**minor fault**) is cheap; one that must read from disk/swap (**major fault**) is thousands of times slower. **Demand paging** is the payoff: memory is backed by RAM only when first *touched*, so `malloc(1 GB)` that you never write to costs almost no physical RAM.

**Why it exists:** Decoupling virtual from physical enables the whole modern OS: **isolation** (a process literally cannot name another's physical memory), **overcommit** (the sum of all processes' virtual memory can exceed RAM, because unused pages aren't backed), **swap** (cold pages evicted to disk), **shared pages** (one physical copy of libc backing every process's text), and **copy-on-write** (Module 5's `fork` shares pages read-only until a write faults and copies). Paging is the mechanism under all of it.

**Java analogy:** No direct API — this is *below* the JVM — but the effects surface. GC pause spikes when the JVM touches cold pages that were swapped out (a major fault storm); `-XX:+AlwaysPreTouch` exists precisely to force all the faults up front at startup so they don't hit you at runtime. "Why did my idle JVM's RSS grow?" and "why is my container getting OOM-killed below `-Xmx`?" are page/RSS questions. The JVM's own "commit vs reserve" vocabulary mirrors virtual-reserved vs physically-backed pages exactly.

### 3. `malloc`/`free`: a userspace allocator over rare syscalls

**What it is:** `malloc` and `free` are **library** functions (in libc), *not* system calls. `malloc` manages a pool of memory, tracking free and in-use **chunks** (typically via free lists and size classes), and hands you a pointer into it. It only calls the kernel when it needs to *grow* the pool. `free` returns your chunk to the pool's free list — marking it reusable — but usually does **not** hand the memory back to the OS.

```
   ptr = malloc(24);
   ┌──────────────────────────────────────────────┐
   │ malloc's heap pool (managed in USER space):    │
   │  [hdr|  free 100B  ] [hdr| USED 24B ] [ free ] │
   │            ▲ carved from here          ▲       │
   │  malloc split a free chunk; returns ptr just   │
   │  past the header. NO syscall (pool had room).  │
   └──────────────────────────────────────────────┘
   free(ptr);   // marks that chunk free again, coalesces with neighbors.
                // Memory stays in the pool for the NEXT malloc -- not returned to OS.
```

**Why it exists:** Syscalls are expensive (a user→kernel round trip); a program that `malloc`'d via syscall on every call would crawl. By amortizing — grab a big slab from the kernel rarely, then satisfy thousands of `malloc`/`free` calls from it in user space — allocation becomes a few pointer operations. The cost is **fragmentation** (free space split into unusable small gaps) and the fact that freed memory inflates your process's footprint without shrinking it (RSS stays high; the pool just holds freed chunks ready to reuse).

**Java analogy:** This is the layer Java hides most aggressively. `new Object()` bumps a pointer in a thread-local allocation buffer (TLAB) — even *faster* than `malloc` — and you never call `free`; the garbage collector reclaims unreachable objects. So the entire "return the chunk to the pool" step is automated, and compaction moves live objects to eliminate the fragmentation C suffers. The tradeoff you're now meeting from the other side: Java trades GC pauses and memory overhead for never leaking or use-after-freeing; C trades manual burden for deterministic, pause-free control.

### 4. Where the pool comes from: `brk`/`sbrk` vs `mmap`

**What it is:** The two syscalls `malloc` uses to get memory from the kernel:

- **`brk`/`sbrk`** move the **program break** — the top of the contiguous heap segment. `sbrk(4096)` grows the heap by a page and returns the old top; `sbrk(0)` just reports the current break. This is how glibc `malloc` satisfies **small** allocations: bump the break up, carve chunks out.
- **`mmap`** maps a fresh, page-aligned region of virtual memory anywhere in the gap (for anonymous memory: `mmap(NULL, len, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0)`). glibc `malloc` uses `mmap` for **large** allocations (default threshold ~128 KB) because such a region can be `munmap`'d and *actually returned* to the OS independently, avoiding heap fragmentation.

```
   small malloc(24)      →  (pool has room? just carve)  → maybe sbrk() to grow heap top
   large malloc(1 MB)    →  mmap() a dedicated region     → free() → munmap() → REALLY returned
```

**Why it exists:** Two mechanisms for two regimes. `sbrk` is simple and cheap for the common flood of small objects, but it's a single stack-like frontier — you can only shrink it by freeing from the *top*, so a live chunk anywhere pins everything below it (why freed small memory rarely returns to the OS). `mmap` gives independent, individually-returnable regions ideal for big blocks, and it's also how files get mapped into memory (Module 8) and how shared/thread-stack memory is created. `malloc` picks per request.

**Java analogy:** The JVM does its own `mmap` at startup to reserve the whole heap (`-Xmx`) as one big region, then manages objects inside it itself — it does **not** call `malloc`/`sbrk` per object. So a JVM makes very few memory syscalls after warmup, whereas a C program's `malloc` traffic maps to periodic `brk`/`mmap`. `-XX:MaxDirectMemorySize` and `ByteBuffer.allocateDirect` are Java reaching *around* its managed heap to `mmap` native memory directly — the one place a Java dev touches this layer explicitly.

### 5. The four bug classes and the tools that catch them

**What it is:** Manual memory management fails in four canonical ways, all **undefined behavior** (may crash, may silently corrupt, may be exploitable):

```
   LEAK:            p = malloc(n);  ... return;      // never freed -> RSS grows forever
   USE-AFTER-FREE:  free(p);  ... x = *p;            // dangling pointer -> corruption/RCE
   DOUBLE-FREE:     free(p);  ... free(p);           // corrupts allocator metadata
   BUFFER OVERFLOW: char b[8]; strcpy(b, "toolong"); // writes past the end -> smash
```

The defenses are **tools**, not vigilance alone:
- **`valgrind` (memcheck)** — runs your program on a synthetic CPU, tracking every byte's allocation state. Reports leaks (with allocation stack traces), invalid reads/writes, use-after-free, and double-free. ~10–30× slower but needs no recompile.
- **AddressSanitizer (ASan)** — compile with `-fsanitize=address`; instruments allocations with "red zones" and a shadow map. ~2× slower, catches overflows and use-after-free with precise reports, and it's the modern default in CI.

**Why it exists (why you must internalize this):** Without a GC, correctness is *your* job, and the failure modes aren't polite exceptions — they're silent memory corruption that manifests as a crash three functions later, or a security hole. A huge fraction of historical CVEs (Heartbleed, countless RCEs) are exactly these four bugs. The tools turn "undefined behavior that hides" into "a precise error message at the exact line," which is the only scalable way to write correct C.

**Java analogy:** The GC makes leaks (mostly) and use-after-free/double-free (entirely) impossible — a freed-then-used reference can't exist because references keep objects alive, and there's no manual `free`. Java's residual "leak" is the *logical* leak: a live reference you forgot (a growing static `Map`, an unremoved listener), which tools like Eclipse MAT and heap dumps hunt. Buffer overflows are prevented by bounds-checked arrays (`ArrayIndexOutOfBoundsException` instead of a silent smash). So `valgrind`/ASan are the C world reconstructing, at great effort, the safety Java gives you for free — which is exactly why understanding them teaches you what the GC was doing all along.

---

## Code

### Program 1 — `layout.c`: see the address-space segments with your own eyes

```c
/* layout.c
 *
 * Prints the address of a symbol from each memory segment, so you can SEE
 * the layout: text (code) low, then data/bss globals, then the heap growing
 * up, and the stack way up high growing down. Also shows sbrk(0) (the current
 * program break = top of the heap).
 *
 * Compile:  gcc -Wall -Wextra -o layout layout.c
 * Run:      ./layout
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int   g_init   = 42;      /* DATA segment (initialized global)     */
int   g_bss;              /* BSS  segment (zero-initialized global) */

static void some_function(void) { /* its address is in TEXT */ }

int main(void)
{
    int      local = 1;               /* STACK */
    int     *heap  = malloc(sizeof *heap);   /* points into the HEAP */

    printf("text  (code)   some_function = %p\n", (void *)some_function);
    printf("data  (init'd) &g_init       = %p\n", (void *)&g_init);
    printf("bss   (zero)   &g_bss        = %p\n", (void *)&g_bss);
    printf("heap  (malloc) heap          = %p\n", (void *)heap);
    printf("break (sbrk 0) top-of-heap   = %p\n", (void *)sbrk(0));
    printf("stack (local)  &local        = %p\n", (void *)&local);

    free(heap);
    return 0;
}
```

**Expected output (exact addresses vary per run due to ASLR, but the *ordering* is stable):**
```
$ ./layout
text  (code)   some_function = 0x561e2a3f0169
data  (init'd) &g_init       = 0x561e2a3f4010
bss   (zero)   &g_bss        = 0x561e2a3f4018
heap  (malloc) heap          = 0x561e2b1c22a0
break (sbrk 0) top-of-heap   = 0x561e2b1c3000
stack (local)  &local        = 0x7ffe4d5b8a3c
```

**Walkthrough of the non-obvious parts:**
- The addresses climb in exactly the layout order: **text < data < bss < heap < break**, then a *huge* jump to the **stack** near `0x7fff...`. That gap between the heap top and the stack is the unused span where `mmap` regions (and thread stacks, Module 6) get placed.
- `&g_init` and `&g_bss` are *adjacent* — both are globals, just in different segments (data holds the initialized `42`, bss holds the zero). bss costs nothing in the executable file on disk (it's just "reserve N zero bytes"), which is why huge zero-initialized arrays don't bloat your binary.
- `heap` (from `malloc`) sits just below `sbrk(0)` (the program break) — the malloc'd chunk came out of the heap segment, and the break is the current top. Small allocations live between the bss and the break.
- The stack address is enormous compared to everything else and lives near the top of the space, growing *downward* as you nest calls. A local's address being ~`0x7fff...` while a global is ~`0x561e...` is the fastest way to eyeball "is this pointer stack or heap/global?"
- Addresses differ every run because of **ASLR** (address space layout randomization) — the kernel randomizes segment base addresses to make exploits harder. The *relative* order never changes.

### Program 2 — `bugs.c`: write the four bugs on purpose, then let the tools catch them

```c
/* bugs.c
 *
 * A rogues' gallery of the four classic memory bugs. Each is guarded by a
 * command-line selector so you can trigger ONE at a time and watch valgrind
 * or AddressSanitizer pinpoint it. This program is WRONG on purpose.
 *
 * Compile (valgrind):  gcc -Wall -Wextra -g -o bugs bugs.c
 *   then:  valgrind --leak-check=full ./bugs leak
 *
 * Compile (ASan):      gcc -Wall -Wextra -g -fsanitize=address -o bugs bugs.c
 *   then:  ./bugs uaf
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void do_leak(void)
{
    char *p = malloc(64);        /* allocated... */
    strcpy(p, "I am never freed");
    printf("%s\n", p);
    /* ...and we return without free(p): a LEAK. */
}

static void do_use_after_free(void)
{
    char *p = malloc(64);
    strcpy(p, "hello");
    free(p);                     /* freed here */
    printf("%s\n", p);           /* USE-AFTER-FREE: reading dangling memory */
}

static void do_double_free(void)
{
    char *p = malloc(64);
    free(p);
    free(p);                     /* DOUBLE-FREE: corrupts allocator metadata */
}

static void do_overflow(void)
{
    char *p = malloc(8);
    strcpy(p, "way too long for eight bytes");  /* BUFFER OVERFLOW past the end */
    printf("%s\n", p);
    free(p);
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s leak|uaf|double|overflow\n", argv[0]);
        return 2;
    }
    if      (strcmp(argv[1], "leak")     == 0) do_leak();
    else if (strcmp(argv[1], "uaf")      == 0) do_use_after_free();
    else if (strcmp(argv[1], "double")   == 0) do_double_free();
    else if (strcmp(argv[1], "overflow") == 0) do_overflow();
    else { fprintf(stderr, "unknown: %s\n", argv[1]); return 2; }
    return 0;
}
```

**Expected output (ASan on the use-after-free — the report pinpoints the exact line):**
```
$ gcc -g -fsanitize=address -o bugs bugs.c
$ ./bugs uaf
=================================================================
==12841==ERROR: AddressSanitizer: heap-use-after-free on address 0x602000000010
READ of size 1 at 0x602000000010 thread T0
    #0 0x... in do_use_after_free bugs.c:23
    #1 0x... in main bugs.c:51
0x602000000010 is located 0 bytes inside of 64-byte region
freed by thread T0 here:
    #0 0x... in free
    #1 0x... in do_use_after_free bugs.c:22   <-- freed on line 22
previously allocated by thread T0 here:
    #1 0x... in do_use_after_free bugs.c:20   <-- allocated on line 20
```

**Expected output (valgrind on the leak):**
```
$ gcc -g -o bugs bugs.c
$ valgrind --leak-check=full ./bugs leak
==13002== HEAP SUMMARY:
==13002==    definitely lost: 64 bytes in 1 blocks
==13002==    at 0x...: malloc
==13002==    by 0x...: do_leak (bugs.c:12)     <-- leaked allocation site
==13002== LEAK SUMMARY:
==13002==    definitely lost: 64 bytes in 1 blocks
```

**Walkthrough of the non-obvious parts:**
- Run *without* the tools, several of these bugs **appear to work** — `do_use_after_free` often prints "hello" because the freed memory hasn't been reused *yet*. That deceptive success is the whole danger: the bug is real but invisible until the memory gets recycled under load, then it corrupts. The tools make the latent bug *loud and immediate*.
- ASan gives you **three stack traces** for a use-after-free: where you touched it, where it was freed, and where it was allocated — enough to fix the bug without a debugger. That triangulation is why ASan is the modern default.
- `-g` (debug symbols) is what turns the tool's addresses into `file:line`. Always compile with `-g` when hunting memory bugs, or you get bare hex addresses.
- valgrind needs *no recompile or special flags* (just `-g` for line numbers) — great for binaries you can't rebuild. ASan needs `-fsanitize=address` at compile time but is far faster and catches overflows more precisely. Use ASan in CI, valgrind for the occasional deep audit.
- The overflow (`do_overflow`) writes past an 8-byte chunk; ASan reports a `heap-buffer-overflow` with the exact byte offset, while a bare run might crash *later* in unrelated code (or not at all) — the textbook "corruption manifests far from its cause" problem.

### Project — `arena.c`: a bump/arena allocator (understand malloc by building a tiny one)

```c
/* arena.c
 *
 * A minimal ARENA (a.k.a. bump/region) allocator: grab ONE big block from
 * malloc up front, then hand out sub-allocations by just advancing a pointer.
 * Individual frees are impossible; instead you free the WHOLE arena at once.
 * This is how compilers, request handlers, and game engines get O(1) alloc
 * and zero fragmentation for data with a common lifetime.
 *
 * Compile:  gcc -Wall -Wextra -o arena arena.c
 * Run:      ./arena
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

typedef struct {
    unsigned char *base;   /* start of the backing block          */
    size_t         cap;    /* total bytes                          */
    size_t         used;   /* bytes handed out so far (the "bump") */
} Arena;

static Arena arena_new(size_t cap)
{
    Arena a = { malloc(cap), cap, 0 };
    if (!a.base) { perror("malloc"); exit(1); }
    return a;
}

/* Hand out `size` bytes, aligned up to 16 (so any type is safely stored). */
static void *arena_alloc(Arena *a, size_t size)
{
    size_t aligned = (a->used + 15) & ~(size_t)15;   /* round up to 16 */
    if (aligned + size > a->cap) return NULL;         /* arena exhausted */
    void *p = a->base + aligned;
    a->used = aligned + size;                          /* just BUMP the pointer */
    return p;
}

static void arena_reset(Arena *a) { a->used = 0; }     /* "free" everything: O(1) */
static void arena_destroy(Arena *a) { free(a->base); a->base = NULL; }

int main(void)
{
    Arena a = arena_new(1024);

    /* Allocate a few things of different types out of the one arena. */
    char *name = arena_alloc(&a, 32);
    strcpy(name, "arena-allocated string");

    int *nums = arena_alloc(&a, 5 * sizeof(int));
    for (int i = 0; i < 5; i++) nums[i] = i * i;

    printf("name : %s\n", name);
    printf("nums : ");
    for (int i = 0; i < 5; i++) printf("%d ", nums[i]);
    printf("\n");
    printf("arena: %zu / %zu bytes used\n", a.used, a.cap);

    /* Free EVERYTHING in one shot -- no per-object free, no leaks possible. */
    arena_reset(&a);
    printf("after reset: %zu bytes used\n", a.used);

    arena_destroy(&a);   /* return the one big block to malloc */
    return 0;
}
```

**Expected output:**
```
$ ./arena
name : arena-allocated string
nums : 0 1 4 9 16
arena: 56 / 1024 bytes used
after reset: 0 bytes used
```

**Walkthrough of the non-obvious parts:**
- **One `malloc`, many sub-allocations.** The arena calls `malloc` *once* for the whole 1024-byte block; every `arena_alloc` is then just a pointer bump — no per-object allocator bookkeeping, no free list, no syscall. This is dramatically faster than `malloc`-per-object and produces **zero fragmentation**.
- **You cannot free one object** — there's deliberately no `arena_free(ptr)`. That's the trade: arenas suit data with a *shared lifetime* (everything for one web request, one compiler pass, one frame) that you discard all at once via `arena_reset` (O(1): just set `used = 0`). Giving up individual free is what buys the speed and the impossibility of leaks/use-after-free *within* the arena.
- **Alignment matters.** `(a->used + 15) & ~(size_t)15` rounds the offset up to a 16-byte boundary so that an `int*`, `double*`, or any type stored there is properly aligned (misaligned access is a crash on some CPUs and slow on others). Real `malloc` guarantees this for you; an allocator must do it by hand.
- **`arena_reset` vs `arena_destroy`.** Reset recycles the *same* block for a new batch (used in a loop: allocate a batch, reset, repeat) — no return to the OS. Destroy hands the one block back to `malloc`. This mirrors the real `malloc`/OS relationship: cheap internal recycling, rare actual release.
- This pattern is everywhere: `apr_pool` (Apache), Go's arena experiments, per-request allocators in high-performance servers, and the "region" in Rust/Zig. When Java devs hear "off-heap arena," *this* is the shape of it.

---

## Under the Hood

Run `strace -e trace=brk,mmap,munmap ./arena` and then, more revealingly, `strace -e trace=brk,mmap ./bugs leak` to watch `malloc` actually talk to the kernel:

```
brk(NULL)                       = 0x55e0a1c3b000            ← [1] find current break at startup
brk(0x55e0a1c5c000)             = 0x55e0a1c5c000            ← [2] glibc grows heap for its arena
mmap(NULL, 135168, PROT_READ|PROT_WRITE,
     MAP_PRIVATE|MAP_ANONYMOUS, -1, 0) = 0x7f3c9a2b1000     ← [3] a LARGE alloc goes via mmap
...
write(1, "I am never freed\n", 17) = 17
exit_group(0)                                               ← [4] leak: never munmap'd/freed
```

Annotated:
1. **`brk(NULL)`** — `sbrk(0)` under the hood: asks "where is the top of the heap right now?" glibc does this once at startup to learn the initial break.
2. **`brk(0x...c5c000)`** — glibc grows the heap by moving the break up, claiming a slab it will carve **many** `malloc` chunks from. Notice: your program called `malloc(64)`, but there's *no syscall for that call* — it was satisfied entirely in user space from this pre-grown slab. **This is the amortization**: one `brk` backs thousands of `malloc`s.
3. **`mmap(NULL, 135168, ... MAP_ANONYMOUS ...)`** — a request above glibc's `mmap` threshold (~128 KB) bypasses the heap and gets its *own* anonymous region. On `free`, this one can be `munmap`'d and genuinely returned to the OS — unlike small chunks, which just go back to the free list. (135168 ≈ 132 KB: a big allocation, or glibc's per-thread arena.)
4. **No `free`/`munmap` before `exit_group`** — the leak, visible at the syscall level: memory was mapped and never released. (The kernel reclaims *everything* on process exit, which is why a short-lived leaky program "gets away with it" — but a long-running server does not.)

The headline: **`malloc` is userspace bookkeeping; it hits the kernel (`brk` for small-pool growth, `mmap` for big blocks) only occasionally, which is exactly why allocation is fast and why freed small memory doesn't shrink your process.** Run `strace -c ./bugs leak` and you'll see a handful of `brk`/`mmap` calls for a program that "allocated" many times — the gap between library calls and syscalls made numeric.

---

## Try This

Ordered easy → hard.

1. **(Easy) Read your own memory map.** Run `./layout`, then in another terminal `cat /proc/$(pgrep layout)/maps` (add a `sleep` to `layout` first so it stays alive). Match the printed addresses to the named regions (`[heap]`, `[stack]`, the executable's `r-xp` text). *Hint: Module 12 is all about `/proc`; this is a preview — the maps file *is* the segment table.*

2. **(Easy) Make bss free and data expensive.** Declare `int big[1000000];` as a global (bss) and check the binary size with `ls -l`; then initialize it `= {1, 2, ...}` (forcing data) and check again. Explain why the zero version doesn't bloat the file. *Hint: bss is "reserve N zero bytes"; data must store every byte on disk.*

3. **(Medium) Catch each bug with a tool.** Compile `bugs.c` with `-fsanitize=address -g` and run all four modes (`leak`, `uaf`, `double`, `overflow`). Then compile *without* ASan and run under `valgrind`. Compare what each tool reports and how precisely it points to the line. *Hint: some bugs "work" without the tools — that's the lesson, not a pass.*

4. **(Medium) Prove demand paging.** `malloc(1L << 30)` (1 GB) but *don't* write to it; print RSS from `/proc/self/status` (`VmRSS`). Then write one byte to every 4096th address and print RSS again. Watch RSS jump only after you *touch* the pages. *Hint: virtual size (`VmSize`) grows at malloc; resident size (`VmRSS`) grows at first touch — that's demand paging.*

5. **(Hard) Write a real allocator on `sbrk`.** Implement `my_malloc(size)`/`my_free(ptr)` using `sbrk` to grow the heap and a linked free list of chunks (each with a size header). Support splitting a large free chunk and coalescing adjacent free chunks on `free`. Test it, then run it under ASan. Explain why coalescing matters and why your allocator can't easily return memory to the OS. *Hint: this is a miniature glibc malloc; the free-list header before each chunk is the key data structure.*

---

## Gotchas

- **Forgetting `free` (leak).** Every `malloc` needs exactly one matching `free` after the last use. In a short program the OS reclaims everything at exit so a leak seems harmless — but in a long-running server every leaked byte accumulates until OOM. Run under valgrind/ASan; treat "definitely lost" as a bug, always.

- **Use-after-free and the dangling pointer.** After `free(p)`, `p` still holds the old address but the memory is no longer yours — reading or writing it is undefined behavior and a top-tier security hole. Defensive habit: set `p = NULL;` right after `free(p)` so a later use crashes loudly (on `NULL`) instead of silently corrupting reused memory.

- **Double-free corrupts the allocator.** Freeing the same pointer twice scribbles on malloc's internal metadata (free-list links), often crashing much *later* in an unrelated `malloc`. The `p = NULL` habit also defuses this (`free(NULL)` is a safe no-op). Never free a pointer whose ownership you've handed away.

- **Off-by-one / buffer overflow.** Writing `n+1` bytes into an `n`-byte buffer (the classic: forgetting the `\0` terminator needs a byte) smashes adjacent heap metadata or stack frames. Use sized functions (`snprintf`, `strncpy` with care, `memcpy` with a checked length) and let ASan find the ones you miss. This single bug class is the historical root of most remote exploits.

- **Returning a pointer to a stack local.** `char *f() { char buf[64]; ...; return buf; }` returns a pointer into a stack frame that's destroyed the instant `f` returns — the memory is immediately reused by the next call. The value *looks* fine right after return, then corrupts. Return heap memory (`malloc`), or have the caller pass in a buffer.

- **`malloc` can return `NULL`.** Under memory pressure (or a huge request) `malloc` fails and returns `NULL`; dereferencing it segfaults. Check every allocation (or wrap it). (On Linux with overcommit, `malloc` often "succeeds" and the OOM killer strikes later at *touch* time instead — a subtler failure mode, but the check is still mandatory portable hygiene.)

- **Freeing something you didn't `malloc`.** Calling `free` on a stack pointer, a pointer into the *middle* of an allocation, or a static/global is undefined behavior and usually a crash. Only free the exact pointer `malloc`/`calloc`/`realloc` returned, exactly once.

- **`realloc` pitfalls.** `p = realloc(p, n)` is a leak-and-lose bug if `realloc` fails: it returns `NULL` but the *old* block is still allocated, and you just overwrote your only pointer to it. Use a temp: `tmp = realloc(p, n); if (tmp) p = tmp;`. Also, `realloc` may *move* the block, invalidating every other pointer into the old one.

---

## Checkpoint

1. Sketch the segments of a process's virtual address space from low to high addresses. Which grows up, which grows down, and where does `malloc`'d memory come from? Why does bss cost nothing on disk while data does?
2. Explain virtual memory and a page fault. What does "demand paging" mean, and why can `malloc(1 GB)` succeed while using almost no physical RAM until you write to it?
3. `malloc` and `free` are not system calls. What are they, what do they do with the memory internally, and which syscalls does `malloc` use to get memory from the kernel — and when does it pick each?
4. Name the four classic memory bugs. For each, say what goes wrong and why it's dangerous even when the program *appears* to run correctly. What tools catch them, and what does each tool need at compile time?
5. What is an arena/bump allocator, what does it give up compared to `malloc`/`free`, and what does that buy you? Name a real situation where an arena is the right choice.

---

<details>
<summary><b>Checkpoint Answers</b></summary>

1. Low → high: **text** (code, read-only), **data** (initialized globals), **bss** (zero-initialized globals), **heap** (grows **up**), a large gap (where `mmap` regions live), and the **stack** near the top (grows **down**). `malloc`'d memory comes from the **heap** (grown via `brk`/`sbrk`) for small requests, or from an `mmap`'d region for large ones. **bss** costs nothing on disk because the executable only records "reserve N zero bytes" — the kernel zero-fills those pages on demand — whereas **data** must store every initialized byte's actual value in the file.

2. **Virtual memory** decouples the addresses a process uses from physical RAM: the MMU translates virtual pages (4 KB) to physical frames via per-process page tables, and a virtual page may map to no frame yet. A **page fault** is the CPU trap that fires when you touch such an unbacked page; the kernel then allocates/loads a physical frame, updates the page table, and resumes the instruction. **Demand paging** means a page is backed by RAM only when first *touched*, not when reserved — so `malloc(1 GB)` merely reserves virtual address space (and, with overcommit, promises nothing physical); RSS stays near zero until you actually write to the pages, at which point faults allocate frames one page at a time.

3. They are **libc library functions**, not syscalls. `malloc` manages a userspace **pool** of memory, tracking free/used **chunks** (free lists, size classes), carving your request out of existing free space when it can; `free` returns a chunk to the pool's free list (and may coalesce with neighbors) but usually does **not** return it to the OS. `malloc` gets memory from the kernel via **`brk`/`sbrk`** (move the heap top up) for **small** allocations, and via **`mmap`** (a separate anonymous region) for **large** ones (default threshold ~128 KB) — because an `mmap`'d region can be `munmap`'d and truly returned, while the `brk` frontier can only shrink from the top.

4. **Memory leak** (never freed → footprint grows until OOM); **use-after-free** (using memory after `free` → corruption or a security/RCE hole); **double-free** (freeing twice → corrupts allocator metadata, crashes later elsewhere); **buffer overflow** (writing past a buffer's end → smashes adjacent memory). All are dangerous even when the program *seems* to work because the corruption is often latent — the freed/overflowed memory hasn't been reused *yet*, so it manifests unpredictably later, under load, or as an exploit. **valgrind** (memcheck) catches all four with no recompile (just `-g` for line numbers); **AddressSanitizer** catches them faster with `-fsanitize=address -g` at compile time. Both want `-g` for `file:line` reports.

5. An **arena/bump allocator** grabs one big block up front and satisfies each allocation by simply advancing ("bumping") a pointer; it **gives up the ability to free individual objects** — you can only reset/free the *whole* arena at once. In exchange you get **O(1) allocation** (just a pointer add, no free-list search or syscall), **zero fragmentation**, and **no possibility of per-object leaks or use-after-free within the arena's lifetime**. It's the right choice when many allocations share a common lifetime and die together — e.g. all the memory for handling one web request, one compiler pass, or one game frame — freed in a single O(1) reset.

</details>

---

*Next up: **Module 8 — Interprocess Communication (IPC).** Now that processes are isolated (Module 5) and you understand memory (Module 7), how do they *talk*? Pipes (anonymous and named), the `pipe()`+`fork`+`dup2` pattern that powers shell `|`, `mmap`'d shared memory, and a tour of message queues and Unix-domain sockets. The fd spine returns in force: a pipe is just a pair of file descriptors. Continuing straight on.*
