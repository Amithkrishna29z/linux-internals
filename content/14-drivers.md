# Module 14 — Device Drivers, Controllers, and Hardware I/O

> **Estimated time:** 4–5 hours · **Core path:** Concepts 1–3 (what a driver is, `/dev` nodes and major/minor, the `file_operations` dispatch table) and the `hellochar` kernel module + `usehello` program are core. Hardware mechanics — MMIO/port I/O, interrupts, DMA (Concept 4) — and the driver-model/safety discussion (Concept 5) are core-but-deep; building/loading the module needs a Linux box with kernel headers.
>
> **Prerequisites:** Modules 0–13. This is the payoff of the whole "everything is a file descriptor" spine: you finally write the code *behind* the fd. You need `read`/`write`/`open`/`ioctl` (Modules 3, 4, 11), the user/kernel boundary and `copy_from/to_user` (Module 13 — used directly here), and interrupts/signals intuition (Module 5). **This module runs kernel code** — a bug can hang the machine, so use a VM.

---

## The Big Picture

Since Module 3 you've called `read(fd, ...)` thousands of times and something on the other side answered. When `fd` was a file, the filesystem answered; when it was a socket, the network stack; when it was `/dev/something`, a **device driver** answered — kernel code written specifically to make that hardware behave like a file. This module crosses all the way over and writes that code. A **device driver** is the kernel component that translates the generic file operations (`open`, `read`, `write`, `ioctl`, `close`) into the specific pokes and reads that make a particular piece of hardware do its job. Your keyboard, disk, GPU, network card, sound chip, and the `/dev/null` you redirect into — each has a driver that presents it through the uniform file interface, so that `cat`, your shell, and your own `open`/`read` code work on a mouse the same way they work on a text file. The driver is where the beautiful uniform abstraction meets the messy specific hardware, and it's the reason the abstraction holds.

The connection point is the **device node** in `/dev` — a special file (`/dev/sda`, `/dev/null`, `/dev/tty`) that isn't data but a *pointer to a driver*. Each node carries a **major number** (which driver) and a **minor number** (which specific device that driver manages). When you `open("/dev/sda")`, the kernel sees the major number, looks up the registered driver, and from then on every `read`/`write`/`ioctl` on that fd is **dispatched** to functions the driver registered — through a table called **`file_operations`**, a struct of function pointers (`.read = my_read, .write = my_write, ...`). This is the exact same idea as the syscall table from Module 13, one level down: the kernel indexes into a table of function pointers to route a generic operation to specific code. When you finally see `struct file_operations` with your own `hello_read` filled in, the entire course clicks — *that's* what was answering your `read` all along. We'll build a real, loadable **character device driver** — a kernel module that creates `/dev/hellochar`, stores what you write to it, and returns it when you read — using `copy_from_user`/`copy_to_user` (Module 13) to move data across the boundary safely.

Drivers come in two great families. **Character devices** (`/dev/tty`, `/dev/null`, serial ports, our example) present an *unstructured byte stream* — you read/write bytes in sequence, like a pipe; that's most of what we'll build. **Block devices** (`/dev/sda`, disks, SSDs) present *fixed-size, randomly-addressable blocks* and sit under the filesystem and page cache, optimized for the very different access pattern of storage. Knowing which model a device fits — stream vs addressable blocks — tells you how its driver is shaped and why (a keyboard is a character device; a hard drive is a block device).

Below the driver's file-operations facade is the genuinely physical part: **how software touches hardware at all.** A driver controls a device by reading and writing the device's **registers**, which the hardware exposes either as special memory addresses (**memory-mapped I/O** — the device's registers appear in the physical address space, so the driver reads/writes them like memory) or through separate I/O ports (**port I/O**, x86's `in`/`out` instructions). But polling hardware wastes the CPU, so devices signal the CPU asynchronously with **interrupts** — a hardware line that yanks the CPU away to run the driver's **interrupt handler** the instant the device needs attention (a key was pressed, a disk read finished, a packet arrived). This is the hardware original of the signals you met in Module 5. And moving bulk data through the CPU register-by-register is far too slow, so devices use **DMA** (direct memory access) to transfer data straight to/from RAM without the CPU, interrupting only when the whole transfer is done. Registers, interrupts, and DMA are the three mechanisms *all* hardware I/O reduces to — the physical bedrock under every `read` in this course. This module is where the file descriptor finally touches copper.

---

## Concepts

### 1. What a device driver is, and the `/dev` node that reaches it

**What it is:** A **device driver** is kernel code that implements the file operations for a specific device, making hardware accessible through the uniform `open`/`read`/`write`/`ioctl`/`close` interface. Userspace reaches a driver through a **device node**: a special file in `/dev` created with a **major number** (identifies the driver) and a **minor number** (identifies which device that driver handles).

```
   $ ls -l /dev/null /dev/sda
   crw-rw-rw- 1 root root 1, 3 /dev/null      ← 'c'=char device, major 1, minor 3
   brw-rw---- 1 root disk 8, 0 /dev/sda       ← 'b'=block device, major 8, minor 0
     │                        │  │
     type (c/b)               major minor
                              (which driver)(which device)

   open("/dev/sda") → kernel reads major 8 → routes to the sd (disk) driver
   every read/write on that fd → dispatched to that driver's functions
```

**Why it exists:** Hardware is wildly diverse — thousands of device types, each with its own control protocol — yet applications must use them without knowing those details. The driver is the adapter that hides the specifics behind the standard file interface, so the *same* `cat`/`read`/`write` works on every device. The `/dev` node with its major/minor numbers is the naming and routing scheme that connects a userspace `open` to the right kernel driver and the right physical device. It's the linchpin that makes "everything is a file" extend to hardware.

**Java analogy:** Java is *entirely* insulated from this — you never see a driver — but you use their output constantly: `System.in` is ultimately the tty driver, `new FileInputStream("/dev/urandom")` reaches the random driver, a serial-port library (jSerialComm) opens `/dev/ttyUSB0` and thus its driver. The design pattern — a uniform interface (`InputStream`) with pluggable implementations behind it — is one you know well; a device driver is that pattern implemented in the kernel, with the `/dev` node as the factory key selecting the implementation.

### 2. Character vs block devices

**What it is:** The two principal driver models:
- **Character devices** present an **unstructured byte stream** — data flows in sequence, read/written byte-by-byte (or in arbitrary-sized chunks), with no notion of addressable position for many of them. Examples: terminals (`/dev/tty`), serial ports, `/dev/null`, `/dev/random`, sound devices, and our `hellochar`.
- **Block devices** present **fixed-size blocks** (e.g. 512 B or 4 KB) that are **randomly addressable** — you can read block 5000 then block 12 — and they sit beneath the filesystem and page cache. Examples: disks (`/dev/sda`), SSDs, USB drives.

```
   CHARACTER device:  [ b y t e   s t r e a m ... ]   read/write sequentially
                      (keyboard, serial, /dev/null)     — like a pipe

   BLOCK device:      [blk0][blk1][blk2]...[blkN]      addressable, cached
                      (disk, SSD)                        — random access, under a FS
```

**Why it exists:** The two models match two fundamentally different access patterns. A keyboard or serial line *is* a stream — bytes arrive in order and you consume them once; a character interface fits perfectly. A disk is a randomly-addressable array of blocks that benefits enormously from caching and reordering (the page cache, I/O schedulers); a block interface, with its extra machinery, fits *that*. Forcing a disk through a byte-stream interface would lose random access and caching; forcing a keyboard through a block interface would be absurd. The split lets each device use the model that matches its nature.

**Java analogy:** The distinction echoes `InputStream`/`OutputStream` (sequential byte streams — the character-device model) vs `RandomAccessFile`/`FileChannel` with `position()` (seekable, block-like access — the block-device model). When you pick a `RandomAccessFile` to seek within a data file vs an `InputStream` to consume a socket, you're making the same stream-vs-addressable choice that separates character and block drivers.

### 3. The `file_operations` table: how `read`/`write` reach the driver

**What it is:** Every character driver registers a **`struct file_operations`** — a table of function pointers telling the kernel which of the driver's functions to call for each file operation on the device:

```c
   static struct file_operations hello_fops = {
       .owner   = THIS_MODULE,
       .open    = hello_open,     // called on open("/dev/hellochar")
       .read    = hello_read,     // called on read(fd, ...)
       .write   = hello_write,    // called on write(fd, ...)
       .release = hello_release,  // called on close(fd)
       // .unlocked_ioctl = hello_ioctl,  // for ioctl (Module 11)
   };

   // userspace read(fd, buf, n)  →  VFS  →  hello_fops.read(file, buf, n, &pos)
```

When userspace calls `read(fd, ...)` on the device, the kernel's **VFS** (virtual filesystem layer) follows the fd to this driver's `file_operations` and calls `.read` — i.e., *your* `hello_read`. The driver's `read`/`write` functions receive a *user-space* buffer pointer and must use `copy_to_user`/`copy_from_user` (Module 13) to move data safely.

**Why it exists:** This function-pointer table is the **dispatch mechanism** that makes the file interface pluggable: the generic VFS code for `read`/`write`/`open` doesn't know or care what's behind the fd — it just calls whatever functions the driver registered. It's polymorphism in C (a vtable), and it's *the* structural reason one `read` syscall works uniformly across files, pipes, sockets, and every device. It's the same "table of function pointers routes a generic op to specific code" idea as the syscall table (Module 13), applied to devices.

**Java analogy:** `file_operations` is a **vtable** / interface implementation. It's exactly `InputStream`'s abstract methods (`read()`, `close()`) that concrete subclasses override — the VFS holds a reference typed to the "interface" and calls the overridden method without knowing the concrete class. Filling in `.read = hello_read` is overriding `InputStream.read()`. Once you see `file_operations` as "the driver implements the file interface," your entire OO intuition transfers to kernel driver structure.

### 4. Touching hardware: registers (MMIO/port I/O), interrupts, and DMA

**What it is:** Beneath the file-operations facade, a driver actually controls hardware through three mechanisms:
- **Device registers via MMIO or port I/O:** hardware exposes control/status/data **registers**. In **memory-mapped I/O (MMIO)**, those registers appear at physical memory addresses, so the driver reads/writes them like memory (`ioread32`/`iowrite32` on a mapped region). In **port I/O** (x86 legacy), separate `in`/`out` instructions access an I/O port space. Writing a register commands the device; reading one gets its status/data.
- **Interrupts:** rather than the CPU polling "are you done yet?", the device asserts an **interrupt** — a hardware signal that preempts the CPU to run the driver's registered **interrupt handler (ISR)** immediately (key pressed, DMA finished, packet arrived). The ISR does the minimum and defers heavy work.
- **DMA (direct memory access):** for bulk transfers, the device copies data **directly to/from RAM** without the CPU moving each byte; the CPU sets up the transfer, the device does it, and raises one interrupt when the whole block is done.

```
   CPU ──MMIO/port write──▶ [DEVICE registers]   "start reading sector 5000 into RAM@X"
   DEVICE ──DMA──▶ [RAM @ X]                       (moves the data itself, CPU free)
   DEVICE ──interrupt──▶ CPU                        "done!" → driver's ISR runs
```

**Why it exists:** These are the physical primitives all hardware I/O reduces to. Registers are the command/query channel (how software *talks* to a device). Interrupts exist because polling wastes the CPU — a device that needs attention rarely and unpredictably (a keypress) should *signal* rather than be *asked*, freeing the CPU for real work meanwhile (the hardware root of Module 5's signals and Module 10's event-driven efficiency). DMA exists because routing megabytes through CPU registers one word at a time would saturate the CPU; letting the device write RAM directly, with one interrupt at completion, is orders of magnitude more efficient. Together they're why a disk read doesn't peg your CPU.

**Java analogy:** Utterly below the JVM — Java has no concept of a hardware register or interrupt — but the *architectural pattern* is deeply familiar: interrupts + DMA are the hardware version of **asynchronous, event-driven I/O with completion notification** (Module 10's `epoll`/`io_uring`, or `CompletableFuture`). "Set up the operation, let it happen in the background, get notified on completion, don't block the CPU spinning" is the interrupt/DMA model *and* the async-I/O model — the same idea at two layers. Your NIO event loop is, in spirit, the userspace echo of what the interrupt controller does in silicon.

### 5. Kernel modules, the driver model, and why kernel code is dangerous

**What it is:** Drivers are usually built as **loadable kernel modules** (`.ko` files) — kernel code you can insert (`insmod`) and remove (`rmmod`) at runtime without rebooting. A module defines an **init** function (`module_init`, runs on load — registers the driver) and an **exit** function (`module_exit`, runs on unload — unregisters), plus metadata (`MODULE_LICENSE`, etc.). It's compiled against the running kernel's headers with the **kbuild** system (a special `Makefile` using `make -C /lib/modules/$(uname -r)/build M=$PWD`). And it runs in **kernel mode** (Module 13) — with all the power and none of the guardrails.

```
   insmod hellochar.ko  → module_init → misc_register(&hello) → /dev/hellochar appears
   ... device usable via open/read/write ...
   rmmod hellochar      → module_exit → misc_deregister      → /dev/hellochar gone

   ⚠ kernel mode: no memory protection saving you. A null deref = KERNEL PANIC,
     not a segfault. A leaked resource persists until reboot. Use a VM.
```

**Why it exists / why it's dangerous:** Loadable modules exist so the kernel needn't statically include every possible driver (there are thousands) — you load only what your hardware needs, at runtime, and can update a driver without rebooting. But driver code runs with **full privilege and no safety net**: there's no per-process memory protection catching your mistakes (a bad pointer *panics the whole machine* instead of segfaulting one process, Module 7), no `malloc`-style forgiveness (a leaked kernel allocation is gone until reboot), and a bug can corrupt any memory or hang the system. This is why kernel programming demands extreme care (always `copy_*_user`, always free what you allocate, always handle every error path) and why you develop drivers in a **VM** you can freely crash. The power that makes drivers able to touch hardware is the same power that makes their bugs catastrophic.

**Java analogy:** The polar opposite of Java's philosophy — no GC, no exceptions-as-safety-net, no bounds checks, no "crash just this thread." If Java is maximally guarded, kernel-module programming is maximally exposed: the closest Java experience is JNI/`Unsafe` where a mistake segfaults the JVM, but even that only kills *your process*, whereas a kernel bug kills the *machine*. Understanding this contrast is understanding what every layer of managed-runtime safety was buying you — and what it costs to work where that safety doesn't exist.

---

## Code

> **Environment:** these must run on **Linux** with kernel headers installed (`sudo apt install build-essential linux-headers-$(uname -r)`) and **root** to load the module. Do it in a **VM** — a driver bug can hang the machine. The kernel module (`hellochar.c`) builds with its own **kbuild `Makefile`**, *not* the course's top-level `make`.

### Program 1 — `hellochar.c`: a real character-device kernel module

```c
/* hellochar.c  -- a minimal character-device driver (loadable kernel module).
 *
 * Creates /dev/hellochar. Reading it returns the stored message; writing it
 * replaces the message. Demonstrates the file_operations dispatch table and
 * copy_to_user/copy_from_user (Module 13) -- the code that ANSWERS your read().
 * Uses the misc-device framework, which auto-creates the /dev node.
 *
 * Build:   make            (uses the kbuild Makefile in this directory)
 * Load:    sudo insmod hellochar.ko
 * Use:     cat /dev/hellochar ; echo "new text" | sudo tee /dev/hellochar
 * Unload:  sudo rmmod hellochar
 * Log:     dmesg | tail       (see the printk messages)
 */

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>    /* copy_to_user, copy_from_user */
#include <linux/init.h>

#define BUFSZ 256
static char   message[BUFSZ] = "hello from kernel space\n";
static size_t message_len   = 24;   /* length of the initial message */

/* Called on read(fd,...). ubuf is a USER pointer -- must use copy_to_user. */
static ssize_t hello_read(struct file *f, char __user *ubuf,
                          size_t count, loff_t *ppos)
{
    /* simple_read_from_buffer handles the *ppos/EOF bookkeeping and the
     * copy_to_user for us -- returns 0 at EOF so `cat` stops. */
    return simple_read_from_buffer(ubuf, count, ppos, message, message_len);
}

/* Called on write(fd,...). ubuf is a USER pointer -- must use copy_from_user. */
static ssize_t hello_write(struct file *f, const char __user *ubuf,
                           size_t count, loff_t *ppos)
{
    size_t n = count < BUFSZ - 1 ? count : BUFSZ - 1;   /* never overflow buf */
    if (copy_from_user(message, ubuf, n))               /* SAFE cross-boundary copy */
        return -EFAULT;                                  /* bad user pointer */
    message[n] = '\0';
    message_len = n;
    pr_info("hellochar: stored %zu bytes\n", n);        /* -> dmesg */
    return count;                                        /* pretend we consumed it all */
}

static int hello_open(struct inode *ino, struct file *f)
{
    pr_info("hellochar: opened\n");
    return 0;
}

/* THE DISPATCH TABLE: maps file ops to our functions. This is what the VFS
 * calls when userspace does read/write/open on /dev/hellochar. */
static const struct file_operations hello_fops = {
    .owner = THIS_MODULE,
    .read  = hello_read,
    .write = hello_write,
    .open  = hello_open,
};

/* misc device: a simple char device that auto-gets a minor and /dev node. */
static struct miscdevice hello_dev = {
    .minor = MISC_DYNAMIC_MINOR,
    .name  = "hellochar",          /* -> /dev/hellochar */
    .fops  = &hello_fops,
    .mode  = 0666,                 /* world read/write, for the demo */
};

static int __init hello_init(void)     /* runs on insmod */
{
    int ret = misc_register(&hello_dev);
    if (ret) { pr_err("hellochar: register failed\n"); return ret; }
    pr_info("hellochar: loaded, /dev/hellochar ready\n");
    return 0;
}

static void __exit hello_exit(void)    /* runs on rmmod */
{
    misc_deregister(&hello_dev);
    pr_info("hellochar: unloaded\n");
}

module_init(hello_init);
module_exit(hello_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("linux-internals course");
MODULE_DESCRIPTION("Minimal character device: stores and returns a message");
```

The **kbuild `Makefile`** (in the same directory — required to build a module):

```makefile
# Kbuild Makefile for the hellochar kernel module. NOT the course's top-level
# make -- kernel modules must build against the running kernel's headers.
obj-m += hellochar.o

all:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
```

**Expected session:**
```
$ make                                   # builds hellochar.ko
$ sudo insmod hellochar.ko
$ dmesg | tail -1
[ 1234.5] hellochar: loaded, /dev/hellochar ready
$ cat /dev/hellochar
hello from kernel space
$ echo "written from userspace" | sudo tee /dev/hellochar
$ cat /dev/hellochar
written from userspace
$ dmesg | tail -2
[ 1240.1] hellochar: stored 23 bytes
[ 1240.1] hellochar: opened
$ sudo rmmod hellochar
```

**Walkthrough of the non-obvious parts:**
- **`hello_fops` is the whole point of the module** — `.read = hello_read, .write = hello_write` is the driver saying "when userspace reads my device, call *this* function." When you finally see your own function pointer sitting in `struct file_operations`, you understand what has been answering every `read` since Module 3: a `file_operations.read` somewhere, for whatever was behind the fd.
- **`char __user *ubuf`** — the `__user` annotation marks a pointer that lives in *user* space; the driver must **never** dereference it directly (Module 13's rule). `simple_read_from_buffer` and `copy_from_user` are the sanctioned safe crossings. Dereferencing `ubuf` directly might read a bad/hostile address and **panic the kernel** — the security discipline of Module 13 made mandatory.
- **`copy_from_user` returns the number of bytes it *couldn't* copy** (0 on success) — a bad user pointer yields non-zero, and we return `-EFAULT`. Kernel functions return negative errno values (which the syscall layer, Module 13, turns into userspace `errno`).
- **`misc_register` auto-creates `/dev/hellochar`** with a dynamic minor — sidestepping the more verbose classic path (`alloc_chrdev_region` + `cdev_add` + `class_create` + `device_create`) that manually manages major/minor numbers and the node. The misc framework is the shortest correct path to a working char device.
- **`__init`/`__exit` and `module_init`/`module_exit`** register the load/unload hooks. `pr_info`/`pr_err` write to the kernel log (`dmesg`) — there's no `printf` to a terminal in the kernel; logging *is* your primary debugging tool (there's no `gdb`-stepping a live kernel casually).
- **`MODULE_LICENSE("GPL")`** is not decoration — without a GPL-compatible license the module can't use many kernel symbols (the kernel "taints" and restricts non-GPL modules). It's a real load-time gate.

### Program 2 — `usehello.c`: drive the device from userspace

```c
/* usehello.c
 *
 * Userspace program that exercises /dev/hellochar with plain open/read/write --
 * the SAME calls as any file (Module 3). Each one dispatches into the kernel
 * module's file_operations. This is the userspace half of the boundary; the
 * driver is the kernel half. (This one DOES build with the course's top-level
 * make -- it's an ordinary userspace program.)
 *
 * Compile:  gcc -Wall -Wextra -o usehello usehello.c
 * Run:      sudo insmod hellochar.ko   (first, from the kbuild build)
 *           ./usehello
 */

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

#define DEV "/dev/hellochar"

int main(void)
{
    /* READ what's there now -- dispatches to the driver's hello_read. */
    int fd = open(DEV, O_RDWR);
    if (fd < 0) { perror("open " DEV " (is the module loaded?)"); return 1; }

    char buf[256];
    ssize_t n = read(fd, buf, sizeof buf - 1);
    if (n < 0) { perror("read"); return 1; }
    buf[n] = '\0';
    printf("initial read: %s", buf);

    /* WRITE a new message -- dispatches to the driver's hello_write. */
    const char *msg = "set by usehello.c\n";
    if (write(fd, msg, strlen(msg)) < 0) { perror("write"); return 1; }
    close(fd);

    /* Re-open and READ back to confirm the driver stored it. */
    fd = open(DEV, O_RDONLY);
    n = read(fd, buf, sizeof buf - 1);
    buf[n] = '\0';
    printf("after write: %s", buf);
    close(fd);

    return 0;
}
```

**Expected output:**
```
$ sudo insmod hellochar.ko
$ ./usehello
initial read: hello from kernel space
after write: set by usehello.c
```

**Walkthrough of the non-obvious parts:**
- **This program is *identical in form* to Module 3's file I/O** — `open`, `read`, `write`, `close` on a path. Nothing signals that `/dev/hellochar` is backed by kernel code rather than a disk file; that's the entire achievement of the driver abstraction. The uniform interface means userspace code doesn't change based on what's behind the fd.
- **Each call round-trips into the module**: `open` → `hello_open`, `read` → `hello_read`, `write` → `hello_write` (visible in `dmesg` via the `pr_info` lines). Run `strace ./usehello` and you see ordinary `openat`/`read`/`write` syscalls (Module 13) — which the kernel then dispatches through `hello_fops` to your driver code. Two boundaries in play: the syscall trap (Module 13) and the VFS→driver dispatch (this module).
- **The write-then-reopen-and-read** proves the driver holds *state* (the `message` buffer) across separate opens — a real, if tiny, device with memory. This is the seed of every stateful device (a framebuffer's pixels, a NIC's config).
- If you run it *without* loading the module, `open` fails with `ENOENT` (no `/dev/hellochar`) — the node exists only while the driver is loaded, exactly as `insmod`/`rmmod` create and destroy it.

---

## Under the Hood

`strace ./usehello` shows the userspace syscalls; `dmesg` shows the driver answering them. Watching both side-by-side is watching the two halves of the boundary meet:

```
# strace ./usehello  (USERSPACE side -- ordinary syscalls, Module 13):
openat(AT_FDCWD, "/dev/hellochar", O_RDWR) = 3            ← [1] traps into kernel
read(3, "hello from kernel space\n", 255)  = 24          ← [2] VFS → hello_read
write(3, "set by usehello.c\n", 18)        = 18          ← [3] VFS → hello_write
close(3)                                   = 0

# dmesg  (KERNEL side -- the DRIVER's printk, same instants):
[ 1250.1] hellochar: opened                              ← hello_open ran
[ 1250.1] hellochar: stored 18 bytes                     ← hello_write ran
```

Annotated:
1. **`openat("/dev/hellochar", ...) = 3`** — an ordinary `open` syscall (Module 13 trap). The kernel resolves the path to a device node, reads its major/minor, finds the registered driver (our misc device), and calls `hello_open` — which logs "opened" to `dmesg`. The fd (3) now routes to `hello_fops`.
2. **`read(3, ...) = 24`** — the `read` syscall enters the VFS, which follows fd 3 to `hello_fops.read` and calls **`hello_read`**. `hello_read` uses `simple_read_from_buffer` (i.e. `copy_to_user`) to move the 24-byte message across the boundary into the userspace `buf`. The `= 24` userspace sees is what the *driver* returned.
3. **`write(3, ...) = 18`** — dispatches to **`hello_write`**, which `copy_from_user`s the 18 bytes into the kernel `message` buffer and logs "stored 18 bytes". The syscall's return value (18) came from the driver.

The headline: **a `read` on a device fd traps into the kernel (Module 13), the VFS follows the fd to the driver's `file_operations` table, and calls the driver's `read` function — which `copy_*_user`s the data across the boundary.** The `strace` line and the `dmesg` line are the *same event* seen from the two sides. Every device interaction in your entire computing life has been this: your `read`, dispatched through a `file_operations` table, into some driver's function, touching (eventually) registers/interrupts/DMA. You are now on both ends of the wire.

---

## Try This

Ordered easy → hard. (All require a Linux VM with kernel headers + root.)

1. **(Easy) Build, load, and talk to the device.** `make`, `sudo insmod hellochar.ko`, then `cat /dev/hellochar`, `echo hi | sudo tee /dev/hellochar`, `cat` again, and watch `dmesg -w` in another terminal show `hello_open`/`hello_write` firing. `sudo rmmod hellochar` and confirm `/dev/hellochar` disappears. *Hint: the node's lifetime = the module's lifetime; that's `misc_register`/`misc_deregister` at work.*

2. **(Easy) Inspect the node's major/minor.** With the module loaded, `ls -l /dev/hellochar` — note the `c` (char) and the major/minor. Compare to `/dev/null` (major 1) and `/dev/sda` (`b`, major 8). Explain what the major number selects. *Hint: major → which driver; the kernel routes `open` by it.*

3. **(Medium) Add an `ioctl` to the driver.** Implement `.unlocked_ioctl = hello_ioctl` with a command that resets the message to the default (define a request with `_IO`, Module 11). Call it from a userspace program with `ioctl(fd, HELLO_RESET)`. *Hint: this is Module 11's `ioctl` from the driver's side — you write the handler the request dispatches to.*

4. **(Medium) Make reads report a counter.** Add a `static int read_count` incremented in `hello_read`, and have the message include it. Observe that the driver holds mutable state across calls — and reason about what happens if two processes read concurrently (a race, Module 6, now in the kernel). Add a mutex (`DEFINE_MUTEX`, `mutex_lock`). *Hint: kernel code faces the same concurrency hazards as Module 6; the fix is kernel mutexes/spinlocks.*

5. **(Hard) Write the classic char driver without the misc shortcut.** Replace `misc_register` with the full path: `alloc_chrdev_region` (get a major/minor), `cdev_init`/`cdev_add` (register the `file_operations`), and `class_create`/`device_create` (auto-create the `/dev` node). Explain what the misc framework was doing for you. *Hint: this is the canonical LDD3 "scull" skeleton; it exposes the major/minor and node machinery the misc device hid.*

---

## Gotchas

- **A kernel bug is a machine bug, not a process bug.** There's no per-process memory protection saving you (Module 7): a null/wild pointer dereference in a driver **panics or hangs the whole system**, a leaked allocation persists until reboot, and an infinite loop can lock a CPU. **Always develop in a VM**, and treat every error path and every pointer with paranoia. The safety nets you've relied on all course are gone in kernel mode.

- **Never dereference a `__user` pointer directly.** A pointer from userspace (`char __user *`) must only be accessed via `copy_from_user`/`copy_to_user`/`get_user`/`put_user` (Module 13). A direct dereference may touch an invalid or malicious address → panic or security hole. This is the #1 driver security rule, and the compiler's `__user` annotation (with `sparse`) exists to catch violations.

- **Free/unregister everything on unload — and on every error path.** The kernel has no GC and no automatic cleanup. Whatever `module_init` registers/allocates, `module_exit` must undo (and each error path in init must unwind what it already did), or you leak resources that survive until reboot and may make the module un-reloadable. Cleanup discipline is mandatory, not optional.

- **`MODULE_LICENSE` is load-bearing.** Omit it or use a non-GPL license and the kernel taints, warns, and *refuses to let your module use GPL-only exported symbols* (many core APIs). `MODULE_LICENSE("GPL")` isn't metadata boilerplate — it gates what your module can call.

- **Build against the *running* kernel's headers.** A module is tightly bound to the exact kernel it's built for; loading a `.ko` built against a different kernel version fails (version magic mismatch) or, worse, corrupts memory. The kbuild `Makefile`'s `/lib/modules/$(uname -r)/build` ensures you build against the right headers. Reboot to a new kernel → rebuild.

- **Don't block or sleep in an interrupt handler.** (For real hardware drivers.) An ISR runs in a restricted context where sleeping/blocking is forbidden and long work stalls the system; do the minimum in the ISR and defer the rest (workqueues, tasklets, threaded IRQs). This is the kernel version of Module 5's "do the minimum in a signal handler" and Module 10's "don't block the event loop."

- **The course's top-level `make` won't build the module.** Kernel modules need kbuild (`make -C /lib/modules/.../build M=$PWD`), not `gcc`. The module source is excluded from the top-level build for exactly this reason; use the module's own `Makefile`. (`usehello.c`, being ordinary userspace, *does* build with the top-level make.)

---

## Checkpoint

1. What is a device driver, and how does a userspace `open("/dev/sda")` actually reach the right one? What do the major and minor numbers select?
2. Contrast character and block devices — their data model and a typical example of each. Why do the two models exist rather than one?
3. What is `struct file_operations`, and precisely how does a userspace `read(fd, ...)` on a device end up running the driver's code? Why is this the same idea as the syscall table (Module 13)?
4. Name the three mechanisms by which a driver actually interacts with hardware, and say what each is for. How do interrupts and DMA relate to the async-I/O ideas from Module 10?
5. Why is kernel-module programming so much more dangerous than userspace programming? Give three specific ways a driver bug is worse than a userspace bug, and the discipline each demands.

---

<details>
<summary><b>Checkpoint Answers</b></summary>

1. A **device driver** is kernel code that implements the file operations (`open`/`read`/`write`/`ioctl`/`close`) for a specific device, exposing hardware through the uniform file interface. `open("/dev/sda")` reaches the right driver via the **device node**: `/dev/sda` is a special file carrying a **major number** (which identifies the driver — 8 for the SCSI/SATA disk driver) and a **minor number** (which specific device that driver manages — which disk). The kernel reads the major number from the node, looks up the registered driver, and routes that fd's subsequent operations to it; the minor number tells the driver which of its devices this is.

2. **Character devices** present an unstructured, sequential **byte stream** (read/write bytes in order, like a pipe) — e.g. a terminal, serial port, or `/dev/null`. **Block devices** present fixed-size, **randomly addressable blocks** (read block 5000 then block 12), sit under the filesystem and page cache — e.g. a disk `/dev/sda`. Both models exist because they match fundamentally different access patterns: a keyboard/serial line *is* a stream consumed once in order (character model fits), while a disk is a randomly-addressable array that benefits from caching and I/O scheduling (block model fits). Forcing either into the other's model would lose the properties that matter (random access + caching for disks; simplicity for streams).

3. `struct file_operations` is a **table of function pointers** a driver registers, mapping each file operation to one of the driver's functions (`.read = hello_read`, `.write = hello_write`, ...). When userspace calls `read(fd, ...)` on the device, the syscall traps into the kernel (Module 13), the **VFS** follows the fd to the driver's `file_operations`, and calls its `.read` — i.e. the driver's function — passing the user buffer (which the driver moves with `copy_to_user`). It's the **same idea as the syscall table**: a generic operation is dispatched to specific code by indexing a table of function pointers — polymorphism/vtables in C. The syscall table routes by syscall *number*; `file_operations` routes by *operation* for whatever object is behind the fd.

4. (1) **Device registers via MMIO or port I/O** — the command/query channel: the driver reads/writes the device's control/status/data registers (mapped into memory addresses for MMIO, or accessed via `in`/`out` port instructions) to command the device and read its state. (2) **Interrupts** — the device asserts a hardware signal that preempts the CPU to run the driver's interrupt handler when it needs attention, instead of the CPU polling. (3) **DMA** — the device transfers bulk data directly to/from RAM without the CPU moving each byte, raising one interrupt at completion. Interrupts + DMA are the hardware embodiment of Module 10's **async, completion-notified I/O**: set up the operation, let it proceed in the background without blocking the CPU, and get notified (interrupt) when it's done (DMA completion) — the same "don't spin waiting" architecture as `epoll`/`io_uring`, one layer down in silicon.

5. Kernel code runs in **kernel mode with full privilege and no safety net** (Module 13/7). Three ways a driver bug is worse: (a) **no memory protection** — a null/wild pointer dereference *panics or hangs the entire machine* rather than segfaulting one process (demands paranoid pointer handling and `copy_*_user` for user pointers); (b) **no automatic cleanup / no GC** — a leaked kernel allocation or unreleased resource persists until reboot and can make the module un-reloadable (demands undoing everything on unload and on every error path); (c) **full hardware/memory access** — a bug can corrupt *any* memory or lock a CPU, and there's no "kill just this thread" (demands developing in a disposable VM, and not blocking/sleeping in restricted contexts like interrupt handlers). The privilege that lets drivers touch hardware is exactly what makes their bugs catastrophic.

</details>

---

*Next up: **Module 15 — Berkeley Packet Filter (BPF) and eBPF.** The modern superpower: instead of writing a risky kernel module, you load small, **verified** programs that run safely inside the kernel at hooks (syscalls, network events, tracepoints) — powering observability (`bpftrace`), networking (Cilium/XDP), and security. It's how you get kernel-level insight and control *without* the panic risk you just learned to fear. The kernel becomes programmable — safely. Continuing straight on.*
