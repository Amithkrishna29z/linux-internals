/* pipe_basic.c
 *
 * The simplest IPC: create a pipe BEFORE fork, so parent and child share it.
 * The child writes a message into the pipe; the parent reads it out. Shows
 * the must-close-the-unused-end rule (or the reader never sees EOF).
 *
 * Compile:  gcc -Wall -Wextra -o pipe_basic pipe_basic.c
 * Run:      ./pipe_basic
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/wait.h>

int main(void)
{
    int fds[2];
    if (pipe(fds) < 0) { perror("pipe"); return 1; }   /* fds[0]=read, fds[1]=write */

    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 1; }

    if (pid == 0) {
        /* CHILD: writer. Close the read end we don't use, then write. */
        close(fds[0]);
        const char *msg = "hello from the child";
        write(fds[1], msg, strlen(msg));
        close(fds[1]);        /* closing the write end lets the reader see EOF */
        _exit(0);
    }

    /* PARENT: reader. Close the write end we don't use, then read to EOF. */
    close(fds[1]);            /* CRUCIAL: if we leave OUR write end open, read never EOFs */
    char buf[128];
    ssize_t n;
    printf("parent received: ");
    while ((n = read(fds[0], buf, sizeof buf)) > 0)
        fwrite(buf, 1, (size_t)n, stdout);   /* read returns 0 (EOF) when all write ends closed */
    printf("\n");
    close(fds[0]);
    wait(NULL);
    return 0;
}
