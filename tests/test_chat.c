#define _POSIX_C_SOURCE 200809L
#include "test.h"
#include "rikka/ai/chat.h"
#include "rikka/util/arena.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_server.h"

static RikkaMessage *mk_msg(Arena *a, RikkaRole role, const char *text) {
    RikkaMessage *m = rmsg_new(a, role);
    RikkaPart *p = rmsg_add_part(a, m, RIKKA_PART_TEXT);
    p->data = text;
    p->len = strlen(text);
    return m;
}

/* ---------- 工具循环端到端（mock /toolcall 回放） ---------- */

typedef struct {
    int deltas;
    int tool_calls;
    int tool_results;
    char delta_text[256];
    char called_name[64];
} ChatRec;

static void on_delta(void *ud, int kind, const char *data, size_t len) {
    ChatRec *r = (ChatRec *)ud;
    if (kind == 0) {
        r->deltas++;
        size_t n = len < sizeof(r->delta_text) - 1 - strlen(r->delta_text)
                       ? len
                       : sizeof(r->delta_text) - 1 - strlen(r->delta_text);
        memcpy(r->delta_text + strlen(r->delta_text), data, n);
        r->delta_text[strlen(r->delta_text) + n] = '\0';
    }
}

static void on_tool_call(void *ud, const char *name, const char *args_json) {
    ChatRec *r = (ChatRec *)ud;
    r->tool_calls++;
    snprintf(r->called_name, sizeof(r->called_name), "%s", name);
    (void)args_json;
}

static void on_tool_result(void *ud, const char *name, const char *result_json) {
    ChatRec *r = (ChatRec *)ud;
    r->tool_results++;
    (void)name;
    (void)result_json;
}

TEST(chat_tool_loop) {
    if (getenv("CI")) {
        printf("  [skip: CI environment]\n");
        return;
    }
    start_mock_server();
    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d", g_port);
    RikkaProviderCfg pcfg = {RIKKA_PROVIDER_OPENAI, base, "test-key",
                             "mock-model", 100, 0, NULL, {0}, NULL, 0};

    /* 工具集：get_time_info 可用 */
    RkToolRegistry reg;
    rk_tools_init(&reg);
    RkToolEnv env = {0};
    rk_tools_register_builtin(&reg, &env);

    RkChatConfig cfg = {0};
    cfg.provider = pcfg;
    cfg.tools = &reg;
    cfg.tool_env = &env;
    cfg.timeout_ms = 15000;

    Arena *a = arena_create(0);
    const RikkaMessage *msgs[1];
    msgs[0] = mk_msg(a, RIKKA_ROLE_USER, "what time is it");
    RkChatCallbacks cb = {0};
    ChatRec rec = {0};
    cb.ud = &rec;
    cb.on_delta = on_delta;
    cb.on_tool_call = on_tool_call;
    cb.on_tool_result = on_tool_result;

    char *final_text = NULL, *err = NULL;
    ASSERT_EQ_INT(0, rk_chat_run(&cfg, &cb, msgs, 1, &final_text, &err, NULL));
    /* 第一轮请求工具 → 第二轮出最终文本 */
    ASSERT_EQ_INT(1, rec.tool_calls);
    ASSERT_EQ_INT(1, rec.tool_results);
    ASSERT(strcmp(rec.called_name, "get_time_info") == 0);
    ASSERT_NOT_NULL(final_text);
    ASSERT(strstr(final_text, "Final answer") != NULL);
    /* 增量：第二轮文本流式转发 */
    ASSERT(rec.deltas >= 2);
    ASSERT(strstr(rec.delta_text, "Final") != NULL);
    free(final_text);
    free(err);
    arena_destroy(a);
    rk_tools_destroy(&reg);
    stop_mock_server();
}

/* ---------- 纯文本流（无工具） ---------- */

TEST(chat_plain_stream) {
    if (getenv("CI")) {
        printf("  [skip: CI environment]\n");
        return;
    }
    start_mock_server();
    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d", g_port);
    RikkaProviderCfg pcfg = {RIKKA_PROVIDER_OPENAI, base, "test-key",
                             "mock-model", 100, 0, NULL, {0}, NULL, 0};
    RkChatConfig cfg = {0};
    cfg.provider = pcfg;
    cfg.timeout_ms = 15000;

    Arena *a = arena_create(0);
    const RikkaMessage *msgs[1];
    msgs[0] = mk_msg(a, RIKKA_ROLE_USER, "hello");
    RkChatCallbacks cb = {0};
    ChatRec rec = {0};
    cb.ud = &rec;
    cb.on_delta = on_delta;
    char *final_text = NULL, *err = NULL;
    ASSERT_EQ_INT(0, rk_chat_run(&cfg, &cb, msgs, 1, &final_text, &err, NULL));
    ASSERT_EQ_INT(0, rec.tool_calls);
    ASSERT_NOT_NULL(final_text);
    /* mock /openai 回放内容（Hello ... world） */
    ASSERT(strlen(final_text) > 0);
    free(final_text);
    free(err);
    arena_destroy(a);
    stop_mock_server();
}

/* ---------- 流式取消 ---------- */

static void *cancel_runner(void *v) {
    /* v = {cfg, msgs 数组, cancel_flag, 结果指针} */
    RkChatConfig *cfg = ((void **)v)[0];
    RikkaMessage **msgs = ((void **)v)[1];
    volatile int *cancel = ((void **)v)[2];
    int *result = ((void **)v)[3];
    (void)msgs;
    /* 直接测 provider 层：/slow 慢流 + pump_async_cancel */
    Arena *arena = arena_create(0);
    RikkaStream out;
    rstream_init(&out, arena, RIKKA_ROLE_ASSISTANT);
    Buf body;
    buf_init(&body);
    if (rp_build_request(&cfg->provider, NULL, 0, 1, &body) != 0) {
        *result = -2;
        buf_free(&body);
        rstream_destroy(&out);
        arena_destroy(arena);
        return NULL;
    }
    RikkaStreamSession *ss = rp_session_create(&cfg->provider);
    int status = 0;
    int rc = rp_stream_start(ss, "/slow", (const char *)body.data, body.len,
                             &out, 30000, &status);
    if (rc == 0) {
        rc = rp_stream_pump_async_cancel(ss, 30000, cancel);
    }
    *result = rc;
    rp_session_destroy(ss);
    buf_free(&body);
    rstream_destroy(&out);
    arena_destroy(arena);
    return NULL;
}

TEST(chat_cancel_mid_stream) {
    if (getenv("CI")) {
        printf("  [skip: CI environment]\n");
        return;
    }
    start_mock_server();
    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d", g_port);
    RikkaProviderCfg pcfg = {RIKKA_PROVIDER_OPENAI, base, "test-key",
                             "mock-model", 100, 0, NULL, {0}, NULL, 0};
    RkChatConfig cfg = {0};
    cfg.provider = pcfg;
    cfg.timeout_ms = 30000;

    Arena *a = arena_create(0);
    RikkaMessage *m = mk_msg(a, RIKKA_ROLE_USER, "hello");
    RikkaMessage *msgs[1] = {m};
    (void)a; (void)m;

    volatile int cancel = 0;
    int result = 99;
    void *args[4] = {&cfg, msgs, (void *)&cancel, &result};
    pthread_t th;
    ASSERT_EQ_INT(0, pthread_create(&th, NULL, cancel_runner, args));
    /* 等 800ms（流式进行中）后取消 */
    msleep(800);
    cancel = 1;
    pthread_join(th, NULL);
    /* 取消应让 rk_chat_run 快速返回 -1（< 1.5s 内） */
    ASSERT_EQ_INT(-1, result);
    arena_destroy(a);
    stop_mock_server();
}

/* ---------- 流式 think_tag visual ---------- */

typedef struct {
    char text[512];
    char reason[512];
    size_t text_len, reason_len;
} ThinkRec;

static void think_delta(void *ud, int kind, const char *data, size_t len) {
    ThinkRec *r = (ThinkRec *)ud;
    char *dst = kind == 1 ? r->reason : r->text;
    size_t *dl = kind == 1 ? &r->reason_len : &r->text_len;
    size_t room = 511 - *dl;
    if (room > len) room = len;
    memcpy(dst + *dl, data, room);
    *dl += room;
    dst[*dl] = '\0';
}

TEST(chat_visual_think_tag) {
    RkThinkState st = {0};
    ThinkRec rec = {0};
    /* 标签跨块切分 + 中途误匹配（"<thx" 应输出原文） */
    rk_chat_think_feed(&st, "<th", 3, think_delta, &rec);
    rk_chat_think_feed(&st, "ink>deep rea", 12, think_delta, &rec);
    rk_chat_think_feed(&st, "soning</th", 10, think_delta, &rec);
    rk_chat_think_feed(&st, "ink>Final ", 10, think_delta, &rec);
    rk_chat_think_feed(&st, "answer", 6, think_delta, &rec);
    ASSERT(strcmp(rec.reason, "deep reasoning") == 0);
    ASSERT(strcmp(rec.text, "Final answer") == 0);
    /* 误匹配：未闭合前缀按原文输出 */
    RkThinkState st2 = {0};
    ThinkRec rec2 = {0};
    rk_chat_think_feed(&st2, "<thx", 4, think_delta, &rec2);
    rk_chat_think_feed(&st2, "ello", 4, think_delta, &rec2);
    ASSERT(strcmp(rec2.text, "<thxello") == 0);
    ASSERT(rec2.reason_len == 0);
    /* 流结束未闭合：前缀输出为文本 */
    RkThinkState st3 = {0};
    ThinkRec rec3 = {0};
    rk_chat_think_feed(&st3, "a<thi", 5, think_delta, &rec3);
    rk_chat_think_feed(&st3, "b", 1, think_delta, &rec3);
    ASSERT(strcmp(rec3.text, "a<thib") == 0);
}

int run_chat_suite(void) {
    const RikkaTest tests[] = {
        RIKKA_TEST_REGISTER(chat, chat_tool_loop),
        RIKKA_TEST_REGISTER(chat, chat_plain_stream),
        RIKKA_TEST_REGISTER(chat, chat_cancel_mid_stream),
        RIKKA_TEST_REGISTER(chat, chat_visual_think_tag),
    };
    return run_suite("chat", tests, sizeof(tests) / sizeof(tests[0]));
}
