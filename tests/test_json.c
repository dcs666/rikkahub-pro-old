#include "test.h"
#include "rikka/json/json.h"
#include "rikka/util/arena.h"
#include "rikka/core/buffer.h"
#include <string.h>

/* ---------- 值解析 ---------- */

TEST(parse_basic) {
    Arena *a = arena_create(0);
    const char *txt = "{\"a\": 1, \"b\": [true, false, null], \"c\": \"hi\", \"d\": 3.5e2}";
    size_t err = 0;
    RJson *v = rjson_parse(a, txt, strlen(txt), &err);
    ASSERT_NOT_NULL(v);
    ASSERT(rjson_is(v, RJSON_OBJECT));
    const RJson *a_ = rjson_obj_get(v, "a");
    ASSERT(rjson_is(a_, RJSON_NUMBER));
    ASSERT(a_->u.number == 1.0);
    const RJson *b = rjson_obj_get(v, "b");
    ASSERT(rjson_is(b, RJSON_ARRAY));
    ASSERT_EQ_SIZE(3, b->u.arr.count);
    ASSERT(rjson_is(rjson_arr_at(b, 0), RJSON_BOOL));
    ASSERT(rjson_arr_at(b, 0)->u.boolean == 1);
    ASSERT(rjson_is(rjson_arr_at(b, 1), RJSON_BOOL));
    ASSERT(rjson_arr_at(b, 1)->u.boolean == 0);
    ASSERT(rjson_is(rjson_arr_at(b, 2), RJSON_NULL));
    const RJson *c = rjson_obj_get(v, "c");
    size_t clen = 0;
    const char *cs = rjson_str(c, &clen);
    ASSERT_NOT_NULL(cs);
    ASSERT_EQ_SIZE(2, clen);
    ASSERT(memcmp(cs, "hi", 2) == 0);
    const RJson *d = rjson_obj_get(v, "d");
    ASSERT(d->u.number == 350.0);
    arena_destroy(a);
}

TEST(parse_escapes) {
    Arena *a = arena_create(0);
    const char *txt = "{\"s\": \"a\\n\\\"b\\\\c\\u4e2d\\uD83D\\uDE00\"}";
    size_t err = 0;
    RJson *v = rjson_parse(a, txt, strlen(txt), &err);
    ASSERT_NOT_NULL(v);
    const RJson *s = rjson_obj_get(v, "s");
    size_t len = 0;
    const char *str = rjson_str(s, &len);
    ASSERT_NOT_NULL(str);
    /* "a\n\"b\\c中😀" */
    const char *expect = "a\n\"b\\c\xe4\xb8\xad\xf0\x9f\x98\x80";
    ASSERT_EQ_SIZE(strlen(expect), len);
    ASSERT(memcmp(str, expect, len) == 0);
    arena_destroy(a);
}

TEST(parse_errors) {
    Arena *a = arena_create(0);
    const char *bad1 = "{\"a\": }";
    const char *bad2 = "[1, 2";
    const char *bad3 = "tru";
    const char *bad4 = "{\"a\":1} junk";
    size_t err = 0;
    ASSERT_NULL(rjson_parse(a, bad1, strlen(bad1), &err));
    ASSERT_NULL(rjson_parse(a, bad2, strlen(bad2), &err));
    ASSERT_NULL(rjson_parse(a, bad3, strlen(bad3), &err));
    ASSERT_NULL(rjson_parse(a, bad4, strlen(bad4), &err));
    arena_destroy(a);
}

TEST(parse_nested) {
    Arena *a = arena_create(0);
    const char *txt = "[[1,[2,3]],{\"x\":{\"y\":[null]}}]";
    size_t err = 0;
    RJson *v = rjson_parse(a, txt, strlen(txt), &err);
    ASSERT_NOT_NULL(v);
    const RJson *inner = rjson_arr_at(rjson_arr_at(v, 0), 1);
    ASSERT(rjson_is(inner, RJSON_ARRAY));
    ASSERT(rjson_arr_at(inner, 0)->u.number == 2.0);
    const RJson *x = rjson_obj_get(rjson_arr_at(v, 1), "x");
    const RJson *y = rjson_obj_get(x, "y");
    ASSERT(rjson_is(rjson_arr_at(y, 0), RJSON_NULL));
    arena_destroy(a);
}

/* ---------- 序列化 ---------- */

TEST(serialize_roundtrip) {
    Arena *a = arena_create(0);
    const char *txt = "{\"a\":1,\"b\":[1.5,true,null],\"c\":\"x\\ny\"}";
    size_t err = 0;
    RJson *v = rjson_parse(a, txt, strlen(txt), &err);
    ASSERT_NOT_NULL(v);
    RJsonOut o;
    rjson_out_init(&o);
    rjson_write_value(&o, v);
    /* 再解析输出应得到同样结构 */
    Arena *a2 = arena_create(0);
    RJson *v2 = rjson_parse(a2, o.buf, o.len, &err);
    ASSERT_NOT_NULL(v2);
    const RJson *c2 = rjson_obj_get(v2, "c");
    size_t len = 0;
    const char *s2 = rjson_str(c2, &len);
    ASSERT_EQ_SIZE(3, len);
    ASSERT(memcmp(s2, "x\ny", 3) == 0);
    rjson_out_free(&o);
    arena_destroy(a);
    arena_destroy(a2);
}

/* ---------- 增量流式提取 ---------- */

static const RJsonStreamPathElem path_choices_content[] = {
    {0, {.key = "choices"}},
    {1, {.index = 0}},
    {0, {.key = "delta"}},
    {0, {.key = "content"}},
    {-2, {0}},
};

/* sink 累积器 */
typedef struct { Buf b; } SinkCtx;
static void sink_cb(void *ctx, const char *data, size_t len) {
    SinkCtx *s = (SinkCtx *)ctx;
    buf_append(&s->b, data, len);
}

static void feed_all(RJsonStream *st, const char *txt) {
    size_t n = strlen(txt);
    RJsonStreamStatus sts = rjson_stream_feed(st, txt, n);
    ASSERT(sts == RJSON_STREAM_OK || sts == RJSON_STREAM_DONE);
}

TEST(stream_extract_content) {
    const char *ev = "{\"id\":\"x\",\"choices\":[{\"index\":0,\"delta\":{\"content\":\"Hello\"},\"finish_reason\":null}]}";
    SinkCtx ctx; buf_init(&ctx.b);
    RJsonStream *st = rjson_stream_create(path_choices_content, sink_cb, &ctx);
    feed_all(st, ev);
    ASSERT(rjson_stream_hit(st));
    RJsonStreamStatus sts = rjson_stream_finish(st);
    ASSERT(sts == RJSON_STREAM_DONE);
    ASSERT_EQ_SIZE(5, ctx.b.len);
    ASSERT(memcmp(ctx.b.data, "Hello", 5) == 0);
    buf_free(&ctx.b);
    rjson_stream_destroy(st);
}

TEST(stream_fragmented_bytes) {
    /* 1 字节分片 + 转义/unicode 跨分片 */
    const char *ev = "{\"choices\":[{\"delta\":{\"content\":\"a\\n\\u4e2d\\uD83D\\uDE00b\"}}]}";
    SinkCtx ctx; buf_init(&ctx.b);
    RJsonStream *st = rjson_stream_create(path_choices_content, sink_cb, &ctx);
    size_t n = strlen(ev);
    RJsonStreamStatus sts = RJSON_STREAM_OK;
    for (size_t i = 0; i < n && sts == RJSON_STREAM_OK; i++) {
        sts = rjson_stream_feed(st, ev + i, 1);
    }
    ASSERT(sts == RJSON_STREAM_OK || sts == RJSON_STREAM_DONE);
    ASSERT(rjson_stream_hit(st));
    const char *expect = "a\n\xe4\xb8\xad\xf0\x9f\x98\x80" "b";
    ASSERT_EQ_SIZE(strlen(expect), ctx.b.len);
    ASSERT(memcmp(ctx.b.data, expect, ctx.b.len) == 0);
    buf_free(&ctx.b);
    rjson_stream_destroy(st);
}

TEST(stream_fragmented_escape_split) {
    /* 转义序列本身跨分片：\\n 拆成 '\\' 和 'n' 两次喂入 */
    const char *ev = "{\"choices\":[{\"delta\":{\"content\":\"ab\\nc\"}}]}";
    SinkCtx ctx; buf_init(&ctx.b);
    RJsonStream *st = rjson_stream_create(path_choices_content, sink_cb, &ctx);
    const char *prefix = "{\"choices\":[{\"delta\":{\"content\":\"ab\\";
    rjson_stream_feed(st, prefix, strlen(prefix)); /* 喂到 '\' 为止 */
    rjson_stream_feed(st, "n", 1);
    rjson_stream_feed(st, "c\"}}]}", 6);
    RJsonStreamStatus sts = rjson_stream_finish(st);
    ASSERT(sts == RJSON_STREAM_DONE);
    ASSERT_EQ_SIZE(4, ctx.b.len);
    ASSERT(memcmp(ctx.b.data, "ab\nc", 4) == 0);
    buf_free(&ctx.b);
    rjson_stream_destroy(st);
}

TEST(stream_capture_raw_object) {
    /* 目标是容器：choices[0].delta 应输出整棵子树原始字节 */
    static const RJsonStreamPathElem path_delta[] = {
        {0, {.key = "choices"}},
        {1, {.index = 0}},
        {0, {.key = "delta"}},
        {-2, {0}},
    };
    const char *ev = "{\"choices\":[{\"delta\":{\"content\":\"Hi\",\"role\":\"assistant\"}}]}";
    SinkCtx ctx; buf_init(&ctx.b);
    RJsonStream *st = rjson_stream_create(path_delta, sink_cb, &ctx);
    feed_all(st, ev);
    ASSERT(rjson_stream_hit(st));
    ASSERT(rjson_stream_finish(st) == RJSON_STREAM_DONE);
    const char *expect = "{\"content\":\"Hi\",\"role\":\"assistant\"}";
    ASSERT_EQ_SIZE(strlen(expect), ctx.b.len);
    ASSERT(memcmp(ctx.b.data, expect, ctx.b.len) == 0);
    buf_free(&ctx.b);
    rjson_stream_destroy(st);
}

TEST(stream_no_hit) {
    const char *ev = "{\"other\":[{\"x\":1}]}";
    SinkCtx ctx; buf_init(&ctx.b);
    RJsonStream *st = rjson_stream_create(path_choices_content, sink_cb, &ctx);
    feed_all(st, ev);
    ASSERT(!rjson_stream_hit(st));
    ASSERT_EQ_SIZE(0, ctx.b.len);
    ASSERT(rjson_stream_finish(st) == RJSON_STREAM_DONE);
    buf_free(&ctx.b);
    rjson_stream_destroy(st);
}

TEST(stream_multi_events) {
    /* SSE 每事件一个 JSON：连续喂 3 个事件，各自新建 stream */
    const char *events[] = {
        "{\"choices\":[{\"delta\":{\"content\":\"Hel\"}}]}",
        "{\"choices\":[{\"delta\":{\"content\":\"lo \"}}]}",
        "{\"choices\":[{\"delta\":{\"content\":\"world\"}}]}",
    };
    SinkCtx ctx; buf_init(&ctx.b);
    for (int i = 0; i < 3; i++) {
        RJsonStream *st = rjson_stream_create(path_choices_content, sink_cb, &ctx);
        feed_all(st, events[i]);
        ASSERT(rjson_stream_hit(st));
        ASSERT(rjson_stream_finish(st) == RJSON_STREAM_DONE);
        rjson_stream_destroy(st);
    }
    ASSERT_EQ_SIZE(11, ctx.b.len);
    ASSERT(memcmp(ctx.b.data, "Hello world", 11) == 0);
    buf_free(&ctx.b);
    rjson_stream_destroy(NULL); /* 空指针安全 */
}

TEST(stream_array_index_mismatch) {
    /* 目标 choices[1] 而非 [0]：不应命中 */
    static const RJsonStreamPathElem path_second[] = {
        {0, {.key = "choices"}},
        {1, {.index = 1}},
        {0, {.key = "delta"}},
        {0, {.key = "content"}},
        {-2, {0}},
    };
    const char *ev = "{\"choices\":[{\"delta\":{\"content\":\"first\"}},{\"delta\":{\"content\":\"second\"}}]}";
    SinkCtx ctx; buf_init(&ctx.b);
    RJsonStream *st = rjson_stream_create(path_second, sink_cb, &ctx);
    feed_all(st, ev);
    ASSERT(rjson_stream_hit(st));
    ASSERT_EQ_SIZE(6, ctx.b.len);
    ASSERT(memcmp(ctx.b.data, "second", 6) == 0);
    buf_free(&ctx.b);
    rjson_stream_destroy(st);
}

TEST(stream_whitespace_and_finish) {
    /* 空白 + finish 在中间调用 */
    const char *ev = "  {\"choices\":[{\"delta\":{\"content\":\"ok\"}}]}  ";
    SinkCtx ctx; buf_init(&ctx.b);
    RJsonStream *st = rjson_stream_create(path_choices_content, sink_cb, &ctx);
    rjson_stream_feed(st, ev, strlen(ev));
    ASSERT(rjson_stream_finish(st) == RJSON_STREAM_DONE);
    ASSERT_EQ_SIZE(2, ctx.b.len);
    buf_free(&ctx.b);
    rjson_stream_destroy(st);
}

int run_json_suite(void) {
    const RikkaTest tests[] = {
        RIKKA_TEST_REGISTER(json, parse_basic),
        RIKKA_TEST_REGISTER(json, parse_escapes),
        RIKKA_TEST_REGISTER(json, parse_errors),
        RIKKA_TEST_REGISTER(json, parse_nested),
        RIKKA_TEST_REGISTER(json, serialize_roundtrip),
        RIKKA_TEST_REGISTER(json, stream_extract_content),
        RIKKA_TEST_REGISTER(json, stream_fragmented_bytes),
        RIKKA_TEST_REGISTER(json, stream_fragmented_escape_split),
        RIKKA_TEST_REGISTER(json, stream_capture_raw_object),
        RIKKA_TEST_REGISTER(json, stream_no_hit),
        RIKKA_TEST_REGISTER(json, stream_multi_events),
        RIKKA_TEST_REGISTER(json, stream_array_index_mismatch),
        RIKKA_TEST_REGISTER(json, stream_whitespace_and_finish),
    };
    return run_suite("json", tests, sizeof(tests) / sizeof(tests[0]));
}
