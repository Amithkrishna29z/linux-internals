# Module 11 — ioctl and the Many Kinds of I/O

> **Estimated time:** ~2 hours · **Core path:** Concepts 1–3 (why `read`/`write` isn't enough, `ioctl` mechanics, terminal ioctls) and the `winsize` program are core. The device/network ioctls and the "why ioctl is ugly / its successors" critique (Concepts 4–5) are enrichment — valuable context, lighter on hands-on.
>
> **Prerequisites:** Modules 0–10. This builds directly on the terminal (Module 4 — `termios` is really `ioctl` underneath) and sockets (Module 9 — network-interface control is `ioctl` too). The "everything is a file descriptor" spine now bends from *moving data* to *controlling the thing behind the fd*.

---

## The Big Picture

For eleven modules the file descriptor has carried you through an astonishing range of things — files, pipes, terminals, sockets, event queues — all with the same `read`/`write`/`close`. But `read` and `write` only move a *stream of bytes*, and plenty of what you need to do to a device isn't "move bytes" at all. What's the terminal's width in columns? Set a serial port to 9600 baud. Get a network interface's IP address. Eject the CD-ROM. Put the sound card in a particular mode. None of these are "read N bytes" or "write N bytes" — they're **control operations**, out-of-band commands to the driver behind the fd. The Unix answer to "everything that doesn't fit `read`/`write`" is one gloriously general, famously ugly syscall: **`ioctl`** (I/O control).

`ioctl(fd, request, argp)` is the catch-all. The `fd` names the device (or terminal, or socket); the `request` is a magic number identifying *which* control operation you want; and `argp` is a pointer to a struct that carries data in, out, or both — its meaning entirely determined by the request. It's the escape hatch, the "and anything else" clause of the I/O interface. When the elegant `read`/`write` abstraction doesn't fit — and for device control it often doesn't — `ioctl` is where the messy, device-specific reality lives. You've already used it without knowing: every `termios` call from Module 4 (`tcgetattr`, `tcsetattr`) is a thin wrapper over `ioctl(fd, TCGETS/TCSETS, ...)`. When your program notices the terminal was resized and re-lays-out its UI, that's `ioctl(TIOCGWINSZ)` fetching the new dimensions.

The trouble is that `ioctl`'s generality is also its curse. Because the `request` number and the `argp` struct are unique to each driver, `ioctl` is essentially *thousands of tiny, undocumented, type-unsafe mini-syscalls wearing one name*. The compiler can't check that your struct matches the request; the man pages are scattered; the request-number encoding (which packs in the direction and size) is arcane. It's powerful and indispensable and nobody's favorite. So Linux has spent two decades growing *better-structured* alternatives for whole categories that used to be `ioctl`: **`sysfs`** (`/sys`) exposes device attributes as readable/writable files (Module 12), and **`netlink`** sockets provide a structured, extensible channel for network configuration (which is why modern tools use `ip` instead of the old `ioctl`-based `ifconfig`). This module teaches you `ioctl` — because it's everywhere and you must be able to read and use it — while being honest that it's a design of last resort, and showing you what replaced it.

This is a shorter module: one important syscall, a couple of concrete uses (terminal size, network interfaces), and a clear-eyed view of why it's both essential and the part of the Unix I/O model everyone wishes were cleaner.

---

## Concepts

### 1. Why `read`/`write` isn't enough — the need for control operations

**What it is:** `read` and `write` transfer a *byte stream* to or from whatever's behind an fd. But many operations on a device are not byte transfers — they're **commands** or **queries** about the device's *state or configuration*: get the terminal size, set baud rate, query link speed, flush buffers, set non-standard modes. These are **out-of-band control operations**, distinct from the in-band data flow.

```
   read(fd, buf, n) / write(fd, buf, n)   →  moves DATA (a byte stream)

   but what about:
     "how many columns is this terminal?"      ← a QUERY, not bytes
     "set this serial port to 9600 baud"        ← a COMMAND, not bytes
     "what's eth0's IP address?"                ← a QUERY, not bytes
     "eject the disc"                            ← a COMMAND, no data at all
   └──────────── none of these fit read/write; they need ioctl ───────────┘
```

**Why it exists:** The file abstraction is deliberately narrow — a uniform "stream of bytes" — which is what makes it so composable. But real devices have rich, idiosyncratic control surfaces that don't reduce to a byte stream. Rather than invent a new syscall for every one ("`getterminalsize`", "`setbaudrate`", ...), Unix provides *one* generic control syscall, `ioctl`, whose meaning is parameterized by a request code. It's the pressure-relief valve that keeps `read`/`write` clean by absorbing everything that doesn't fit into a single, admittedly-messy, catch-all.

**Java analogy:** Java hides almost all of this behind typed methods: a terminal size isn't standard Java at all (you shell out or use JNI/JLine); socket options are `socket.setSoTimeout(...)`, `setTcpNoDelay(...)` — typed wrappers that call `setsockopt`/`ioctl` underneath. The pattern "the main API moves data; a side-channel of named control operations configures the thing" is everywhere though — e.g. JDBC's `Statement.execute` (data) vs `Connection.setAutoCommit` (control). `ioctl` is that control side-channel, unified into one raw call.

### 2. `ioctl` mechanics: the request code and the third argument

**What it is:** The syscall is `int ioctl(int fd, unsigned long request, ...)` — variadic, but in practice always called with a third argument that's either an integer or (usually) a *pointer to a struct*:

```c
   struct winsize ws;
   ioctl(fd, TIOCGWINSZ, &ws);     // fd=terminal, request="get window size",
                                    // argp=&ws (kernel fills it in)

   //  request encodes 4 things, packed into the number by _IOR/_IOW/_IOWR macros:
   //    - a "magic" type (which driver/subsystem)
   //    - a command number (which operation)
   //    - the DIRECTION: _IOR (read: kernel->user), _IOW (write), _IOWR (both)
   //    - the SIZE of the argp struct
```

The `request` is not an arbitrary integer — it's a bit-packed code built by macros (`_IOR`, `_IOW`, `_IOWR`) that embed the *direction* of data flow and the *size* of the argument struct, so the kernel can sanity-check the call. `argp` points to memory the kernel reads from (`_IOW`), writes to (`_IOR`), or both (`_IOWR`), depending on the request.

**Why it exists:** One syscall must serve thousands of unrelated operations, so the *operation selector* (`request`) has to be self-describing: encoding the subsystem, command, direction, and size into the number lets the kernel route the call to the right driver and validate the buffer without a separate registration for each. It's a clever scheme to make a single generic entry point safely dispatch to arbitrarily many device-specific handlers — the mechanism that makes "one syscall for everything" workable.

**Java analogy:** There's no real Java equivalent to a variadic, integer-selected, type-erased syscall — and that *absence* is telling. Java's design philosophy (typed methods, no raw pointers) is the opposite of `ioctl`'s. The closest conceptual cousin is a command dispatcher keyed by an opcode with a loosely-typed payload — e.g. a reflection call, or sending an `int` command plus a `byte[]` over a channel and trusting both sides to agree on the format. The type-unsafety you'd feel doing that is exactly `ioctl`'s ergonomics.

### 3. Terminal ioctls: window size and the `termios` you already used

**What it is:** The terminal (Module 4) is controlled almost entirely through `ioctl`. Two families:
- **`TIOCGWINSZ`** — "get window size": fills a `struct winsize { ws_row, ws_col, ... }` with the terminal's current dimensions. Paired with the **`SIGWINCH`** signal, which the kernel sends your process whenever the terminal is resized — so a full-screen program re-queries the size and redraws.
- **`TCGETS`/`TCSETS`** — get/set the `termios` structure (canonical vs raw mode, echo, etc.). You called these in Module 4 as `tcgetattr`/`tcsetattr`, which are *library wrappers* over exactly these ioctls.

```
   struct winsize ws;
   ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws);
   printf("%d rows x %d cols\n", ws.ws_row, ws.ws_col);

   // terminal gets resized  →  kernel sends SIGWINCH  →  handler re-queries TIOCGWINSZ
```

**Why it exists:** A terminal's configuration — its size, its line discipline, its modes — is pure device state, not a byte stream, so it's the archetypal `ioctl` domain. `TIOCGWINSZ` exists because programs like `vim`, `top`, `less`, and your shell must know the screen geometry to lay out output, and that geometry changes at runtime (you drag the window), signalled by `SIGWINCH`. It's the cleanest, most everyday example of "control operation behind an fd."

**Java analogy:** Standard Java has *no* portable way to get the terminal size — a real gap that libraries like **JLine** fill by calling this very `ioctl` through JNI (or shelling out to `stty size`). When you use a Java TUI library and it reflows on resize, JLine is doing `TIOCGWINSZ` + `SIGWINCH` under the hood. This is a place where "Java has no equivalent" is the whole lesson: terminal control lives below the JVM's abstractions, in `ioctl`.

### 4. Device and network ioctls (and the move to netlink)

**What it is:** Beyond terminals, `ioctl` controls a vast zoo of devices and, historically, network configuration:
- **Network interfaces:** `SIOCGIFCONF` (list interfaces), `SIOCGIFADDR` (get an interface's IP), `SIOCGIFFLAGS` (up/down, etc.), issued on a socket fd. This is how the classic `ifconfig` tool worked.
- **Block/char devices:** `BLKGETSIZE64` (disk size), `CDROMEJECT` (eject), sound/framebuffer/GPU modes — thousands, each driver defining its own.

Modern Linux increasingly *replaces* network ioctls with **netlink** sockets — a structured, extensible, socket-based protocol for kernel↔userspace configuration — which is why the modern `ip` command (iproute2) uses netlink while the deprecated `ifconfig` uses `ioctl`.

```
   int s = socket(AF_INET, SOCK_DGRAM, 0);      // any socket works as the "handle"
   struct ifreq ifr;
   strcpy(ifr.ifr_name, "eth0");
   ioctl(s, SIOCGIFADDR, &ifr);                  // ifr now holds eth0's IP
```

**Why it exists / why it's moving:** `ioctl` was the original catch-all for device *and* network control, and for one-off device operations it's still fine. But for network configuration — which is complex, evolving, and benefits from atomic multi-attribute updates, notifications, and dump operations — `ioctl`'s fixed structs and lack of extensibility became painful, so **netlink** was designed to do it properly. The trend is: keep `ioctl` for simple, stable, device-specific commands; use netlink (and `sysfs`) for rich, evolving configuration. Knowing both, and *why* the split happened, is the mark of understanding modern Linux I/O.

**Java analogy:** Java's `NetworkInterface.getNetworkInterfaces()` / `getInetAddresses()` gives you interface addresses portably — and on Linux the JVM implements it via these very `ioctl`/netlink calls. So the clean Java API you've used to enumerate IPs is `SIOCGIFCONF`/`SIOCGIFADDR` (or netlink) in disguise. Once again the typed method sits atop the raw control call.

### 5. Why `ioctl` is ugly — and what that teaches about interface design

**What it is:** A candid appraisal. `ioctl`'s weaknesses:
- **Type-unsafe:** the third argument is `void *`-ish; the compiler can't verify your struct matches the request. Mismatch → silent corruption or `EINVAL`.
- **Undiscoverable:** requests are `#define`d numbers scattered across driver headers; there's no uniform catalog.
- **Non-uniform:** every driver invents its own requests and structs; no consistency.
- **Portability landmines:** request numbers and struct layouts vary across architectures and kernel versions.

The successors fix specific categories: **`sysfs`/`procfs`** (Module 12) turn device attributes into plain files you `cat`/`echo` (discoverable, scriptable, typed-as-text); **netlink** gives network config a structured, extensible, event-capable protocol.

**Why it exists (and the lesson):** `ioctl` is what you get when you need *unbounded extensibility* through a *single fixed entry point* and you prioritize "make anything possible" over "make it clean." It exists because the alternative — a bespoke syscall per operation — doesn't scale, and because much device control genuinely is irreducibly device-specific. The design lesson is real: a maximally-general escape hatch buys flexibility at the cost of safety, discoverability, and consistency, and mature systems tend to grow *structured* replacements for the categories that matter most, leaving the escape hatch for the long tail. `ioctl` is simultaneously a triumph of pragmatism and a cautionary tale.

**Java analogy:** The tension shows up in Java API design too: typed methods (`socket.setTcpNoDelay`) vs generic escape hatches (`socketChannel.setOption(SocketOption, T)`, or reflection, or `Unsafe`). Java overwhelmingly chooses the typed, discoverable path — the anti-`ioctl` philosophy — which is why Java feels safe and why touching the raw layer (JNI to call `ioctl`) feels like leaving the guardrails. Understanding `ioctl` is understanding what those guardrails are protecting you from.

---

## Code

### Program 1 — `winsize.c`: get the terminal size and react to resizes

```c
/* winsize.c
 *
 * Uses ioctl(TIOCGWINSZ) to read the terminal's size, and catches SIGWINCH to
 * re-read it whenever you resize the window. This is what vim/top/less do to
 * lay out the screen. Ctrl-C to quit; resize the terminal to see it update.
 *
 * Compile:  gcc -Wall -Wextra -o winsize winsize.c
 * Run:      ./winsize   (then drag/resize your terminal window)
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/ioctl.h>      /* ioctl, TIOCGWINSZ, struct winsize */
#include <string.h>

static volatile sig_atomic_t g_resized = 1;   /* start true so we print once */

static void on_winch(int signo)
{
    (void)signo;
    g_resized = 1;          /* just set a flag (async-signal-safe, Module 5) */
}

static void print_size(void)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) < 0) {
        perror("ioctl(TIOCGWINSZ)");   /* fails if stdout isn't a terminal */
        return;
    }
    printf("terminal is %d rows x %d cols\n", ws.ws_row, ws.ws_col);
    fflush(stdout);
}

int main(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_winch;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGWINCH, &sa, NULL);    /* kernel sends SIGWINCH on resize */

    printf("Resize the terminal window (Ctrl-C to quit).\n");
    for (;;) {
        if (g_resized) {
            g_resized = 0;
            print_size();              /* re-query size on each resize */
        }
        pause();                       /* sleep until a signal arrives */
    }
    return 0;
}
```

**Expected output:**
```
$ ./winsize
Resize the terminal window (Ctrl-C to quit).
terminal is 24 rows x 80 cols
terminal is 30 rows x 100 cols     ← after you drag the window bigger
terminal is 18 rows x 62 cols      ← after you shrink it
^C
```

**Walkthrough of the non-obvious parts:**
- **`ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws)`** is the whole trick: the fd is the terminal, the request is "get window size," and `&ws` is where the kernel writes the dimensions. `TIOCGWINSZ` is a `_IOR`-type request (data flows kernel→user), which is why we pass an *empty* struct for it to fill.
- **`SIGWINCH`** is the kernel's "the terminal changed size" notification. Without it you'd have to poll the size continuously; instead you sleep and get woken exactly when it changes. The handler is minimal — just set a flag (the async-signal-safety discipline from Module 5) — and the real work (`print_size`, which calls `printf`) happens back in `main`.
- **`pause()`** sleeps until *any* signal arrives, then returns — so the loop wakes on each `SIGWINCH`, checks the flag, re-queries, and sleeps again. Zero CPU while idle, instant reaction to resize.
- If you run it with output redirected (`./winsize > file`), the `ioctl` fails with `ENOTTY` ("not a terminal") — because a regular file has no window size. This is the honest failure of asking a device-specific question of the wrong kind of fd.

### Project — `ifaddr.c`: list network interfaces and their IPs via `ioctl`

```c
/* ifaddr.c
 *
 * Lists the machine's network interfaces and their IPv4 addresses using the
 * classic ioctl interface (SIOCGIFCONF + SIOCGIFADDR) -- the same calls the
 * old `ifconfig` used. Issued on an ordinary socket fd, which acts as the
 * handle to the kernel's networking. (Modern `ip` uses netlink instead.)
 *
 * Compile:  gcc -Wall -Wextra -o ifaddr ifaddr.c
 * Run:      ./ifaddr
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <net/if.h>         /* struct ifconf, struct ifreq */
#include <arpa/inet.h>

int main(void)
{
    /* Any socket works as the handle for network ioctls. */
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) { perror("socket"); return 1; }

    /* SIOCGIFCONF fills a buffer with an array of struct ifreq (one per iface). */
    struct ifreq ifrs[32];
    struct ifconf ifc;
    ifc.ifc_len = sizeof ifrs;
    ifc.ifc_req = ifrs;
    if (ioctl(s, SIOCGIFCONF, &ifc) < 0) { perror("ioctl(SIOCGIFCONF)"); return 1; }

    int count = ifc.ifc_len / sizeof(struct ifreq);
    printf("found %d interface(s):\n", count);

    for (int i = 0; i < count; i++) {
        struct ifreq ifr = ifrs[i];       /* copy: the next ioctl overwrites fields */

        /* SIOCGIFADDR: get THIS interface's IPv4 address into ifr.ifr_addr. */
        if (ioctl(s, SIOCGIFADDR, &ifr) < 0) {
            printf("  %-10s  (no IPv4 address)\n", ifrs[i].ifr_name);
            continue;
        }
        struct sockaddr_in *sin = (struct sockaddr_in *)&ifr.ifr_addr;
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof ip);

        /* SIOCGIFFLAGS: is it up? */
        char *state = "";
        if (ioctl(s, SIOCGIFFLAGS, &ifr) == 0)
            state = (ifr.ifr_flags & IFF_UP) ? "UP" : "DOWN";

        printf("  %-10s  %-15s  %s\n", ifrs[i].ifr_name, ip, state);
    }

    close(s);
    return 0;
}
```

**Expected output:**
```
$ ./ifaddr
found 2 interface(s):
  lo          127.0.0.1        UP
  eth0        192.168.1.42     UP
```

**Walkthrough of the non-obvious parts:**
- **A socket is the "handle" for network ioctls.** We don't connect it anywhere — `socket(AF_INET, SOCK_DGRAM, 0)` just gives us an fd into the kernel's IPv4 stack, and the `ioctl`s query the stack, not any particular connection. This is idiomatic: network-configuration ioctls are issued on *any* socket of the right family.
- **`SIOCGIFCONF` is a two-level call**: it fills `ifc.ifc_req` (our `ifrs[]` array) with one `struct ifreq` per interface and sets `ifc.ifc_len` to the bytes used, so `ifc_len / sizeof(struct ifreq)` is the interface count. This "give me a buffer, I'll tell you how much I filled" pattern is common in `ioctl`.
- **We copy each `ifreq` before the per-interface ioctls** (`struct ifreq ifr = ifrs[i];`) because `SIOCGIFADDR`/`SIOCGIFFLAGS` *overwrite* fields of the struct (the address, the flags) — reusing the original array entry would clobber the interface name we still need. A subtle aliasing trap `ioctl`'s in/out structs invite.
- **The same `ifr` serves three different requests** (`SIOCGIFADDR`, `SIOCGIFFLAGS`), each interpreting/filling different fields of the shared `struct ifreq` — a vivid illustration of `ioctl`'s "one struct, meaning depends on the request" design (and its type-unsafety: nothing stops you pairing the wrong request with the wrong field).
- The modern equivalent (`ip addr`) gets this via **netlink**, which returns structured, extensible messages and can notify you of changes — the successor discussed in Concept 4. This program is deliberately the *old* way, so you can read legacy code and appreciate why netlink exists.

---

## Under the Hood

Run `strace ./winsize` (resize once) and `strace ./ifaddr` and you see `ioctl` for what it is — one syscall, many meanings:

```
# winsize.c
ioctl(1, TIOCGWINSZ, {ws_row=24, ws_col=80, ws_xpixel=0, ws_ypixel=0}) = 0   ← [1]
--- SIGWINCH {si_signo=SIGWINCH} ---                                          ← [2] resize!
ioctl(1, TIOCGWINSZ, {ws_row=30, ws_col=100, ...}) = 0                        ← [3] re-query

# ifaddr.c
socket(AF_INET, SOCK_DGRAM, IPPROTO_IP)  = 3
ioctl(3, SIOCGIFCONF, {ifc_len=64, ifc_req=[{"lo"}, {"eth0"}]}) = 0           ← [4]
ioctl(3, SIOCGIFADDR, {ifr_name="lo",   ifr_addr={AF_INET, 127.0.0.1}}) = 0   ← [5]
ioctl(3, SIOCGIFADDR, {ifr_name="eth0", ifr_addr={AF_INET, 192.168.1.42}}) = 0
```

Annotated:
1. **`ioctl(1, TIOCGWINSZ, {...}) = 0`** — strace helpfully *decodes* the request name (`TIOCGWINSZ`) and the struct it filled (`ws_row=24, ws_col=80`). fd 1 is stdout (the terminal); the kernel wrote the dimensions into our `struct winsize`. Note strace knows this struct's layout *because* it recognizes the request — the same coupling the kernel uses.
2–3. **`SIGWINCH` then a second `ioctl(TIOCGWINSZ)`** — the resize delivered the signal, our handler set the flag, and `main` re-issued the *same* `ioctl` to get the *new* size (`ws_row=30, ws_col=100`). The whole "size changed → re-query" cycle, visible in three lines.
4. **`ioctl(3, SIOCGIFCONF, ...)`** — a *different* request on a *socket* fd, returning an array of interfaces. Same syscall (`ioctl`), completely different subsystem and struct — the generality laid bare.
5. **`ioctl(3, SIOCGIFADDR, {ifr_name="lo", ifr_addr={... 127.0.0.1}})`** — one `ioctl` per interface, each filling the address. strace shows the request-specific struct interpretation again.

The headline: **`ioctl` is one syscall whose behavior is entirely defined by the `request` code — the same `ioctl(fd, req, argp)` fetches a terminal's size, lists network interfaces, or ejects a disc, with the struct at `argp` meaning something different each time.** That is its power (infinite extensibility) and its curse (no uniformity, no type safety) in a single strace. Notice how strace must *special-case* each request to decode it — mirroring exactly why `ioctl` is hard to work with and why structured successors (sysfs, netlink) exist.

---

## Try This

Ordered easy → hard.

1. **(Easy) Watch `TIOCGWINSZ` track your window.** Run `./winsize` and drag your terminal to several sizes; confirm the rows×cols update each time. Then run `stty size` — it prints the same numbers via the same `ioctl`. Then `./winsize > out.txt` and see it fail with "not a terminal." *Hint: a file has no window size — `ENOTTY` is the honest answer to a device-specific question aimed at the wrong device.*

2. **(Easy) Compare `ifaddr` to the real tools.** Run `./ifaddr`, then `ifconfig` (if installed) and `ip addr`. Confirm the addresses match. Note `ifconfig` uses the same `ioctl`s you just wrote; `ip` uses netlink. *Hint: `strace -e trace=ioctl ifconfig` vs `strace -e trace=sendmsg,recvmsg ip addr` shows the two mechanisms.*

3. **(Medium) Find the request encoding.** Look up `TIOCGWINSZ` and `SIOCGIFADDR` in the headers (`grep -r TIOCGWINSZ /usr/include`) and decode the direction/size bits (they're built with `_IOR`/`_IOW`). Explain what the kernel learns from the number alone. *Hint: the request encodes read-vs-write and the struct size so the kernel can validate `argp` before trusting it.*

4. **(Medium) Add broadcast and netmask to `ifaddr`.** Extend it with `SIOCGIFNETMASK` and `SIOCGIFBRDADDR` to print each interface's netmask and broadcast address. Note you reuse the *same* `struct ifreq`, just with different requests. *Hint: this is exactly how `ifconfig` builds its full per-interface line — one struct, many ioctls.*

5. **(Hard) Read a block device's size with `ioctl`.** Write a program that opens a block device (e.g. `/dev/sda`, needs root) and issues `BLKGETSIZE64` to print its size in bytes. Compare to `lsblk`. Explain why this is an `ioctl` and not a `read` (reading the device gives you its *contents*, not its size). *Hint: `#include <linux/fs.h>`; the size is device metadata, orthogonal to the byte stream you'd get from reading it.*

---

## Gotchas

- **Mismatched request and struct = silent corruption.** `ioctl`'s third argument is type-erased; if you pass a struct that doesn't match what the request expects, the kernel reads/writes the wrong number of bytes — memory corruption or `EINVAL`, with no compiler warning. Always pair each request with *exactly* its documented struct. This type-unsafety is `ioctl`'s original sin.

- **`ENOTTY` doesn't (usually) mean "not a typewriter."** A terminal-family `ioctl` on a non-terminal fd (a file, a pipe) fails with `ENOTTY` — historically "not a typewriter," practically "this fd doesn't support this ioctl." Don't assume an fd supports an `ioctl`; check the return and handle the "wrong kind of device" case.

- **In/out struct fields get overwritten.** Many `ioctl`s use one struct for both input (which device/field) and output (the value), so a sequence of ioctls on the same struct clobbers earlier fields (the `ifaddr` name-vs-address trap). Copy the struct or re-set the input fields before each call.

- **Request numbers and struct layouts aren't portable.** `ioctl` request values and their structs vary across architectures, kernel versions, and OSes. Code using them is inherently non-portable; guard with `#ifdef __linux__` and don't assume a Linux `ioctl` exists or has the same number elsewhere. (This is a big reason netlink/sysfs are preferred — text and structured protocols travel better.)

- **Prefer the typed wrapper when one exists.** For terminals, use `tcgetattr`/`tcsetattr` (Module 4) rather than raw `TCGETS`/`TCSETS` `ioctl`s — the wrappers are portable and documented. Reach for raw `ioctl` only when there's no wrapper. Raw `ioctl` in code that had a wrapper available is a smell.

- **Network config: reach for netlink/`ip`, not `ioctl`/`ifconfig`, in new code.** The `SIOCGIF*` ioctls are legacy and limited (e.g. they don't handle multiple IPv6 addresses per interface well). Use them to read *old* code; write *new* network-configuration code against netlink (or shell out to `ip`). Knowing the ioctl way is for literacy, not greenfield use.

---

## Checkpoint

1. Why do `read` and `write` not suffice for interacting with devices? Give three concrete operations that need `ioctl` instead, and say what they have in common.
2. What are the three arguments to `ioctl`, and what does the `request` code encode beyond "which operation"? Why does the kernel benefit from that encoding?
3. How does a program get the terminal's size, and how does it know when the size changed? Which `ioctl` and which signal are involved, and what does each do?
4. `ioctl` is powerful but widely considered ugly. Give three specific weaknesses, and name the two modern mechanisms Linux uses to replace whole categories of it (and which category each handles).
5. In `ifaddr.c`, why is a plain unconnected socket used as the fd, and why must you copy each `struct ifreq` before issuing the per-interface `ioctl`s?

---

<details>
<summary><b>Checkpoint Answers</b></summary>

1. `read`/`write` only transfer a **byte stream** to/from an fd, but many device interactions are **control operations** — commands or queries about the device's state/configuration, not data transfer. Examples: getting a terminal's window size (`TIOCGWINSZ`), setting a serial port's baud rate (`termios`/`TCSETS`), getting a network interface's IP (`SIOCGIFADDR`), ejecting a CD (`CDROMEJECT`). What they share: none is "move N bytes" — they're out-of-band device control, which is exactly the niche `ioctl` fills as the catch-all control syscall.

2. `ioctl(int fd, unsigned long request, ...)`: **`fd`** names the device/terminal/socket; **`request`** selects the operation; the third argument (**`argp`**) is usually a pointer to a struct carrying data in, out, or both. Beyond "which operation," the `request` code is bit-packed (via `_IOR`/`_IOW`/`_IOWR`) to encode the **subsystem/magic type**, the **direction** of data flow (kernel→user, user→kernel, or both), and the **size** of the `argp` struct. The kernel benefits because it can route the call to the right driver and **validate `argp`** (direction and size) from the number alone, making one generic entry point safely dispatch to thousands of device-specific handlers.

3. A program calls `ioctl(fd, TIOCGWINSZ, &ws)` on a terminal fd, and the kernel fills a `struct winsize` with `ws_row`/`ws_col`. It learns of *changes* via the **`SIGWINCH`** signal, which the kernel sends the process whenever the terminal is resized; the handler notes the change (sets a flag), and the program re-issues `ioctl(TIOCGWINSZ)` to fetch the new dimensions and redraw. So `TIOCGWINSZ` *reads* the size and `SIGWINCH` *notifies* of size changes — together they let full-screen programs (vim/top/less) stay correctly laid out as you resize.

4. Weaknesses: **type-unsafe** (the `argp` struct isn't checked against the request by the compiler — mismatches silently corrupt); **undiscoverable** (requests are scattered `#define`d numbers with no uniform catalog); **non-uniform** (every driver invents its own requests/structs); **non-portable** (request numbers and struct layouts vary by arch/kernel/OS). Modern replacements: **`sysfs`/`procfs`** (`/sys`, `/proc`) expose device attributes as plain text files you read/write — handling device *attributes* discoverably and scriptably; and **netlink** sockets provide a structured, extensible, event-capable protocol — handling *network configuration* (which is why `ip` replaced `ifconfig`).

5. A plain unconnected `socket(AF_INET, SOCK_DGRAM, 0)` is used because network-configuration ioctls query the kernel's *networking stack*, not any particular connection — any socket of the right family serves as a handle into that subsystem, so no `connect`/`bind` is needed. You must **copy each `struct ifreq`** before the per-interface calls because `SIOCGIFADDR`/`SIOCGIFFLAGS` **overwrite fields** of the struct (writing the address/flags into the same memory that holds the interface name); operating on the original array entry would clobber the `ifr_name` you still need for subsequent lookups — an aliasing hazard of `ioctl`'s shared in/out structs.

</details>

---

*Next up: **Module 12 — The /proc and /sys Filesystems.** The structured successors to `ioctl` that we just previewed get their own module: how Linux exposes kernel and process state as a *filesystem* you can `cat`, `grep`, and `echo` into — `/proc/[pid]/` (maps, fd, status, cmdline), `/proc/meminfo` and friends, and `/sys` device attributes. You'll read a process's memory map (the Module 7 segments, live) and tune the kernel through files. Everything really is a file. Continuing straight on.*
