#define _POSIX_C_SOURCE 200809L
#include "test.h"
#include "rikka/ai/provider.h"
#include "rikka/json/json.h"
#include "rikka/util/arena.h"
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "test_server.h"

/* 消息构造辅助 */
static RikkaMessage *mk_msg(Arena *a, RikkaRole role, const char *text) {
    RikkaMessage *m = rmsg_new(a, role);
    RikkaPart *p = rmsg_add_part(a, m, RIKKA_PART_TEXT);
    p->data = text;
    p->len = strlen(text);
    return m;
}

static void text_of(const RikkaStream *s, char *out, size_t cap, int reasoning) {
    out[0] = '\0';
    for (size_t i = 0; i < s->msg->part_count; i++) {
        const RikkaPart *p = &s->msg->parts[i];
        if (reasoning ? p->type == RIKKA_PART_REASONING : p->type == RIKKA_PART_TEXT) {
            size_t n = p->len < cap - 1 ? p->len : cap - 1;
            memcpy(out, p->data, n);
            out[n] = '\0';
            return;
        }
    }
}

/* ---------- 请求构建 ---------- */

TEST(build_openai) {
    Arena *a = arena_create(0);
    const RikkaMessage *msgs[4];
    msgs[0] = mk_msg(a, RIKKA_ROLE_SYSTEM, "sys");
    msgs[1] = mk_msg(a, RIKKA_ROLE_USER, "hi");
    /* assistant + tool_call */
    RikkaMessage *asst = rmsg_new(a, RIKKA_ROLE_ASSISTANT);
    RikkaPart *tc = rmsg_add_part(a, asst, RIKKA_PART_TOOL_CALL);
    tc->tool_id = "call_1";
    tc->tool_name = "get_weather";
    tc->data = "{\"city\":\"beijing\"}";
    tc->len = strlen(tc->data);
    msgs[2] = asst;
    /* tool 结果 */
    RikkaMessage *tr = rmsg_new(a, RIKKA_ROLE_TOOL);
    RikkaPart *tp = rmsg_add_part(a, tr, RIKKA_PART_TOOL_RESULT);
    tp->tool_id = "call_1";
    tp->data = "sunny";
    tp->len = 5;
    msgs[3] = tr;

    RikkaProviderCfg cfg = {RIKKA_PROVIDER_OPENAI, "https://api.openai.com/v1", "sk-test", "gpt-4o", 100, 0};
    Buf out;
    buf_init(&out);
    ASSERT_EQ_INT(0, rp_build_request(&cfg, msgs, 4, 1, &out));

    Arena *a2 = arena_create(0);
    size_t err = 0;
    RJson *v = rjson_parse(a2, (const char *)out.data, out.len, &err);
    ASSERT_NOT_NULL(v);
    const RJson *model = rjson_obj_get(v, "model");
    size_t ml = 0;
    const char *ms = rjson_str(model, &ml);
    ASSERT(ms && strncmp(ms, "gpt-4o", 6) == 0);
    const RJson *stream = rjson_obj_get(v, "stream");
    ASSERT(rjson_is(stream, RJSON_BOOL) && stream->u.boolean == 1);
    const RJson *messages = rjson_obj_get(v, "messages");
    ASSERT(rjson_is(messages, RJSON_ARRAY));
    ASSERT_EQ_SIZE(4, messages->u.arr.count);
    /* 第 3 条 assistant 有 tool_calls */
    const RJson *m2 = rjson_arr_at(messages, 2);
    const RJson *tcs = rjson_obj_get(m2, "tool_calls");
    ASSERT_NOT_NULL(tcs);
    const RJson *fn = rjson_obj_get(rjson_arr_at(tcs, 0), "function");
    const RJson *name = rjson_obj_get(fn, "name");
    size_t nl = 0;
    const char *ns = rjson_str(name, &nl);
    ASSERT(ns && strncmp(ns, "get_weather", 11) == 0);
    /* 第 4 条 tool */
    const RJson *m3 = rjson_arr_at(messages, 3);
    const RJson *tid = rjson_obj_get(m3, "tool_call_id");
    const char *ts = rjson_str(tid, &nl);
    ASSERT(ts && strncmp(ts, "call_1", 6) == 0);

    buf_free(&out);
    arena_destroy(a);
    arena_destroy(a2);
}

TEST(build_claude) {
    Arena *a = arena_create(0);
    const RikkaMessage *msgs[3];
    msgs[0] = mk_msg(a, RIKKA_ROLE_SYSTEM, "be concise");
    msgs[1] = mk_msg(a, RIKKA_ROLE_USER, "hello");
    RikkaMessage *asst = rmsg_new(a, RIKKA_ROLE_ASSISTANT);
    RikkaPart *tc = rmsg_add_part(a, asst, RIKKA_PART_TOOL_CALL);
    tc->tool_id = "tu_1";
    tc->tool_name = "search";
    tc->data = "{\"q\":\"x\"}";
    tc->len = strlen(tc->data);
    msgs[2] = asst;

    RikkaProviderCfg cfg = {RIKKA_PROVIDER_CLAUDE, "https://api.anthropic.com", "sk-ant", "claude-3-5-sonnet", 4096, 0};
    Buf out;
    buf_init(&out);
    ASSERT_EQ_INT(0, rp_build_request(&cfg, msgs, 3, 1, &out));

    Arena *a2 = arena_create(0);
    size_t err = 0;
    RJson *v = rjson_parse(a2, (const char *)out.data, out.len, &err);
    ASSERT_NOT_NULL(v);
    /* system 提取为独立字段 */
    const RJson *sys = rjson_obj_get(v, "system");
    ASSERT_NOT_NULL(sys);
    const RJson *msgsj = rjson_obj_get(v, "messages");
    ASSERT(rjson_is(msgsj, RJSON_ARRAY));
    ASSERT_EQ_SIZE(2, msgsj->u.arr.count); /* system 不在 messages 里 */
    /* assistant 有 tool_use block */
    const RJson *m1 = rjson_arr_at(msgsj, 1);
    const RJson *content = rjson_obj_get(m1, "content");
    ASSERT(rjson_is(content, RJSON_ARRAY));
    const RJson *block = rjson_arr_at(content, 0);
    const RJson *type = rjson_obj_get(block, "type");
    size_t tl = 0;
    const char *ts = rjson_str(type, &tl);
    ASSERT(ts && strncmp(ts, "tool_use", 8) == 0);
    const RJson *input = rjson_obj_get(block, "input");
    ASSERT_NOT_NULL(input);
    const RJson *q = rjson_obj_get(input, "q");
    const char *qs = rjson_str(q, &tl);
    ASSERT(qs && strncmp(qs, "x", 1) == 0);

    buf_free(&out);
    arena_destroy(a);
    arena_destroy(a2);
}

TEST(build_google) {
    Arena *a = arena_create(0);
    const RikkaMessage *msgs[2];
    msgs[0] = mk_msg(a, RIKKA_ROLE_SYSTEM, "sys");
    msgs[1] = mk_msg(a, RIKKA_ROLE_USER, "q?");

    RikkaProviderCfg cfg = {RIKKA_PROVIDER_GOOGLE, "https://generativelanguage.googleapis.com", "AIza", "gemini-pro", 0, 0};
    Buf out;
    buf_init(&out);
    ASSERT_EQ_INT(0, rp_build_request(&cfg, msgs, 2, 1, &out));

    Arena *a2 = arena_create(0);
    size_t err = 0;
    RJson *v = rjson_parse(a2, (const char *)out.data, out.len, &err);
    ASSERT_NOT_NULL(v);
    const RJson *si = rjson_obj_get(v, "systemInstruction");
    ASSERT_NOT_NULL(si);
    const RJson *contents = rjson_obj_get(v, "contents");
    ASSERT(rjson_is(contents, RJSON_ARRAY));
    ASSERT_EQ_SIZE(1, contents->u.arr.count);
    const RJson *role = rjson_obj_get(rjson_arr_at(contents, 0), "role");
    size_t rl = 0;
    const char *rs = rjson_str(role, &rl);
    ASSERT(rs && strncmp(rs, "user", 4) == 0);

    buf_free(&out);
    arena_destroy(a);
    arena_destroy(a2);
}

/* ---------- 流式管线（mock 回放） ---------- */

static void run_stream_test(RikkaProviderId id, const char *mock_path,
                            const char *expect_text, const char *expect_reason) {
    start_mock_server();
    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d", g_port);
    RikkaProviderCfg cfg = {id, base, "test-key", "mock-model", 100, 0};

    Arena *a = arena_create(0);
    const RikkaMessage *msgs[1];
    msgs[0] = mk_msg(a, RIKKA_ROLE_USER, "hello");
    RikkaStream out;
    rstream_init(&out, a, RIKKA_ROLE_ASSISTANT);

    RikkaStreamSession *ss = rp_session_create(&cfg);
    ASSERT_NOT_NULL(ss);
    Buf body;
    buf_init(&body);
    ASSERT_EQ_INT(0, rp_build_request(&cfg, msgs, 1, 1, &body));
    int status = 0;
    ASSERT_EQ_INT(0, rp_stream_start(ss, mock_path, (const char *)body.data, body.len, &out, 5000, &status));
    ASSERT_EQ_INT(200, status);
    ASSERT_EQ_INT(0, rp_stream_pump(ss, 3000));
    rstream_freeze(&out);

    const RikkaSessionStats *st = rp_session_stats(ss);
    ASSERT(st->events >= 1); /* Google mock 仅 2 事件；OpenAI/Claude 5 个 */

    char text[512], reason[512];
    text_of(&out, text, sizeof(text), 0);
    text_of(&out, reason, sizeof(reason), 1);
    if (expect_text) ASSERT(strcmp(text, expect_text) == 0);
    if (expect_reason) ASSERT(strcmp(reason, expect_reason) == 0);

    rstream_destroy(&out);
    buf_free(&body);
    rp_session_destroy(ss);
    arena_destroy(a);
    stop_mock_server();
}

TEST(stream_openai) {
    run_stream_test(RIKKA_PROVIDER_OPENAI, "/openai", "Hello world", "think");
}

TEST(stream_claude) {
    run_stream_test(RIKKA_PROVIDER_CLAUDE, "/claude", "Hi there", "hmm");
}

TEST(stream_google) {
    run_stream_test(RIKKA_PROVIDER_GOOGLE, "/google", "Google answer", NULL);
}

TEST(stream_bad_sse_no_deadlock) {
    /* 回归: 畸形 SSE（超长行）触发解析错误时, pump_async 不得死锁 */
    start_mock_server();
    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d", g_port);
    RikkaProviderCfg cfg = {RIKKA_PROVIDER_OPENAI, base, "test-key", "mock-model", 100, 0};
    Arena *a = arena_create(0);
    const RikkaMessage *msgs[1];
    msgs[0] = mk_msg(a, RIKKA_ROLE_USER, "hi");
    RikkaStream out;
    rstream_init(&out, a, RIKKA_ROLE_ASSISTANT);
    RikkaStreamSession *ss = rp_session_create(&cfg);
    ASSERT_NOT_NULL(ss);
    Buf body;
    buf_init(&body);
    rp_build_request(&cfg, msgs, 1, 1, &body);
    int status = 0;
    ASSERT_EQ_INT(0, rp_stream_start(ss, "/sse_bad", (const char *)body.data, body.len, &out, 5000, &status));
    ASSERT_EQ_INT(200, status);
    /* 必须返回（-1 解析错误），不得死锁（超时会暴露） */
    int rc = rp_stream_pump_async(ss, 3000);
    ASSERT_EQ_INT(-1, rc);
    rstream_destroy(&out);
    buf_free(&body);
    rp_session_destroy(ss);
    arena_destroy(a);
    stop_mock_server();
}

int run_provider_suite(void) {
    const RikkaTest tests[] = {
        RIKKA_TEST_REGISTER(provider, build_openai),
        RIKKA_TEST_REGISTER(provider, build_claude),
        RIKKA_TEST_REGISTER(provider, build_google),
        RIKKA_TEST_REGISTER(provider, stream_openai),
        RIKKA_TEST_REGISTER(provider, stream_claude),
        RIKKA_TEST_REGISTER(provider, stream_google),
        RIKKA_TEST_REGISTER(provider, stream_bad_sse_no_deadlock),
    };
    return run_suite("provider", tests, sizeof(tests) / sizeof(tests[0]));
}
