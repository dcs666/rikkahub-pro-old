/*
 * P3 基准：SSE 解析器吞吐。
 * 模拟真实 MCP/OpenAI 事件流：1400B 分片（TCP MSS）喂入。
 */
#define _POSIX_C_SOURCE 200809L
#include "rikka/http/sse.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void cb_ignore(void *ctx, const char *ev, const char *d, size_t n,
                      const char *id, long long retry) {
    (void)ctx; (void)ev; (void)d; (void)n; (void)id; (void)retry;
}

int main(void) {
    const char *ev =
        "event: message\ndata: {\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"ok\":true}}\n\n";
    size_t evlen = strlen(ev);
    const int N = 50000;
    size_t total = evlen * (size_t)N;
    char *buf = (char *)malloc(total);
    if (!buf) return 1;
    for (int i = 0; i < N; i++) memcpy(buf + (size_t)i * evlen, ev, evlen);

    RsseParser *p = rsse_create(cb_ignore, NULL);
    size_t off = 0;
    double t0 = now_sec();
    while (off < total) {
        size_t n = total - off < 1400 ? total - off : 1400;
        if (rsse_feed(p, buf + off, n) != 0) return 2;
        off += n;
    }
    rsse_finish(p);
    double t1 = now_sec();
    double dt = t1 - t0;
    rsse_destroy(p);
    free(buf);

    printf("SSE parser (%d events, %.2f MB, 1400B 分片):\n", N, total / 1e6);
    printf("  elapsed    : %8.2f ms\n", dt * 1e3);
    printf("  throughput : %8.2f M events/s\n", (double)N / dt / 1e6);
    printf("  parsed     : %8.2f MB/s\n", (double)total / dt / 1e6);
    return 0;
}
