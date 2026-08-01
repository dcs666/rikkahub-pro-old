/*
 * P3 基准：arena 分配/复用 vs malloc/free。
 * 生成式场景（每轮 JSON/流式处理）分配模式固定——arena 应显著更快。
 */
#define _POSIX_C_SOURCE 200809L
#include "rikka/util/arena.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

#define ROUNDS 200
#define ALLOCS 100000

static const int sizes[] = {16, 64, 256, 1024};

int main(void) {
    /* arena：每轮 10 万次混合分配 + reset 复用 */
    double t0 = now_sec();
    Arena *a = arena_create(0);
    for (int r = 0; r < ROUNDS; r++) {
        for (int i = 0; i < ALLOCS; i++) {
            if (!arena_alloc(a, 16, (size_t)sizes[i & 3])) return 1;
        }
        arena_reset(a);
    }
    arena_destroy(a);
    double t1 = now_sec();
    double arena_ms = (t1 - t0) * 1e3;

    /* malloc/free 对照（同分配模式） */
    void *ptrs[ALLOCS];
    t0 = now_sec();
    for (int r = 0; r < ROUNDS; r++) {
        for (int i = 0; i < ALLOCS; i++) {
            ptrs[i] = malloc((size_t)sizes[i & 3]);
            if (!ptrs[i]) return 2;
        }
        for (int i = 0; i < ALLOCS; i++) free(ptrs[i]);
    }
    double t2 = now_sec();
    double malloc_ms = (t2 - t0) * 1e3;

    printf("arena (%d rounds x %d allocs):\n", ROUNDS, ALLOCS);
    printf("  arena alloc+reset : %8.2f ms\n", arena_ms);
    printf("  malloc+free       : %8.2f ms\n", malloc_ms);
    printf("  speedup           : %.1fx\n", malloc_ms / arena_ms);
    return 0;
}
