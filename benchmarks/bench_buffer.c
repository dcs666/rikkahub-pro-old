/*
 * M0 基准：Buf 流式累积（模拟每 token append + 周期 reset 复用）。
 * 输出：吞吐 MB/s 与 每 op 纳秒，作为后续流式管线优化的基线。
 */
#define _POSIX_C_SOURCE 200809L
#include "rikka/core/buffer.h"
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
    const size_t token_len = 32;   /* 模拟一个 token 的文本长度 */
    const size_t tokens = 1u << 20; /* 100 万 token */
    const size_t tokens_per_flush = 256; /* 每 256 token reset 一次复用（模拟 32ms 帧） */

    char token[token_len];
    memset(token, 'x', sizeof(token));

    Buf b;
    buf_init(&b);

    double t0 = now_sec();
    size_t written = 0;
    for (size_t i = 0; i < tokens; i++) {
        buf_append(&b, token, sizeof(token));
        written += sizeof(token);
        if ((i + 1) % tokens_per_flush == 0) {
            /* 模拟消费：读走长度后 reset 复用容量 */
            buf_reset(&b);
        }
    }
    double t1 = now_sec();
    double dt = t1 - t0;
    double mb = (double)written / (1024.0 * 1024.0);
    printf("buf_append (token=%zuB x %zu, flush every %zu):\n",
           token_len, tokens, tokens_per_flush);
    printf("  total written : %.1f MB\n", mb);
    printf("  elapsed       : %.3f s\n", dt);
    printf("  throughput    : %.1f MB/s\n", mb / dt);
    printf("  per-append    : %.1f ns\n", dt * 1e9 / (double)tokens);

    buf_free(&b);
    return 0;
}
