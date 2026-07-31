/*
 * M2 基准：流式文本累积 —— C 零拷贝累积 vs "JVM 风格每 token 重建字符串"。
 * 后者的总复制量是 O(n²)：n 个 token × 累积文本长。
 */
#define _POSIX_C_SOURCE 200809L
#include "rikka/core/message.h"
#include "rikka/util/arena.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

int main(void) {
    const size_t TOKEN = 32;
    const int N = 10000; /* 重建版 O(n²) 太大，用 1 万 token（≈160MB 总复制） */
    char token[TOKEN];
    for (size_t i = 0; i < TOKEN; i++) token[i] = (char)('a' + (i % 26));

    /* C 版：Buf 零拷贝累积 */
    Arena *a = arena_create(0);
    RikkaStream s;
    rstream_init(&s, a, RIKKA_ROLE_ASSISTANT);
    double t0 = now_sec();
    for (int i = 0; i < N; i++) rstream_append_text(&s, token, TOKEN);
    double t1 = now_sec();
    size_t final_len = s.msg->parts[0].len;
    rstream_freeze(&s);
    rstream_destroy(&s);
    arena_destroy(a);

    /* JVM 风格：每 token 重建整个字符串（a+b → new String） */
    char *buf = (char *)malloc(1);
    size_t blen = 0;
    double t2 = now_sec();
    for (int i = 0; i < N; i++) {
        char *nb = (char *)malloc(blen + TOKEN);
        memcpy(nb, buf, blen);
        memcpy(nb + blen, token, TOKEN);
        free(buf);
        buf = nb;
        blen += TOKEN;
    }
    double t3 = now_sec();
    free(buf);

    double c_dt = t1 - t0;
    double jvm_dt = t3 - t2;
    double total_copy_gb = (double)N * (double)(N + 1) / 2.0 * TOKEN / 1e9; /* 重建版总复制 */

    printf("stream accumulate (%d token x %zu B, final %zu B):\n", N, TOKEN, final_len);
    printf("  C   buf append : %8.2f ms   (%.1f ns/token, 分配次数 ~log2(N)=%d)\n",
           c_dt * 1e3, c_dt * 1e9 / N, 14);
    printf("  JVM rebuild    : %8.2f ms   (%.1f ns/token, 分配 %d 次)\n",
           jvm_dt * 1e3, jvm_dt * 1e9 / N, N);
    printf("  total copied   : C  ~%.2f MB (memcpy 实际写入)  vs  JVM 理论 %.1f GB (O(n^2))\n",
           (double)N * TOKEN / 1e6, total_copy_gb);
    printf("  speedup        : %.0fx\n", jvm_dt / (c_dt > 1e-9 ? c_dt : 1e-9));
    return 0;
}
