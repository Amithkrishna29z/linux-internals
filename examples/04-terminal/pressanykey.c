/* pressanykey.c
 *
 * Switches the terminal into RAW mode to read ONE keystroke at a time,
 * with no line-buffering and no echo -- the machinery behind vim, less,
 * htop. Critically, it RESTORES the original terminal settings on exit
 * (even via atexit), so it never leaves your shell broken.
 *
 * Compile:  gcc -Wall -Wextra -o pressanykey pressanykey.c
 * Run:      ./pressanykey        (press keys; 'q' quits)
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>     /* read, STDIN_FILENO, isatty */
#include <termios.h>    /* tcgetattr, tcsetattr, struct termios */
#include <errno.h>

static struct termios g_saved;   /* original settings, to restore */
static int g_raw_active = 0;

/* Restore cooked mode. Registered with atexit so it runs no matter how
 * we leave main -- the single most important habit in terminal code. */
static void restore_terminal(void)
{
    if (g_raw_active) {
        tcsetattr(STDIN_FILENO, TCSANOW, &g_saved);
        g_raw_active = 0;
    }
}

static int enable_raw_mode(void)
{
    if (!isatty(STDIN_FILENO)) {                 /* not a terminal? bail. */
        fprintf(stderr, "stdin is not a terminal\n");
        return -1;
    }
    if (tcgetattr(STDIN_FILENO, &g_saved) < 0) { /* save current settings */
        perror("tcgetattr");
        return -1;
    }
    atexit(restore_terminal);                    /* guarantee restore */

    struct termios raw = g_saved;                /* start from a copy */
    /* Turn OFF: ICANON (line-buffering) and ECHO (visible typing).
     * ISIG left ON so Ctrl-C still works here; real editors turn it off. */
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN]  = 1;   /* read returns after >=1 byte */
    raw.c_cc[VTIME] = 0;   /* with no timeout: block until a key */

    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) < 0) {
        perror("tcsetattr");
        return -1;
    }
    g_raw_active = 1;
    return 0;
}

int main(void)
{
    if (enable_raw_mode() < 0)
        return 1;

    printf("Raw mode ON. Press keys (they appear immediately, no Enter).\n");
    printf("Press 'q' to quit.\n");
    fflush(stdout);   /* flush: we're about to read char-by-char */

    char c;
    ssize_t n;
    while ((n = read(STDIN_FILENO, &c, 1)) == 1) {   /* one byte at a time */
        if (c == 'q') {
            printf("\r\nBye.\r\n");     /* \r\n: raw mode needs explicit CR */
            break;
        }
        /* Show the byte and its numeric code. Printable? show it; else code. */
        if (c >= 32 && c < 127)
            printf("you pressed '%c' (code %d)\r\n", c, c);
        else
            printf("you pressed control/non-printable (code %d)\r\n", c);
        fflush(stdout);
    }
    if (n < 0 && errno != 0) perror("read");

    /* restore_terminal() runs via atexit here too. */
    return 0;
}
