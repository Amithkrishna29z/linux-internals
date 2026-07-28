/* signals.c
 *
 * Installs signal handlers with sigaction: SIGINT (Ctrl-C) sets a flag for
 * a graceful shutdown, and SIGCHLD reaps children so they never become
 * zombies. Demonstrates async-signal-safety: handlers do the MINIMUM
 * (set a flag / call waitpid), real work happens in the main loop.
 *
 * Compile:  gcc -Wall -Wextra -o signals signals.c
 * Run:      ./signals        (spawns children; press Ctrl-C to stop)
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <errno.h>
#include <string.h>

/* volatile sig_atomic_t: the ONLY type safe to touch from a handler and
 * read from main. 'volatile' stops the compiler caching it in a register. */
static volatile sig_atomic_t g_stop = 0;

/* SIGINT handler: do the minimum. Just record that we should stop.
 * We do NOT printf here (not async-signal-safe). */
static void on_sigint(int signo)
{
    (void)signo;
    g_stop = 1;
    /* write() IS async-signal-safe, so a tiny note is allowed: */
    const char msg[] = "\n[SIGINT received: will shut down]\n";
    write(STDERR_FILENO, msg, sizeof msg - 1);
}

/* SIGCHLD handler: reap ALL finished children in a loop. waitpid with
 * WNOHANG is async-signal-safe. Looping handles multiple children exiting
 * "at once" (signals don't queue, so one SIGCHLD may cover several). */
static void on_sigchld(int signo)
{
    (void)signo;
    int saved = errno;                 /* preserve errno across the handler */
    while (waitpid(-1, NULL, WNOHANG) > 0)
        ;                               /* reap each; loop until none left */
    errno = saved;
}

static void install(int signo, void (*fn)(int), int flags)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = fn;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = flags;
    if (sigaction(signo, &sa, NULL) < 0) { perror("sigaction"); exit(1); }
}

int main(void)
{
    install(SIGINT,  on_sigint,  0);
    install(SIGCHLD, on_sigchld, SA_RESTART);   /* SA_RESTART: resume slept syscalls */

    printf("PID %d running. I spawn a child every 2s. Ctrl-C to stop.\n",
           getpid());

    while (!g_stop) {
        pid_t c = fork();
        if (c < 0) { perror("fork"); break; }
        if (c == 0) {
            /* child: do a little work, then exit with a code */
            printf("  child %d working...\n", getpid());
            fflush(stdout);
            sleep(1);
            _exit(0);           /* triggers SIGCHLD in the parent -> reaped */
        }
        /* parent: keep going; the SIGCHLD handler reaps children for us,
         * so NO zombies accumulate even though we never call wait() here. */
        sleep(2);
    }

    printf("Graceful shutdown. Reaping any stragglers...\n");
    while (waitpid(-1, NULL, 0) > 0)      /* reap remaining children */
        ;
    printf("Done.\n");
    return 0;
}
