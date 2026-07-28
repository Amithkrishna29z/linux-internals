/* winsize.c
 *
 * Uses ioctl(TIOCGWINSZ) to read the terminal's size, and catches SIGWINCH to
 * re-read it whenever you resize the window. This is what vim/top/less do to
 * lay out the screen. Ctrl-C to quit; resize the terminal to see it update.
 *
 * Compile:  gcc -Wall -Wextra -o winsize winsize.c
 * Run:      ./winsize   (then drag/resize your terminal window)
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/ioctl.h>      /* ioctl, TIOCGWINSZ, struct winsize */
#include <string.h>

static volatile sig_atomic_t g_resized = 1;   /* start true so we print once */

static void on_winch(int signo)
{
    (void)signo;
    g_resized = 1;          /* just set a flag (async-signal-safe, Module 5) */
}

static void print_size(void)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) < 0) {
        perror("ioctl(TIOCGWINSZ)");   /* fails if stdout isn't a terminal */
        return;
    }
    printf("terminal is %d rows x %d cols\n", ws.ws_row, ws.ws_col);
    fflush(stdout);
}

int main(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_winch;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGWINCH, &sa, NULL);    /* kernel sends SIGWINCH on resize */

    printf("Resize the terminal window (Ctrl-C to quit).\n");
    for (;;) {
        if (g_resized) {
            g_resized = 0;
            print_size();              /* re-query size on each resize */
        }
        pause();                       /* sleep until a signal arrives */
    }
    return 0;
}
