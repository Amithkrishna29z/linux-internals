/* hello_syscall.c
 *
 * Prints a message TWO ways:
 *   (a) via write(2)  -- a raw system call, straight through the wall
 *   (b) via printf(3) -- a C-library function that buffers, then calls write(2)
 *
 * The point is not the output (it looks identical). The point is what
 * strace shows underneath: printf and write are NOT the same thing.
 *
 * Compile:  gcc -Wall -Wextra -o hello_syscall hello_syscall.c
 * Run:      ./hello_syscall
 * Inspect:  strace -e trace=write ./hello_syscall
 */

#include <unistd.h>     /* write(), STDOUT_FILENO */
#include <stdio.h>      /* printf(), perror()     */
#include <string.h>     /* strlen()               */
#include <errno.h>      /* errno                  */

int main(void)
{
    const char *msg = "Hello via write() syscall\n";

    /* write(fd, buffer, count) returns the number of bytes actually
     * written, or -1 on error. fd 1 is standard output (STDOUT_FILENO).
     * RULE OF THE COURSE: check every syscall's return value. */
    ssize_t n = write(STDOUT_FILENO, msg, strlen(msg));
    if (n == -1) {
        perror("write");   /* prints "write: <reason>" using errno */
        return 1;
    }

    /* printf lives in the C library. It formats into an internal buffer
     * and (for a terminal) flushes on the newline, eventually calling
     * write() itself. You never invoked write() here -- glibc did. */
    printf("Hello via printf() library call\n");

    return 0;   /* exit code 0 = success, like System.exit(0) */
}
