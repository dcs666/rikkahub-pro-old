/*
 * M1 基准：SSE 事件解析 —— 增量流式提取 vs 全量值解析。
 * 模拟 OpenAI chat.completion.chunk 事件流，提取 choices[0].delta.content。
 */
#define _POSIX_C_SOURCE 200809L
#include "rikka/json/json.h"
#include "rikka/util/arena.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static const RJsonStreamPathElem PATH[] = {
    {0, {.key = "choices"}},
    {1, {.index = 0}},
    {0, {.key = "delta"}},
    {0, {.key = "content"}},
    {-2, {0}},
};

static void sink_ignore(void *ctx, const char *d, size_t n) { (void)ctx; (void)d; (void)n; }

int main(void) {
    const char *ev =
        "{\"id\":\"chatcmpl-9\",\"object\":\"chat.completion.chunk\","
        "\"created\":1700000000,\"model\":\"gpt-4o\","
        "\"choices\":[{\"index\":0,\"delta\":{\"content\":\"Hello world, this is a token of streaming text.\"},"
        "\"finish_reason\":null}]}";
    size_t evlen = strlen(ev);
    const int N = 200000;

    /* 全量解析：每次重新 arena + rjson_parse（对齐 kotlinx 每次完整反序列化） */
    double t0 = now_sec();
    for (int i = 0; i < N; i++) {
        Arena *a = arena_create(4096);
        size_t err = 0;
        RJson *v = rjson_parse(a, ev, evlen, &err);
        if (!v) return 1;
        arena_destroy(a);
    }
    double t1 = now_sec();
    double full_dt = t1 - t0;

    /* 增量提取：stream 复用（reset），对齐真实 SSE 事件循环 */
    RJsonStream *st = rjson_stream_create(PATH, sink_ignore, NULL);
    t0 = now_sec();
    for (int i = 0; i < N; i++) {
        rjson_stream_reset(st);
        RJsonStreamStatus s = rjson_stream_feed(st, ev, evlen);
        if (s == RJSON_STREAM_ERROR) return 2;
        rjson_stream_finish(st);
    }
    double t2 = now_sec();
    double inc_dt = t2 - t0;
    rjson_stream_destroy(st);

    printf("SSE event (%zu B):\n", evlen);
    printf("  full parse : %8.1f ns/ev  (%.0f ev/s)  [arena alloc + 全树构建]\n",
           full_dt * 1e9 / N, N / full_dt);
    printf("  inc extract: %8.1f ns/ev  (%.0f ev/s)  [仅 delta.content 提取]\n",
           inc_dt * 1e9 / N, N / inc_dt);
    printf("  speedup    : %.1fx\n", full_dt / inc_dt);
    return 0;
}
