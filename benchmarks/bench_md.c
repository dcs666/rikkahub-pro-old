/*
 * P3 基准：Markdown 全量解析 vs 增量（流式）。
 * 增量解析器每次 feed 只重解析最后一个块——流式场景每 token 成本
 * 应远低于整篇重解析。
 */
#define _POSIX_C_SOURCE 200809L
#include "rikka/markdown/md.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

int main(void) {
    /* 生成 300 段 markdown（heading + 段落 + 列表 + 引用 + 代码块） */
    char md[256 * 1024];
    size_t off = 0;
    for (int i = 0; i < 300; i++) {
        int n = snprintf(md + off, sizeof(md) - off,
            "# Heading %d\n"
            "Paragraph with **bold** and *italic* and `code` and [link](https://example.com/%d).\n"
            "- item one\n- item two\n- item three\n"
            "> quote line %d\n"
            "```c\nint main(void) { return %d; }\n```\n\n",
            i, i, i, i);
        if (n <= 0) return 1;
        off += (size_t)n;
    }
    size_t mdlen = off;

    /* 全量：30 次完整解析 */
    size_t cnt = 0;
    double t0 = now_sec();
    for (int i = 0; i < 30; i++) {
        RikkaMdBlock *b = rmd_parse_all(md, mdlen, &cnt);
        if (!b) return 2;
        rmd_blocks_free(b, cnt);
    }
    double t1 = now_sec();
    double full_ms = (t1 - t0) * 1e3 / 30;

    /* 增量：模拟流式，每 token 64B 喂入 */
    RikkaMdParser *p = rmd_create();
    size_t fed = 0, tokens = 0;
    t0 = now_sec();
    while (fed < mdlen) {
        size_t n = mdlen - fed < 64 ? mdlen - fed : 64;
        rmd_feed(p, md + fed, n);
        fed += n;
        tokens++;
    }
    const RikkaMdBlock *blks;
    size_t blk_cnt = 0;
    blks = rmd_blocks(p, &blk_cnt);
    if (!blks || blk_cnt == 0) return 3;
    double t2 = now_sec();
    double inc_ms = (t2 - t0) * 1e3;
    rmd_destroy(p);

    printf("markdown (%zu B, %zu blocks):\n", mdlen, cnt);
    printf("  full parse  : %8.2f ms\n", full_ms);
    printf("  incremental : %8.2f ms  (%zu tokens)\n", inc_ms, tokens);
    printf("  per token   : %8.2f us\n", inc_ms * 1e3 / (double)tokens);
    return 0;
}
