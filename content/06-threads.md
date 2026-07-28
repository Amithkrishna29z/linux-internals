# Module 6 — Threads

> **Estimated time:** 3–4 hours · **Core path:** Concepts 1–4 (threads share an address space, the race, mutexes, condition variables) and the `race` + `prodcons` programs are core. Semaphores and the thread-safety-vs-reentrancy distinction (Concept 5) are core-but-subtle; the memory-model/`volatile` deep-dive is a second-pass topic.
>
> **Prerequisites:** Modules 0–5. You need `fork` and the *separate* address spaces it gives (Module 5) — threads are the exact opposite and only make sense in contrast. You'll also reuse the producer/consumer intuition behind pipes (previewed in Module 5's Try This, paid off fully in Module 8).

---

## The Big Picture

Module 5 gave you `fork`: a new process with its **own copy** of memory. Change a variable in the child, the parent never sees it. That isolation is safe but expensive to coordinate — to share data between processes you need explicit plumbing (pipes, shared memory, Module 8). Threads are the other bargain: **many threads inside one process, all sharing the same address space.** Every thread sees the same globals, the same heap, the same open file descriptors — the same everything except its own stack and registers. Sharing is free; that's the whole appeal, and also the whole danger.

Start with the shape of it. `pthread_create` spawns a new thread that begins running a function you hand it; `pthread_join` waits for a thread to finish and collects its return value (the thread-level echo of `waitpid`). All the threads run "at the same time" — on a multicore machine, *genuinely* in parallel, on different CPUs. And because they share memory, one thread can hand another a pointer and they're both looking at the same bytes. Coming from Java this is deeply familiar: `new Thread(runnable).start()` and `thread.join()` are `pthread_create`/`pthread_join`, and Java threads share the heap exactly the way pthreads share the address space. This is the one module where your Java instincts mostly transfer — the concepts are the same, only the syntax is rawer.

Then comes the bill. When two threads touch the same memory and at least one writes, without coordination, you have a **race condition** — and the classic demonstration is a shared counter incremented by two threads. `count++` looks atomic but is really *load, add, store*: three steps the scheduler can interleave, so two threads can both load `5`, both compute `6`, and both store `6` — one increment vanishes. Run it and you'll see a final count *less* than expected, differently wrong each run. This non-determinism is the defining misery of concurrency: the bug appears once in a thousand runs, never on your machine, always in production.

The fix is **mutual exclusion**: a **mutex** (mutex = *mut*ual *ex*clusion) that only one thread can hold at a time. Lock it before touching shared data, unlock after; whoever arrives second blocks until the first releases. That's Java's `synchronized` block, unrolled into explicit `pthread_mutex_lock`/`unlock` calls. But mutexes only solve "don't touch it at the same time." The other half of coordination is **waiting for a condition** — "block until the buffer is non-empty." Spinning in a loop checking burns CPU; the right tool is a **condition variable**, which lets a thread *sleep* until another thread signals that something changed. That's Java's `wait()`/`notify()`, and it comes with the same infamous rule: **always re-check your condition in a `while` loop, never an `if`** (spurious wakeups are real). We'll tie mutex + condvars together into a **producer–consumer bounded buffer** — the single most important concurrency pattern, the thing underneath every thread pool, message queue, and `BlockingQueue` you've ever used. Build it once by hand and you'll understand all of them.

---

## Concepts

### 1. `pthread_create`/`pthread_join`: threads share one address space

**What it is:** A **thread** is an independent flow of execution *inside* a process. `pthread_create` starts a new one running a function you pass; `pthread_join` blocks until it finishes and retrieves its return value. All threads of a process share the **same address space** — globals, heap, and open file descriptors are common — but each thread has its **own stack** and its **own registers** (so each has its own local variables and its own place in the code).

```
   ONE PROCESS
   ┌────────────────────────────────────────────────┐
   │  CODE (shared)   GLOBALS (shared)   HEAP (shared)│
   │        fds (shared)                              │
   │  ┌──────────┐  ┌──────────┐  ┌──────────┐        │
   │  │ thread 0 │  │ thread 1 │  │ thread 2 │        │
   │  │ own stack│  │ own stack│  │ own stack│        │
   │  │ own regs │  │ own regs │  │ own regs │        │
   │  └──────────┘  └──────────┘  └──────────┘        │
   └────────────────────────────────────────────────┘
     ↕ contrast: fork() gives each PROCESS its OWN copy of all of the above
```

```c
   void *worker(void *arg) {        /* every thread runs a function like this */
       long id = (long)arg;
       return (void *)(id * 2);     /* the "return value" join collects */
   }
   pthread_t t;
   pthread_create(&t, NULL, worker, (void *)3);   /* start it */
   void *ret;
   pthread_join(t, &ret);           /* wait; ret == (void*)6 */
```

**Why it exists:** Two reasons. **Parallelism** — on N cores, N threads do N times the work (for CPU-bound tasks). **Shared-state concurrency** — threads that need to work on the *same* data (a shared cache, a connection pool) can just... share it, with no serialization/IPC overhead. `fork` gives isolation; threads give sharing. You pick based on whether the tasks need to cooperate on common data.

**Java analogy:** Direct. `pthread_create(&t, NULL, worker, arg)` ≈ `new Thread(() -> worker(arg)).start()`. `pthread_join(t, &ret)` ≈ `thread.join()` (Java's join returns void; the pthread version also hands back the thread's return value). Java threads share the heap exactly as pthreads share the address space — a `static` field in Java is a global in C: visible to every thread, and therefore a race waiting to happen. The `Runnable`/`Callable` you pass is the `worker` function.

### 2. The race condition: why `count++` is a lie

**What it is:** A **race condition** exists when two threads access the same memory concurrently, at least one writes, and the result depends on the unpredictable *timing* of their interleaving. The canonical example: two threads each increment a shared `counter` a million times. You expect 2,000,000. You get less — and a *different* less each run.

```
   counter++  is NOT one step. The CPU does:

     thread A                    thread B
     ─────────                   ─────────
     load  counter (5)  ──┐
                          │ ...B runs here...
                          │      load  counter (5)     ← reads the SAME 5
     add   1  -> 6        │      add   1  -> 6
     store counter (6) ───┘      store counter (6)     ← both store 6

   Two increments happened; counter went 5 -> 6. One update LOST.
```

**Why it exists (why you must care):** It doesn't "exist for a reason" — it's the *hazard* that shared memory creates. The insidious part is statistics: the load-add-store window is a few nanoseconds, so the bad interleaving is rare. Your test passes, code review passes, it runs fine for months, then corrupts data under load. Concurrency bugs are non-deterministic, environment-dependent, and often invisible to the debugger (adding a `printf` changes the timing and "fixes" it — a *Heisenbug*). The only real defense is to *never* leave shared mutable state unsynchronized, by discipline, not by testing.

**Java analogy:** Identical. `count++` on a shared `int` (or even a `long`, which isn't even atomic to *read*) is the textbook Java data race. Java's answers are `synchronized`, `java.util.concurrent.atomic.AtomicInteger`, or `volatile` (for visibility, not compound actions). The C answers are a mutex or C11 `<stdatomic.h>`. The bug — and the reason it's hard — is exactly the same in both worlds; only the fix's spelling differs.

### 3. Mutexes: mutual exclusion, one holder at a time

**What it is:** A **mutex** (mutual exclusion lock) is an object that at most one thread can *hold* at once. `pthread_mutex_lock` acquires it (blocking if another thread holds it); `pthread_mutex_unlock` releases it. The code between lock and unlock is a **critical section** — guaranteed to run without another thread simultaneously in *its* critical section for the same mutex. Wrap every access to shared data in the same mutex and races vanish.

```
   pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;

   pthread_mutex_lock(&m);      // ── enter critical section (others wait)
   counter++;                   //    now the load-add-store is indivisible
   pthread_mutex_unlock(&m);    // ── leave; a waiting thread may now enter

     thread A        thread B
     lock ✓          lock … (blocks, B sleeps)
     counter++       …still blocked…
     unlock          lock ✓  (wakes, acquires)
                     counter++
                     unlock
   -> increments are now SERIALIZED; no update is lost.
```

**Why it exists:** To make a multi-step operation on shared data **atomic** with respect to other threads — to restore the illusion that `counter++` happens all-at-once. It's the most basic concurrency primitive; almost everything else (condvars, thread-safe data structures) is built on top of it. The cost is serialization: threads waiting on a lock aren't doing work, so a too-coarse lock throttles parallelism (Concept 5 / Gotchas).

**Java analogy:** `pthread_mutex_lock(&m); ...; pthread_mutex_unlock(&m)` **is** a `synchronized (obj) { ... }` block — with one crucial difference: Java's `synchronized` unlocks *automatically* when the block exits, even via exception or early return. C does not. **You** must call `unlock` on every path out, including error returns — forget one and you deadlock the next thread forever. `ReentrantLock` with an explicit `lock()`/`unlock()` in a `try/finally` is the closer analogy, and the `finally` is exactly the discipline C forces you to keep by hand.

### 4. Condition variables: sleep until something changes

**What it is:** A **condition variable** lets a thread *wait* (sleep, using no CPU) until another thread signals that some shared condition may have become true. It's always paired with a mutex. `pthread_cond_wait(&cond, &mutex)` **atomically** unlocks the mutex and sleeps; when woken, it re-locks the mutex before returning. Another thread calls `pthread_cond_signal` (wake one waiter) or `pthread_cond_broadcast` (wake all) after changing the shared state.

```
   // CONSUMER waits for the buffer to be non-empty:
   pthread_mutex_lock(&m);
   while (count == 0)                       // ← WHILE, not if! (see below)
       pthread_cond_wait(&not_empty, &m);   // unlocks m + sleeps; re-locks on wake
   item = buffer[--count];
   pthread_mutex_unlock(&m);

   // PRODUCER, after adding an item:
   pthread_mutex_lock(&m);
   buffer[count++] = item;
   pthread_cond_signal(&not_empty);         // wake a waiting consumer
   pthread_mutex_unlock(&m);
```

**Why the `while`, not `if`:** Two reasons, both real. (1) **Spurious wakeups** — `pthread_cond_wait` is permitted to return *without* any signal; the standard allows it, and real systems do it. (2) **Stolen conditions** — between the signal and the woken thread re-acquiring the mutex, a *third* thread may have grabbed the item, so the condition is false again. A `while` re-checks the predicate after every wake and goes back to sleep if it's still false. An `if` assumes one wake means the condition holds — a classic, catastrophic bug.

**Why it exists:** To wait efficiently. The naive alternative — a **busy-wait** `while (count == 0) ;` — pins a CPU core at 100% doing nothing, and starves the very thread that would change the condition. A condition variable puts the waiter to sleep so the OS can run useful work, and wakes it precisely when there's a reason to re-check. It's the "block until ready" primitive.

**Java analogy:** `pthread_cond_wait` = `obj.wait()`, `pthread_cond_signal` = `obj.notify()`, `pthread_cond_broadcast` = `obj.notifyAll()` — and Java's `wait()` must *also* be called inside a `while` loop for the exact same spurious-wakeup reason (it's right there in the `Object.wait` Javadoc). Java couples the monitor lock and condition into one object; pthreads make the mutex and condvar separate, so you pass both to `cond_wait`. `java.util.concurrent`'s `Condition` (from `ReentrantLock.newCondition()`) is the one-to-one match: `await()`/`signal()`/`signalAll()`.

### 5. Semaphores, thread-safety, and reentrancy

**What it is:** Three related ideas that round out the toolkit.

A **semaphore** is a counter with two atomic operations: `sem_wait` (decrement; block if it would go negative) and `sem_post` (increment; wake a waiter). A mutex is essentially a semaphore capped at 1 ("binary semaphore"); a counting semaphore with initial value N lets *N* threads through at once — perfect for "at most N concurrent connections" or counting free slots in a buffer.

**Thread-safe** means a function can be called by multiple threads concurrently without corrupting shared state (usually because it locks internally, or touches no shared state). **Reentrant** means a function can be safely re-entered before a prior call completes — including from a signal handler (Module 5) or recursion — which requires it to use *no* static/global state and no locks that could deadlock. The two overlap but differ:

```
                     uses no shared state    locks internally
   reentrant              ✓                       ✗ (a lock isn't reentrant-safe
                                                     from a signal handler)
   thread-safe            ✓                       ✓
```

The classic offender is `strtok` (holds state between calls in a static variable): **not** thread-safe *and* not reentrant. Its fix, `strtok_r`, takes a caller-provided `char **saveptr` so the state lives on the caller's stack — the `_r` suffix ("reentrant") marks the whole family: `strtok_r`, `rand_r`, `readdir_r`, `localtime_r`.

**Why it exists:** Semaphores generalize the mutex to "N permits" and cleanly express resource *counting*. The thread-safe/reentrant distinction exists because sharing turns previously-innocent functions (anything with a `static` buffer) into hazards; the library grew `_r` variants once threads arrived.

**Java analogy:** `java.util.concurrent.Semaphore` is `sem_wait`/`sem_post` = `acquire()`/`release()`, initialized with a permit count. Thread-safety is the property behind `Collections.synchronizedList`, `ConcurrentHashMap`, and the "is this class thread-safe?" question in every Java API doc. Reentrancy has a subtler Java echo: `ReentrantLock` is named for a *different* reentrancy (the same thread re-acquiring a lock it already holds), but the "no shared static state" flavor is what makes `SimpleDateFormat` infamously **not** thread-safe (mutable static-ish internal state) while `DateTimeFormatter` (Java 8+, immutable) is. Same lesson: hidden mutable state breaks under threads.

---

## Code

### Program 1 — `race.c`: watch an increment get lost, then fix it with a mutex

```c
/* race.c
 *
 * Demonstrates a data race and its fix. Two threads each increment a shared
 * counter N times. WITHOUT a mutex the final total is less than 2*N (lost
 * updates); WITH the mutex it is exactly 2*N, every time.
 *
 * Compile:  gcc -Wall -Wextra -pthread -o race race.c
 * Run:      ./race            (racy: total < 2000000, varies each run)
 *           ./race -lock      (locked: total == 2000000, always)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#define ITERS 1000000

static long counter = 0;                 /* SHARED between both threads */
static int use_lock = 0;                 /* toggled by the -lock argument */
static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;

static void *bump(void *arg)
{
    (void)arg;
    for (int i = 0; i < ITERS; i++) {
        if (use_lock) pthread_mutex_lock(&m);
        counter++;                       /* load, add, store -- not atomic! */
        if (use_lock) pthread_mutex_unlock(&m);
    }
    return NULL;
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "-lock") == 0)
        use_lock = 1;

    pthread_t a, b;
    pthread_create(&a, NULL, bump, NULL);
    pthread_create(&b, NULL, bump, NULL);
    pthread_join(a, NULL);               /* wait for BOTH before reading */
    pthread_join(b, NULL);

    long expected = 2L * ITERS;
    printf("counter = %ld  (expected %ld)  %s%s\n",
           counter, expected,
           use_lock ? "[locked]" : "[racy]",
           counter == expected ? "  OK" : "  <-- LOST UPDATES");
    return 0;
}
```

**Expected output:**
```
$ ./race
counter = 1487213  (expected 2000000)  [racy]  <-- LOST UPDATES
$ ./race
counter = 1633090  (expected 2000000)  [racy]  <-- LOST UPDATES     # different!
$ ./race -lock
counter = 2000000  (expected 2000000)  [locked]  OK
```

**Walkthrough of the non-obvious parts:**
- `counter` is a `static` global — that's what makes it *shared*. If it were a local in `bump`, each thread would have its own copy on its own stack and there'd be no race (and no useful work either).
- The racy run prints a *different* wrong number each time, and *never* the same wrong number twice — that non-determinism **is** the signature of a data race. If you get 2000000 racy occasionally, your machine just got lucky on the interleaving; it's still a bug.
- `-pthread` (not `-lpthread`) at compile time sets both the preprocessor defines and links the threading library. Forgetting it gives you link errors or, worse, a silently non-functional stub.
- The mutex serializes the load-add-store so no update is lost — but notice the locked version is *much slower* (millions of lock/unlock pairs). That's the cost of synchronization; real code locks around bigger chunks of work, not single increments (for a bare counter, C11 `atomic_long` or `AtomicLong` in Java is the right tool).
- We `pthread_join` **both** threads before reading `counter` — joining establishes a *happens-before* edge, so main is guaranteed to see the threads' final writes. Reading a shared value while threads still run is itself a race.

### Program 2 — `waitcond.c`: a condition variable done right (the `while` matters)

```c
/* waitcond.c
 *
 * A minimal condition-variable demo. The main thread waits until a worker
 * sets `ready`, using the correct mutex + while-loop + cond_wait pattern.
 * Shows why the predicate is re-checked in a while (spurious wakeups).
 *
 * Compile:  gcc -Wall -Wextra -pthread -o waitcond waitcond.c
 * Run:      ./waitcond
 */

#include <stdio.h>
#include <pthread.h>
#include <unistd.h>

static pthread_mutex_t m   = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  cv  = PTHREAD_COND_INITIALIZER;
static int ready = 0;                       /* the shared PREDICATE */

static void *worker(void *arg)
{
    (void)arg;
    sleep(1);                               /* simulate slow setup work */

    pthread_mutex_lock(&m);
    ready = 1;                               /* change the shared state... */
    pthread_cond_signal(&cv);                /* ...then wake the waiter */
    pthread_mutex_unlock(&m);
    printf("worker: signalled ready\n");
    return NULL;
}

int main(void)
{
    pthread_t t;
    pthread_create(&t, NULL, worker, NULL);

    pthread_mutex_lock(&m);
    while (!ready)                           /* WHILE: re-check after every wake */
        pthread_cond_wait(&cv, &m);          /* unlocks m, sleeps, re-locks on wake */
    pthread_mutex_unlock(&m);

    printf("main: observed ready, proceeding\n");
    pthread_join(t, NULL);
    return 0;
}
```

**Expected output:**
```
$ ./waitcond
worker: signalled ready
main: observed ready, proceeding
```
(The worker's line prints ~1 second in; then main wakes and proceeds.)

**Walkthrough of the non-obvious parts:**
- `pthread_cond_wait(&cv, &m)` is called **with the mutex held**, and it *atomically* releases the mutex and sleeps — that atomicity is the whole point. If releasing and sleeping were two steps, the signal could slip in between (the "lost wakeup" bug) and main would sleep forever. The condvar API fuses them so that can't happen.
- The predicate `ready` is checked in a **`while`**, not an `if`. Here the logic looks like an `if` would suffice, but the `while` is mandatory: `cond_wait` may return spuriously, and re-checking `ready` sends it back to sleep harmlessly. Train the `while` into your fingers — it is *never* wrong and an `if` is *sometimes* catastrophically wrong.
- The worker changes `ready` **and** signals **while holding the mutex**. Signalling without the lock can work but invites the lost-wakeup race in more complex code; "hold the lock across the state change and the signal" is the safe default.
- `cond_wait` **re-locks** the mutex before returning, which is why `main` can safely read `ready` and then must `unlock` afterward. The mutex is held every time the predicate is evaluated.

### Project — `prodcons.c`: a producer–consumer bounded buffer

```c
/* prodcons.c
 *
 * The canonical concurrency pattern: a fixed-size ring buffer shared by a
 * producer (puts items in) and a consumer (takes them out). Two condition
 * variables coordinate: `not_full` (producer waits when the buffer is full)
 * and `not_empty` (consumer waits when it's empty). This is a BlockingQueue
 * built by hand -- the core of every thread pool and message queue.
 *
 * Compile:  gcc -Wall -Wextra -pthread -o prodcons prodcons.c
 * Run:      ./prodcons
 */

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>

#define CAP   4          /* buffer capacity (small, to force blocking) */
#define TOTAL 12         /* how many items the producer will make */

static int   buf[CAP];
static int   count = 0;  /* items currently in the buffer */
static int   head  = 0;  /* next slot to write */
static int   tail  = 0;  /* next slot to read */

static pthread_mutex_t m         = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  not_full  = PTHREAD_COND_INITIALIZER;
static pthread_cond_t  not_empty = PTHREAD_COND_INITIALIZER;

static void *producer(void *arg)
{
    (void)arg;
    for (int i = 0; i < TOTAL; i++) {
        pthread_mutex_lock(&m);
        while (count == CAP)                       /* buffer full: wait */
            pthread_cond_wait(&not_full, &m);
        buf[head] = i;
        head = (head + 1) % CAP;
        count++;
        printf("produced %2d   (buffer now %d/%d)\n", i, count, CAP);
        pthread_cond_signal(&not_empty);           /* a consumer may proceed */
        pthread_mutex_unlock(&m);
    }
    return NULL;
}

static void *consumer(void *arg)
{
    (void)arg;
    for (int i = 0; i < TOTAL; i++) {
        pthread_mutex_lock(&m);
        while (count == 0)                          /* buffer empty: wait */
            pthread_cond_wait(&not_empty, &m);
        int item = buf[tail];
        tail = (tail + 1) % CAP;
        count--;
        printf("          consumed %2d   (buffer now %d/%d)\n", item, count, CAP);
        pthread_cond_signal(&not_full);             /* a producer may proceed */
        pthread_mutex_unlock(&m);
    }
    return NULL;
}

int main(void)
{
    pthread_t p, c;
    pthread_create(&p, NULL, producer, NULL);
    pthread_create(&c, NULL, consumer, NULL);
    pthread_join(p, NULL);
    pthread_join(c, NULL);
    printf("done: produced and consumed %d items\n", TOTAL);
    return 0;
}
```

**Expected output (exact interleaving varies, but count never exceeds CAP or drops below 0):**
```
$ ./prodcons
produced  0   (buffer now 1/4)
produced  1   (buffer now 2/4)
          consumed  0   (buffer now 1/4)
produced  2   (buffer now 2/4)
produced  3   (buffer now 3/4)
produced  4   (buffer now 4/4)
          consumed  1   (buffer now 3/4)
produced  5   (buffer now 4/4)
          consumed  2   (buffer now 3/4)
...
          consumed 11   (buffer now 0/4)
done: produced and consumed 12 items
```

**Walkthrough of the non-obvious parts:**
- **Two condition variables, one mutex.** The single mutex `m` protects *all* shared state (`buf`, `count`, `head`, `tail`). The two condvars name the two distinct reasons to wait: producer waits on `not_full`, consumer waits on `not_empty`. Using one condvar for both would force `broadcast` and needless wakeups; two condvars wake exactly the right waiter.
- **Each wait is a `while` loop over its predicate** (`count == CAP` / `count == 0`) — same rule as Program 2, now load-bearing: with multiple producers/consumers the woken thread must re-verify the buffer state, because another thread of the same kind may have raced in first.
- **Signal the *opposite* condition after each operation.** After producing, the buffer is non-empty, so signal `not_empty` to wake a consumer. After consuming, there's a free slot, so signal `not_full` to wake a producer. Cross-signalling is what keeps both sides making progress.
- **The ring buffer** (`head`/`tail`/`% CAP`) reuses the array circularly so no data shifting is needed — `count` alone tells us full (`== CAP`) vs empty (`== 0`). All of it is touched only inside the critical section, so it's race-free.
- This is a `java.util.concurrent.ArrayBlockingQueue` in miniature. When you call `queue.put()` and it blocks because the queue is full, *this* is the machinery underneath — a bounded buffer, a lock, and two conditions.

---

## Under the Hood

Compile and run `strace -f ./race` (the `-f` follows threads, which are just processes sharing memory to the kernel). Two things jump out: how a thread is *created*, and what a *contended mutex* actually does.

```
clone(child_stack=0x7f..., flags=CLONE_VM|CLONE_FS|CLONE_FILES|CLONE_SIGHAND
      |CLONE_THREAD|CLONE_SETTLS|CLONE_PARENT_SETTID|CLONE_CHILD_CLEARTID,
      parent_tid=..., tls=..., child_tid=...) = 24522                 ← [1] a THREAD
...
futex(0x55..., FUTEX_WAIT_PRIVATE, 2, NULL) = 0                       ← [2] blocked on a mutex
futex(0x55..., FUTEX_WAKE_PRIVATE, 1) = 1                             ← [3] releasing it
```

Annotated:
1. **`clone(... CLONE_VM|CLONE_THREAD ...)`** — a thread is created by the *same* `clone` syscall as `fork` (Module 5), but with a very different flag set. `CLONE_VM` = **share the address space** (don't copy it — this is the entire difference from fork). `CLONE_FILES` = share the fd table. `CLONE_THREAD` = put the new task in the *same thread group* (same PID, distinct thread ID). Contrast Module 5's fork, which was `clone(... SIGCHLD)` with *none* of the sharing flags — that's copy-on-write separate memory. **Same syscall, opposite bargain:** the flags decide whether you get a process or a thread. To the Linux kernel, a thread is just a task that happens to share its `mm_struct` with its siblings.
2–3. **`futex(...)`** — a mutex is *not* a kernel object you syscall into on every lock. In the common **uncontended** case, `pthread_mutex_lock` is a single atomic compare-and-swap in user space — *no syscall at all*, which is why locking is cheap when there's no contention. Only when a thread must actually **block** (the lock is held) does it call `futex(FUTEX_WAIT)` to sleep in the kernel [2]; the unlocker calls `futex(FUTEX_WAKE)` to wake one waiter [3]. **futex = "fast userspace mutex"**: fast path in user space, kernel involvement only on contention. Condition variables are built on the same `futex` primitive.

The headline: **threads and processes are the same `clone` syscall with different sharing flags, and a mutex is a userspace atomic backed by `futex` only when it has to block.** Run `strace -f -e trace=clone,futex ./prodcons` and you'll watch the two worker threads spawn and then ping-pong through `futex` waits as the buffer fills and drains — the condvar coordination made visible at the syscall level.

---

## Try This

Ordered easy → hard.

1. **(Easy) Reproduce the race, then defeat it.** Run `./race` five times and record the five different wrong totals — internalize that a data race is *non-deterministic*. Then run `./race -lock` five times and confirm it's always exactly 2000000. *Hint: if a racy run ever hits 2000000, run it again — it's luck, not correctness.*

2. **(Easy) Pass real arguments to a thread.** Spawn 4 threads, giving each its index `i` (0–3) so each prints "thread i". Do it the **wrong** way first — pass `&i` where `i` is the loop variable — and watch threads print duplicate/garbage indices (they all read the same changing variable). Then fix it by passing `(void *)(long)i` by value. *Hint: this is the #1 beginner threading bug; the loop variable is shared, so by the time the thread reads it, it's changed.*

3. **(Medium) Turn the counter atomic.** Replace the mutex in `race.c` with C11 atomics: `#include <stdatomic.h>`, make `counter` an `atomic_long`, and use `counter++` (or `atomic_fetch_add`). Confirm it's correct *and* faster than the mutex version. Explain why atomics beat a mutex here but not for multi-step operations. *Hint: an atomic add is one lock-free CPU instruction; a mutex is a whole acquire/release protocol.*

4. **(Medium) Make prodcons multi-threaded on both ends.** Run 3 producers and 2 consumers sharing the one buffer (adjust `TOTAL` so counts balance). Confirm no item is lost or duplicated and `count` never leaves `[0, CAP]`. *Hint: this is exactly why the waits are `while` loops — with multiple consumers, a woken one may find the item already taken.*

5. **(Hard) Provoke a deadlock, then break it.** Write two mutexes `A` and `B`. Thread 1 locks A then B; thread 2 locks B then A. Run it until it hangs (both threads holding one lock, each waiting for the other) — that's a **deadlock**. Then fix it by **lock ordering**: make *both* threads always acquire A before B. Explain why a consistent global lock order prevents all such deadlocks. *Hint: deadlock needs a cycle in "who waits for whom"; a total order on locks makes a cycle impossible.*

---

## Gotchas

- **Forgetting `-pthread` at compile time.** Symptoms range from link errors (`undefined reference to pthread_create`) to subtle misbehavior. Always compile *and* link with `-pthread` (it does more than `-lpthread`: it also sets thread-safe preprocessor flags). This is the first thing to check when threaded code acts weird.

- **`if` instead of `while` around `cond_wait`.** The single most common condvar bug. Spurious wakeups and stolen conditions mean a wake does **not** guarantee the predicate is true. Always loop: `while (!predicate) pthread_cond_wait(&cv, &m);`. An `if` works 99% of the time and then corrupts state at 3 a.m.

- **Unlocking on only *some* paths.** Unlike Java's `synchronized` (auto-unlocks on any exit), C makes you unlock manually on **every** return/break/error path. Miss one and the next thread blocks forever. Keep critical sections short and single-exit, or use a `goto unlock:` cleanup label. This is the tax for not having `try/finally`.

- **Passing `&loop_variable` to `pthread_create`.** The loop variable is shared and keeps changing; by the time the new thread dereferences the pointer, the value has moved. Pass small values *by value* cast through `(void *)(long)`, or give each thread its own heap-allocated argument struct. Classic "all my threads printed the same number" bug.

- **Reading shared data after the threads without joining.** If `main` reads `counter` while workers might still be running (or without a join/lock establishing happens-before), it's a race even on the read side — you may see a stale value. `pthread_join` (or a lock, or an atomic) creates the memory-ordering guarantee that makes the final writes visible.

- **Deadlock from inconsistent lock ordering.** Two threads acquiring the same two locks in opposite orders will eventually deadlock. Prevent it by defining a **global lock order** and acquiring locks in that order everywhere. (Also: never call unknown code — a callback — while holding a lock; it might try to grab a lock you hold.)

- **Using non-reentrant library functions across threads.** `strtok`, `rand`, `localtime`, `readdir`, `gmtime` keep internal `static` state and corrupt each other across threads. Use the `_r` variants (`strtok_r`, `rand_r`, `localtime_r`, `readdir_r`). The `strtok` in your `minishell` (Module 5) is fine *there* because that shell is single-threaded — but drop it into a threaded server and it's a landmine.

- **Over-coarse locking = no parallelism.** One giant lock around everything is easy and *correct*, but serializes all threads — you paid for N cores and use one. The art is locking as *little* as possible for as *short* as possible (fine-grained locks, or lock-free structures) without introducing the ordering bugs above. Correctness first, then measure, then narrow the locks.

---

## Checkpoint

1. Threads share almost everything in a process but not *everything*. What exactly is shared, and what does each thread get its own private copy of? How does this contrast with what `fork` gives you?
2. Explain precisely why `counter++` on a shared variable can lose updates across two threads. At the machine level, what are the steps, and what interleaving causes the loss?
3. Why must the predicate check around `pthread_cond_wait` be a `while` loop and not an `if`? Give the two distinct reasons a wake can happen when the predicate is still false.
4. In the producer–consumer buffer, why are there **two** condition variables but only **one** mutex? What does each condvar mean, and which one does each side signal?
5. What's the difference between a function being *thread-safe* and being *reentrant*? Give an example of a standard library function that is neither, and name its safe replacement.

---

<details>
<summary><b>Checkpoint Answers</b></summary>

1. All threads of a process **share the address space** — the same code, global/`static` variables, heap, and open file descriptors. Each thread has its **own stack** (so its own local variables and call chain) and its **own registers** (its own CPU state / instruction pointer). This is the exact opposite of `fork`, which gives the new process its **own copy** of the entire address space (copy-on-write), so parent and child *cannot* see each other's memory changes. Threads = share by default; processes = isolate by default.

2. `counter++` compiles to three steps: **load** `counter` from memory into a register, **add** 1, **store** the register back to memory. It is not atomic. If thread A loads `5`, then thread B also loads `5` before A stores, both compute `6` and both store `6`. Two increments occurred but `counter` advanced by only one — one update is **lost**. The loss depends on the exact timing of the interleaving, which is why the final total is non-deterministic and usually *less* than expected.

3. Because a return from `pthread_cond_wait` does **not** guarantee the predicate is true, for two reasons: (1) **spurious wakeups** — the standard explicitly permits `cond_wait` to return without any signal, and real implementations do; (2) **stolen wakeups** — between the signal and the woken thread re-acquiring the mutex, another thread may have already consumed the condition (e.g. taken the item), so it's false again. A `while` re-evaluates the predicate after every wake and re-sleeps if it's still false; an `if` would proceed on a false predicate and corrupt state.

4. One mutex protects **all** the shared state (the buffer, `count`, `head`, `tail`) — using two mutexes for the same data would allow concurrent corruption. The two condition variables name the two distinct *reasons a thread waits*: `not_full` (the producer waits here when `count == CAP`) and `not_empty` (the consumer waits here when `count == 0`). Each side signals the **opposite** condition after acting: the producer signals `not_empty` after adding an item (a consumer can now proceed); the consumer signals `not_full` after removing one (a producer can now proceed). Two condvars let each `signal` wake exactly the right kind of waiter instead of broadcasting to everyone.

5. **Thread-safe** = safe to call concurrently from multiple threads without corrupting shared state (typically via internal locking or by touching no shared state). **Reentrant** = safe to re-enter before a prior call returns, including from a signal handler or recursion — which requires *no* static/global state and no locks (a lock held by the interrupted call would deadlock the reentrant one). A thread-safe function that locks internally is **not** necessarily reentrant (unsafe from a signal handler); a function using no shared state is both. `strtok` is **neither** (it keeps parsing state in a `static` variable); its safe replacement is `strtok_r`, which stores that state in a caller-provided `char **saveptr`. The `_r` family (`rand_r`, `localtime_r`, `readdir_r`) exists for exactly this reason.

</details>

---

*Next up: **Module 7 — Memory Management.** The virtual address space layout (text/data/bss/heap/stack), `malloc`/`free` and what the allocator really does, `brk`/`sbrk` vs `mmap`, page faults and demand paging, memory leaks and use-after-free, and the tools that catch them (`valgrind`, ASan). The Java contrast is the big one: you've had a garbage collector your whole life, and now you meet the world where `free` is yours to call — and to forget. Continuing straight on.*
