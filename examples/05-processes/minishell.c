/* minishell.c
 *
 * A minimal but REAL shell. It ties together the whole module: read a line,
 * parse it into argv, handle output redirection (`cmd > file`), fork, set up
 * redirection in the child (the fork/exec GAP), execvp, and wait in the
 * parent -- reporting the exit code as $?. Also handles built-in `cd` and
 * `exit`. This is bash's core loop in ~130 lines.
 *
 * Compile:  gcc -Wall -Wextra -o minishell minishell.c
 * Run:      ./minishell
 *           mysh$ ls -l
 *           mysh$ echo hello > greeting.txt
 *           mysh$ cat greeting.txt
 *           mysh$ cd /tmp
 *           mysh$ exit
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <errno.h>

#define MAX_ARGS 64

/* Split `line` in place into argv words on whitespace. Also detects a
 * `> filename` redirection, removing it from argv and returning the target
 * filename via *outfile (NULL if none). Returns the argument count. */
static int parse(char *line, char *argv[], char **outfile)
{
    *outfile = NULL;
    int argc = 0;
    char *tok = strtok(line, " \t\r\n");
    while (tok != NULL && argc < MAX_ARGS - 1) {
        if (strcmp(tok, ">") == 0) {
            /* next token is the redirection target */
            tok = strtok(NULL, " \t\r\n");
            if (tok == NULL) {
                fprintf(stderr, "mysh: syntax error: expected filename after >\n");
                return -1;
            }
            *outfile = tok;
        } else {
            argv[argc++] = tok;
        }
        tok = strtok(NULL, " \t\r\n");
    }
    argv[argc] = NULL;   /* execvp needs a NULL-terminated array */
    return argc;
}

int main(void)
{
    char line[1024];
    char *argv[MAX_ARGS];
    char *outfile;

    while (1) {
        /* PROMPT */
        printf("mysh$ ");
        fflush(stdout);

        /* READ a line. NULL => EOF (Ctrl-D): exit cleanly. */
        if (fgets(line, sizeof line, stdin) == NULL) {
            printf("\n");
            break;
        }

        int argc = parse(line, argv, &outfile);
        if (argc < 0) continue;      /* parse error already reported */
        if (argc == 0) continue;     /* empty line */

        /* BUILT-INS: cd and exit must run in the SHELL itself, not a child
         * (a child chdir wouldn't affect the shell). */
        if (strcmp(argv[0], "exit") == 0)
            break;
        if (strcmp(argv[0], "cd") == 0) {
            const char *dir = argv[1] ? argv[1] : getenv("HOME");
            if (chdir(dir) < 0) perror("cd");
            continue;
        }

        /* EXTERNAL command: fork + exec + wait. */
        pid_t pid = fork();
        if (pid < 0) { perror("fork"); continue; }

        if (pid == 0) {
            /* CHILD: the fork/exec GAP -- set up redirection, then exec. */
            if (outfile != NULL) {
                int fd = open(outfile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (fd < 0) { perror(outfile); _exit(1); }
                if (dup2(fd, STDOUT_FILENO) < 0) { perror("dup2"); _exit(1); }
                close(fd);
            }
            execvp(argv[0], argv);
            /* only if exec failed: */
            fprintf(stderr, "mysh: %s: %s\n", argv[0], strerror(errno));
            _exit(127);
        }

        /* PARENT: wait and report status (this is $?). */
        int status;
        if (waitpid(pid, &status, 0) < 0) { perror("waitpid"); continue; }
        if (WIFEXITED(status) && WEXITSTATUS(status) != 0)
            fprintf(stderr, "mysh: [exit %d]\n", WEXITSTATUS(status));
        else if (WIFSIGNALED(status))
            fprintf(stderr, "mysh: [killed by signal %d]\n", WTERMSIG(status));
    }
    return 0;
}
