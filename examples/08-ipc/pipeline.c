/* pipeline.c
 *
 * Implements the shell pipeline `ls | wc -l` by hand: one pipe, two forks,
 * and dup2 to wire ls's stdout to the pipe and wc's stdin from the pipe.
 * This is EXACTLY what your shell does for the `|` operator.
 *
 * Compile:  gcc -Wall -Wextra -o pipeline pipeline.c
 * Run:      ./pipeline        (prints the number of entries, like `ls | wc -l`)
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>

int main(void)
{
    int p[2];
    if (pipe(p) < 0) { perror("pipe"); return 1; }

    /* --- Child 1: `ls`, stdout -> pipe write end --- */
    pid_t c1 = fork();
    if (c1 < 0) { perror("fork"); return 1; }
    if (c1 == 0) {
        dup2(p[1], STDOUT_FILENO);   /* ls writes to the pipe instead of the terminal */
        close(p[0]); close(p[1]);    /* close BOTH original pipe fds after dup2 */
        execlp("ls", "ls", (char *)NULL);
        perror("execlp ls"); _exit(127);
    }

    /* --- Child 2: `wc -l`, stdin <- pipe read end --- */
    pid_t c2 = fork();
    if (c2 < 0) { perror("fork"); return 1; }
    if (c2 == 0) {
        dup2(p[0], STDIN_FILENO);    /* wc reads from the pipe instead of the keyboard */
        close(p[0]); close(p[1]);
        execlp("wc", "wc", "-l", (char *)NULL);
        perror("execlp wc"); _exit(127);
    }

    /* --- Parent: MUST close both pipe ends, or wc never sees EOF --- */
    close(p[0]);
    close(p[1]);
    waitpid(c1, NULL, 0);
    waitpid(c2, NULL, 0);
    return 0;
}
