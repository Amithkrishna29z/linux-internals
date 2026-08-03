/* bugs.c
 *
 * A rogues' gallery of the four classic memory bugs. Each is guarded by a
 * command-line selector so you can trigger ONE at a time and watch valgrind
 * or AddressSanitizer pinpoint it. This program is WRONG on purpose.
 *
 * Compile (valgrind):  gcc -Wall -Wextra -g -o bugs bugs.c
 *   then:  valgrind --leak-check=full ./bugs leak
 *
 * Compile (ASan):      gcc -Wall -Wextra -g -fsanitize=address -o bugs bugs.c
 *   then:  ./bugs uaf
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void do_leak(void)
{
    char *p = malloc(64);        /* allocated... */
    strcpy(p, "I am never freed");
    printf("%s\n", p);
    /* ...and we return without free(p): a LEAK. */
}

static void do_use_after_free(void)
{
    char *p = malloc(64);
    strcpy(p, "hello");
    free(p);                     /* freed here */
    printf("%s\n", p);           /* USE-AFTER-FREE: reading dangling memory */
}

static void do_double_free(void)
{
    char *p = malloc(64);
    free(p);
    free(p);                     /* DOUBLE-FREE: corrupts allocator metadata */
}

static void do_overflow(void)
{
    char *p = malloc(8);
    strcpy(p, "way too long for eight bytes");  /* BUFFER OVERFLOW past the end */
    printf("%s\n", p);
    free(p);
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s leak|uaf|double|overflow\n", argv[0]);
        return 2;
    }
    if      (strcmp(argv[1], "leak")     == 0) do_leak();
    else if (strcmp(argv[1], "uaf")      == 0) do_use_after_free();
    else if (strcmp(argv[1], "double")   == 0) do_double_free();
    else if (strcmp(argv[1], "overflow") == 0) do_overflow();
    else { fprintf(stderr, "unknown: %s\n", argv[1]); return 2; }
    return 0;
}
