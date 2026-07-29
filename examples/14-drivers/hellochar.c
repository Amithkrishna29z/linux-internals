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
 *
 * NOTE: builds with kbuild, NOT the course's top-level make (it's kernel code).
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
