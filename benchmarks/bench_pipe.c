/*
 * M6 基准：SPSC 无锁队列吞吐。
 * 对标 JVM 版协程链串行（每阶段同线程）；本队列让阶段独立线程并行。
 */
#define _POSIX_C_SOURCE 200809L
#include "rikka/pipe/spsc.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

typedef struct {
    RkSpsc *q;
    int n;
    long long total;
} BenchCtx;

static void *writer(void *v) {
    BenchCtx *b = (BenchCtx *)v;
    char item[64];
    memset(item, 'x', sizeof(item));
    for (int i = 0; i < b->n; i++) {
        while (rk_spsc_push(b->q, item, 32) != 0) usleep(10);
    }
    rk_spsc_close(b->q);
    return NULL;
}

static void *reader(void *v) {
    BenchCtx *b = (BenchCtx *)v;
    char buf[128];
    for (;;) {
        ssize_t n = rk_spsc_pop(b->q, buf, sizeof(buf));
        if (n == 0) break;
        if (n < 0) { usleep(10); continue; }
        b->total += n;
    }
    return NULL;
}

int main(void) {
    const int N = 1000000; /* 100 万次传输 */
    RkSpsc q;
    rk_spsc_init(&q, 1 << 20);
    BenchCtx w = {&q, N, 0}, r = {&q, 0, 0};
    pthread_t wt, rt;
    double t0 = now_sec();
    pthread_create(&wt, NULL, writer, &w);
    pthread_create(&rt, NULL, reader, &r);
    pthread_join(wt, NULL);
    pthread_join(rt, NULL);
    double t1 = now_sec();
    double dt = t1 - t0;

    printf("SPSC transfer (%d items x 32 B):\n", N);
    printf("  elapsed     : %.2f ms\n", dt * 1e3);
    printf("  throughput  : %.1f Mops/s (%.0f MB/s)\n",
           (double)N / dt / 1e6, (double)N * 32 / dt / 1e6);
    printf("  per-item    : %.1f ns\n", dt * 1e9 / N);
    printf("  received    : %lld bytes\n", r.total);
    rk_spsc_destroy(&q);
    return 0;
}
