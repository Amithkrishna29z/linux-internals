/* layout.c
 *
 * Prints the address of a symbol from each memory segment, so you can SEE
 * the layout: text (code) low, then data/bss globals, then the heap growing
 * up, and the stack way up high growing down. Also shows sbrk(0) (the current
 * program break = top of the heap).
 *
 * Compile:  gcc -Wall -Wextra -o layout layout.c
 * Run:      ./layout
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int   g_init   = 42;      /* DATA segment (initialized global)     */
int   g_bss;              /* BSS  segment (zero-initialized global) */

static void some_function(void) { /* its address is in TEXT */ }

int main(void)
{
    int      local = 1;               /* STACK */
    int     *heap  = malloc(sizeof *heap);   /* points into the HEAP */

    printf("text  (code)   some_function = %p\n", (void *)some_function);
    printf("data  (init'd) &g_init       = %p\n", (void *)&g_init);
    printf("bss   (zero)   &g_bss        = %p\n", (void *)&g_bss);
    printf("heap  (malloc) heap          = %p\n", (void *)heap);
    printf("break (sbrk 0) top-of-heap   = %p\n", (void *)sbrk(0));
    printf("stack (local)  &local        = %p\n", (void *)&local);

    (void)local;
    free(heap);
    return 0;
}
