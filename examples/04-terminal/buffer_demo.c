/* buffer_demo.c
 *
 * Demonstrates why output ORDER can surprise you: stdout is line-buffered
 * (to a terminal) or fully-buffered (to a file), while stderr is always
 * unbuffered. Run it to the terminal, then redirect, and compare.
 *
 * Compile:  gcc -Wall -Wextra -o buffer_demo buffer_demo.c
 * Run:      ./buffer_demo
 *           ./buffer_demo > out.txt 2> err.txt ; echo "--out--"; cat out.txt; echo "--err--"; cat err.txt
 *           ./buffer_demo 2>&1 | cat        # merge streams through a pipe
 */

#include <stdio.h>
#include <unistd.h>

int main(void)
{
    /* Interleave stdout and stderr WITHOUT newlines to expose buffering.
     * On a terminal you may still see them roughly in order (line-buffered
     * flushes at program exit too); redirect stdout to a file and the
     * ordering / timing changes because stdout becomes FULLY buffered. */
    fprintf(stdout, "1:stdout ");   /* buffered */
    fprintf(stderr, "2:stderr ");   /* UNbuffered -> appears immediately */
    fprintf(stdout, "3:stdout ");   /* buffered */
    fprintf(stderr, "4:stderr ");   /* UNbuffered */

    /* Force stdout out now so the final newline groups things sensibly. */
    fprintf(stdout, "\n");
    fflush(stdout);                 /* explicit flush: guarantee visibility */

    /* Demonstrate changing the buffering mode yourself. */
    setvbuf(stdout, NULL, _IONBF, 0);   /* make stdout UNbuffered too */
    printf("now stdout is unbuffered: this appears instantly\n");

    return 0;
}
