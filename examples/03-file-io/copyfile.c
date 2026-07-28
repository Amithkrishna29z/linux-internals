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
