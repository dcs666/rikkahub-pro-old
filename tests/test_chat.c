#define _POSIX_C_SOURCE 200809L
#include "test.h"
#include "rikka/ai/chat.h"
#include "rikka/util/arena.h"
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
                             "mock-model", 100, 0, NULL, {0}};

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
    ASSERT_EQ_INT(0, rk_chat_run(&cfg, &cb, msgs, 1, &final_text, &err));
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
                             "mock-model", 100, 0, NULL, {0}};
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
    ASSERT_EQ_INT(0, rk_chat_run(&cfg, &cb, msgs, 1, &final_text, &err));
    ASSERT_EQ_INT(0, rec.tool_calls);
    ASSERT_NOT_NULL(final_text);
    /* mock /openai 回放内容（Hello ... world） */
    ASSERT(strlen(final_text) > 0);
    free(final_text);
    free(err);
    arena_destroy(a);
    stop_mock_server();
}

int run_chat_suite(void) {
    const RikkaTest tests[] = {
        RIKKA_TEST_REGISTER(chat, chat_tool_loop),
        RIKKA_TEST_REGISTER(chat, chat_plain_stream),
    };
    return run_suite("chat", tests, sizeof(tests) / sizeof(tests[0]));
}
