/*
 * M5 基准：会话序列化/反序列化 + 索引。
 * 对标 JVM 版：打开 1 万消息会话 = Room 读 + kotlinx JSON 全量反序列化（几百 ms）。
 */
#define _POSIX_C_SOURCE 200809L
#include "rikka/data/rbin.h"
#include "rikka/data/index.h"
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

static RikkaMessage *mk_msg(Arena *a, int i) {
    RikkaMessage *m = rmsg_new(a, (RikkaRole)(i % 2));
    RikkaPart *p = rmsg_add_part(a, m, RIKKA_PART_TEXT);
    char buf[128];
    snprintf(buf, sizeof(buf), "user message %d with some content words %d", i, i % 100);
    char *d = (char *)arena_alloc(a, 1, strlen(buf) + 1);
    strcpy(d, buf);
    p->data = d;
    p->len = strlen(d);
    return m;
}

int main(void) {
    const int N = 10000;
    Arena *a = arena_create(0);
    RConversation c;
    rconv_init(&c, a);
    for (int i = 0; i < N; i++) rconv_append(&c, mk_msg(a, i));

    Buf bin;
    buf_init(&bin);
    double t0 = now_sec();
    rbin_save(&c, &bin);
    double t1 = now_sec();
    double save_ms = (t1 - t0) * 1e3;

    Arena *a2 = arena_create(0);
    RikkaMessage **msgs = NULL;
    size_t n = 0;
    t0 = now_sec();
    rbin_load(bin.data, bin.len, a2, &msgs, &n);
    t1 = now_sec();
    double load_ms = (t1 - t0) * 1e3;

    RkIndex *ix = rk_index_create();
    t0 = now_sec();
    for (size_t i = 0; i < n; i++)
        rk_index_add(ix, (uint64_t)i, msgs[i]->parts[0].data, msgs[i]->parts[0].len);
    t1 = now_sec();
    double idx_ms = (t1 - t0) * 1e3;

    uint64_t hits[64];
    size_t hn = rk_index_search(ix, "words 42", 8, hits, 64);

    printf("rbin: %d messages, %zu KB binary\n", N, bin.len / 1024);
    printf("  save        : %6.2f ms  (%.0f ns/msg)\n", save_ms, save_ms * 1e6 / N);
    printf("  load        : %6.2f ms  (%.0f ns/msg)  <- 打开长会话延迟 (JVM Room+JSON 几百 ms)\n",
           load_ms, load_ms * 1e6 / N);
    printf("  index build : %6.2f ms  (%zu tokens)\n", idx_ms, rk_index_token_count(ix));
    printf("  search      : %zu hits\n", hn);
    buf_free(&bin);
    arena_destroy(a);
    arena_destroy(a2);
    rk_index_destroy(ix);
    return 0;
}
