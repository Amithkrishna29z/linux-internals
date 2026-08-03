/* arena.c
 *
 * A minimal ARENA (a.k.a. bump/region) allocator: grab ONE big block from
 * malloc up front, then hand out sub-allocations by just advancing a pointer.
 * Individual frees are impossible; instead you free the WHOLE arena at once.
 * This is how compilers, request handlers, and game engines get O(1) alloc
 * and zero fragmentation for data with a common lifetime.
 *
 * Compile:  gcc -Wall -Wextra -o arena arena.c
 * Run:      ./arena
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

typedef struct {
    unsigned char *base;   /* start of the backing block          */
    size_t         cap;    /* total bytes                          */
    size_t         used;   /* bytes handed out so far (the "bump") */
} Arena;

static Arena arena_new(size_t cap)
{
    Arena a = { malloc(cap), cap, 0 };
    if (!a.base) { perror("malloc"); exit(1); }
    return a;
}

/* Hand out `size` bytes, aligned up to 16 (so any type is safely stored). */
static void *arena_alloc(Arena *a, size_t size)
{
    size_t aligned = (a->used + 15) & ~(size_t)15;   /* round up to 16 */
    if (aligned + size > a->cap) return NULL;         /* arena exhausted */
    void *p = a->base + aligned;
    a->used = aligned + size;                          /* just BUMP the pointer */
    return p;
}

static void arena_reset(Arena *a) { a->used = 0; }     /* "free" everything: O(1) */
static void arena_destroy(Arena *a) { free(a->base); a->base = NULL; }

int main(void)
{
    Arena a = arena_new(1024);

    /* Allocate a few things of different types out of the one arena. */
    char *name = arena_alloc(&a, 32);
    strcpy(name, "arena-allocated string");

    int *nums = arena_alloc(&a, 5 * sizeof(int));
    for (int i = 0; i < 5; i++) nums[i] = i * i;

    printf("name : %s\n", name);
    printf("nums : ");
    for (int i = 0; i < 5; i++) printf("%d ", nums[i]);
    printf("\n");
    printf("arena: %zu / %zu bytes used\n", a.used, a.cap);

    /* Free EVERYTHING in one shot -- no per-object free, no leaks possible. */
    arena_reset(&a);
    printf("after reset: %zu bytes used\n", a.used);

    arena_destroy(&a);   /* return the one big block to malloc */
    return 0;
}
