/* funcptr.c
 *
 * Function pointers: variables that hold the ADDRESS of a function, so you
 * can pass behavior around as data. This is THE pattern behind qsort's
 * comparator, signal handlers (Module 5), and the file_operations struct
 * you'll fill in when you write a device driver (Module 14).
 *
 * Compile:  gcc -Wall -Wextra -o funcptr funcptr.c
 * Run:      ./funcptr
 */

#include <stdio.h>
#include <stdlib.h>     /* qsort */

/* A comparator: qsort will CALL BACK into this to order elements.
 * Signature is fixed by qsort: it hands us void* pointers to two elements.
 * We cast them back to the real type. Returns <0, 0, >0 like Java's
 * Comparator.compare(). */
static int cmp_int_asc(const void *a, const void *b)
{
    int x = *(const int *)a;
    int y = *(const int *)b;
    return (x > y) - (x < y);          /* branchless -1/0/+1, no overflow */
}

static int cmp_int_desc(const void *a, const void *b)
{
    int x = *(const int *)a;
    int y = *(const int *)b;
    return (y > x) - (y < x);
}

/* Our own function that takes a callback: apply `fn` to each element.
 * `void (*fn)(int)` reads as "fn is a pointer to a function taking int,
 * returning void". */
static void for_each(int *arr, size_t n, void (*fn)(int))
{
    for (size_t i = 0; i < n; i++)
        fn(arr[i]);                    /* call THROUGH the pointer */
}

static void print_one(int v) { printf("%d ", v); }

int main(void)
{
    int a[] = {5, 2, 9, 1, 7};
    size_t n = sizeof a / sizeof a[0]; /* idiomatic element count */

    /* qsort(base, count, element_size, comparator).
     * The 4th arg is a FUNCTION POINTER -- we pass behavior as data. */
    qsort(a, n, sizeof a[0], cmp_int_asc);
    printf("ascending:  "); for_each(a, n, print_one); printf("\n");

    qsort(a, n, sizeof a[0], cmp_int_desc);
    printf("descending: "); for_each(a, n, print_one); printf("\n");

    /* A function pointer is just a variable. Store it, reassign it, choose
     * at runtime -- exactly how a driver's file_operations table works. */
    int (*chosen)(const void *, const void *) = cmp_int_asc;
    printf("cmp(3,8) via pointer = %d\n", chosen(&(int){3}, &(int){8}));

    return 0;
}
