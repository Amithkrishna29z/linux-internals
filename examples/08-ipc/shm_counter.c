/* shm_counter.c
 *
 * Shared-memory IPC: parent and child both increment a counter that lives in
 * memory mmap'd MAP_SHARED (so it's the SAME physical memory in both). A POSIX
 * semaphore placed IN that shared memory synchronizes them -- because shared
 * mutable memory across processes races exactly like threads do (Module 6).
 *
 * Compile:  gcc -Wall -Wextra -pthread -o shm_counter shm_counter.c
 * Run:      ./shm_counter        (final count == 2 * ITERS, every time)
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <semaphore.h>

#define ITERS 100000

/* The shared region's layout: a semaphore for mutual exclusion + the counter. */
typedef struct {
    sem_t sem;
    long  counter;
} Shared;

int main(void)
{
    /* mmap an anonymous, SHARED region: survives fork and is common to both. */
    Shared *s = mmap(NULL, sizeof *s, PROT_READ | PROT_WRITE,
                     MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (s == MAP_FAILED) { perror("mmap"); return 1; }

    /* sem_init with pshared=1: a semaphore usable BETWEEN processes, initial value 1. */
    if (sem_init(&s->sem, 1, 1) < 0) { perror("sem_init"); return 1; }
    s->counter = 0;

    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 1; }

    /* BOTH parent and child run this loop, on the SAME s->counter. */
    for (int i = 0; i < ITERS; i++) {
        sem_wait(&s->sem);       /* lock: like pthread_mutex_lock, across processes */
        s->counter++;            /* the shared increment -- race without the semaphore */
        sem_post(&s->sem);       /* unlock */
    }

    if (pid == 0) {
        _exit(0);                /* child done */
    }

    wait(NULL);                  /* parent waits for child */
    printf("counter = %ld  (expected %d)  %s\n",
           s->counter, 2 * ITERS,
           s->counter == 2 * ITERS ? "OK" : "<-- RACE (lost updates)");

    sem_destroy(&s->sem);
    munmap(s, sizeof *s);
    return 0;
}
