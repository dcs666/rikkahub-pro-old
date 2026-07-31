#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "test.h"
#include "rikka/pipe/spsc.h"
#include "rikka/http/sse.h"
#include "rikka/json/json.h"
#include "rikka/core/message.h"
#include "rikka/util/arena.h"
#include <pthread.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void msleep(long ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

/* ---------- SPSC 基本 ---------- */

TEST(spsc_order) {
    RkSpsc q;
    rk_spsc_init(&q, 4096);
    char out[64];
    const char *a = "alpha", *b = "beta";
    ASSERT_EQ_INT(0, rk_spsc_push(&q, a, 5));
    ASSERT_EQ_INT(0, rk_spsc_push(&q, b, 4));
    ssize_t n = rk_spsc_pop(&q, out, sizeof(out));
    ASSERT_EQ_SIZE(5, n);
    ASSERT(memcmp(out, "alpha", 5) == 0);
    n = rk_spsc_pop(&q, out, sizeof(out));
    ASSERT_EQ_SIZE(4, n);
    ASSERT(memcmp(out, "beta", 4) == 0);
    rk_spsc_close(&q);
    ASSERT_EQ_SIZE(0, rk_spsc_pop(&q, out, sizeof(out)));
    rk_spsc_destroy(&q);
}

TEST(spsc_wraparound) {
    /* 环形回绕：数据跨边界 */
    RkSpsc q;
    rk_spsc_init(&q, 64);
    char out[128];
    char big[40];
    memset(big, 'A', sizeof(big));
    /* 填满接近边界，再推大块触发回绕 */
    for (int i = 0; i < 3; i++) rk_spsc_push(&q, "x", 1);
    rk_spsc_push(&q, big, sizeof(big));
    rk_spsc_push(&q, big, sizeof(big));
    /* 消费所有 */
    int seen = 0;
    while (1) {
        ssize_t n = rk_spsc_pop(&q, out, sizeof(out));
        if (n <= 0) break;
        if (n == 40) { int allA = 1; for (int i = 0; i < 40; i++) if (out[i] != 'A') allA = 0; ASSERT(allA); seen++; }
        if (n == 1) seen += 100;
    }
    ASSERT(seen >= 200); /* 2 个大块 + 3 个单字节 */
    rk_spsc_destroy(&q);
}

TEST(spsc_full) {
    RkSpsc q;
    rk_spsc_init(&q, 16);
    char d[4] = "123";
    /* 16 字节环形：可容纳 ~2 个块（4+3=7 字节/块） */
    int ok = 0, full = 0;
    for (int i = 0; i < 100; i++) {
        if (rk_spsc_push(&q, d, 3) == 0) ok++;
        else { full = 1; break; }
    }
    ASSERT(ok >= 1);
    ASSERT(full); /* 容量有限必然满 */
    rk_spsc_destroy(&q);
}

/* ---------- 端到端流水线 ---------- */

static const RJsonStreamPathElem PATH_CONTENT[] = {
    {0, {.key = "choices"}}, {1, {.index = 0}}, {0, {.key = "delta"}},
    {0, {.key = "content"}}, {-2, {0}},
};

typedef struct {
    RkSpsc *q;
    int n_events;
} ProdCtx;

static void *producer(void *v) {
    ProdCtx *pc = (ProdCtx *)v;
    for (int i = 0; i < pc->n_events; i++) {
        char ev[128];
        snprintf(ev, sizeof(ev),
                 "data: {\"choices\":[{\"delta\":{\"content\":\"token%d \"}}]}\n\n", i);
        while (rk_spsc_push(pc->q, ev, strlen(ev)) != 0) msleep(1);
    }
    rk_spsc_close(pc->q);
    return NULL;
}

typedef struct {
    RkSpsc *q;
    RikkaStream *out;
    int done;
} ConsCtx;

static void sink_accum(void *ctx, const char *data, size_t len) {
    RikkaStream *s = (RikkaStream *)ctx;
    rstream_append_text(s, data, len);
}

static void cons_event(void *ctx, const char *event, const char *data, size_t len,
                       const char *id, long long retry) {
    (void)event; (void)id; (void)retry;
    ConsCtx *cc = (ConsCtx *)ctx;
    /* 简化：每事件重建流（真实场景复用 session 级 stream） */
    RJsonStream *st = rjson_stream_create(PATH_CONTENT, sink_accum, cc->out);
    rjson_stream_feed(st, data, len);
    rjson_stream_finish(st);
    rjson_stream_destroy(st);
}

static void *consumer(void *v) {
    ConsCtx *cc = (ConsCtx *)v;
    RsseParser *sse = rsse_create(cons_event, cc);
    char buf[4096];
    for (;;) {
        ssize_t n = rk_spsc_pop(cc->q, buf, sizeof(buf));
        if (n == 0) break;
        if (n < 0) { msleep(1); continue; }
        rsse_feed(sse, buf, (size_t)n);
    }
    rsse_finish(sse);
    rsse_destroy(sse);
    cc->done = 1;
    return NULL;
}

TEST(spsc_wraparound_stress) {
    /* 小块反复 push/pop 触发环形回绕，验证数据完整性 */
    RkSpsc q;
    rk_spsc_init(&q, 256); /* 小容量强制回绕 */
    char out[64];
    for (int round = 0; round < 1000; round++) {
        char item[20];
        snprintf(item, sizeof(item), "msg-%d", round);
        ASSERT_EQ_INT(0, rk_spsc_push(&q, item, strlen(item)));
        ssize_t n = rk_spsc_pop(&q, out, sizeof(out));
        ASSERT_EQ_SIZE(strlen(item), n);
        ASSERT(memcmp(out, item, n) == 0);
    }
    rk_spsc_destroy(&q);
}

TEST(spsc_empty_block_rejected) {
    RkSpsc q;
    rk_spsc_init(&q, 64);
    char out[16];
    ASSERT_EQ_INT(-1, rk_spsc_push(&q, "x", 0)); /* 空块拒绝 */
    ASSERT_EQ_INT(-1, rk_spsc_pop(&q, out, sizeof(out))); /* 队列空 */
    rk_spsc_destroy(&q);
}

TEST(spsc_single_byte) {
    RkSpsc q;
    rk_spsc_init(&q, 64);
    char out[16];
    char b = 'Z';
    ASSERT_EQ_INT(0, rk_spsc_push(&q, &b, 1));
    ssize_t n = rk_spsc_pop(&q, out, sizeof(out));
    ASSERT_EQ_SIZE(1, n);
    ASSERT(out[0] == 'Z');
    rk_spsc_destroy(&q);
}

TEST(spsc_max_block_boundary) {
    /* 块大小 = cap-4（长度前缀后恰好填满） */
    RkSpsc q;
    rk_spsc_init(&q, 128); /* cap 取 2 的幂 = 128 */
    char big[124];
    memset(big, 'B', sizeof(big));
    char out[256];
    ASSERT_EQ_INT(0, rk_spsc_push(&q, big, sizeof(big)));
    ssize_t n = rk_spsc_pop(&q, out, sizeof(out));
    ASSERT_EQ_SIZE(124, n);
    ASSERT(memcmp(out, big, 124) == 0);
    /* 超 cap 的块拒绝 */
    char huge[256];
    ASSERT_EQ_INT(-1, rk_spsc_push(&q, huge, sizeof(huge)));
    rk_spsc_destroy(&q);
}

TEST(pipeline_sse_to_stream) {
    const int N = 500;
    RkSpsc q;
    rk_spsc_init(&q, 1 << 16);

    Arena *a = arena_create(0);
    RikkaStream out;
    rstream_init(&out, a, RIKKA_ROLE_ASSISTANT);

    ProdCtx pc = {&q, N};
    ConsCtx cc = {&q, &out, 0};
    pthread_t pt, ct;
    pthread_create(&pt, NULL, producer, &pc);
    pthread_create(&ct, NULL, consumer, &cc);
    pthread_join(pt, NULL);
    pthread_join(ct, NULL);

    /* 验证累积内容：token0 token1 ... tokenN-1 */
    char expect[8192];
    expect[0] = '\0';
    size_t off = 0;
    for (int i = 0; i < N; i++) {
        int k = snprintf(expect + off, sizeof(expect) - off, "token%d ", i);
        off += (size_t)k;
    }
    ASSERT_EQ_SIZE(off, out.msg->parts[0].len);
    ASSERT(memcmp(out.msg->parts[0].data, expect, off) == 0);

    rstream_destroy(&out);
    arena_destroy(a);
    rk_spsc_destroy(&q);
}

int run_pipe_suite(void) {
    const RikkaTest tests[] = {
        RIKKA_TEST_REGISTER(pipe, spsc_order),
        RIKKA_TEST_REGISTER(pipe, spsc_wraparound),
        RIKKA_TEST_REGISTER(pipe, spsc_full),
        RIKKA_TEST_REGISTER(pipe, spsc_single_byte),
        RIKKA_TEST_REGISTER(pipe, spsc_empty_block_rejected),
        RIKKA_TEST_REGISTER(pipe, spsc_wraparound_stress),
        RIKKA_TEST_REGISTER(pipe, spsc_max_block_boundary),
        RIKKA_TEST_REGISTER(pipe, pipeline_sse_to_stream),
    };
    return run_suite("pipe", tests, sizeof(tests) / sizeof(tests[0]));
}
