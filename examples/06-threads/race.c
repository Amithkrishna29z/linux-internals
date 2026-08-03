/* race.c
 *
 * Demonstrates a data race and its fix. Two threads each increment a shared
 * counter N times. WITHOUT a mutex the final total is less than 2*N (lost
 * updates); WITH the mutex it is exactly 2*N, every time.
 *
 * Compile:  gcc -Wall -Wextra -pthread -o race race.c
 * Run:      ./race            (racy: total < 2000000, varies each run)
 *           ./race -lock      (locked: total == 2000000, always)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#define ITERS 1000000

static long counter = 0;                 /* SHARED between both threads */
static int use_lock = 0;                 /* toggled by the -lock argument */
static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;

static void *bump(void *arg)
{
    (void)arg;
    for (int i = 0; i < ITERS; i++) {
        if (use_lock) pthread_mutex_lock(&m);
        counter++;                       /* load, add, store -- not atomic! */
        if (use_lock) pthread_mutex_unlock(&m);
    }
    return NULL;
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "-lock") == 0)
        use_lock = 1;

    pthread_t a, b;
    pthread_create(&a, NULL, bump, NULL);
    pthread_create(&b, NULL, bump, NULL);
    pthread_join(a, NULL);               /* wait for BOTH before reading */
    pthread_join(b, NULL);

    long expected = 2L * ITERS;
    printf("counter = %ld  (expected %ld)  %s%s\n",
           counter, expected,
           use_lock ? "[locked]" : "[racy]",
           counter == expected ? "  OK" : "  <-- LOST UPDATES");
    return 0;
}
