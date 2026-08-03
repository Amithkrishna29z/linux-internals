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
    if (fd < 0) { perror("reopen"); return 1; }
    n = read(fd, buf, sizeof buf - 1);
    if (n < 0) { perror("read"); return 1; }
    buf[n] = '\0';
    printf("after write: %s", buf);
    close(fd);

    return 0;
}
