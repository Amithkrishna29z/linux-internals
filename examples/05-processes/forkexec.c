/* forkexec.c
 *
 * The canonical fork+exec+wait cycle -- what your shell does for every
 * command. Demonstrates: the two returns of fork, using the fork/exec GAP
 * to redirect the child's stdout to a file (dup2), execvp with PATH search,
 * and decoding the child's exit status in the parent.
 *
 * Compile:  gcc -Wall -Wextra -o forkexec forkexec.c
 * Run:      ./forkexec              (runs `ls -l`, child's output -> out.txt)
 *           cat out.txt
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>     /* fork, execvp, dup2, close, _exit */
#include <fcntl.h>      /* open */
#include <sys/wait.h>   /* waitpid, W* macros */
#include <errno.h>

int main(void)
{
    printf("parent PID = %d, about to fork...\n", getpid());
    fflush(stdout);   /* FLUSH before fork: avoid the duplicated-buffer bug (Module 3) */

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return 1;
    }

    if (pid == 0) {
        /* ---- CHILD ----
         * We are still running THIS program's code. This is the GAP:
         * set up the environment the new program will inherit, THEN exec. */
        printf("  child PID = %d (fork returned 0 here)\n", getpid());
        fflush(stdout);

        /* Redirect the child's stdout to out.txt -- exactly like `ls > out.txt`.
         * The exec'd `ls` will inherit fd 1 pointing at the file. */
        int fd = open("out.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) { perror("open"); _exit(1); }
        if (dup2(fd, STDOUT_FILENO) < 0) { perror("dup2"); _exit(1); }
        close(fd);

        /* Replace this process with `ls -l`. execvp searches $PATH.
         * On success, NOTHING below runs -- the image is gone. */
        char *args[] = { "ls", "-l", NULL };
        execvp("ls", args);

        /* Only reached if exec FAILED. Use _exit (not exit) in a child
         * after fork to avoid flushing the parent's stdio buffers twice. */
        perror("execvp");
        _exit(127);   /* 127 = command not found, by convention */
    }

    /* ---- PARENT ----
     * fork returned the child's PID here. Wait for the child to finish
     * and decode HOW it ended. */
    int status;
    pid_t w = waitpid(pid, &status, 0);   /* blocks until child exits */
    if (w < 0) { perror("waitpid"); return 1; }

    if (WIFEXITED(status))
        printf("parent: child %d exited normally, code %d\n",
               w, WEXITSTATUS(status));
    else if (WIFSIGNALED(status))
        printf("parent: child %d killed by signal %d\n",
               w, WTERMSIG(status));

    printf("parent: child's output was redirected to out.txt\n");
    return 0;
}
