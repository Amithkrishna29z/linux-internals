# Module 2 — C for Systems Programming

> **Estimated time:** 3–4 hours · **Core path:** Concepts 1–4 and both code programs are core — you cannot write the rest of this course without them. Concept 5 (compilation pipeline / ELF) and the `objdump`/`ldd` deep-dive are important but can be a second pass.
>
> **Prerequisites:** Modules 0–1. You know a syscall is the door to the kernel and that the fd is the spine. Now we learn the language that talks to that door.

---

## The Big Picture

You already know how to program. This module is not "learn C" — it's "learn how C *thinks*, because C thinks the way the machine thinks, and that's the whole point." In Java you manipulate objects: references to things the JVM manages, garbage-collects, bounds-checks, and hides the address of. In C there is no such comfort. A variable is a named box at a numeric address in memory, a pointer is *that address written down*, and an array is just a run of boxes with the pointer to the first one. There is no object header, no GC, no bounds check, no `NullPointerException` — dereference a bad pointer and the CPU traps you into a segfault (Module 0's ring wall, doing its job). C is a thin, honest layer over memory, which is exactly why the kernel is written in it and why every syscall's interface is expressed in it.

The single biggest adjustment for a Java developer is **memory ownership**. In Java, you `new` an object and forget about it; the GC reclaims it when nobody's looking. In C, *you* own every byte you allocate on the heap, and *you* must free it — exactly once, not zero times (leak), not twice (corruption). This sounds terrifying and it is, a little, but it's also clarifying: you will finally understand what the GC was doing for you, why "off-heap" and `DirectByteBuffer` exist in Java, and why a memory leak in a long-running service is a real category of bug. The stack-versus-heap distinction — automatic, scoped, fast memory versus manual, long-lived, flexible memory — is one you've been living with unconsciously in every language; here it becomes explicit and visible.

Then there's the distinction that ties this module back to Module 0: **library functions versus system calls.** `printf`, `malloc`, `strlen`, `fopen` are *library* functions — ordinary C code that ships in glibc and runs in ring 3 alongside yours. `write`, `read`, `mmap`, `brk` are *system calls* — the doors into the kernel. The crucial relationship is that the library functions are built *on top of* the syscalls: `printf` formats your string in user space and then calls `write` to actually emit it; `malloc` manages a pool of memory in user space and only occasionally calls `brk`/`mmap` to get more from the kernel. Understanding who calls whom is understanding the entire shape of a Unix program: a thin layer of your code, on a thick layer of glibc, on a thin set of syscalls, on the kernel.

Finally, you'll see what actually happens when you type `gcc`. That one command hides a four-stage pipeline — preprocess, compile, assemble, link — that turns human-readable `.c` into an **ELF binary** the kernel can `execve`. You'll learn the difference between static and dynamic linking (why your tiny "hello world" depends on a 2MB `libc.so`), and you'll meet the ELF format that Module 0's `execve` was loading. As a JVM developer you know the `javac` → bytecode → classloader story; this is the native-world equivalent, and it demystifies a lot: why binaries aren't portable across OSes, why "it works on my machine" happens with shared libraries, and what `ldd`, `nm`, and `objdump` are telling you.

By the end you'll read C the way the OS does — as addresses and bytes — and you'll have the vocabulary (pointer, heap, stack, function pointer, linkage, ELF) that every remaining module assumes.

---

## Concepts

### 1. Pointers, arrays, and strings — memory as the OS sees it

**What it is:** Memory is one enormous array of bytes, each with a numeric **address**. A *variable* is a human name for a box at some address. A **pointer** is a variable whose *value is an address* — it "points at" another box. The `&` operator means "address of" and `*` means "the thing at this address" (dereference).

```
   int x = 42;           address of x: 0x7ffc008
   int *p = &x;          p holds 0x7ffc008 (points at x)

        address        contents
      0x7ffc008  ┌────────────┐
        x  ───►  │     42     │ ◄── *p reads/writes THIS box
                 └────────────┘
      0x7ffc010  ┌────────────┐
        p  ───►  │ 0x7ffc008  │ ◄── p itself is a box; it stores an address
                 └────────────┘
```

An **array** is a contiguous run of boxes, and its name decays to a pointer to the first element. `arr[i]` is *defined as* `*(arr + i)` — indexing is pointer arithmetic. A **C string** is the starkest example: it's just an array of `char` (bytes) ending in a `'\0'` (null) byte. There is no length field, no `String` object — the `\0` *is* the length marker. Every string function (`strlen`, `strcpy`) walks bytes until it hits `\0`.

```
   char s[] = "hi";     really 3 bytes:
      index:   0    1    2
             ┌────┬────┬────┐
             │'h' │'i' │'\0'│   strlen(s) == 2  (counts up to, not including, '\0')
             └────┴────┴────┘
```

**Why it exists:** Because this *is* the machine. The CPU addresses memory by number; pointers are how you express that in a language. C doesn't add an abstraction here — it exposes the hardware directly, which is why it's fast and dangerous.

**Java analogy:** A Java *reference* is a pointer with the training wheels bolted on — you can't do arithmetic on it, can't see its numeric value, can't dereference a null without a caught exception. A C pointer is a reference with the wheels off. Java's `String` is an object with a `length` field and a `char[]`; a C string is *just the `char[]`* with a `\0` sentinel and no length stored. **The null terminator has no Java equivalent** — and forgetting it (or overrunning it) is the source of the entire buffer-overflow exploit family.

### 2. The stack vs the heap

**What it is:** A running process has two main regions where data lives (Module 7 draws the full map; here's what you need now):

- **The stack** — automatic, scoped storage for local variables and function-call bookkeeping. Every function call *pushes* a frame (its locals, return address); every return *pops* it. Fast (just move a pointer), automatically cleaned up when the function returns, but small (typically 8MB) and short-lived — a local variable ceases to exist the moment its function returns.
- **The heap** — a big pool of memory you request explicitly with `malloc` and release with `free`. Slower, manually managed, but large and long-lived: heap memory survives until *you* free it, so it can outlive the function that created it. This is where you put things whose size you don't know at compile time or that must live beyond one function call.

```
   high addresses
   ┌───────────────────┐
   │       STACK       │  grows DOWN ▼   locals, function frames
   │        │          │                 auto-freed on return
   │        ▼          │
   │        .          │
   │        .          │   (the gap between them is virtual address
   │        .          │    space, mostly unmapped — Module 7)
   │        ▲          │
   │        │          │
   │       HEAP        │  grows UP ▲     malloc'd memory
   ├───────────────────┤                 freed only when YOU free()
   │   BSS / DATA      │  globals, static vars
   ├───────────────────┤
   │       TEXT        │  your compiled code (read-only)
   └───────────────────┘
   low addresses
```

**Why it exists:** Two different lifetimes need two different strategies. Most data is scoped to a function call — the stack handles that automatically and blazingly fast. Some data must outlive its creator or be sized at runtime — the heap handles that, at the cost of manual management. Conflating them causes the two deadliest C bugs: returning a pointer to a stack local (it's gone after return — *use-after-return*) and forgetting to `free` heap memory (*leak*).

**Java analogy:** This maps almost exactly onto the JVM's own memory model. Java *locals and the reference variables themselves* live on the JVM stack; Java *objects* live on the JVM heap. The difference is reclamation: Java's heap is swept by the **garbage collector**, so you never `free`. In C, `free` is the GC — and *you* are it. Every `malloc` without a matching `free` is the leak the GC would have caught. Understanding this is the "aha" for why Java has a GC at all, and why `-Xmx` and OOM errors exist.

### 3. `malloc`/`free` vs Java's GC — manual memory management

**What it is:** `malloc(n)` asks for `n` bytes of heap memory and returns a pointer to it (or `NULL` if it failed — **always check**). `free(p)` returns that block to the pool. The contract is strict and unforgiving:

- Every `malloc` must be matched by **exactly one** `free`.
- After `free(p)`, `p` is a **dangling pointer** — using it is *use-after-free*, undefined behavior, and a top exploit primitive. Set it to `NULL` after freeing.
- `free`ing the same pointer twice is *double-free* — heap corruption.
- Losing the pointer without freeing is a **leak** — the memory is gone until the process exits.

```
   char *buf = malloc(100);   // ask kernel-backed pool for 100 bytes
   if (buf == NULL) { perror("malloc"); ... }   // NEVER skip this check
   // ... use buf[0..99] ...
   free(buf);                 // return it
   buf = NULL;                // defuse the dangling pointer
```

**Why it exists:** Predictable, immediate reclamation with zero background overhead. No GC pauses, no unpredictable latency — critical for kernels, real-time systems, and databases. The price is that correctness is *your* job.

**Java analogy:** `malloc` ≈ `new`, but there is **no equivalent of `free`** in Java — that's the GC's whole job, and its absence in C is the single biggest day-to-day difference. When you tune a Spring service with `-Xmx`, chase a memory leak with a heap dump, or reach for a `try-with-resources` to close a native resource, you're managing exactly the problem C makes explicit. Tools like Valgrind and AddressSanitizer (see Try This) are the C world's answer to "the GC won't save you" — they catch leaks and use-after-free at runtime.

### 4. glibc vs syscalls — `printf` vs `write`, who calls whom

**What it is:** Your program is a sandwich:

```
   ┌──────────────────────────────────────┐
   │  YOUR CODE   (main, your functions)   │  ring 3
   ├──────────────────────────────────────┤
   │  glibc / libc  (printf, malloc,       │  ring 3  ← library functions
   │                 strlen, fopen ...)    │           (man section 3)
   ├──────────────────────────────────────┤
   │  SYSCALLS  (write, read, mmap, brk)   │  the wall ← man section 2
   ├──────────────────────────────────────┤
   │            KERNEL                      │  ring 0
   └──────────────────────────────────────┘
```

`printf("x=%d\n", 42)` runs entirely in *your* address space: glibc parses the format string, converts `42` to the characters `"4","2"`, assembles the bytes into an internal buffer — all ring-3 library code, no kernel involved yet. Only when the buffer flushes does glibc call the **`write` syscall** to push bytes across the wall. So `printf` *is a customer of* `write`. Same for memory: `malloc` manages a user-space free-list and only calls the `brk`/`mmap` **syscalls** when it needs more raw memory from the kernel; most `malloc`s make *no syscall at all*. That's why `strace` (syscalls only) shows far fewer lines than `ltrace` (library calls) — most of the action is in glibc, never reaching the kernel.

**Why it exists:** Layering and efficiency. Syscalls are expensive (Module 0 — they cross the privilege wall). Buffering many `printf`s into one `write`, or many `malloc`s into one `brk`, amortizes that cost. glibc is the convenience-and-performance layer between your code and the raw kernel interface.

**Java analogy:** The JVM's standard library plays glibc's role: `System.out.println` buffers and formats in Java land, then eventually funnels down to a native `write`. `ByteBuffer`/allocation pools echo `malloc`'s user-space management. The layering instinct — "batch work in userspace, cross the boundary rarely" — is identical to why you'd batch DB writes instead of one round-trip per row.

### 5. The compilation pipeline and the ELF binary

**What it is:** `gcc hello.c -o hello` looks like one step but is four:

```
   hello.c
     │  1. PREPROCESS  (cpp): expand #include, #define, #ifdef
     ▼
   hello.i   (pure C, headers pasted in, macros expanded)
     │  2. COMPILE  (cc1): C -> assembly for this CPU
     ▼
   hello.s   (x86-64 assembly text)
     │  3. ASSEMBLE  (as): assembly -> machine code
     ▼
   hello.o   (ELF object file: machine code + unresolved symbols)
     │  4. LINK  (ld): resolve symbols, pull in libc, lay out the binary
     ▼
   hello     (ELF executable the kernel can execve)
```

The output is an **ELF** file (Executable and Linkable Format) — the standard binary container on Linux. It has a header the kernel reads during `execve` (Module 0's very first strace line), plus sections: `.text` (code), `.data` (initialized globals), `.bss` (zero-initialized globals), `.rodata` (constants/strings), and symbol/relocation tables.

**Static vs dynamic linking:** By default gcc links **dynamically** — your binary doesn't *contain* `printf`; it contains a reference to `libc.so`, which the **dynamic linker** (`ld-linux.so`) maps in at startup (those first mysterious `strace` lines in Module 0). This keeps binaries small and lets one copy of libc serve every program. **Static** linking (`gcc -static`) bakes libc *into* your binary — bigger, but self-contained (no dependency on the system's libc version). `ldd ./hello` lists the shared libraries a dynamic binary needs.

**Why it exists:** Separation of concerns (each stage does one job and is independently useful) and code sharing (dynamic linking means the whole system shares one libc in memory). ELF is the agreed container so the kernel loader, the linker, and debuggers all speak the same format.

**Java analogy:** `javac` (compile to bytecode) + the classloader (link/resolve at load time) is the JVM's two-phase version of this. Dynamic linking to `libc.so` is conceptually loading a shared `.jar` on the classpath at runtime rather than fat-jarring everything in. ELF is the native analogue of the `.class`/`.jar` format — a structured container the runtime knows how to load. **The key difference:** ELF is native machine code tied to one CPU+OS, which is *why* a Linux binary won't run on Windows, while a `.class` runs anywhere a JVM exists. `man 2` (syscalls) vs `man 3` (library) maps to "kernel API" vs "libc API" — the two documentation worlds you now live in.

---

## Code

### Program 1 — `stack_heap.c`: stack vs heap, pointers, and the two classic bugs (safely shown)

```c
/* stack_heap.c
 *
 * Demonstrates the difference between stack and heap memory, correct
 * malloc/free discipline, and WHY returning a pointer to a stack local
 * is a bug (we show the safe heap version instead). Also prints addresses
 * so you can SEE stack vs heap live in different regions.
 *
 * Compile:  gcc -Wall -Wextra -o stack_heap stack_heap.c
 * Run:      ./stack_heap
 * Bonus:    gcc -Wall -Wextra -fsanitize=address -o stack_heap stack_heap.c
 *           (AddressSanitizer will catch leaks/use-after-free if you add them)
 */

#include <stdio.h>
#include <stdlib.h>     /* malloc, free */
#include <string.h>     /* strcpy, strlen */

/* WRONG (shown as a comment so it can't bite): returns the address of a
 * local array that dies when this function returns -> use-after-return.
 *
 *   char *make_greeting_BROKEN(const char *name) {
 *       char buf[64];                 // lives on the STACK
 *       snprintf(buf, sizeof buf, "Hello, %s", name);
 *       return buf;                   // BUG: buf is gone after return!
 *   }
 */

/* RIGHT: allocate on the HEAP so the memory outlives this function.
 * The CALLER now owns it and must free() it. That ownership transfer is
 * the heart of manual memory management. */
char *make_greeting(const char *name)
{
    size_t len = strlen("Hello, ") + strlen(name) + 1;  /* +1 for '\0' */
    char *buf = malloc(len);
    if (buf == NULL) {                 /* ALWAYS check malloc */
        perror("malloc");
        return NULL;
    }
    snprintf(buf, len, "Hello, %s", name);
    return buf;                        /* heap memory survives the return */
}

int main(void)
{
    int stack_var = 42;                /* on the stack */
    int *heap_var = malloc(sizeof(int));
    if (heap_var == NULL) { perror("malloc"); return 1; }
    *heap_var = 99;

    /* Print addresses to SEE the regions (Module 7 explains the full map).
     * %p prints a pointer; cast to (void*) is the correct form. */
    printf("stack_var value=%d  at address %p\n", stack_var, (void *)&stack_var);
    printf("heap_var  value=%d  at address %p\n", *heap_var,  (void *)heap_var);
    printf("(notice the addresses are far apart: stack is high, heap is low)\n");

    char *greeting = make_greeting("Amith");
    if (greeting != NULL) {
        printf("%s\n", greeting);
        free(greeting);                /* WE own it now -> we free it */
        greeting = NULL;               /* defuse the dangling pointer */
    }

    free(heap_var);                    /* match the earlier malloc */
    heap_var = NULL;

    return 0;                          /* every malloc above has a matching free */
}
```

**Expected output (addresses vary each run):**
```
stack_var value=42  at address 0x7ffe3ab4c8ac
heap_var  value=99  at address 0x561f2c8a12a0
(notice the addresses are far apart: stack is high, heap is low)
Hello, Amith
```

**Walkthrough of the non-obvious parts:**
- The commented-out `make_greeting_BROKEN` — the classic *use-after-return* bug. `buf` lives in the function's stack frame; the moment the function returns, that frame is popped and the memory reused. Returning `buf` hands the caller a pointer to garbage. The fix is `malloc` (heap), which outlives the function. `-Wall` may even warn you about returning a local address.
- `size_t len = ... + 1` — the `+1` is for the `\0` terminator (Concept 1). Forget it and `snprintf` truncates or you under-allocate. Off-by-one on the null byte is a signature C bug.
- **Ownership transfer** — `make_greeting` mallocs but does *not* free; it *returns* the pointer, transferring ownership to `main`, which frees it. Every heap allocation needs a clear answer to "who frees this?" This discipline is what the GC does invisibly in Java.
- The far-apart addresses — `stack_var` prints a high address (`0x7ffe...`), `heap_var` a much lower one (`0x561f...`). You're seeing the memory map from Concept 2 with your own eyes: stack near the top, heap near the bottom.
- `(void *)` casts for `%p` — the correct, warning-free way to print a pointer.

### Program 2 — `funcptr.c`: function pointers and the callback pattern (a driver preview)

```c
/* funcptr.c
 *
 * Function pointers: variables that hold the ADDRESS of a function, so you
 * can pass behavior around as data. This is THE pattern behind qsort's
 * comparator, signal handlers (Module 5), and the file_operations struct
 * you'll fill in when you write a device driver (Module 14).
 *
 * Compile:  gcc -Wall -Wextra -o funcptr funcptr.c
 * Run:      ./funcptr
 */

#include <stdio.h>
#include <stdlib.h>     /* qsort */

/* A comparator: qsort will CALL BACK into this to order elements.
 * Signature is fixed by qsort: it hands us void* pointers to two elements.
 * We cast them back to the real type. Returns <0, 0, >0 like Java's
 * Comparator.compare(). */
static int cmp_int_asc(const void *a, const void *b)
{
    int x = *(const int *)a;
    int y = *(const int *)b;
    return (x > y) - (x < y);          /* branchless -1/0/+1, no overflow */
}

static int cmp_int_desc(const void *a, const void *b)
{
    int x = *(const int *)a;
    int y = *(const int *)b;
    return (y > x) - (y < x);
}

/* Our own function that takes a callback: apply `fn` to each element.
 * `void (*fn)(int)` reads as "fn is a pointer to a function taking int,
 * returning void". */
static void for_each(int *arr, size_t n, void (*fn)(int))
{
    for (size_t i = 0; i < n; i++)
        fn(arr[i]);                    /* call THROUGH the pointer */
}

static void print_one(int v) { printf("%d ", v); }

int main(void)
{
    int a[] = {5, 2, 9, 1, 7};
    size_t n = sizeof a / sizeof a[0]; /* idiomatic element count */

    /* qsort(base, count, element_size, comparator).
     * The 4th arg is a FUNCTION POINTER -- we pass behavior as data. */
    qsort(a, n, sizeof a[0], cmp_int_asc);
    printf("ascending:  "); for_each(a, n, print_one); printf("\n");

    qsort(a, n, sizeof a[0], cmp_int_desc);
    printf("descending: "); for_each(a, n, print_one); printf("\n");

    /* A function pointer is just a variable. Store it, reassign it, choose
     * at runtime -- exactly how a driver's file_operations table works. */
    int (*chosen)(const void *, const void *) = cmp_int_asc;
    printf("cmp(3,8) via pointer = %d\n", chosen(&(int){3}, &(int){8}));

    return 0;
}
```

**Expected output:**
```
ascending:  1 2 5 7 9
descending: 9 7 5 2 1
cmp(3,8) via pointer = -1
```

**Walkthrough of the non-obvious parts:**
- `int cmp_int_asc(const void *a, const void *b)` — `qsort` is generic: it sorts *anything* by calling back into a comparator you supply, receiving `void*` (typeless pointers) to two elements. You cast them to `const int *` and dereference. This is C's version of `Comparator<Integer>` — behavior passed as a value.
- `(x > y) - (x < y)` — a branchless way to return `-1/0/+1`. Note *why* not `x - y`: integer subtraction can **overflow** for large values and return the wrong sign — a subtle, real bug in naive comparators. Interviewers love this one.
- `void (*fn)(int)` — the syntax that trips everyone: read it inside-out. `fn` is a pointer (`*fn`), to a function taking `int`, returning `void`. `for_each` takes *behavior* as a parameter and calls it per element.
- `chosen(&(int){3}, &(int){8})` — a function pointer is an ordinary variable you can store and call later. `&(int){3}` is a compound literal: a throwaway `int` with value 3, address taken. This "choose the function at runtime" is *exactly* how a device driver registers `open`/`read`/`write` handlers in a `file_operations` struct (Module 14) and how `sigaction` installs a signal handler (Module 5). You're learning the mechanism now so those modules click.

---

## Under the Hood

Let's watch the compilation pipeline (Concept 5) turn `funcptr.c` into an ELF binary, then inspect it. Run each stage by hand:

```
$ gcc -E funcptr.c -o funcptr.i      # 1. preprocess only  (headers expanded)
$ wc -l funcptr.c funcptr.i          # funcptr.i is HUGE now -- stdio.h got pasted in
   40 funcptr.c
  842 funcptr.i                       # ← #include <stdio.h> expanded to ~800 lines

$ gcc -S funcptr.c -o funcptr.s      # 2. compile to assembly
$ head -20 funcptr.s                 # human-readable x86-64 -- you can see cmp_int_asc

$ gcc -c funcptr.c -o funcptr.o      # 3. assemble to an ELF object file
$ file funcptr.o
funcptr.o: ELF 64-bit LSB relocatable, x86-64 ...   # ← ELF, but not yet executable

$ gcc funcptr.o -o funcptr           # 4. link -> final ELF executable
$ file funcptr
funcptr: ELF 64-bit LSB pie executable, x86-64, dynamically linked,
         interpreter /lib64/ld-linux-x86-64.so.2 ...   # ← needs the dynamic linker
```

Now inspect the finished binary:

```
$ ldd funcptr                        # what shared libraries does it need?
    linux-vdso.so.1                  # the vDSO (Module 13 -- fast syscalls)
    libc.so.6 => /lib/x86_64-linux-gnu/libc.so.6   # ← glibc: where printf/qsort live!
    /lib64/ld-linux-x86-64.so.2      # the dynamic linker itself

$ nm funcptr | grep -E 'qsort|printf|main'
                 U printf            # 'U' = UNDEFINED: resolved from libc at load time
                 U qsort             # 'U' = qsort isn't in YOUR binary -- it's in libc.so
0000000000001229 T main             # 'T' = defined in .text, at this offset

$ objdump -d funcptr | grep -A6 '<main>:'   # disassemble main to real machine code
```

What this proves:
- **`printf` and `qsort` are NOT in your binary** — `nm` marks them `U` (undefined). They live in `libc.so.6`, which `ldd` shows your binary depends on, and the **dynamic linker** (`ld-linux`) maps them in at startup. This *is* Concept 4 (glibc vs your code) and Concept 5 (dynamic linking) made concrete. Those unexplained first lines of every Module 0 `strace` were the dynamic linker doing exactly this.
- **`funcptr.o` is ELF but "relocatable," not "executable"** — it has machine code but unresolved references (to `printf`, `qsort`) and no final memory layout. Linking (stage 4) fixes that, producing the ELF the kernel's `execve` can load.
- **`funcptr.i` ballooned to ~800 lines** — the preprocessor literally pasted all of `stdio.h` in. That's why "just add an `#include`" can slow compilation and why header hygiene matters.

The headline: **`gcc` is a pipeline, your binary is an ELF file that borrows most of its code (printf, qsort, malloc) from a shared `libc.so` at runtime, and `nm`/`ldd`/`objdump` let you see exactly what's yours versus what's the library's.** This is the native-world version of "compile, then classload."

---

## Try This

Ordered easy → hard.

1. **(Easy) See the string's bytes.** Write a tiny program: `char s[] = "hello";` then `printf("%zu\n", strlen(s));` and `printf("%zu\n", sizeof s);`. Explain why `strlen` gives 5 but `sizeof` gives 6. *Hint: `sizeof` counts the `\0`; `strlen` stops before it.*

2. **(Easy) Watch the pipeline.** Run the four `gcc -E/-S/-c` stages on `stack_heap.c` from Under the Hood. Open `stack_heap.s` and find the `call malloc` and `call free` instructions in the assembly. *Hint: `grep 'call' stack_heap.s`.*

3. **(Medium) Make the leak, then catch it.** In `stack_heap.c`, delete the `free(greeting);` line. Rebuild with AddressSanitizer: `gcc -Wall -Wextra -fsanitize=address -o stack_heap stack_heap.c`, run it, and read the leak report. Then restore the `free`. *Hint: ASan prints "detected memory leaks" with the exact allocation stack — this is the tool that replaces the GC's safety net.*

4. **(Medium) Trigger use-after-free on purpose.** After `free(heap_var); heap_var = NULL;`, add a line that does `printf("%d\n", *heap_var);`. It will crash (null deref). Now instead *don't* set it to NULL and print `*heap_var` after the free — undefined behavior, may print garbage or crash. Run both under `-fsanitize=address` and read what it says. Explain why "set the pointer to NULL after free" is a defensive habit. *Hint: NULL deref is a clean, predictable crash; use-after-free is silent corruption — the second is far worse.*

5. **(Hard) Static vs dynamic linking, measured.** Build `funcptr` two ways: `gcc -o funcptr_dyn funcptr.c` and `gcc -static -o funcptr_static funcptr.c`. Compare sizes with `ls -l` and dependencies with `ldd` on each. Explain why the static one is ~100× bigger and reports "not a dynamic executable." Which would you ship in a minimal container image, and what's the tradeoff? *Hint: static bakes libc in (big, self-contained, no runtime dependency); dynamic borrows the system's libc (small, but must exist and match at runtime).*

---

## Gotchas

- **Returning a pointer to a stack local.** The `#1` beginner-from-a-GC-language bug: `char buf[64]; ... return buf;`. The memory is reclaimed on return; the caller gets a dangling pointer. Fix: `malloc` it (and document who frees), or have the caller pass in a buffer. `-Wall` often warns; heed it.

- **Forgetting the `\0` / off-by-one on string buffers.** A C string needs `strlen + 1` bytes. Allocate `strlen(name)` and `strcpy` into it and you write one byte past the end — a buffer overflow. Always `+1`. Prefer `snprintf` (bounds-checked) over `strcpy`/`sprintf` (not).

- **Not checking `malloc`'s return.** `malloc` can return `NULL` (out of memory). Dereferencing that is a null crash at best. Every `malloc` needs an `if (p == NULL)` check — the course rule "check every syscall/allocation" applies here too. (glibc rarely returns NULL on Linux because of overcommit — Module 7 — but portable, correct code checks anyway.)

- **`sizeof` on a pointer vs an array.** `sizeof arr` inside the function where `arr` is declared as `int arr[5]` gives 20 (bytes). But pass `arr` to a function taking `int *arr` and `sizeof arr` there gives **8** (the pointer size), because arrays *decay to pointers* when passed. This silently breaks `sizeof a / sizeof a[0]` element-count idioms across function boundaries. Pass the length explicitly.

- **`x - y` in a comparator.** Using subtraction to compare ints can **overflow** and flip the sign for large operands, corrupting your sort. Use `(x > y) - (x < y)`. Classic interview trap that also appears in Java's `Comparator` (which is why `Integer.compare` exists).

- **Confusing `man 2` and `man 3`.** `write` is `man 2` (syscall); `printf`/`fwrite` are `man 3` (library). If you're debugging why bytes aren't appearing, knowing which layer you're in tells you whether to suspect buffering (library) or a real I/O error (syscall). This distinction is the whole reason for Module 3's buffered-vs-unbuffered discussion.

- **Double-free and use-after-free.** `free(p)` twice, or using `p` after `free(p)`, corrupts the heap and is a prime exploitation target. Discipline: `free(p); p = NULL;`. Freeing `NULL` is safe and does nothing, so the NULL-after-free habit also makes accidental double-frees harmless.

---

## Checkpoint

Answer from memory, then check below.

1. What is the difference between the **stack** and the **heap**, and which one does a variable declared as a plain local (`int x = 5;` inside a function) live on? What reclaims each?
2. `printf` and `write` — which is a library function and which is a system call, and what is the relationship between them when you call `printf("hi\n")`?
3. Why is returning a pointer to a local array a bug, and what's the correct fix if the caller needs that data after the function returns?
4. `char s[] = "cat";` — how many bytes does `s` occupy, what does `strlen(s)` return, and why do those two numbers differ?
5. When you run `gcc hello.c -o hello`, name the four stages that run, and explain (using `nm`/`ldd`) why `printf` does not actually appear *inside* your `hello` binary.

---

<details>
<summary><b>Checkpoint Answers</b></summary>

1. The **stack** is automatic, scoped storage for local variables and call frames — fast, small, and reclaimed automatically when a function returns. The **heap** is a large pool you request with `malloc` and must release with `free` — slower, but its contents live until you free them. A plain local like `int x = 5;` lives on the **stack** and is reclaimed automatically on return. Heap memory is reclaimed only by your explicit `free` (in Java, the GC does this for the heap).

2. **`printf` is a library function (man 3)**; **`write` is a system call (man 2)**. When you call `printf("hi\n")`, glibc formats the string entirely in user space (ring 3) into an internal buffer, and then — when it flushes — calls the **`write` syscall** to actually send the bytes across the wall to the kernel. So `printf` is built on top of `write`; `printf` may buffer and not call `write` on every invocation.

3. A local array lives in the function's **stack frame**, which is popped (and its memory reused) the instant the function returns — so the returned pointer dangles at freed memory (*use-after-return*). The fix: allocate the data on the **heap** with `malloc` and return that pointer (transferring ownership so the caller must `free` it), or have the caller pass in a buffer to fill.

4. `s` occupies **4 bytes** (`'c'`, `'a'`, `'t'`, `'\0'`). `strlen(s)` returns **3**. They differ because `strlen` counts characters *up to but not including* the null terminator, while the array must also *store* that terminating `'\0'` — so `sizeof s` is `strlen(s) + 1`.

5. The four stages: **preprocess** (expand `#include`/macros → `.i`), **compile** (C → assembly `.s`), **assemble** (assembly → ELF object `.o`), **link** (resolve symbols, pull in libc, produce the ELF executable). `printf` doesn't appear inside `hello` because gcc links **dynamically** by default: your binary only holds an *undefined reference* to `printf` (visible as `U printf` in `nm`), and the actual code lives in `libc.so.6` (visible in `ldd`), which the dynamic linker (`ld-linux`) maps in at startup.

</details>

---

*Next up: **Module 3 — File I/O: open, read, write, close.** The three-table fd model drawn in full, short reads/writes and why you must loop, `dup2` and how shell redirection actually works, and the buffered-vs-unbuffered split that is `FileInputStream` vs `BufferedInputStream`. Say **"next"** (or **"continue"**) when you're ready.*
