# Module 4 — Console I/O and the Terminal

> **Estimated time:** 2–3 hours · **Core path:** Concepts 1–3 and the `pressanykey` program are core. The full `termios` flag tour and the pipe/redirection-from-the-shell section are useful but lighter — skim on a first pass.
>
> **Prerequisites:** Modules 0–3. You know fds, the three-table model, and buffered vs unbuffered I/O. The terminal is where all of that becomes visible and interactive.

---

## The Big Picture

Every program you've run so far has quietly assumed a magical thing on the other end of fds 0, 1, and 2: a *terminal*. When you type into your shell and see characters echo back, when you press Backspace and the last letter vanishes, when you hit Ctrl-C and the program dies — none of that is your program's doing. It's the **terminal subsystem**, a piece of the kernel sitting between your keyboard and your process, doing an enormous amount of invisible work. This module pulls that curtain back. Once you see what the terminal is actually doing, a whole class of "why does my program behave weirdly interactively" mysteries dissolves, and you gain the power to build things like text editors, REPLs, progress bars, and games that need character-at-a-time input.

The key realization is that **stdin, stdout, and stderr are just fds 0, 1, and 2** — nothing more. They're pre-opened for you by the shell before your program starts (the shell inherits them from *its* parent, all the way back to login). Usually all three point at the same terminal device, which is why your output and your typed input share a screen. But because they're ordinary fds, they can be independently redirected — and that's the entire basis of shell plumbing. `2>&1` isn't shell magic; it's "make fd 2 point wherever fd 1 points," a `dup2` you now understand from Module 3. Seeing stderr as a *separate fd* from stdout is what lets you write `command > out.log 2> err.log` and split them.

Between your process and the physical terminal sits the **line discipline** — a kernel module that processes characters as they flow. In its default *canonical mode*, it buffers a whole line, handles your Backspace and Ctrl-U editing *before your program ever sees the input*, echoes what you type back to the screen, and only delivers the line to `read()` when you press Enter. It also turns Ctrl-C into a signal (Module 5), Ctrl-D into end-of-file, and Ctrl-Z into suspend. Your program, calling `read(0, ...)`, receives a clean edited line and has no idea how much cooking happened upstream. This is a beautiful abstraction — and sometimes exactly the wrong one, because if you're building a game or an editor you want *every keystroke immediately*, not a line at a time with the kernel eating your arrow keys as editing commands.

That's where **`termios`** comes in: the API to reconfigure the line discipline. Flip the terminal into *raw mode* and suddenly `read()` returns each keystroke the instant it's pressed, with no echo, no line editing, no signal generation — your program sees the raw bytes. Every full-screen terminal program you've ever used (vim, less, htop, top, nano) does exactly this dance: save the current terminal settings, switch to raw mode, do its thing, and — critically — *restore the settings on exit* so it doesn't leave your shell broken. You'll build a "press any key" reader that does this properly, and in doing so you'll understand the machinery behind every TUI on your system.

By the end you'll understand why output ordering sometimes surprises you (buffering, revisited from Module 3 but now with the terminal's role clear), what a TTY actually is, how to read single keystrokes, and how the shell's `|`, `>`, and `2>&1` manipulate the fds you now know intimately. This is the module that makes the terminal stop being magic.

---

## Concepts

### 1. stdin, stdout, stderr — fds 0, 1, 2

**What it is:** Every process starts with three standard descriptors already open, by convention:

```
   fd 0  STDIN   ── where the program READS input from   (default: keyboard)
   fd 1  STDOUT  ── where NORMAL output goes             (default: screen)
   fd 2  STDERR  ── where ERROR/diagnostic output goes   (default: screen)
```

They're set up by the shell (which inherited them, ultimately from the login process) and handed to your program across `fork`/`exec` (Module 5). Normally all three point at your terminal device (e.g. `/dev/pts/0`), which is why typed input and program output share one screen. The split between **stdout (1)** and **stderr (2)** is deliberate and important: normal results go to 1, errors/logs go to 2, so they can be redirected *separately*. `program > results.txt` sends only fd 1 to the file; errors still appear on screen via fd 2. That's why well-behaved tools write errors with `fprintf(stderr, ...)` (as our earlier programs did) — so a user piping the output doesn't get error text mixed into their data.

**Why it exists:** Three standard streams give every program a uniform, redirectable I/O interface without the program needing to know *where* its input/output actually go. Composability again: because they're just fds, the shell can rewire them.

**Java analogy:** `System.in` (fd 0), `System.out` (fd 1), `System.err` (fd 2) — *literally* these three descriptors, wrapped in stream objects. You already write errors to `System.err` and results to `System.out` for the same separation reason. `System.out` being line-buffered-ish and `System.err` being unbuffered mirrors the C defaults exactly.

### 2. Buffering, revisited: why output order surprises you

**What it is:** (Building on Module 3, Concept 5, now with the terminal's role explicit.) stdout and stderr have *different default buffering* when connected to a terminal:

```
   stdout (fd 1):  LINE-buffered when it's a terminal  → flushes on '\n'
                   FULLY-buffered when redirected to a file/pipe → flushes when full
   stderr (fd 2):  UNBUFFERED always → every write appears immediately
```

This produces the classic surprise: interleave `printf` (stdout) and `fprintf(stderr,...)` (stderr) and the order on screen may not match your source order — because stderr flushes instantly while stdout sits in a buffer until a newline (or until the buffer fills, if redirected). Redirect stdout to a file and it gets *worse*: now stdout is fully buffered, so a `printf("progress...")` without a newline may not appear for a long time, or appear out of order relative to stderr. The fixes: `fflush(stdout)` at the points you need output to be visible, or `setvbuf` to change the mode, or write diagnostics to stderr (which is always unbuffered).

**Why it exists:** Performance vs immediacy tradeoff. Buffering stdout batches many small writes into few syscalls (fast). Leaving stderr unbuffered guarantees you see errors *before* a crash swallows the buffered stdout — critical for debugging. The terminal-vs-file distinction exists because interactive users want line-by-line responsiveness, while file/pipe consumers want throughput.

**Java analogy:** `System.err` is auto-flushed and unbuffered; `System.out` is a `PrintStream` with autoflush on newline when interactive. The "my log lines came out in the wrong order" bug happens in Java too, for the identical reason, and the fix is identical: `flush()`. Anyone who's debugged interleaved `System.out`/`System.err` in a captured log has met this.

### 3. The TTY subsystem and the line discipline

**What it is:** "TTY" is a historical name (teletype) for a terminal. Between your keyboard/screen and your program sits a kernel layer called the **line discipline**:

```
   KEYBOARD ──bytes──►  ┌──────────────────────────────┐  ──►  your program's
                        │   TTY / LINE DISCIPLINE       │       read(0, ...)
                        │   (kernel)                    │
   SCREEN  ◄──echo───   │  - echoes chars back to screen│
                        │  - buffers a whole LINE       │
                        │  - handles Backspace, Ctrl-U  │  ◄──  your program's
                        │  - Ctrl-C → SIGINT (a signal) │       write(1, ...)
                        │  - Ctrl-D → EOF               │
                        │  - Ctrl-Z → SIGTSTP (suspend) │
                        └──────────────────────────────┘
```

In the default **canonical (cooked) mode**, the line discipline:
- **Echoes** each character you type back to the screen (your program didn't do that!).
- **Buffers a full line**, letting you edit it with Backspace/Ctrl-U *before* the program sees anything.
- Delivers the line to `read()` only when you press **Enter**. So `read(0, buf, 100)` blocks until you hit Enter, then returns the whole line at once.
- **Translates control keys into signals**: Ctrl-C → `SIGINT` (kill), Ctrl-\ → `SIGQUIT`, Ctrl-Z → `SIGTSTP` (suspend). These are generated by the *terminal*, not your program (Module 5 handles them).
- **Ctrl-D** sends end-of-file: `read` returns 0 (which is why Ctrl-D exits a shell or `cat`).

**Why it exists:** So that *every* program gets free, consistent line editing and job control without implementing it. Imagine if `cat`, `grep`, and `bash` each had to handle Backspace themselves — chaos. The kernel does it once, uniformly. It's the same "provide a universal service" instinct as the fd abstraction.

**Java analogy:** **No direct equivalent** — the JVM has no concept of a line discipline; it just reads bytes from fd 0. When you call `Scanner.nextLine()` or `BufferedReader.readLine()`, the *kernel's* line discipline already did the line-buffering and editing; Java just receives the finished line. This is why reading a single keypress in pure Java is famously awkward (`System.in.read()` still waits for Enter in canonical mode) — Java has no portable API to switch the terminal to raw mode, so people shell out to `stty`. That awkwardness *is* this concept leaking through.

### 4. `termios`: raw mode vs canonical mode

**What it is:** `termios` is the POSIX API to inspect and change terminal settings — i.e., reconfigure the line discipline. The workflow every TUI uses:

```
   1. tcgetattr(fd, &saved)      -- save current settings
   2. copy saved -> raw; modify raw's flags to disable cooking:
        raw.c_lflag &= ~(ICANON | ECHO);   -- turn OFF line-buffering and echo
   3. tcsetattr(fd, TCSANOW, &raw)  -- apply raw mode
   4. ... read keystrokes one at a time ...
   5. tcsetattr(fd, TCSANOW, &saved) -- RESTORE, or you leave the shell broken
```

The `struct termios` has four flag fields — `c_iflag` (input), `c_oflag` (output), `c_cflag` (control), `c_lflag` (local) — plus a `c_cc[]` array of control characters. The two you care about most live in `c_lflag`:
- **`ICANON`** — canonical mode. **On** = line-buffered/cooked (default). **Off** = raw: `read` returns bytes as they arrive.
- **`ECHO`** — echo typed characters. **On** = you see what you type (default). **Off** = silent (how password prompts hide input).

In raw mode, `c_cc[VMIN]` and `c_cc[VTIME]` control how `read` blocks: `VMIN=1, VTIME=0` means "return as soon as at least 1 byte is available" — perfect for a keypress reader.

**Why it exists:** Some programs need character-at-a-time, unechoed, signal-free input — editors, pagers, games, password prompts. `termios` is the knob that turns off the kernel's helpful-but-unwanted cooking. Turning off `ECHO` alone is how `sudo`/`passwd` read your password without displaying it.

**Java analogy:** **No portable Java API for this.** JLine and similar libraries exist precisely to wrap the native `termios` calls (or shell out to `stty`) because the JVM won't do it for you. When a Java CLI reads a password with `Console.readPassword()`, it's asking the underlying terminal to disable echo — the `ECHO` flag, reached indirectly. This is a place where "Java can't do this natively" is the honest answer, and it's *why* those libraries exist.

### 5. Redirection and pipes from the shell side

**What it is:** Now that you know fds and `dup2` (Module 3), the shell's plumbing operators demystify completely:

```
   command > file       open file, dup2(file_fd, 1)         stdout → file
   command >> file       open with O_APPEND, dup2(fd, 1)     stdout → end of file
   command < file        open file O_RDONLY, dup2(fd, 0)     stdin  ← file
   command 2> file       open file, dup2(fd, 2)              stderr → file
   command 2>&1          dup2(1, 2)                          stderr → wherever stdout goes
   cmd1 | cmd2           pipe(); wire cmd1's fd 1 → cmd2's fd 0  (Module 8)
```

The order matters for `2>&1`: `> file 2>&1` sends both to the file (redirect 1 to file, *then* point 2 at 1), but `2>&1 > file` sends stderr to the *original* screen (point 2 at 1 while 1 is still the screen, *then* move 1 to the file). This ordering gotcha is a direct consequence of `dup2` copying "where fd 1 points *right now*." Pipes (`|`) are Module 8's topic, but the mechanism is the same fd-rewiring: the shell creates a pipe, `dup2`s the write end onto cmd1's fd 1 and the read end onto cmd2's fd 0.

**Why it exists:** Composability. Because I/O is just fds and fds can be rewired with `dup2`, the shell can connect programs' inputs and outputs arbitrarily without the programs knowing. This is the Unix philosophy's superpower — small tools, wired together.

**Java analogy:** `ProcessBuilder.redirectOutput(File)`, `.redirectErrorStream(true)` (that last one is `2>&1`), and connecting processes via `.pipe`-style wiring. Java exposes these as methods precisely because it can't do `dup2` on its own fds the way the shell does — it asks the OS to set up the redirection when launching the child.

---

## Code

### Program 1 — `pressanykey.c`: raw-mode single-keystroke reader (with proper restore)

```c
/* pressanykey.c
 *
 * Switches the terminal into RAW mode to read ONE keystroke at a time,
 * with no line-buffering and no echo -- the machinery behind vim, less,
 * htop. Critically, it RESTORES the original terminal settings on exit
 * (even via atexit), so it never leaves your shell broken.
 *
 * Compile:  gcc -Wall -Wextra -o pressanykey pressanykey.c
 * Run:      ./pressanykey        (press keys; 'q' quits)
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>     /* read, STDIN_FILENO, isatty */
#include <termios.h>    /* tcgetattr, tcsetattr, struct termios */
#include <errno.h>

static struct termios g_saved;   /* original settings, to restore */
static int g_raw_active = 0;

/* Restore cooked mode. Registered with atexit so it runs no matter how
 * we leave main -- the single most important habit in terminal code. */
static void restore_terminal(void)
{
    if (g_raw_active) {
        tcsetattr(STDIN_FILENO, TCSANOW, &g_saved);
        g_raw_active = 0;
    }
}

static int enable_raw_mode(void)
{
    if (!isatty(STDIN_FILENO)) {                 /* not a terminal? bail. */
        fprintf(stderr, "stdin is not a terminal\n");
        return -1;
    }
    if (tcgetattr(STDIN_FILENO, &g_saved) < 0) { /* save current settings */
        perror("tcgetattr");
        return -1;
    }
    atexit(restore_terminal);                    /* guarantee restore */

    struct termios raw = g_saved;                /* start from a copy */
    /* Turn OFF: ICANON (line-buffering) and ECHO (visible typing).
     * ISIG left ON so Ctrl-C still works here; real editors turn it off. */
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN]  = 1;   /* read returns after >=1 byte */
    raw.c_cc[VTIME] = 0;   /* with no timeout: block until a key */

    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) < 0) {
        perror("tcsetattr");
        return -1;
    }
    g_raw_active = 1;
    return 0;
}

int main(void)
{
    if (enable_raw_mode() < 0)
        return 1;

    printf("Raw mode ON. Press keys (they appear immediately, no Enter).\n");
    printf("Press 'q' to quit.\n");
    fflush(stdout);   /* flush: we're about to read char-by-char */

    char c;
    ssize_t n;
    while ((n = read(STDIN_FILENO, &c, 1)) == 1) {   /* one byte at a time */
        if (c == 'q') {
            printf("\r\nBye.\r\n");     /* \r\n: raw mode needs explicit CR */
            break;
        }
        /* Show the byte and its numeric code. Printable? show it; else code. */
        if (c >= 32 && c < 127)
            printf("you pressed '%c' (code %d)\r\n", c, c);
        else
            printf("you pressed control/non-printable (code %d)\r\n", c);
        fflush(stdout);
    }
    if (n < 0 && errno != 0) perror("read");

    /* restore_terminal() runs via atexit here too. */
    return 0;
}
```

**Expected output (interactive):**
```
$ ./pressanykey
Raw mode ON. Press keys (they appear immediately, no Enter).
Press 'q' to quit.
you pressed 'a' (code 97)      ← appeared the instant you hit 'a', no Enter, no echo of 'a'
you pressed 'H' (code 72)
you pressed control/non-printable (code 27)   ← e.g. the Escape key / arrow-key prefix
Bye.
```

**Walkthrough of the non-obvious parts:**
- `isatty(STDIN_FILENO)` — checks fd 0 actually *is* a terminal. If you pipe input in (`echo x | ./pressanykey`), there's no terminal to put in raw mode; we detect and bail cleanly. Real tools do this.
- `tcgetattr(&g_saved)` then `atexit(restore_terminal)` — **save first, register the restore immediately.** This is the discipline that separates working terminal code from code that leaves users with a broken shell (no echo, no line editing). If your program crashes or exits any way, `atexit` restores cooked mode. (Type `reset` if you ever get stuck in a broken terminal.)
- `raw.c_lflag &= ~(ICANON | ECHO)` — the actual raw switch: clear the ICANON (line mode) and ECHO bits. Now `read` returns per-keystroke and typing is invisible. Note we left `ISIG` on so Ctrl-C still kills this demo; a real editor clears it too (and handles Ctrl-C itself).
- `VMIN=1, VTIME=0` — "block until at least 1 byte, no timeout." That makes `read(0, &c, 1)` return the moment a key is pressed. Setting `VTIME>0` would add a read timeout (useful for detecting multi-byte escape sequences from arrow keys).
- `\r\n` instead of `\n` — in raw mode the terminal no longer auto-translates `\n` into carriage-return+newline, so you must emit `\r\n` yourself or lines "stair-step" down the screen. A dead giveaway you're in raw mode.

### Program 2 — `buffer_demo.c`: see stdout vs stderr ordering, and buffering modes

```c
/* buffer_demo.c
 *
 * Demonstrates why output ORDER can surprise you: stdout is line-buffered
 * (to a terminal) or fully-buffered (to a file), while stderr is always
 * unbuffered. Run it to the terminal, then redirect, and compare.
 *
 * Compile:  gcc -Wall -Wextra -o buffer_demo buffer_demo.c
 * Run:      ./buffer_demo
 *           ./buffer_demo > out.txt 2> err.txt ; echo "--out--"; cat out.txt; echo "--err--"; cat err.txt
 *           ./buffer_demo 2>&1 | cat        # merge streams through a pipe
 */

#include <stdio.h>
#include <unistd.h>

int main(void)
{
    /* Interleave stdout and stderr WITHOUT newlines to expose buffering.
     * On a terminal you may still see them roughly in order (line-buffered
     * flushes at program exit too); redirect stdout to a file and the
     * ordering / timing changes because stdout becomes FULLY buffered. */
    fprintf(stdout, "1:stdout ");   /* buffered */
    fprintf(stderr, "2:stderr ");   /* UNbuffered -> appears immediately */
    fprintf(stdout, "3:stdout ");   /* buffered */
    fprintf(stderr, "4:stderr ");   /* UNbuffered */

    /* Force stdout out now so the final newline groups things sensibly. */
    fprintf(stdout, "\n");
    fflush(stdout);                 /* explicit flush: guarantee visibility */

    /* Demonstrate changing the buffering mode yourself. */
    setvbuf(stdout, NULL, _IONBF, 0);   /* make stdout UNbuffered too */
    printf("now stdout is unbuffered: this appears instantly\n");

    return 0;
}
```

**Expected output (to a terminal — roughly):**
```
2:stderr 4:stderr 1:stdout 3:stdout
now stdout is unbuffered: this appears instantly
```
(stderr's `2` and `4` may appear *before* stdout's `1` and `3` because stderr is unbuffered and stdout's bytes waited in the buffer until the flush.)

**Expected output (`./buffer_demo > out.txt 2> err.txt`):**
```
--out--
1:stdout 3:stdout
now stdout is unbuffered: this appears instantly
--err--
2:stderr 4:stderr
```
(Redirected, the streams are cleanly separated into their files — and stdout is now fully buffered, all emitted at flush/exit.)

**Walkthrough of the non-obvious parts:**
- The interleaving without newlines — deliberately exposes that stderr (unbuffered) races ahead of stdout (buffered). This is *the* reason your logs sometimes look scrambled.
- `2> err.txt` splitting — because stderr is a *separate fd* (Concept 1), redirecting it separately captures only the error stream. That's the practical payoff of the stdout/stderr split.
- `setvbuf(stdout, NULL, _IONBF, 0)` — you can change the mode: `_IONBF` (unbuffered), `_IOLBF` (line), `_IOFBF` (full). Servers and daemons often force line- or unbuffered stdout so logs appear promptly even when redirected to a file/pipe (otherwise fully-buffered logs can "disappear" until a crash flushes nothing). Real-world fix for "my container logs are delayed."

---

## Under the Hood

Run `pressanykey` under strace to watch the terminal reconfiguration and per-keystroke reads:

```
$ strace -e trace=ioctl,read,write ./pressanykey
```
(press `a`, then `q`)

```
ioctl(0, TCGETS, {c_lflag=ICANON|ECHO|ISIG|..., ...}) = 0    ← [1] tcgetattr: read current settings
ioctl(0, TCSETS, {c_lflag=ISIG|..., ...})            = 0    ← [2] tcsetattr: ICANON & ECHO now CLEARED
write(1, "Raw mode ON...\n", ...)                    = ...
read(0, "a", 1)                                       = 1    ← [3] ONE byte, the instant you pressed 'a'
write(1, "you pressed 'a' (code 97)\r\n", 27)        = 27
read(0, "q", 1)                                       = 1    ← [4] the 'q'
write(1, "\r\nBye.\r\n", 8)                           = 8
ioctl(0, TCSETS, {c_lflag=ICANON|ECHO|ISIG|..., ...}) = 0    ← [5] restore: ICANON & ECHO back ON
exit_group(0)                                        = ?
```

Annotated:
1. **`ioctl(0, TCGETS, ...)`** — surprise: `tcgetattr` is really an **`ioctl`** (Module 11) on fd 0. Terminal control doesn't fit `read`/`write`, so it uses `ioctl` — the "everything else" I/O syscall. You can see the current `c_lflag` includes `ICANON|ECHO` (cooked mode).
2. **`ioctl(0, TCSETS, ...)`** — `tcsetattr` applying raw mode. Compare the `c_lflag`: `ICANON` and `ECHO` are **gone**. The line discipline is now in raw mode for fd 0.
3. **`read(0, "a", 1) = 1`** — the payoff. In cooked mode, `read` would block until Enter and return the whole line. Here it returns **one byte the instant you pressed `a`** — no Enter, no echo (notice there's no automatic echo write; only *your* explicit write shows the key). This is raw mode working.
4–5. **The final `ioctl(0, TCSETS, ...)`** — `atexit`'s `restore_terminal` putting `ICANON|ECHO` back. Without this line, your shell would be left in raw mode (no echo, no line editing) — the "broken terminal" everyone hits once. Its presence in the trace is the proof your cleanup ran.

The headline: **terminal control is done via `ioctl` on fd 0, raw mode is literally clearing two flag bits, and per-keystroke `read` returning 1 byte is what canonical mode was hiding from you.** You can now see, at the syscall level, the difference between "line at a time" and "key at a time."

---

## Try This

Ordered easy → hard.

1. **(Easy) Prove stderr is unbuffered.** Run `buffer_demo` to the terminal a few times and note the stdout/stderr ordering. Then run `./buffer_demo 2>/dev/null` (discard stderr) and `./buffer_demo 1>/dev/null` (discard stdout) and confirm which text each removes. *Hint: `1>` targets fd 1, `2>` targets fd 2.*

2. **(Easy) Hide a password.** Copy `pressanykey`'s raw-mode setup but clear only `ECHO` (keep `ICANON`), then read a line with `fgets`. You've built a password prompt: the user types normally with Enter, but nothing shows. Restore afterward. *Hint: `raw.c_lflag &= ~ECHO;` only. This is exactly what `sudo` does.*

3. **(Medium) Detect the terminal size.** Use `ioctl(1, TIOCGWINSZ, &ws)` with `struct winsize ws;` to print the terminal's rows and columns. Resize your terminal and rerun. (This is a Module 11 preview — terminal size is another `ioctl`.) *Hint: `#include <sys/ioctl.h>`; fields are `ws.ws_row`, `ws.ws_col`.*

4. **(Medium) Read an arrow key.** In raw mode, press an arrow key and print the byte codes. You'll see a 3-byte escape sequence: `27` (ESC), `91` (`[`), then `65`/`66`/`67`/`68` (up/down/right/left). Explain why a single arrow press yields three `read`-able bytes. *Hint: terminals encode special keys as ANSI escape sequences; set `VTIME` to detect the sequence vs a lone Esc.*

5. **(Hard) Build a mini "menu" TUI.** Combine raw mode + arrow-key reading + `\r` cursor control to draw a 3-item menu where Up/Down move a `>` marker and Enter selects. Restore the terminal on every exit path (including Ctrl-C — install a handler, a Module 5 preview, or at minimum rely on `atexit`). Explain why forgetting the restore leaves the shell "broken" and how `reset` fixes it. *Hint: this is `htop`'s core loop in 40 lines. The restore discipline is the whole lesson.*

---

## Gotchas

- **Leaving the terminal in raw mode.** The cardinal sin. If your program switches to raw mode and exits (or crashes) without restoring, the user's shell has no echo and no line editing — it looks "broken." *Always* save settings and restore via `atexit` and/or signal handlers. Recovery for the user: type `reset` (blindly) and press Enter. Interviewers/reviewers of TUI code look for this immediately.

- **Assuming `read` on a terminal returns one keystroke.** In the *default* (canonical) mode it does **not** — it blocks until Enter and returns a whole line. Only in raw mode (`ICANON` off) do you get per-keystroke reads. Newcomers writing "press any key" in canonical mode are baffled when it waits for Enter. That confusion *is* the line discipline.

- **Buffered stdout "eating" your output on crash.** If stdout is fully buffered (redirected to a file/pipe) and your program crashes before flushing, the buffered output is **lost** — you see nothing in the log even though the `printf` "ran." Meanwhile stderr (unbuffered) survives. This is why crucial diagnostics go to stderr, and why daemons `setvbuf` stdout to line/unbuffered. Classic "why are my container logs empty/delayed" bug.

- **`\n` vs `\r\n` in raw mode.** Once `ICANON` (and output post-processing) is off, `\n` no longer implies a carriage return, so output stair-steps diagonally. Emit `\r\n`. Seeing stair-stepped text is the instant tell that you're in raw mode without handling CR.

- **`2>&1` ordering.** `> file 2>&1` (both to file) vs `2>&1 > file` (stderr to the *original* stdout, i.e. screen; stdout to file) do different things because `dup2` captures where the target fd points *at that moment*. Redirections are applied left to right. A perennial shell gotcha and interview question.

- **Forgetting `isatty`.** Code that assumes stdin/stdout is a terminal breaks when piped or redirected (`echo x | yourprog`). Check `isatty(fd)` before doing terminal-specific things (raw mode, colors, progress bars). This is why `ls` emits colors to a terminal but plain text into a pipe.

---

## Checkpoint

1. What are fds 0, 1, and 2 by convention, who sets them up before your program runs, and why is keeping stdout (1) and stderr (2) separate useful?
2. In the terminal's *canonical* mode, what does the line discipline do to your keystrokes before your program's `read()` sees them? Name three things.
3. What two `termios` `c_lflag` bits do you clear to get "read one keystroke, don't echo it," and which one alone gives you a hidden password prompt?
4. You redirect a program's stdout to a file and now its progress messages (printed without newlines) don't appear until it finishes. Why? Give a fix.
5. What does `command 2>&1 > file` do to stderr, and why is it different from `command > file 2>&1`?

---

<details>
<summary><b>Checkpoint Answers</b></summary>

1. **fd 0 = stdin, fd 1 = stdout, fd 2 = stderr.** They're set up by the **shell** (inherited across `fork`/`exec`, tracing back to the login process) before your program starts. Keeping stdout and stderr separate lets you **redirect normal output and errors independently** — e.g. `prog > results.txt 2> errors.log` — so error text doesn't contaminate piped/redirected data.

2. In canonical mode the line discipline: **(a)** echoes each typed character back to the screen; **(b)** buffers a whole line and handles line editing (Backspace, Ctrl-U) before the program sees it; **(c)** delivers input to `read()` only when Enter is pressed (and translates control keys — Ctrl-C→SIGINT, Ctrl-D→EOF, Ctrl-Z→suspend). (Any three.)

3. Clear **`ICANON`** (line/canonical mode) and **`ECHO`** (visible typing) in `c_lflag` for per-keystroke, non-echoed input. Clearing **`ECHO` alone** (leaving `ICANON` on) gives a hidden password prompt: the user types a full line with Enter as usual, but nothing is displayed.

4. Because when stdout is redirected to a file it becomes **fully buffered** (not line-buffered as it is for a terminal), so output without newlines sits in the buffer until it fills or the program exits/flushes. Fixes: call **`fflush(stdout)`** after each progress message, set the stream unbuffered/line-buffered with **`setvbuf(stdout, NULL, _IOLBF/_IONBF, ...)`**, or write progress to **stderr** (always unbuffered).

5. `command 2>&1 > file` points **stderr at wherever stdout currently points** (the terminal/screen) *first*, and *then* moves stdout to the file — so **stderr still goes to the screen**, stdout goes to the file. `command > file 2>&1` moves stdout to the file first, *then* points stderr at stdout's new target — so **both go to the file**. The difference is because `dup2` copies the destination fd's *current* target, and redirections apply left to right.

</details>

---

*Next up: **Module 5 — Processes.** `fork` (one call, two returns) and copy-on-write, the `exec` family, `wait`/`waitpid`, zombies and orphans, signals with `sigaction`, and the capstone: a working mini-shell. This is a big, pivotal module. Continuing straight on.*
