/* rawsyscall.c
 *
 * Invokes syscalls THREE ways to make the mechanism visible: the normal glibc
 * wrapper write(), the raw syscall(SYS_write, ...) that skips the wrapper but
 * makes the same trap, and getpid via both the wrapper and raw. All of them
 * end at the SAME `syscall` instruction and the SAME kernel handler.
 *
 * Compile:  gcc -Wall -Wextra -o rawsyscall rawsyscall.c
 * Run:      ./rawsyscall
 *   Inspect:  strace ./rawsyscall   (see identical write/getpid syscalls)
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <unistd.h>
#include <sys/syscall.h>   /* SYS_write, SYS_getpid -- the syscall NUMBERS */
#include <string.h>

int main(void)
{
    const char *msg = "hello via ";

    /* 1) The normal way: glibc's write() wrapper. */
    write(STDOUT_FILENO, msg, strlen(msg));
    write(STDOUT_FILENO, "glibc write()\n", 14);

    /* 2) The raw way: syscall(SYS_write, ...). SAME trap, SAME sys_write,
     *    just without glibc's wrapper (and it does NOT set errno for you). */
    const char *raw = "hello via ";
    syscall(SYS_write, STDOUT_FILENO, raw, strlen(raw));
    syscall(SYS_write, STDOUT_FILENO, "raw syscall()\n", 14);

    /* 3) getpid two ways -- prove they agree (same kernel handler). */
    pid_t a = getpid();                     /* glibc wrapper */
    pid_t b = (pid_t)syscall(SYS_getpid);   /* raw trap */
    printf("getpid(): wrapper=%d  raw=%d  %s\n",
           a, b, a == b ? "(identical)" : "(MISMATCH?!)");

    /* Show the syscall NUMBERS -- these index the kernel's sys_call_table. */
    printf("SYS_write = %d, SYS_getpid = %d (indices into sys_call_table)\n",
           (int)SYS_write, (int)SYS_getpid);
    return 0;
}
