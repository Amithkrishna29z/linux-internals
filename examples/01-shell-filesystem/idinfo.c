/* idinfo.c
 *
 * Prints the process's real and effective UID/GID, and resolves the real
 * UID back to a username. Run it normally, then run it as a setuid binary
 * to SEE effective UID diverge from real UID -- the exact mechanism that
 * lets passwd write /etc/shadow.
 *
 * Compile:  gcc -Wall -Wextra -o idinfo idinfo.c
 * Run:      ./idinfo
 * Make it setuid-root and run again (see "Try This"):
 *      sudo chown root idinfo && sudo chmod u+s idinfo && ./idinfo
 */

#include <stdio.h>
#include <unistd.h>     /* getuid, geteuid, getgid, getegid */
#include <sys/types.h>
#include <pwd.h>        /* getpwuid -> username from UID     */
#include <errno.h>
#include <string.h>

int main(void)
{
    uid_t ruid = getuid();    /* real UID  : who launched me       */
    uid_t euid = geteuid();   /* effective : who I act as for perms */
    gid_t rgid = getgid();
    gid_t egid = getegid();

    printf("real UID = %d, effective UID = %d\n", (int)ruid, (int)euid);
    printf("real GID = %d, effective GID = %d\n", (int)rgid, (int)egid);

    /* Turn the real UID number into a name by looking it up in the
     * user database (/etc/passwd). getpwuid returns NULL on failure and,
     * unusually, sets errno only sometimes -- so we clear it first. */
    errno = 0;
    struct passwd *pw = getpwuid(ruid);
    if (pw == NULL) {
        if (errno != 0)
            perror("getpwuid");
        else
            fprintf(stderr, "no passwd entry for uid %d\n", (int)ruid);
        return 1;
    }
    printf("you are: %s (home: %s, shell: %s)\n",
           pw->pw_name, pw->pw_dir, pw->pw_shell);

    if (ruid != euid)
        printf(">> effective UID differs from real UID: "
               "this process is running with ELEVATED privilege "
               "(setuid in effect).\n");
    else
        printf(">> real == effective: ordinary privilege.\n");

    return 0;
}
