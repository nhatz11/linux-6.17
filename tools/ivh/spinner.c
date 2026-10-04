#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <signal.h>

static volatile int stop = 0;
static void handle(int sig) { stop = 1; }

static void *worker(void *arg) {
    long id = (long)arg;
    cpu_set_t s; CPU_ZERO(&s); CPU_SET(id, &s);
    pthread_setaffinity_np(pthread_self(), sizeof(s), &s);
    volatile unsigned long x = 0;
    while (!stop) x++;
    return NULL;
}

int main(int argc, char **argv) {
    int nthr = atoi(argv[1]);
    int secs = atoi(argv[2]);
    pthread_t th[64];
    signal(SIGALRM, handle);
    alarm(secs);
    for (long i = 0; i < nthr; i++) pthread_create(&th[i], NULL, worker, (void*)i);
    for (long i = 0; i < nthr; i++) pthread_join(th[i], NULL);
    return 0;
}
