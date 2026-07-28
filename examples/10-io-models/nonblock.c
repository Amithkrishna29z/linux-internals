/* nonblock.c
 *
 * Demonstrates the difference a non-blocking fd makes. We make stdin
 * non-blocking and read in a loop: instead of the thread sleeping until you
 * type, read() returns -1 with errno==EAGAIN when there's nothing yet, so the
 * loop keeps spinning (and could do other work). Type something to see it read.
 *
 * Compile:  gcc -Wall -Wextra -o nonblock nonblock.c
 * Run:      ./nonblock          (prints "waiting..." until you type a line)
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>

int main(void)
{
    /* Make stdin (fd 0) non-blocking. */
    int flags = fcntl(STDIN_FILENO, F_GETFL);
    fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);

    char buf[256];
    int spins = 0;
    for (;;) {
        ssize_t n = read(STDIN_FILENO, buf, sizeof buf - 1);
        if (n > 0) {
            buf[n] = '\0';
            printf("read %zd bytes: %s", n, buf);
            if (buf[0] == 'q') break;          /* type 'q' to quit */
        } else if (n == 0) {
            printf("EOF\n"); break;
        } else {
            /* n < 0: check WHY. EAGAIN just means "nothing ready right now." */
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (++spins % 5000000 == 0)     /* throttle the demo print */
                    printf("waiting... (read returned EAGAIN, thread NOT blocked)\n");
                continue;                        /* a real program does other work here */
            }
            perror("read"); break;
        }
    }
    return 0;
}
