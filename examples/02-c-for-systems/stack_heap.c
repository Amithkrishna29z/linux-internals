/* stack_heap.c
 *
 * Demonstrates the difference between stack and heap memory, correct
 * malloc/free discipline, and WHY returning a pointer to a stack local
 * is a bug (we show the safe heap version instead). Also prints addresses
 * so you can SEE stack vs heap live in different regions.
 *
 * Compile:  gcc -Wall -Wextra -o stack_heap stack_heap.c
 * Run:      ./stack_heap
 * Bonus:    gcc -Wall -Wextra -fsanitize=address -o stack_heap stack_heap.c
 *           (AddressSanitizer will catch leaks/use-after-free if you add them)
 */

#include <stdio.h>
#include <stdlib.h>     /* malloc, free */
#include <string.h>     /* strcpy, strlen */

/* WRONG (shown as a comment so it can't bite): returns the address of a
 * local array that dies when this function returns -> use-after-return.
 *
 *   char *make_greeting_BROKEN(const char *name) {
 *       char buf[64];                 // lives on the STACK
 *       snprintf(buf, sizeof buf, "Hello, %s", name);
 *       return buf;                   // BUG: buf is gone after return!
 *   }
 */

/* RIGHT: allocate on the HEAP so the memory outlives this function.
 * The CALLER now owns it and must free() it. That ownership transfer is
 * the heart of manual memory management. */
char *make_greeting(const char *name)
{
    size_t len = strlen("Hello, ") + strlen(name) + 1;  /* +1 for '\0' */
    char *buf = malloc(len);
    if (buf == NULL) {                 /* ALWAYS check malloc */
        perror("malloc");
        return NULL;
    }
    snprintf(buf, len, "Hello, %s", name);
    return buf;                        /* heap memory survives the return */
}

int main(void)
{
    int stack_var = 42;                /* on the stack */
    int *heap_var = malloc(sizeof(int));
    if (heap_var == NULL) { perror("malloc"); return 1; }
    *heap_var = 99;

    /* Print addresses to SEE the regions (Module 7 explains the full map).
     * %p prints a pointer; cast to (void*) is the correct form. */
    printf("stack_var value=%d  at address %p\n", stack_var, (void *)&stack_var);
    printf("heap_var  value=%d  at address %p\n", *heap_var,  (void *)heap_var);
    printf("(notice the addresses are far apart: stack is high, heap is low)\n");

    char *greeting = make_greeting("Amith");
    if (greeting != NULL) {
        printf("%s\n", greeting);
        free(greeting);                /* WE own it now -> we free it */
        greeting = NULL;               /* defuse the dangling pointer */
    }

    free(heap_var);                    /* match the earlier malloc */
    heap_var = NULL;

    return 0;                          /* every malloc above has a matching free */
}
