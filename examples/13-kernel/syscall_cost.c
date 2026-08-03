/* syscall_cost.c
 *
 * Makes the cost of crossing the user/kernel boundary CONCRETE by timing:
 *   (a) a plain function call        -- no boundary
 *   (b) a vDSO call (clock_gettime)  -- runs in user mode, NO trap
 *   (c) a real trapping syscall (getpid via raw syscall, uncached) -- full trap
 * You should see (a) < (b) << (c): the trap is ~50-150x a function call.
 *
 * Compile:  gcc -Wall -Wextra -O2 -o syscall_cost syscall_cost.c
 * Run:      ./syscall_cost
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <sys/syscall.h>

#define N 2000000

/* A trivial function the compiler can't optimize away (volatile sink). */
static volatile long sink = 0;
static long plain_call(long x) { return x + 1; }

/* Return nanoseconds per call for a loop, using clock_gettime (vDSO) to time. */
static double time_loop(const char *label, int which)
{
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    for (long i = 0; i < N; i++) {
        switch (which) {
        case 0: sink += plain_call(i);                        break; /* function */
        case 1: { struct timespec ts;
                  clock_gettime(CLOCK_MONOTONIC, &ts);              /* vDSO */
                  sink += ts.tv_nsec; }                       break;
        case 2: sink += syscall(SYS_getpid);                   break; /* real trap */
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ns = (t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec);
    double per = ns / N;
    printf("%-28s %8.2f ns/call\n", label, per);
    return per;
}

int main(void)
{
    printf("timing %d iterations each:\n", N);
    double f = time_loop("(a) plain function call", 0);
    double v = time_loop("(b) vDSO clock_gettime", 1);
    double s = time_loop("(c) real syscall getpid", 2);

    printf("\nratios:  vDSO/func = %.1fx   syscall/func = %.1fx   syscall/vDSO = %.1fx\n",
           v / f, s / f, s / v);
    printf("=> the user/kernel TRAP is the expensive part; the vDSO avoids it.\n");
    return 0;
}
