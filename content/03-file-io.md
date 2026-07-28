# Module 3 — File I/O: open, read, write, close

> **Estimated time:** 3–4 hours · **Core path:** Concepts 1–4 and both code programs are core — this is the heart of the whole course. The buffered-vs-unbuffered deep-dive (Concept 5) and the three-table kernel walk are important; do them on a second pass if short on time.
>
> **Prerequisites:** Modules 0–2. You know the fd is the spine, that syscalls cross the wall, and how pointers/buffers work in C. Now we make the fd do real work.

---

## The Big Picture

This is the module the whole course has been pointing at. In Module 1 I told you "everything is a file" and "the file descriptor is the spine." Now you'll feel it in your hands. A file descriptor is a small integer, and with just five syscalls — `open`, `read`, `write`, `close`, `lseek` — you can manipulate regular files, and (with almost no changes) pipes, sockets, terminals, and devices in every later module. Learn this handful of calls deeply and you've learned the *shape* of Unix I/O forever. Everything else is variation.

Here's the mental model that unlocks it. When you `open("data.txt")`, three data structures light up inside the kernel, arranged in a chain: your process's private **fd table** points to a shared, system-wide **open file table** entry, which points to an **inode** (the file's actual identity on disk). This three-table picture explains almost every "weird" I/O behavior you'll ever hit — why two processes reading the same file have independent positions, why `dup2` makes shell redirection work, why a file deleted while open keeps taking disk space. Draw this diagram once and internalize it; I'll reference it for the rest of the course.

The second big idea is that **`read` and `write` are allowed to do less than you asked**, and this trips up every single beginner. You ask to write 8000 bytes; `write` returns 4096 and says "that's all I did for now." You ask to read 1000 bytes; `read` returns 200. These aren't errors — they're *short reads* and *short writes*, and they're normal, especially on pipes, sockets, and signals. The kernel is telling you the truth about what it managed to do this instant. The consequence: **you must loop.** Never assume one `read` or `write` moved all your bytes. Getting this wrong produces bugs that work fine on small local files and then corrupt data the moment you point them at a network socket — which is exactly the kind of bug that survives to production. We'll build the loop correctly and you'll never unlearn it.

The third idea reconnects to Module 2's glibc-vs-syscall theme, now applied to I/O: there are **two whole I/O worlds** in a Unix program. There's the raw syscall layer (`open`/`read`/`write` on integer fds — *unbuffered*, every call crosses the wall) and there's the C standard library's *buffered* stdio layer (`fopen`/`fread`/`fwrite`/`printf` on `FILE*` handles, which batch your data and cross the wall rarely). This split is *exactly* Java's `FileInputStream` (raw) versus `BufferedInputStream` (buffered) — you already know the tradeoff, you just didn't know it lived here too. And this split is the source of one of the most infamous bugs in all of C: the "printf before fork prints twice" bug, which we'll reproduce and dissect, because understanding it means you truly understand buffering.

By the end you'll open files with the right flags, loop `read`/`write` correctly, understand inodes and the difference between hard links and symlinks, know how `>` redirection actually works (it's `dup2`), and know precisely when to reach for buffered vs unbuffered I/O. This is the module you'll come back to more than any other.

---

## Concepts

### 1. File descriptors and the three tables

**What it is:** When you `open()` a file, the kernel sets up a chain of three data structures:

```
   PROCESS A                          SYSTEM-WIDE                      ON DISK
   ┌─────────────────┐        ┌──────────────────────────┐     ┌───────────────┐
   │ fd table (priv) │        │  OPEN FILE TABLE (shared) │     │ INODE TABLE   │
   │  0 ─► stdin     │        │  entry #12:               │     │ inode 8801:   │
   │  1 ─► stdout    │        │   - file offset = 200     │────►│  size, perms, │
   │  2 ─► stderr    │        │   - flags (O_RDONLY...)   │     │  owner, block │
   │  3 ─────────────┼───────►│   - refcount              │     │  pointers ──► │──► data blocks
   └─────────────────┘        └──────────────────────────┘     └───────────────┘
                                           ▲
   PROCESS B                               │
   ┌─────────────────┐                     │
   │  3 ─────────────┼─────────────────────┘  (B opened same file: SEPARATE
   └─────────────────┘     open-file entry, so B has its OWN offset)
```

- **fd table** — per-process, private. Maps your integer fd → an open-file entry. This is why fd 3 in process A and fd 3 in process B are unrelated.
- **open file table** — system-wide. Each `open()` creates one entry holding the **current file offset** (where the next read/write happens) and the open flags. Crucially, the *offset lives here, not in the inode* — so two independent `open()`s of the same file get two offsets and don't interfere. But `fork` and `dup` make two fds *share* one entry (and thus one offset), which is the whole trick behind redirection.
- **inode** — the file's real identity: size, owner, permissions, timestamps, and pointers to the data blocks. The *filename is not here* — names live in directories and point at inodes (which is how hard links work, Concept 4).

**Why it exists:** The three-way split cleanly separates "my handle" (fd) from "this open session's position" (open-file entry) from "the file itself" (inode). Each can be shared or private independently, which gives Unix its I/O flexibility.

**Java analogy:** The fd table is what `FileDescriptor` objects index into. The open-file entry's offset is what `RandomAccessFile.getFilePointer()`/`seek()` manipulate. The inode is the metadata behind `Files.readAttributes()`. Java hides all three behind objects; here they're explicit kernel structures, and seeing them explains behaviors Java's abstraction papers over.

### 2. `open` flags and mode bits

**What it is:** `open(path, flags, mode)` returns a new fd (the lowest unused integer) or `-1`. The `flags` are OR'd together:

```
   ACCESS (pick exactly one):
     O_RDONLY   read only
     O_WRONLY   write only
     O_RDWR     read and write
   MODIFIERS (OR in as needed):
     O_CREAT    create the file if it doesn't exist (then `mode` matters)
     O_TRUNC    truncate to length 0 if it exists  (what `>` does)
     O_APPEND   every write goes to end-of-file atomically (what `>>` does)
     O_EXCL     with O_CREAT: fail if file already exists (atomic "create new")
     O_NONBLOCK don't block; return EAGAIN instead (Modules 4, 10)

   int fd = open("out.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
                                                          └─ mode: rw-r--r--
                                                             (only used when creating)
```

The `mode` argument (e.g. `0644`) sets the permission bits *if the file is created* — and the real permissions are `mode & ~umask` (your shell's umask masks out bits, usually `022`, turning `0666` into `0644`).

**Why it exists:** One syscall, many behaviors, selected by flags. `O_APPEND`'s atomicity is a gem: it makes each write seek-to-end-and-write as one indivisible kernel operation, so multiple processes appending to the same log file never overwrite each other — the correct way to do concurrent logging.

**Java analogy:** These flags map onto `StandardOpenOption` (`CREATE`, `TRUNCATE_EXISTING`, `APPEND`, `CREATE_NEW`=`O_CREAT|O_EXCL`). `new FileOutputStream(f)` is `O_WRONLY|O_CREAT|O_TRUNC`; `new FileOutputStream(f, true)` (append mode) is `O_WRONLY|O_CREAT|O_APPEND`. You've been choosing these flags all along, through Java's constructors.

### 3. `read`/`write` semantics — short reads/writes and why you MUST loop

**What it is:** The signatures:
```
   ssize_t read (int fd, void *buf, size_t count);   // returns bytes READ
   ssize_t write(int fd, const void *buf, size_t count); // returns bytes WRITTEN
```
The return value is the number of bytes actually transferred, which can be **less than `count`** (a short read/write), `0` (for `read`: end-of-file), or `-1` (error; check errno). Reasons for short transfers: the pipe/socket buffer filled up, a signal interrupted the call (`EINTR`), or you hit EOF partway. **The kernel never promises to move all your bytes in one call.** Therefore the only correct pattern is a loop:

```
   /* write ALL of `count` bytes, looping over short writes */
   ssize_t write_all(int fd, const void *buf, size_t count) {
       const char *p = buf;
       size_t left = count;
       while (left > 0) {
           ssize_t n = write(fd, p, left);
           if (n < 0) {
               if (errno == EINTR) continue;   // interrupted: just retry
               return -1;                        // real error
           }
           left -= (size_t)n;                    // advance past what we wrote
           p    += n;
       }
       return (ssize_t)count;
   }
```

For `read`, the loop also stops on `0` (EOF). This `read_all`/`write_all` pattern is used in *every* networking and IPC module ahead.

**Why it exists:** Because I/O is fundamentally partial. A socket can only accept so much before its buffer is full; a pipe has finite capacity; a signal can interrupt a blocking call. Returning "here's what I managed right now" is honest and lets you make progress without the kernel having to buffer unbounded data on your behalf.

**Java analogy:** Java's `InputStream.read(byte[])` also returns "bytes actually read, or -1 at EOF" and can return fewer than requested — the *same* short-read reality. That's why `readNBytes`/`readFully` and `DataInputStream.readFully` exist: they *are* the loop, wrapped up. In C you write the loop yourself. If you've ever been burned by assuming `in.read(buf)` filled `buf`, this is the same lesson at a lower level.

### 4. `close`, `lseek`, `dup`/`dup2`, `stat`, and links

**What it is:** The supporting cast:
- **`close(fd)`** — releases the fd and decrements the open-file entry's refcount; when it hits zero the entry is freed. Not closing fds *leaks* them (Gotchas).
- **`lseek(fd, offset, whence)`** — moves the file offset in the open-file entry. `SEEK_SET` (absolute), `SEEK_CUR` (relative), `SEEK_END` (from end). Random access. `lseek` past EOF then writing creates a **sparse file** (a hole that reads as zeros but takes no disk).
- **`dup(fd)` / `dup2(oldfd, newfd)`** — duplicate a descriptor. `dup2` makes `newfd` refer to the *same open-file entry* as `oldfd` (closing `newfd` first if needed). **This is how the shell does redirection:** to run `ls > out.txt`, the shell opens `out.txt`, then `dup2(file_fd, 1)` so fd 1 (stdout) now points at the file — and `ls`, which just writes to fd 1 as always, unknowingly writes to the file. Program 2 builds exactly this.
- **`stat`/`fstat`/`lstat`** — fill a `struct stat` with a file's metadata (size, mode, inode number, link count, timestamps). `stat` follows symlinks; `lstat` reports on the link itself; `fstat` works on an open fd.
- **Hard link vs symlink** — a **hard link** is a second *directory entry pointing at the same inode* (same file, two names; the inode's link count tracks how many). The file's data is freed only when the link count hits zero *and* no process has it open. A **symlink** is a tiny file whose *contents are a path* — a pointer by name, which breaks if the target moves. `ln a b` (hard) vs `ln -s a b` (symbolic).

```
   HARD LINK                          SYMLINK
   dir: "a" ─┐                        dir: "a" ─► inode 8801 (data)
   dir: "b" ─┴─► inode 8801 (data)    dir: "b" ─► inode 9002 whose data = "a"
   (both names equal; link count 2)   (b just stores the text "a"; follow it to reach a)
```

**Why it exists:** These fill out the model: `close` for cleanup, `lseek` for random access, `dup2` for wiring fds (the basis of pipes/redirection), `stat` for metadata, links for giving files multiple names or symbolic references.

**Java analogy:** `lseek` ≈ `RandomAccessFile.seek`. `dup2` has **no clean Java equivalent** — you can't rebind `System.out`'s underlying fd the way the shell does (you can `System.setOut` a wrapper, but that's Java-level, not the kernel fd). `stat` ≈ `Files.readAttributes`. Hard/sym links ≈ `Files.createLink`/`Files.createSymbolicLink`. `dup2`'s absence in Java is *why* redirecting a subprocess's output in Java goes through `ProcessBuilder.redirectOutput` — Java is asking the OS to do the `dup2` for it.

### 5. Unbuffered (syscall) vs buffered (stdio) I/O — and the "printf before fork" bug

**What it is:** Two layers, again (Module 2, Concept 4), now for files:

```
   UNBUFFERED (syscall) I/O          BUFFERED (stdio) I/O
   open/read/write/close             fopen/fread/fwrite/fprintf/printf
   works on int fd                   works on FILE* (which WRAPS an fd + a buffer)
   every call crosses the wall       accumulates in a userspace buffer, flushes rarely
   you control exactly when          library decides when (see buffering modes)
```

stdio has three **buffering modes**:
- **Unbuffered** — flush every write immediately (stderr defaults to this — you want errors *now*).
- **Line-buffered** — flush on `\n` (stdout defaults to this *when connected to a terminal*).
- **Fully buffered** — flush only when the buffer fills or you `fflush` (stdout defaults to this *when redirected to a file or pipe* — this is the surprise that breaks output ordering).

The infamous bug: `printf` writes into stdio's userspace buffer. If you `printf("hi")` (no newline) and then `fork()` while the buffer is still unflushed, the buffer is *copied into both* parent and child (fork copies the whole address space, Module 5). When each later flushes, "hi" is written **twice**. Redirect to a file (making stdout fully-buffered) and even a `printf("hi\n")` can double, because the `\n` no longer triggers a flush. The fix: `fflush(stdout)` before `fork`, or use unbuffered `write`. This single bug teaches you everything about where buffers live.

**Why it exists:** Buffering amortizes the cost of syscalls (Module 0/2 — crossing the wall is expensive). One `write` of 4096 bytes beats 4096 one-byte `write`s by orders of magnitude. stdio does this batching for you. The cost is the loss of control over *when* bytes actually leave your process — hence the fork bug and the ordering surprises.

**Java analogy:** This is **exactly** `FileInputStream`/`FileOutputStream` (unbuffered, straight to the fd) versus `BufferedInputStream`/`BufferedOutputStream` (userspace buffer, flush rarely). `System.out` is a `PrintStream` that's autoflush-on-newline when interactive — Java's line-buffering. `flush()` in Java is `fflush` in C. You already reach for `BufferedWriter` to avoid per-write syscalls; that instinct is precisely the unbuffered/buffered choice, and the fork bug is what happens when a buffer you forgot about gets duplicated.

---

## Code

### Program 1 — `copyfile.c`: `open`/`read`/`write`/`close` done correctly, with the short-I/O loop

```c
/* copyfile.c
 *
 * A correct file copier using ONLY raw syscalls -- the Unix `cp` in
 * miniature. Demonstrates open flags, the read/write loop that handles
 * short reads AND short writes, EINTR retry, and disciplined error
 * checking + close.
 *
 * Compile:  gcc -Wall -Wextra -o copyfile copyfile.c
 * Run:      ./copyfile source.txt dest.txt
 *           ./copyfile copyfile.c /tmp/copy.c && diff copyfile.c /tmp/copy.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>     /* read, write, close */
#include <fcntl.h>      /* open, O_* flags    */
#include <errno.h>
#include <string.h>

/* write ALL count bytes from buf to fd, looping over short writes and
 * retrying on EINTR. Returns 0 on success, -1 on real error. */
static int write_all(int fd, const char *buf, size_t count)
{
    size_t left = count;
    while (left > 0) {
        ssize_t n = write(fd, buf, left);
        if (n < 0) {
            if (errno == EINTR) continue;   /* interrupted by a signal: retry */
            return -1;                        /* real error */
        }
        left -= (size_t)n;                    /* advance past what was written */
        buf  += n;
    }
    return 0;
}

int main(int argc, char *argv[])
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s SOURCE DEST\n", argv[0]);
        return 2;
    }

    /* Open source read-only. */
    int in = open(argv[1], O_RDONLY);
    if (in < 0) { perror(argv[1]); return 1; }

    /* Create/truncate dest for writing, mode rw-r--r-- (masked by umask). */
    int out = open(argv[2], O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) { perror(argv[2]); close(in); return 1; }

    char buf[65536];        /* 64 KiB: bigger buffer = fewer syscalls */
    ssize_t r;

    /* read() returns >0 (bytes), 0 (EOF), or -1 (error). Loop until EOF. */
    while ((r = read(in, buf, sizeof buf)) != 0) {
        if (r < 0) {
            if (errno == EINTR) continue;    /* interrupted read: retry */
            perror("read");
            close(in); close(out);
            return 1;
        }
        /* We got r bytes (possibly < sizeof buf -- a SHORT READ, normal).
         * Now write exactly those r bytes, handling short WRITES. */
        if (write_all(out, buf, (size_t)r) < 0) {
            perror("write");
            close(in); close(out);
            return 1;
        }
    }

    /* close() can fail (e.g. deferred write error on some filesystems),
     * so we check it -- especially on the OUTPUT file. */
    if (close(in)  < 0) { perror("close in");  return 1; }
    if (close(out) < 0) { perror("close out"); return 1; }
    return 0;
}
```

**Expected output:**
```
$ ./copyfile copyfile.c /tmp/copy.c && diff copyfile.c /tmp/copy.c && echo "identical"
identical
```

**Walkthrough of the non-obvious parts:**
- `open(argv[2], O_WRONLY | O_CREAT | O_TRUNC, 0644)` — the exact flag combo behind `>`: write-only, create if missing, truncate if present. The `0644` mode only applies when creating.
- The `read` loop condition `!= 0` — `read` returns `0` **only at EOF**, so the loop naturally ends there. Inside, we separately handle `r < 0` (error/EINTR). This ordering (check for EOF in the loop condition, errors inside) is the idiomatic shape.
- `write_all` — the crux. A single `write(out, buf, r)` might write fewer than `r` bytes; ignoring that silently truncates the copy on pipes/sockets/full disks. `write_all` loops until every byte is out. **This function reappears in every network module** — it's not file-specific, it's fd-general.
- `EINTR` retry — a blocking `read`/`write` interrupted by a signal returns `-1` with `errno == EINTR`. That's not a real failure; you just retry. Robust I/O code always handles it.
- Checking `close(out)` — surprising to newcomers, but `close` can surface a deferred write error (the data was buffered in the kernel and the flush failed). For output files you care about, check it.

### Program 2 — `redirect.c`: reproduce shell `>` redirection with `dup2`

```c
/* redirect.c
 *
 * Shows how the shell implements `command > file`: open the file, then
 * dup2() it onto fd 1 (stdout), so ANY code that writes to stdout
 * (printf, write, even a later exec'd program) goes to the file instead.
 * No printf in this program "knows" it's being redirected -- that's the
 * whole point of the fd indirection.
 *
 * Compile:  gcc -Wall -Wextra -o redirect redirect.c
 * Run:      ./redirect out.txt
 *           cat out.txt          # the lines that "should" have hit the screen
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>     /* dup2, write, STDOUT_FILENO */
#include <fcntl.h>
#include <errno.h>

int main(int argc, char *argv[])
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s OUTFILE\n", argv[0]);   /* to stderr (fd 2) */
        return 2;
    }

    /* This line goes to the REAL screen: we haven't redirected yet. */
    printf("BEFORE redirect: this appears on your terminal\n");
    fflush(stdout);   /* flush NOW so it isn't caught by the redirect below
                         (and to avoid the buffered-duplication trap) */

    /* Open (or create) the target file for writing. */
    int fd = open(argv[1], O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { perror(argv[1]); return 1; }

    /* THE TRICK: make fd 1 (stdout) refer to the SAME open-file entry as
     * `fd`. After this, everything written to fd 1 lands in the file.
     * dup2 closes the old fd 1 first, then points it at fd. */
    if (dup2(fd, STDOUT_FILENO) < 0) { perror("dup2"); close(fd); return 1; }

    /* We no longer need the original `fd` number; fd 1 is our handle now. */
    close(fd);

    /* These go to the FILE, even though the code looks identical to before.
     * printf still writes to "stdout" (fd 1) -- but fd 1 now IS the file. */
    printf("AFTER redirect: this line goes into %s\n", argv[1]);
    write(STDOUT_FILENO, "...and so does this raw write()\n", 32);
    fflush(stdout);   /* ensure stdio's buffer reaches the file before exit */

    return 0;
}
```

**Expected output:**
```
$ ./redirect out.txt
BEFORE redirect: this appears on your terminal
$ cat out.txt
AFTER redirect: this line goes into out.txt
...and so does this raw write()
```

**Walkthrough of the non-obvious parts:**
- `dup2(fd, STDOUT_FILENO)` — the heart of it. `STDOUT_FILENO` is `1`. After this call, fd 1 and `fd` both point at the *same open-file entry* (the file). Any write to fd 1 — by `printf`, by `write`, or by a program you'd `exec` next (Module 5) — goes to the file. The program's own code never mentions the file when printing; the *fd table* was rewired underneath it. **This is exactly what bash does** between `fork` and `exec` to implement `>`.
- The `fflush(stdout)` before the redirect — flushes the "BEFORE" line to the terminal *now*, so it isn't sitting in stdio's buffer when we rebind fd 1 (which would send it to the file instead). This is the buffering lesson (Concept 5) biting in miniature.
- `close(fd)` after `dup2` — once fd 1 points at the file, the original `fd` number is redundant; closing it is good hygiene (the open-file entry stays alive because fd 1 still references it — refcount, Concept 1).
- The `printf` "AFTER" line proves the abstraction: identical-looking code, different destination, because the *fd*, not the code, decides where bytes go. That indirection is the entire reason "everything is a file" is powerful.

---

## Under the Hood

Let's watch `copyfile` cross the wall. Run:

```
$ echo "hello file io" > src.txt
$ strace ./copyfile src.txt dst.txt
```

The meaningful lines:

```
openat(AT_FDCWD, "src.txt", O_RDONLY)                 = 3     ← [1] source opened, fd 3
openat(AT_FDCWD, "dst.txt", O_WRONLY|O_CREAT|O_TRUNC, 0644) = 4  ← [2] dest opened, fd 4
read(3, "hello file io\n", 65536)                     = 14    ← [3] asked 65536, GOT 14 (short read!)
write(4, "hello file io\n", 14)                       = 14    ← [4] wrote all 14
read(3, "", 65536)                                    = 0     ← [5] read returns 0 = EOF
close(3)                                              = 0     ← [6] close source
close(4)                                              = 0     ← [7] close dest
exit_group(0)                                         = ?
```

Annotated:
1. **`openat(..., "src.txt", O_RDONLY) = 3`** — `open` is implemented via `openat` on modern Linux. Return value **3**: the lowest free fd (0,1,2 are stdin/out/err). This creates the fd-table→open-file-table→inode chain from Concept 1.
2. **`openat(..., "dst.txt", O_WRONLY|O_CREAT|O_TRUNC, 0644) = 4`** — your `>`-equivalent flags, verbatim, in the trace. Return **4**, the next free fd. The `0644` is the mode you passed.
3. **`read(3, "hello file io\n", 65536) = 14`** — you *asked for up to 65536 bytes* but the file only had **14**, so `read` returned 14. **This is a short read**, and it's completely normal — the file is just small. Your loop handles it correctly by writing exactly the 14 bytes returned, not 65536.
4. **`write(4, "...", 14) = 14`** — `write_all` wrote all 14 in one go (small enough that there was no short write here — but on a socket or full pipe it would loop).
5. **`read(3, "", 65536) = 0`** — the second read hits **EOF** and returns `0`. That's the loop's exit condition. Notice: you always need one extra `read` that returns 0 to *know* you're at EOF.
6–7. **`close(3)`, `close(4)`** — releasing the descriptors, tearing down the chains.

The headline: **`strace` shows the fd numbers (3, 4), proves short reads are real (asked 65536, got 14), and shows EOF as a `read` returning 0.** Every I/O bug you'll ever debug starts by looking at exactly this: which fd, how many bytes asked, how many returned. For `redirect.c`, `strace ./redirect out.txt` will show `dup2(3, 1) = 1` — literally the fd-rewiring that makes redirection work.

*(Bonus: run `ltrace ./copyfile src.txt dst.txt` and you'll see the C-library calls instead. Since copyfile uses only raw syscalls, `ltrace` is nearly empty — proving copyfile talks straight to the kernel with no stdio buffering in between.)*

---

## Try This

Ordered easy → hard.

1. **(Easy) Watch fd numbers grow.** Modify `copyfile` to `printf("in fd = %d, out fd = %d\n", in, out);` after opening. Run it and confirm they're 3 and 4. Then open a third file before them and watch the numbers shift. *Hint: fds are handed out lowest-first; 0/1/2 are already taken.*

2. **(Easy) See O_APPEND atomicity.** Write a program that opens a file with `O_WRONLY|O_CREAT|O_APPEND` and writes a line. Run it several times; confirm lines accumulate (not overwrite). Then remove `O_APPEND` (keep `O_TRUNC` out too) and see the difference. *Hint: `O_APPEND` makes every write seek-to-end atomically — the correct way to append to a log.*

3. **(Medium) Reproduce the printf-before-fork bug.** Write: `printf("hello");` (NO newline), then `fork();`, then in both parent and child `return 0;`. Run it once to the terminal (see "hello" once) and once redirected to a file: `./a.out > out.txt; cat out.txt` (see "hellohello" — twice!). Now add `fflush(stdout);` before the fork and confirm it prints once. Explain why redirection changed the behavior. *Hint: terminal = line-buffered, file = fully-buffered; the unflushed buffer got copied into both processes by fork.*

4. **(Medium) Prove independent offsets.** Open the same file twice (two `open` calls → two fds → two open-file entries). `read` a few bytes from fd A, then `read` from fd B. Show that B starts at the *beginning*, not where A left off. Then `dup` fd A to fd C and show C *shares* A's offset. Explain using the three-table diagram. *Hint: separate `open` = separate offset; `dup` = shared open-file entry = shared offset.*

5. **(Hard) Build a tiny `tail -c N`.** Write a program that prints the last N bytes of a file using `lseek(fd, -N, SEEK_END)` then `read`. Handle files shorter than N. Then explore sparse files: `lseek` 1 MB past the start of a fresh file, write one byte, and compare `ls -l` (logical size ~1MB) with `du` (actual disk blocks, tiny). Explain the hole. *Hint: `lseek` past EOF + write creates a sparse file; the gap reads as zeros but occupies no disk.*

---

## Gotchas

- **Assuming `read`/`write` transfer everything in one call.** The #1 systems-I/O bug. It "works" on small local files (one read gets it all) and silently corrupts data on pipes and sockets (short transfers are common there). *Always* loop with `read_all`/`write_all`. This is the single most important habit in the course — and a very common interview question ("what does `write` return, and why must you check it?").

- **Forgetting `read` returns 0 at EOF, not -1.** `0` means end-of-file (a clean stop); `-1` means error (check errno). Treating `0` as an error, or `-1` as EOF, both break your loop. On a socket, `read` returning `0` means the peer closed the connection (Module 9) — same rule.

- **Leaking file descriptors.** Every `open` needs a matching `close`. A long-running server that opens per request without closing will hit the per-process fd limit (`ulimit -n`, often 1024) and then every `open`/`accept` fails with `EMFILE` ("too many open files") — a classic production outage. `ls -l /proc/<pid>/fd` (Module 12) shows a process's open fds. Interview favorite.

- **The buffered/unbuffered mixing trap.** Mixing `printf` (buffered stdio on fd 1) and `write(1, ...)` (unbuffered) on the *same* fd interleaves unpredictably, because printf's bytes sit in a buffer while write's go straight through. Pick one layer per fd, or `fflush` between them. This is also the root of the fork double-print bug.

- **`O_CREAT` without a mode argument.** If you pass `O_CREAT` but omit the third `mode` argument, the file gets created with garbage permissions (whatever was on the stack). Always pass an explicit mode (e.g. `0644`) with `O_CREAT`.

- **Confusing hard links and symlinks.** A hard link shares the inode (deleting one name doesn't free the data until *all* names and open handles are gone — this is why deleting a file that's still open by a process doesn't reclaim its disk space until the process closes it: the "deleted but held open" gotcha). A symlink stores a path and dangles if the target moves. Interview trap: "what happens to disk space when you `rm` a file a running process still has open?" Answer: nothing until the process closes it — the inode's link count is 0 but its open-count isn't.

- **TOCTOU with `O_CREAT` vs `O_CREAT|O_EXCL`.** To create a file *only if it doesn't exist* atomically (e.g. a lock file), use `O_CREAT|O_EXCL` — it fails with `EEXIST` if the file's there, in one atomic syscall. Checking existence with `stat` then `open`ing is a race (Module 1's TOCTOU again).

---

## Checkpoint

Answer from memory, then check below.

1. Draw (or describe) the three tables involved when a process opens a file, and say which one holds the current file *offset*. Why does that placement matter when two processes `open` the same file?
2. `write(fd, buf, 1000)` returns `600`. Is this an error? What must your code do, and why is this especially important for sockets and pipes?
3. `read(fd, buf, 4096)` returns `0`. What does that mean, and how is it different from a return of `-1`?
4. Explain, in terms of the fd table and `dup2`, how the shell implements `ls > out.txt`.
5. You `printf("hi")` (no newline), then `fork()`, and the output ends up as "hihi" when redirected to a file. Explain exactly why, and give two ways to fix it.

---

<details>
<summary><b>Checkpoint Answers</b></summary>

1. The three tables: the per-process **fd table** (maps integer fd → open-file entry), the system-wide **open file table** (one entry per `open()`, holding the **file offset** and flags), and the **inode table** (the file's on-disk identity: size, perms, data-block pointers). The offset lives in the **open file table entry**, *not* the inode — so two separate `open()`s of the same file create two entries with two independent offsets, and reading through one doesn't move the other's position. (`fork`/`dup`, by contrast, make two fds share *one* entry and thus one offset.)

2. **Not an error** — it's a **short write**. Your code must loop, calling `write` again for the remaining 400 bytes (advancing the buffer pointer), until all 1000 are written (a `write_all`). This matters most on **pipes and sockets**, where the kernel buffer frequently fills and short writes are common; assuming one `write` sent everything silently drops data there even though it "works" on small local files.

3. A `read` return of **`0` means end-of-file** — there is no more data (on a socket, the peer closed the connection). A return of **`-1` means an error** occurred (inspect `errno`; e.g. `EINTR`, `EBADF`). They're distinct: `0` is a normal, clean termination of the read loop; `-1` is a failure you must handle.

4. The shell `fork`s, and in the child: it `open`s `out.txt` (`O_WRONLY|O_CREAT|O_TRUNC`), getting some fd (say 3), then calls **`dup2(3, 1)`** so that **fd 1 (stdout)** now points at the same open-file entry as the file. It closes fd 3 (redundant now) and `exec`s `ls`. `ls` writes to fd 1 as it always does — but fd 1 has been rewired to the file, so the output lands in `out.txt`. The program `ls` is unaware; the redirection happened purely by editing the child's fd table.

5. `printf("hi")` writes into stdio's **userspace buffer** and (no newline; and when redirected to a file, stdout is **fully buffered**) does *not* flush. Then `fork()` **copies the entire address space — including that unflushed buffer — into both parent and child**. When each process later exits and flushes, "hi" is written **twice** → "hihi". Two fixes: (a) `fflush(stdout);` *before* the `fork` so the buffer is empty when it's copied; or (b) use unbuffered `write(1, "hi", 2)` instead of `printf`, so nothing is ever sitting in a stdio buffer.

</details>

---

*Next up: **Module 4 — Console I/O and the Terminal.** stdin/stdout/stderr as fds 0/1/2, the TTY subsystem and line discipline, `termios` raw vs canonical mode (build a "press any key" reader), and what pipes and redirection do to fds from the shell's side. Continuing straight on.*
