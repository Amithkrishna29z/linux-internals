/* prodcons.c
 *
 * The canonical concurrency pattern: a fixed-size ring buffer shared by a
 * producer (puts items in) and a consumer (takes them out). Two condition
 * variables coordinate: `not_full` (producer waits when the buffer is full)
 * and `not_empty` (consumer waits when it's empty). This is a BlockingQueue
 * built by hand -- the core of every thread pool and message queue.
 *
 * Compile:  gcc -Wall -Wextra -pthread -o prodcons prodcons.c
 * Run:      ./prodcons
 */

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>

#define CAP   4          /* buffer capacity (small, to force blocking) */
#define TOTAL 12         /* how many items the producer will make */

static int   buf[CAP];
static int   count = 0;  /* items currently in the buffer */
static int   head  = 0;  /* next slot to write */
static int   tail  = 0;  /* next slot to read */

static pthread_mutex_t m         = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  not_full  = PTHREAD_COND_INITIALIZER;
static pthread_cond_t  not_empty = PTHREAD_COND_INITIALIZER;

static void *producer(void *arg)
{
    (void)arg;
    for (int i = 0; i < TOTAL; i++) {
        pthread_mutex_lock(&m);
        while (count == CAP)                       /* buffer full: wait */
            pthread_cond_wait(&not_full, &m);
        buf[head] = i;
        head = (head + 1) % CAP;
        count++;
        printf("produced %2d   (buffer now %d/%d)\n", i, count, CAP);
        pthread_cond_signal(&not_empty);           /* a consumer may proceed */
        pthread_mutex_unlock(&m);
    }
    return NULL;
}

static void *consumer(void *arg)
{
    (void)arg;
    for (int i = 0; i < TOTAL; i++) {
        pthread_mutex_lock(&m);
        while (count == 0)                          /* buffer empty: wait */
            pthread_cond_wait(&not_empty, &m);
        int item = buf[tail];
        tail = (tail + 1) % CAP;
        count--;
        printf("          consumed %2d   (buffer now %d/%d)\n", item, count, CAP);
        pthread_cond_signal(&not_full);             /* a producer may proceed */
        pthread_mutex_unlock(&m);
    }
    return NULL;
}

int main(void)
{
    pthread_t p, c;
    pthread_create(&p, NULL, producer, NULL);
    pthread_create(&c, NULL, consumer, NULL);
    pthread_join(p, NULL);
    pthread_join(c, NULL);
    printf("done: produced and consumed %d items\n", TOTAL);
    return 0;
}
