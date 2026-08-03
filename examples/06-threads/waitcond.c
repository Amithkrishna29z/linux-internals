/* waitcond.c
 *
 * A minimal condition-variable demo. The main thread waits until a worker
 * sets `ready`, using the correct mutex + while-loop + cond_wait pattern.
 * Shows why the predicate is re-checked in a while (spurious wakeups).
 *
 * Compile:  gcc -Wall -Wextra -pthread -o waitcond waitcond.c
 * Run:      ./waitcond
 */

#include <stdio.h>
#include <pthread.h>
#include <unistd.h>

static pthread_mutex_t m   = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  cv  = PTHREAD_COND_INITIALIZER;
static int ready = 0;                       /* the shared PREDICATE */

static void *worker(void *arg)
{
    (void)arg;
    sleep(1);                               /* simulate slow setup work */

    pthread_mutex_lock(&m);
    ready = 1;                               /* change the shared state... */
    pthread_cond_signal(&cv);                /* ...then wake the waiter */
    pthread_mutex_unlock(&m);
    printf("worker: signalled ready\n");
    return NULL;
}

int main(void)
{
    pthread_t t;
    pthread_create(&t, NULL, worker, NULL);

    pthread_mutex_lock(&m);
    while (!ready)                           /* WHILE: re-check after every wake */
        pthread_cond_wait(&cv, &m);          /* unlocks m, sleeps, re-locks on wake */
    pthread_mutex_unlock(&m);

    printf("main: observed ready, proceeding\n");
    pthread_join(t, NULL);
    return 0;
}
