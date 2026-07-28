/* pathfind.c
 *
 * Given a command name, find the executable the shell WOULD run, by
 * searching each directory in $PATH in order -- reproducing step 4 of
 * the shell's REPL. Also the perfect "first real command" to install
 * into your own PATH.
 *
 * Compile:  gcc -Wall -Wextra -o pathfind pathfind.c
 * Run:      ./pathfind ls
 *           ./pathfind gcc
 *           ./pathfind definitely-not-a-real-command   ; echo $?
 */

#include <stdio.h>
#include <stdlib.h>     /* getenv        */
#include <string.h>     /* strtok, strlen */
#include <unistd.h>     /* access, X_OK   */

int main(int argc, char *argv[])
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s COMMAND\n", argv[0]);
        return 2;                       /* 2 = usage error, a common convention */
    }
    const char *cmd = argv[1];

    /* If the name already contains a slash, the shell does NOT search
     * PATH -- it uses the path as given. We mirror that. */
    if (strchr(cmd, '/') != NULL) {
        if (access(cmd, X_OK) == 0) {   /* X_OK = "is it executable by me?" */
            printf("%s\n", cmd);
            return 0;
        }
        fprintf(stderr, "%s: not found or not executable\n", cmd);
        return 1;
    }

    /* Read $PATH. If unset, fall back to a sane default like the shell. */
    const char *path = getenv("PATH");
    if (path == NULL)
        path = "/usr/local/bin:/usr/bin:/bin";

    /* strtok mutates its input, so work on a copy. strdup mallocs. */
    char *copy = strdup(path);
    if (copy == NULL) {
        perror("strdup");
        return 1;
    }

    char candidate[4096];
    char *dir = strtok(copy, ":");      /* split on ':' */
    int found = 0;
    while (dir != NULL) {
        /* Build "<dir>/<cmd>" safely. snprintf never overflows the buffer
         * and tells us if it would have (return >= size). */
        int n = snprintf(candidate, sizeof candidate, "%s/%s", dir, cmd);
        if (n > 0 && (size_t)n < sizeof candidate) {
            /* access() asks the kernel: does this path exist AND may I
             * execute it, using my REAL uid/gid? (X_OK) */
            if (access(candidate, X_OK) == 0) {
                printf("%s\n", candidate);
                found = 1;
                break;                  /* first match wins -- like the shell */
            }
        }
        dir = strtok(NULL, ":");        /* next directory */
    }

    free(copy);

    if (!found) {
        fprintf(stderr, "%s: command not found\n", cmd);
        return 1;                       /* mirrors the shell's 127-ish "not found" */
    }
    return 0;
}
