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
