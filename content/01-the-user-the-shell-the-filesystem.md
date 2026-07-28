# Module 1 — The User, the Shell, and the Filesystem

> **Estimated time:** 3–4 hours · **Core path:** Concepts 1–5 and both code programs are core. The full FHS tour (Concept 4, deep) and the setuid kernel walk (Under the Hood, part 2) are deep-dive.
>
> **Prerequisites:** Module 0. You should be comfortable with the idea that a syscall is the only door from ring 3 to ring 0, and that `ls` is just a C program.

---

## The Big Picture

In Module 0 you learned that everything running is a process and every process talks to the kernel through syscalls. Now we zoom out one notch and ask: *who is allowed to do what, and how does the machine even decide?* Because a Linux box is a shared machine. Even your laptop, where you're the only human, is running processes owned by `root`, by `systemd`, by `www-data`, by you. The kernel has to arbitrate — and it does that with an identity system built on numbers: **user IDs and group IDs**. Every process carries an identity, every file carries an owner and a permission mask, and the kernel checks the two against each other on every single access. That check is the foundation of all Unix security, and it's shockingly simple once you see it.

Sitting on top of that identity system is the program you spend all day in without thinking about it: **the shell**. Here's the thing that surprises backend developers — the shell is not special. It's an ordinary user-space program, running as you, in ring 3, with no magic powers. Its entire job is a loop: print a prompt, read a line, figure out which program you meant, `fork` a child, `exec` that program in the child, and `wait` for it to finish. That's it. That's bash. Every time you run `ls`, `git`, or `java`, you're watching that loop execute a fork/exec/wait cycle you'll build yourself in Module 5. Understanding that the shell is "just a program that runs other programs" dissolves a huge amount of mystery.

Then there's the **filesystem**, and here Unix makes a design choice so bold it defines the whole system: *everything is a file.* Your keyboard is a file (`/dev/tty`). Your disk is a file (`/dev/sda`). A running process's memory map is a file (`/proc/self/maps`). A network connection, once opened, is accessed through the same `read`/`write` calls as a text file. This isn't a cute slogan — it's a unifying abstraction that means the same handful of syscalls (`open`, `read`, `write`, `close`) work on almost everything, and the number that ties a process to any of these open things is the **file descriptor**. The fd is the spine of this entire course. In this module it's mostly about regular files; by Module 9 that same fd is a TCP socket; by Module 14 it's a device driver you wrote. Same concept, all the way down.

Finally, the filesystem has a *shape* — a standard layout called the **Filesystem Hierarchy Standard (FHS)**. `/bin` holds programs, `/etc` holds config, `/dev` holds devices, `/proc` and `/sys` are windows into the kernel that don't exist on disk at all. This layout isn't arbitrary; it encodes decades of "where does this kind of thing belong." As a Spring Boot developer you already live with conventions like `src/main/resources` and `application.yml` living in predictable places — the FHS is that idea for the entire operating system, and knowing it turns "where the hell is that config" into a reflex.

By the end of this module you'll understand why `passwd` — a program that edits a file you are *forbidden* to write — is allowed to work, you'll resolve a command against `$PATH` in C exactly the way the shell does, and you'll install your own program into the system so the shell treats it like any built-in tool. That last step closes the loop from Module 0: `mycmd` becomes a real command.

---

## Concepts

### 1. Users, UIDs, GIDs, and root

**What it is:** Every human or service account on a Linux system maps to a **user ID (UID)** — just an integer. `root` is UID 0. Your login is something like UID 1000. Names like `amith` or `root` are a convenience layer; the kernel only ever deals in numbers (it looks names up in `/etc/passwd`). Users belong to one or more **groups**, each a **group ID (GID)**, listed in `/etc/group`. A process inherits the UID/GID of whoever started it, and the kernel stamps that identity on everything the process does.

```
   /etc/passwd  (name -> number mapping, one line per user)
   root:x:0:0:root:/root:/bin/bash
   amith:x:1000:1000:Amith:/home/amith:/bin/bash
        │   │  │  │
        │   │  │  └─ primary GID (1000)
        │   │  └──── UID (1000)
        │   └─────── password placeholder ('x' = real hash is in /etc/shadow)
        └─────────── username

   A process owned by amith:  ruid=1000  rgid=1000
   The kernel checks THESE numbers on every file access.
```

**Why it exists:** So one machine can be safely shared. UID 0 (`root`) bypasses permission checks entirely — it's the superuser. Everyone else is fenced into what their UID/GID is allowed to touch. This is the *policy* layer that sits on top of Module 0's *mechanism* layer (rings). Rings stop you from touching hardware; UIDs stop you from touching each other's files.

**Java analogy:** The removed `SecurityManager`/`Subject`/`Principal` model is the closest thing — a "who is running this code and what may they do" layer. But in Java that was optional and in-process. Here it's mandatory, kernel-enforced, and applies to *every* process on the machine. **The concept of "root can do anything" has no Java equivalent** — there's no JVM user who bypasses all checks, because the JVM was never designed to be the security boundary for a whole machine.

### 2. Permissions: rwx, octal, and the three-triad model

**What it is:** Every file and directory carries an **owner UID**, an **owning GID**, and a 9-bit permission mask split into three triads — for the **owner**, the **group**, and **others** (everyone else). Each triad has **r**ead, **w**rite, **e**xecute bits.

```
   $ ls -l /usr/bin/passwd
   -rwsr-xr-x 1 root root 59976 ... /usr/bin/passwd
   │└┬┘└┬┘└┬┘
   │ │  │  └── others:  r-x  (read + execute, no write)
   │ │  └───── group:   r-x
   │ └──────── owner:   rws  (read + write + SETUID 's' instead of 'x')
   └────────── type:    '-' = regular file, 'd' = dir, 'l' = symlink, 'c' = char device

   Octal:  each triad is 3 bits = one octal digit
     r=4  w=2  x=1
     rwx = 4+2+1 = 7      r-x = 4+0+1 = 5      rw- = 4+2+0 = 6
   So  chmod 755 file  ->  rwxr-xr-x   (owner all, group+others read/exec)
       chmod 644 file  ->  rw-r--r--   (owner read/write, others read only)
```

When a process touches a file, the kernel picks **one** triad and checks it: if the process's UID owns the file, the *owner* triad applies (group/other are ignored). Else if the process is in the file's group, the *group* triad applies. Else the *other* triad applies. First match wins — this is a classic gotcha (being in the group can give you *fewer* rights than "other" if the group triad is more restrictive, and the kernel won't fall through).

**Why it exists:** It's the minimal, fast, memorizable model for expressing "who can do what" to a file. Three triads × three bits fits in 9 bits the kernel checks in nanoseconds.

**Java analogy:** `java.nio.file.attribute.PosixFilePermission` is literally this enum (`OWNER_READ`, `GROUP_WRITE`, ...). If you've ever called `Files.setPosixFilePermissions`, you were setting these exact bits. The difference: in Java it's an API you opt into; here it's enforced by the kernel on every `open()` whether you like it or not.

### 3. setuid — why `passwd` can edit a file you can't

**What it is:** Here's a puzzle. Passwords are stored (hashed) in `/etc/shadow`, which is owned by root and readable/writable by **no one** but root:

```
   $ ls -l /etc/shadow
   -rw-r----- 1 root shadow ... /etc/shadow      # you (UID 1000) can't write this
```

Yet you, an ordinary user, can run `passwd` and change *your* password — which writes to `/etc/shadow`. How? The **setuid bit** (that `s` where owner's `x` would be). When a program has its setuid bit set and is owned by root, running it makes the process's **effective UID** become the file's owner (root) — *not* the UID of whoever launched it. So while `passwd` runs, it's effectively root, and root can write `/etc/shadow`.

This is where the "two UIDs" idea appears. A process actually carries:
- **real UID (ruid)** — who you *are* (who launched it): 1000.
- **effective UID (euid)** — who you're *acting as* for permission checks: 0 while inside setuid `passwd`.

```
   Normal program (e.g. ls):        ruid=1000  euid=1000   -> checks run as you
   setuid-root program (passwd):    ruid=1000  euid=0      -> checks run as root!
        │
        └─ the kernel raised euid to the file owner (0) at exec time,
           because the setuid bit was set. ruid stays 1000 so passwd
           still KNOWS who you really are (so it only lets you change
           YOUR OWN password).
```

**Why it exists:** Some operations legitimately need elevated privilege but must be available to unprivileged users, in a *controlled* way. setuid is the mechanism: hand the user a specific, audited program that briefly runs as root and does exactly one privileged thing. It's privilege *delegation*, not privilege *grant*.

**Why it's dangerous:** A setuid-root program is a loaded gun. If it has a bug (buffer overflow, command injection, following a symlink it shouldn't), an attacker can trick it into doing arbitrary things *as root*. Half the history of Unix security exploits is "abuse a setuid binary." This is why modern systems minimize setuid binaries and why you audit them (`find / -perm -4000` lists them all).

**Java analogy:** **None, and this is important.** There is no Java mechanism where "running this jar temporarily makes you a different, more privileged user at the OS level." The JVM runs with whatever UID launched it, full stop. setuid is a pure-OS concept — a frequent systems interview question precisely because it has no application-layer analogy.

### 4. The Filesystem Hierarchy Standard (FHS)

**What it is:** A standardized directory layout so that "where things live" is predictable across Linux systems. The load-bearing directories:

```
   /            root of EVERYTHING (there is one tree, no drive letters like C:)
   ├── bin      essential user commands (ls, cp, cat) — often a symlink to /usr/bin now
   ├── sbin     system admin commands (fdisk, ip) — historically "root's bin"
   ├── etc      system-wide CONFIGuration, all plain text (passwd, hosts, ssh/)
   ├── home     users' home directories (/home/amith)
   ├── dev      DEVICE files — the disk, terminal, /dev/null (Concept 5)
   ├── proc     VIRTUAL fs: live kernel + process info, NOT on disk (Module 12)
   ├── sys      VIRTUAL fs: device/kernel model, NOT on disk (Module 12)
   ├── tmp      world-writable scratch space, wiped on reboot
   ├── var      variable data: logs (/var/log), spools, caches, databases
   ├── usr      the bulk of the OS: /usr/bin, /usr/lib, /usr/include (C headers!)
   └── lib      shared libraries (libc.so) needed to boot / run /bin
```

The big mental shift from Windows: **there are no drive letters.** Everything hangs off a single root `/`. A second disk, a USB stick, or a network share is *mounted* onto a directory (say `/mnt/usb`) and becomes part of the one tree. This is the "everything is a file, and the filesystem is one namespace" philosophy taken to its conclusion.

**Why it exists:** Predictability. Any Linux program, admin, or script can assume config is under `/etc`, logs under `/var/log`, C headers under `/usr/include`. Without a standard, every distro would scatter things randomly and nothing would be portable.

**Java analogy:** Think of the FHS as the OS-scale version of Maven's Standard Directory Layout (`src/main/java`, `src/test`, `target/`). Conventions over configuration, so tools know where to look. `/etc` is `application.yml`'s spiritual home; `/var/log` is where your `logback` file appender would target; `/usr/include` is like the OS's classpath of C headers.

### 5. Everything is a file, and the file descriptor

**What it is:** Almost every resource a process can access — a regular file, a directory, a pipe, a socket, a terminal, a device — is reached through the same abstraction and manipulated with the same syscalls (`open`, `read`, `write`, `close`, `lseek`). When you `open()` something, the kernel gives you back a small non-negative integer: the **file descriptor (fd)**. That integer is an index into your process's private *file descriptor table*. Every process starts with three fds already open:

```
   fd 0  = STDIN   (standard input)
   fd 1  = STDOUT  (standard output)
   fd 2  = STDERR  (standard error)

   process fd table (per-process, private)
   ┌────┬──────────────────────────┐
   │ 0  │ ──► terminal (keyboard)   │
   │ 1  │ ──► terminal (screen)     │
   │ 2  │ ──► terminal (screen)     │
   │ 3  │ ──► /home/amith/data.txt  │  ◄── first fd you open() gets number 3
   │ 4  │ ──► a TCP socket          │  ◄── SAME table, different kind of thing
   └────┴──────────────────────────┘
```

The magic is the *uniformity*: your code does `read(fd, buf, n)` and doesn't care whether `fd` is a file, a pipe, or a network socket. The kernel routes it to the right subsystem behind the abstraction (the VFS layer — Module 3 and 13). We only preview it here; Module 3 draws the full three-table picture (process fd table → system open-file table → inode table).

**Why it exists:** One abstraction to rule them all. Learn `read`/`write` once, apply everywhere. It's why Unix composability works: because `ls`'s output and a file and a network stream are all just fds, you can pipe and redirect them interchangeably (Module 4/8).

**Java analogy:** A file descriptor *is* the thing hiding inside `FileInputStream`, `Socket`, and `System.out`. In fact `FileDescriptor` is a real Java class — `FileInputStream.getFD()` hands you the very integer we're talking about (wrapped in an object). Java's streams and channels are polished OO clothing over this one integer. **The fd is what `InputStream`/`OutputStream` are abstracting.** Once you see that, Java I/O stops being a pile of classes and becomes "wrappers around a kernel fd."

### 6. The shell as a REPL, and `$PATH` resolution

**What it is:** The shell is a read-eval-print loop. Its core cycle:

```
   loop forever:
     1. print prompt         ($ )
     2. read a line          (ls -l /tmp)
     3. parse into argv       ["ls","-l","/tmp"]
     4. resolve "ls" -> a real file path using $PATH
     5. fork()                (make a child copy of the shell)
     6. in the child: exec()  (replace it with /usr/bin/ls)
     7. in the parent: wait()  (block until the child exits)
     8. capture exit code -> $?
```

Step 4 is **PATH resolution** and it's worth understanding precisely. `$PATH` is an environment variable holding a colon-separated list of directories:

```
   $ echo $PATH
   /usr/local/bin:/usr/bin:/bin:/home/amith/bin

   To run "ls", the shell tries, in order, until a file exists and is executable:
     /usr/local/bin/ls   ? no
     /usr/bin/ls         ? YES -> exec this one, stop searching
```

If the name contains a slash (`./mycmd`, `/usr/bin/ls`), the shell skips the search and uses the path directly — *that's why you must type `./mycmd`* to run a program in the current directory: without a slash, the shell searches `$PATH`, and `.` is (deliberately, for security) not in `$PATH`.

**Why it exists:** So you can type `git` instead of `/usr/bin/git`, and so administrators can control which version of a command wins by ordering `$PATH`. It's late-binding for commands.

**Java analogy:** `$PATH` resolution is conceptually the JVM searching the **classpath** for a class to load — an ordered list of locations, first match wins. Environment variables themselves map to `System.getenv("PATH")`. And the whole fork/exec/wait cycle is what `ProcessBuilder` / `Runtime.exec` does under the hood; you'll implement it by hand in Module 5.

---

## Code

### Program 1 — `idinfo.c`: real vs effective identity (the setuid mechanism, visible)

```c
/* idinfo.c
 *
 * Prints the process's real and effective UID/GID, and resolves the real
 * UID back to a username. Run it normally, then run it as a setuid binary
 * to SEE effective UID diverge from real UID -- the exact mechanism that
 * lets passwd write /etc/shadow.
 *
 * Compile:  gcc -Wall -Wextra -o idinfo idinfo.c
 * Run:      ./idinfo
 * Make it setuid-root and run again (see "Try This"):
 *      sudo chown root idinfo && sudo chmod u+s idinfo && ./idinfo
 */

#include <stdio.h>
#include <unistd.h>     /* getuid, geteuid, getgid, getegid */
#include <sys/types.h>
#include <pwd.h>        /* getpwuid -> username from UID     */
#include <errno.h>
#include <string.h>

int main(void)
{
    uid_t ruid = getuid();    /* real UID  : who launched me       */
    uid_t euid = geteuid();   /* effective : who I act as for perms */
    gid_t rgid = getgid();
    gid_t egid = getegid();

    printf("real UID = %d, effective UID = %d\n", (int)ruid, (int)euid);
    printf("real GID = %d, effective GID = %d\n", (int)rgid, (int)egid);

    /* Turn the real UID number into a name by looking it up in the
     * user database (/etc/passwd). getpwuid returns NULL on failure and,
     * unusually, sets errno only sometimes -- so we clear it first. */
    errno = 0;
    struct passwd *pw = getpwuid(ruid);
    if (pw == NULL) {
        if (errno != 0)
            perror("getpwuid");
        else
            fprintf(stderr, "no passwd entry for uid %d\n", (int)ruid);
        return 1;
    }
    printf("you are: %s (home: %s, shell: %s)\n",
           pw->pw_name, pw->pw_dir, pw->pw_shell);

    if (ruid != euid)
        printf(">> effective UID differs from real UID: "
               "this process is running with ELEVATED privilege "
               "(setuid in effect).\n");
    else
        printf(">> real == effective: ordinary privilege.\n");

    return 0;
}
```

**Expected output (run normally as UID 1000):**
```
real UID = 1000, effective UID = 1000
real GID = 1000, effective GID = 1000
you are: amith (home: /home/amith, shell: /bin/bash)
>> real == effective: ordinary privilege.
```

**Expected output (after making it setuid-root, `chmod u+s`, launched by UID 1000):**
```
real UID = 1000, effective UID = 0
real GID = 1000, effective GID = 1000
you are: amith (home: /home/amith, shell: /bin/bash)
>> effective UID differs from real UID: this process is running with ELEVATED privilege (setuid in effect).
```

**Walkthrough of the non-obvious parts:**
- `getuid()` vs `geteuid()` — the whole lesson. `getuid` never lies about who launched you; `geteuid` is who the kernel uses to *check permissions*. In `passwd`, `geteuid()` returns 0 while `getuid()` returns your 1000 — so `passwd` can write `/etc/shadow` (uses euid) yet still knows it's *you* and refuses to change someone else's password (uses ruid).
- `errno = 0;` before `getpwuid` — a real portability gotcha. `getpwuid` returning `NULL` can mean "not found" (errno untouched) *or* "error" (errno set). You must zero errno first to tell them apart. This is exactly the kind of "error handling is part of the lesson" the course insists on.
- `(int)ruid` casts — `uid_t` is an unsigned integer type of unspecified width; casting to `int` for `%d` avoids format-mismatch warnings under `-Wextra`.

### Program 2 — `pathfind.c`: resolve a command against `$PATH`, exactly like the shell

```c
/* pathfind.c
 *
 * Given a command name, find the executable the shell WOULD run, by
 * searching each directory in $PATH in order -- reproducing step 4 of
 * the shell's REPL. Also the perfect "first real command" to install
 * into your own PATH.
 *
 * Compile:  gcc -Wall -Wextra -o pathfind pathfind.c
 * Run:      ./pathfind ls
 *           ./pathfind gcc
 *           ./pathfind definitely-not-a-real-command   ; echo $?
 */

#include <stdio.h>
#include <stdlib.h>     /* getenv        */
#include <string.h>     /* strtok, strlen */
#include <unistd.h>     /* access, X_OK   */

int main(int argc, char *argv[])
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s COMMAND\n", argv[0]);
        return 2;                       /* 2 = usage error, a common convention */
    }
    const char *cmd = argv[1];

    /* If the name already contains a slash, the shell does NOT search
     * PATH -- it uses the path as given. We mirror that. */
    if (strchr(cmd, '/') != NULL) {
        if (access(cmd, X_OK) == 0) {   /* X_OK = "is it executable by me?" */
            printf("%s\n", cmd);
            return 0;
        }
        fprintf(stderr, "%s: not found or not executable\n", cmd);
        return 1;
    }

    /* Read $PATH. If unset, fall back to a sane default like the shell. */
    const char *path = getenv("PATH");
    if (path == NULL)
        path = "/usr/local/bin:/usr/bin:/bin";

    /* strtok mutates its input, so work on a copy. strdup mallocs. */
    char *copy = strdup(path);
    if (copy == NULL) {
        perror("strdup");
        return 1;
    }

    char candidate[4096];
    char *dir = strtok(copy, ":");      /* split on ':' */
    int found = 0;
    while (dir != NULL) {
        /* Build "<dir>/<cmd>" safely. snprintf never overflows the buffer
         * and tells us if it would have (return >= size). */
        int n = snprintf(candidate, sizeof candidate, "%s/%s", dir, cmd);
        if (n > 0 && (size_t)n < sizeof candidate) {
            /* access() asks the kernel: does this path exist AND may I
             * execute it, using my REAL uid/gid? (X_OK) */
            if (access(candidate, X_OK) == 0) {
                printf("%s\n", candidate);
                found = 1;
                break;                  /* first match wins -- like the shell */
            }
        }
        dir = strtok(NULL, ":");        /* next directory */
    }

    free(copy);

    if (!found) {
        fprintf(stderr, "%s: command not found\n", cmd);
        return 1;                       /* mirrors the shell's 127-ish "not found" */
    }
    return 0;
}
```

**Expected output:**
```
$ ./pathfind ls
/usr/bin/ls

$ ./pathfind gcc
/usr/bin/gcc

$ ./pathfind definitely-not-a-real-command ; echo $?
definitely-not-a-real-command: command not found
1
```

**Walkthrough of the non-obvious parts:**
- `strchr(cmd, '/')` — replicates the shell's rule: a name with a slash is a path, not a PATH-searchable command. This is the mechanical reason `./mycmd` works but `mycmd` (without `./`) doesn't, unless `mycmd`'s directory is in `$PATH`.
- `access(candidate, X_OK)` — asks the kernel "does it exist and can I execute it?" **using the real UID/GID**, not effective. That subtlety matters: `access` deliberately checks real IDs so setuid programs can test "could the *actual* user do this?" There's a famous TOCTOU (time-of-check-to-time-of-use) security bug lurking here — see Gotchas.
- `strdup` + `free` — `strtok` writes null bytes into its input string, so you must never hand it a string you don't own (like the `getenv` result, which points into the process's environment). We copy first, free after. This is the manual memory discipline (Module 2) that Java's GC hides from you.
- `snprintf(...)` with the return-value check — the bounds-safe way to build a path. `sprintf` would risk a buffer overflow; `snprintf` caps the write and returns how many chars it *wanted*, so `n >= size` tells you it truncated.

---

## Under the Hood

### Part 1 — what `access()` and PATH search look like at the syscall wall

Run `strace ./pathfind ls` and look at the tail:

```
execve("./pathfind", ["./pathfind", "ls"], 0x7ffc... ) = 0        ← the shell exec'd us
...
access("/usr/local/bin/ls", X_OK)      = -1 ENOENT (No such file or directory)  ← [1] miss
access("/usr/bin/ls", X_OK)            = 0                                        ← [2] HIT
write(1, "/usr/bin/ls\n", 12)          = 12                                       ← [3] print result
exit_group(0)                          = ?
```

Annotated:
1. **`access("/usr/local/bin/ls", X_OK) = -1 ENOENT`** — your first `$PATH` directory doesn't contain `ls`. The kernel returns `-1` and sets errno to `ENOENT` ("no such file"). This *is* the loop iteration: one `access` syscall per directory tried.
2. **`access("/usr/bin/ls", X_OK) = 0`** — second directory, hit. Return 0 = "yes, exists and you may execute it." The loop breaks here. You are watching PATH resolution happen, one syscall per candidate, exactly as bash does it internally.
3. **`write(1, "/usr/bin/ls\n", 12)`** — the single `write` that prints the answer (Module 0's lesson: your output is one wall-crossing).

The headline: **the shell's "find the command" step is just a sequence of `access` (or `stat`) syscalls, one per `$PATH` entry, first success wins.** When you `strace bash -c ls`, you'll see this same pattern buried in bash's own trace. You didn't learn a shell feature — you learned a syscall loop.

### Part 2 — how setuid actually flips euid (deep-dive)

When you `exec` a binary, the kernel (in the `execve` path, `fs/exec.c`) inspects the file's mode bits. The relevant moment:

```
   execve("/usr/bin/passwd", ...) enters the kernel
        │
        ├─ kernel reads the file's inode: owner=0 (root), mode has S_ISUID set
        │
        ├─ normally: new process euid = caller's euid (unchanged)
        │
        └─ BUT S_ISUID is set  ->  new process euid = FILE OWNER (0)
                                    (real uid stays = caller = 1000)
        │
        ▼
   passwd now runs with  ruid=1000  euid=0
   -> open("/etc/shadow", O_RDWR) succeeds because the kernel's permission
      check on the OWNER-writable file uses EUID (0 = root = allowed)
```

So the privilege elevation is decided *at exec time*, by the kernel, based on one bit in the file's mode. No syscall the running program makes can "grab" root that wasn't granted this way — the kernel handed it over during `execve` because the on-disk binary was marked setuid *and owned by root*. That's why `find / -perm -4000` (list all setuid files) is a security audit ritual: each of those files is a potential door to root, and each is trusted to use its power correctly for exactly one purpose and then get out.

---

## Try This

Ordered easy → hard.

1. **(Easy) Read your own identity.** Run `id` and match its output to `idinfo`'s. Then run `cat /etc/passwd | grep $USER` and find your UID, home, and shell in the raw file `idinfo` is reading via `getpwuid`. *Hint: `getpwuid` is just a structured `grep` of `/etc/passwd`.*

2. **(Easy) Decode permissions.** Run `ls -l /usr/bin/passwd /etc/shadow /bin/ls`. For each, write the octal permission and identify who can do what. Find the `s` in `passwd`'s mode. *Hint: `rwsr-xr-x` — the `s` replaces owner's `x`.*

3. **(Medium) Make `idinfo` setuid and watch euid change.** Compile it, then:
   ```
   sudo chown root:root idinfo
   sudo chmod u+s idinfo
   ls -l idinfo          # confirm you see 'rws'
   ./idinfo              # launched as YOU, but effective UID is now 0
   ```
   Explain in one sentence why `real UID` stayed 1000 but `effective UID` became 0. *Hint: reread Under the Hood Part 2. Undo with `sudo chmod u-s idinfo` when done — a setuid binary lying around is a security smell.*

4. **(Medium) Install your own command into PATH.** Take `pathfind` (or `mycmd` from Module 0), copy it into a directory on your `$PATH`:
   ```
   mkdir -p ~/bin
   cp pathfind ~/bin/
   echo $PATH | tr ':' '\n' | grep -q "$HOME/bin" || echo 'export PATH="$HOME/bin:$PATH"' >> ~/.bashrc
   # open a new shell, then:
   pathfind ls          # runs WITHOUT ./  -- it's a real command now
   ```
   Confirm `which pathfind` reports `~/bin/pathfind`. You just closed the Module 0 loop: your C program is now a first-class command. *Hint: `which` does the same PATH search your `pathfind` does.*

5. **(Hard) Break the group-triad gotcha on purpose.** Create a file, put yourself in its group, but set the group triad to `---` and others to `r--`:
   ```
   echo secret > /tmp/g.txt
   chmod 604 /tmp/g.txt        # owner rw-, group ---, other r--
   ```
   Now, as a *different* user who is in the file's group but not the owner, try to read it. It fails — even though "other" can read — because the kernel stops at the *group* triad (first match by category) and doesn't fall through to "other." Explain why. *Hint: reread Concept 2: the kernel picks exactly one triad by category (owner? group? other?) and checks only that one.*

---

## Gotchas

- **`./mycmd` vs `mycmd`.** A name with no slash is searched in `$PATH`; a name with a slash is used as-is. `.` (current dir) is intentionally *not* in `$PATH` for security (so an attacker can't drop a malicious `ls` in a directory you `cd` into and hijack you). That's why you must type `./mycmd`. Interviewers ask "why do you need the `./`?" — this is the answer.

- **Confusing real and effective UID.** `getuid()` = who you are; `geteuid()` = who you're acting as. Permission checks in `open()` use **effective**; `access()` deliberately uses **real**. Mixing these up is the source of many setuid security holes. Classic interview question: "In a setuid-root program, which UID does `open()` check against?" Answer: **effective** (0/root).

- **The `access()` then `open()` TOCTOU race.** Using `access()` to check "can the real user open this?" and then `open()`ing it is a **time-of-check-to-time-of-use** vulnerability: between the two calls an attacker can swap the file (e.g. replace it with a symlink to `/etc/shadow`). Never gate a privileged `open` on a prior `access`. The correct fix is to drop privileges or use `openat`/`O_NOFOLLOW`. This is a famous setuid pitfall and a strong senior-level interview topic.

- **Octal permission confusion.** `chmod 777` (world-writable everything) is almost always wrong and a security red flag, not a "fix your permission problem" hammer. `chmod +x` sets execute for *all three* triads unless you say `u+x`. And `chmod 644` vs `chmod 655` matters — always think in the three triads.

- **`root` is not magic in the way people think.** Root (UID 0) bypasses the *permission* checks (Concept 2), but root still runs in **ring 3** (Module 0) and still makes syscalls to do anything. Root ≠ kernel. A root process that dereferences a bad pointer still segfaults. "I'm root so I can do anything" is only true about file/resource *permissions*, not about the ring boundary.

- **`strtok` on a string you don't own.** `strtok` writes `\0` into its input. Calling it on the pointer returned by `getenv("PATH")` corrupts your process's environment. Always `strdup` first (as `pathfind` does). This bites people porting from GC languages where "just tokenize the string" is harmless.

- **Assuming drive letters / backslashes.** Coming from Windows: there is one tree rooted at `/`, separators are forward slashes, and other disks are *mounted* into the tree, not addressed as `D:\`. Paths are case-sensitive: `/etc/Passwd` ≠ `/etc/passwd`.

---

## Checkpoint

Answer from memory, then check below.

1. What is the difference between a process's **real UID** and **effective UID**, and which one does the kernel use when checking whether an `open()` is allowed?
2. `/usr/bin/passwd` shows mode `-rwsr-xr-x` and is owned by `root`. Explain, step by step, how an ordinary user running it manages to modify root-owned `/etc/shadow`.
3. You type `mycmd` and get "command not found," but `./mycmd` works. Why? What would you change to make `mycmd` work without the `./`?
4. In the permission model, a file is `rw----r--` (owner rw, group nothing, other read). A user who is *in the file's group* but is not the owner tries to read it. Does it succeed? Why or why not?
5. Name the syscall the shell (and your `pathfind`) uses once per `$PATH` directory to test each candidate, and say what its return value of `0` means.

---

<details>
<summary><b>Checkpoint Answers</b></summary>

1. **Real UID** = who launched the process (who you actually are); **effective UID** = the identity the kernel uses for permission checks. An `open()` permission check uses the **effective UID**. (Real UID is used to remember the true launcher, e.g. so a setuid program still knows who you are; `access()` also uses real UID.)

2. `passwd` has the **setuid bit** set (`s` in the owner triad) and is **owned by root**. When the user `exec`s it, the kernel — seeing `S_ISUID` set and owner=0 — sets the new process's **effective UID to 0 (root)** while leaving its **real UID** as the user (e.g. 1000). Now inside `passwd`, permission checks on `/etc/shadow` (root-writable) use the effective UID (0), so the write is allowed. The real UID (1000) lets `passwd` still verify you're only changing *your own* password.

3. `mycmd` (no slash) is searched for in the directories listed in `$PATH`, and the current directory `.` is not in `$PATH` (by design, for security), so it isn't found. `./mycmd` contains a slash, so the shell treats it as a direct path and skips the search. To make `mycmd` work without `./`, copy it into a directory that *is* on your `$PATH` (e.g. `~/bin` after adding `~/bin` to `$PATH`), or otherwise place/symlink it under `/usr/local/bin`.

4. It **fails** (permission denied). The kernel selects exactly **one** triad by category: since the user is in the file's **group** (but not the owner), the **group** triad applies — and it is `---` (no permissions). The kernel does **not** fall through to the "other" triad even though "other" has read. First matching category wins.

5. **`access()`** (with `X_OK`) — some shells use `stat()`/`faccessat()`, but `access(path, X_OK)` is the canonical one. A return value of **0** means "the file exists and the calling process may execute it" (using the real UID/GID); `-1` (with errno like `ENOENT`) means it doesn't exist or isn't executable.

</details>

---

*Next up: **Module 2 — C for Systems Programming.** Pointers and memory as the OS sees them, the stack vs the heap, `malloc`/`free` vs Java's GC, and the compilation pipeline that turns your `.c` into an ELF binary. Say **"next"** (or **"continue"**) when you're ready.*
