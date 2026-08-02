#define _DEFAULT_SOURCE
#include "test.h"
#include "rikka/ai/tool.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---------- 记忆回调（测试桩） ---------- */

/* 失败调用 + 释放 result（契约：失败时 result 可能被设为错误 JSON） */
#define TOOL_FAIL(call_expr, result_ptr) do { \
    ASSERT_EQ_INT(-1, (call_expr));          \
    free(result_ptr);                        \
    result_ptr = NULL;                       \
} while (0)

static char *stub_memory_create(const char *content, void *ud) {
    (void)ud;
    char *r = (char *)malloc(128);
    snprintf(r, 128, "{\"id\":1,\"content\":\"%s\"}", content);
    return r;
}

static char *stub_memory_edit(int64_t id, const char *content, void *ud) {
    (void)ud;
    char *r = (char *)malloc(128);
    snprintf(r, 128, "{\"id\":%lld,\"content\":\"%s\"}", (long long)id, content);
    return r;
}

static int stub_memory_delete(int64_t id, void *ud) {
    (void)ud;
    return id > 0 ? 0 : -1;
}

TEST(tool_registry) {
    RkToolRegistry r;
    rk_tools_init(&r);
    RkToolEnv env = {0};
    rk_tools_register_builtin(&r, &env);
    ASSERT(rk_tools_count(&r) >= 3); /* time + memory + skill */
    ASSERT_NOT_NULL(rk_tools_find(&r, "get_time_info"));
    ASSERT_NOT_NULL(rk_tools_find(&r, "memory_tool"));
    ASSERT_NOT_NULL(rk_tools_find(&r, "use_skill"));
    ASSERT_NULL(rk_tools_find(&r, "workspace_read_file")); /* 无 root 不注册 */
    /* 重名拒绝 */
    ASSERT_EQ_INT(-1, rk_tools_add(&r, rk_tools_find(&r, "get_time_info")));
    rk_tools_destroy(&r);
}

static char *stub_ask_user(const char *questions, void *ud) {
    (void)ud;
    return strdup(questions); /* 原样返回，验证透传内容 */
}

static char *captured_ask = NULL;
static char *stub_ask_capture(const char *questions, void *ud) {
    (void)ud;
    free(captured_ask);
    captured_ask = strdup(questions);
    return strdup("{\"ok\":true}");
}

TEST(tool_ask_user_passthrough) {
    RkToolRegistry r;
    rk_tools_init(&r);
    RkToolEnv env = {0};
    env.ask_user = stub_ask_capture;
    rk_tools_register_builtin(&r, &env);
    const RkTool *t = rk_tools_find(&r, "ask_user");
    ASSERT_NOT_NULL(t);
    char *res = NULL;
    /* 完整 questions 数组透传 */
    const char *qs = "{\"questions\":[{\"id\":\"a\",\"question\":\"Q1\",\"options\":[\"x\",\"y\"]},"
                     "{\"id\":\"b\",\"question\":\"Q2\"}]}";
    ASSERT_EQ_INT(0, rk_tool_call(t, qs, &env, &res));
    ASSERT_NOT_NULL(res);
    ASSERT(strstr(res, "ok") != NULL); /* 反调返回值原样透传 */
    free(res);
    ASSERT_NOT_NULL(captured_ask);
    ASSERT(strstr(captured_ask, "Q1") != NULL && strstr(captured_ask, "Q2") != NULL);
    ASSERT(strstr(captured_ask, "\"options\"") != NULL);
    /* 旧单 question 字段 → 包装为 questions 数组 */
    res = NULL;
    ASSERT_EQ_INT(0, rk_tool_call(t, "{\"question\":\"old style\"}", &env, &res));
    ASSERT_NOT_NULL(res);
    ASSERT(strstr(res, "ok") != NULL);
    free(res);
    ASSERT_NOT_NULL(captured_ask);
    ASSERT(strstr(captured_ask, "old style") != NULL);
    ASSERT(strstr(captured_ask, "questions") != NULL);
    free(captured_ask);
    captured_ask = NULL;
    free(captured_ask);
    captured_ask = NULL;
    rk_tools_destroy(&r);
}

TEST(tool_whitelist) {
    RkToolRegistry r;
    rk_tools_init(&r);
    RkToolEnv env = {0};
    env.tool_whitelist = "[\"get_time_info\",\"ask_user\",\"memory_tool\"]";
    env.ask_user = stub_ask_user;
    rk_tools_register_builtin(&r, &env);
    ASSERT_NOT_NULL(rk_tools_find(&r, "get_time_info"));
    ASSERT_NOT_NULL(rk_tools_find(&r, "ask_user"));
    ASSERT_NOT_NULL(rk_tools_find(&r, "memory_tool"));
    ASSERT_NULL(rk_tools_find(&r, "use_skill"));        /* 不在白名单 */
    ASSERT_NULL(rk_tools_find(&r, "eval_javascript"));  /* 不在白名单 */
    rk_tools_destroy(&r);
}

TEST(tool_time_info) {
    RkToolRegistry r;
    rk_tools_init(&r);
    RkToolEnv env = {0};
    rk_tools_register_builtin(&r, &env);
    const RkTool *t = rk_tools_find(&r, "get_time_info");
    char *result = NULL;
    ASSERT_EQ_INT(0, rk_tool_call(t, "{}", &env, &result));
    ASSERT_NOT_NULL(result);
    ASSERT(strstr(result, "\"year\":") != NULL);
    ASSERT(strstr(result, "\"weekday\"") != NULL);
    ASSERT(strstr(result, "\"timestamp_ms\"") != NULL);
    ASSERT(strstr(result, "\"timezone\"") != NULL);
    free(result);
    rk_tools_destroy(&r);
}

TEST(tool_workspace_files) {
    char root[128];
    snprintf(root, sizeof(root), "/tmp/rk_tool_ws_%d", (int)getpid());
    mkdir(root, 0755);
    RkToolRegistry r;
    rk_tools_init(&r);
    RkToolEnv env = {0};
    env.workspace_root = root;
    rk_tools_register_builtin(&r, &env);
    ASSERT_NOT_NULL(rk_tools_find(&r, "workspace_read_file"));
    char *result = NULL;
    /* 写 */
    const RkTool *w = rk_tools_find(&r, "workspace_write_file");
    ASSERT_EQ_INT(0, rk_tool_call(w, "{\"path\":\"a/b.txt\",\"text\":\"hello\\nworld\"}",
                                  &env, &result));
    free(result);
    result = NULL;
    /* 读 */
    const RkTool *rd = rk_tools_find(&r, "workspace_read_file");
    ASSERT_EQ_INT(0, rk_tool_call(rd, "{\"path\":\"a/b.txt\"}", &env, &result));
    ASSERT_NOT_NULL(result);
    ASSERT(strstr(result, "hello\\nworld") != NULL);
    free(result);
    result = NULL;
    /* 读不存在 → 错误 */
    TOOL_FAIL(rk_tool_call(rd, "{\"path\":\"nope.txt\"}", &env, &result), result);
    /* 路径逃逸拒绝 */
    TOOL_FAIL(rk_tool_call(rd, "{\"path\":\"../etc/passwd\"}", &env, &result), result);
    TOOL_FAIL(rk_tool_call(rd, "{\"path\":\"/etc/passwd\"}", &env, &result), result);
    /* 编辑：单次替换 */
    const RkTool *ed = rk_tools_find(&r, "workspace_edit_file");
    ASSERT_EQ_INT(0, rk_tool_call(ed, "{\"path\":\"a/b.txt\",\"old_text\":\"hello\","
                                       "\"new_text\":\"hi\"}", &env, &result));
    free(result);
    result = NULL;
    ASSERT_EQ_INT(0, rk_tool_call(rd, "{\"path\":\"a/b.txt\"}", &env, &result));
    ASSERT(strstr(result, "hi\\nworld") != NULL);
    free(result);
    result = NULL;
    /* 编辑：不匹配 → 错误 */
    TOOL_FAIL(rk_tool_call(ed, "{\"path\":\"a/b.txt\",\"old_text\":\"zzz\","
                                        "\"new_text\":\"x\"}", &env, &result), result);
    /* 编辑：多次出现无 replace_all → 错误 */
    ASSERT_EQ_INT(0, rk_tool_call(w, "{\"path\":\"c.txt\",\"text\":\"aa aa\"}", &env, &result));
    free(result);
    result = NULL;
    TOOL_FAIL(rk_tool_call(ed, "{\"path\":\"c.txt\",\"old_text\":\"aa\","
                                        "\"new_text\":\"b\"}", &env, &result), result);
    /* replace_all */
    ASSERT_EQ_INT(0, rk_tool_call(ed, "{\"path\":\"c.txt\",\"old_text\":\"aa\","
                                       "\"new_text\":\"b\",\"replace_all\":true}", &env, &result));
    free(result);
    result = NULL;
    ASSERT_EQ_INT(0, rk_tool_call(rd, "{\"path\":\"c.txt\"}", &env, &result));
    ASSERT(strstr(result, "b b") != NULL);
    free(result);
    rk_tools_destroy(&r);
    /* 清理 */
    unlink("/tmp/rk_tool_ws_tmp"); /* no-op 安全 */
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", root);
    { int _rc = system(cmd); (void)_rc; }
}

TEST(tool_workspace_shell) {
    char root[128];
    snprintf(root, sizeof(root), "/tmp/rk_tool_sh_%d", (int)getpid());
    mkdir(root, 0755);
    RkToolRegistry r;
    rk_tools_init(&r);
    RkToolEnv env = {0};
    env.workspace_root = root;
    rk_tools_register_builtin(&r, &env);
    const RkTool *sh = rk_tools_find(&r, "workspace_shell");
    char *result = NULL;
    ASSERT_EQ_INT(0, rk_tool_call(sh, "{\"command\":\"echo hello; echo err >&2; exit 3\"}",
                                  &env, &result));
    ASSERT_NOT_NULL(result);
    ASSERT(strstr(result, "hello") != NULL);
    ASSERT(strstr(result, "err") != NULL);
    ASSERT(strstr(result, "\"exit_code\":3") != NULL);
    free(result);
    result = NULL;
    /* 超时保护（sleep 2s > timeout 1s） */
    TOOL_FAIL(rk_tool_call(sh, "{\"command\":\"sleep 5\",\"timeout\":1}",
                                   &env, &result), result);
    rk_tools_destroy(&r);
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", root);
    { int _rc = system(cmd); (void)_rc; }
}

TEST(tool_memory) {
    RkToolRegistry r;
    rk_tools_init(&r);
    RkToolEnv env = {0};
    env.memory_create = stub_memory_create;
    env.memory_edit = stub_memory_edit;
    env.memory_delete = stub_memory_delete;
    rk_tools_register_builtin(&r, &env);
    const RkTool *m = rk_tools_find(&r, "memory_tool");
    char *result = NULL;
    ASSERT_EQ_INT(0, rk_tool_call(m, "{\"action\":\"create\",\"content\":\"note\"}",
                                  &env, &result));
    ASSERT(strstr(result, "\"id\":1") != NULL);
    free(result);
    result = NULL;
    ASSERT_EQ_INT(0, rk_tool_call(m, "{\"action\":\"edit\",\"id\":5,\"content\":\"x\"}",
                                  &env, &result));
    ASSERT(strstr(result, "\"id\":5") != NULL);
    free(result);
    result = NULL;
    ASSERT_EQ_INT(0, rk_tool_call(m, "{\"action\":\"delete\",\"id\":7}", &env, &result));
    ASSERT(strstr(result, "\"ok\":true") != NULL);
    free(result);
    result = NULL;
    /* 缺 action → 错误 */
    TOOL_FAIL(rk_tool_call(m, "{}", &env, &result), result);
    /* 未知 action */
    TOOL_FAIL(rk_tool_call(m, "{\"action\":\"x\"}", &env, &result), result);
    rk_tools_destroy(&r);
}

TEST(tool_use_skill) {
    /* 造一个技能目录 */
    mkdir("/tmp/rk_skills_test", 0755);
    FILE *fp = fopen("/tmp/rk_skills_test/SKILL.md", "w");
    ASSERT_NOT_NULL(fp);
    fputs("# Skill Doc\ninstructions here\n", fp);
    fclose(fp);
    RkToolRegistry r;
    rk_tools_init(&r);
    RkToolEnv env = {0};
    rk_tools_register_builtin(&r, &env);
    const RkTool *sk = rk_tools_find(&r, "use_skill");
    /* 重定向 /skills 到测试目录：直接调用底层（skill 工具读 /skills/，测试用临时目录不可行）
     * —— 改用环境变量？不：工具固定读 /skills。这里只测参数校验与错误路径。 */
    char *result = NULL;
    TOOL_FAIL(rk_tool_call(sk, "{\"name\":\"..\"}", &env, &result), result);      /* 非法名 */
    TOOL_FAIL(rk_tool_call(sk, "{\"name\":\"x/..\"}", &env, &result), result);    /* 非法名 */
    TOOL_FAIL(rk_tool_call(sk, "{\"name\":\"nope\"}", &env, &result), result);    /* 不存在 */
    TOOL_FAIL(rk_tool_call(sk, "{\"name\":\"nope\",\"path\":\"../x\"}", &env, &result), result);
    TOOL_FAIL(rk_tool_call(sk, "{}", &env, &result), result);                     /* 缺 name */
    rk_tools_destroy(&r);
    { int _rc = system("rm -rf /tmp/rk_skills_test"); (void)_rc; }
}

TEST(tool_arg_helpers) {
    const char *args = "{\"s\":\"val\",\"n\":42,\"b\":true}";
    char out[64];
    ASSERT_EQ_INT(0, rk_tool_arg_str(args, "s", out, sizeof(out)));
    ASSERT(strcmp(out, "val") == 0);
    ASSERT_EQ_INT(-1, rk_tool_arg_str(args, "missing", out, sizeof(out)));
    ASSERT_EQ_INT(42, (int)rk_tool_arg_i64(args, "n", 0));
    ASSERT_EQ_INT(7, (int)rk_tool_arg_i64(args, "missing", 7));
    ASSERT(rk_tool_arg_bool(args, "b", 0));
    ASSERT(!rk_tool_arg_bool(args, "missing", 0));
    /* 结果构造 */
    char *r = rk_tool_result_json("k", "v");
    ASSERT_NOT_NULL(r);
    ASSERT(strcmp(r, "{\"k\":\"v\"}") == 0);
    free(r);
    r = rk_tool_result_error("bad");
    ASSERT(strcmp(r, "{\"error\":\"bad\"}") == 0);
    free(r);
}

/* ---------- 三级替换（rk_text_replace 直接测试） ---------- */

TEST(tool_text_replace_exact) {
    RkTextReplaceResult tr;
    /* 精确单次 */
    rk_text_replace("hello world hello", 17, "world", "there", 0, &tr);
    ASSERT_EQ_INT(0, tr.error);
    ASSERT(strcmp(tr.strategy, "exact") == 0);
    ASSERT_EQ_INT(1, (int)tr.replacements);
    ASSERT(strcmp(tr.updated, "hello there hello") == 0);
    free(tr.updated);
    /* 多次出现无 replace_all → 错误 */
    rk_text_replace("aa aa", 5, "aa", "b", 0, &tr);
    ASSERT_EQ_INT(2, tr.error);
    ASSERT(strstr(tr.errmsg, "2 locations") != NULL);
    ASSERT(strstr(tr.errmsg, "replace_all") != NULL);
    /* replace_all */
    rk_text_replace("aa aa", 5, "aa", "b", 1, &tr);
    ASSERT_EQ_INT(0, tr.error);
    ASSERT_EQ_INT(2, (int)tr.replacements);
    ASSERT(strcmp(tr.updated, "b b") == 0);
    free(tr.updated);
    /* 无匹配 → not found */
    rk_text_replace("abc", 3, "xyz", "q", 0, &tr);
    ASSERT_EQ_INT(1, tr.error);
    ASSERT(strstr(tr.errmsg, "whitespace-tolerant") != NULL);
    /* 空 old_text */
    rk_text_replace("abc", 3, "", "q", 0, &tr);
    ASSERT_EQ_INT(1, tr.error);
}

TEST(tool_text_replace_line_trimmed) {
    RkTextReplaceResult tr;
    /* 单行子串：exact 命中（JVM 同语义：indexOf 子串匹配） */
    const char *content = "line1\n    foo  \nline3\n";
    rk_text_replace(content, strlen(content), "foo", "bar", 0, &tr);
    ASSERT_EQ_INT(0, tr.error);
    ASSERT(strcmp(tr.strategy, "exact") == 0);
    ASSERT(strstr(tr.updated, "    bar  ") != NULL); /* 保留行内空白 */
    free(tr.updated);
    /* line_trimmed：多行 old，行内含空白差异使 exact 不中 */
    const char *content2 = "a\nX  \nY\t\nb\n";
    const char *old2 = "X\nY\n";
    rk_text_replace(content2, strlen(content2), old2, "Z", 0, &tr);
    ASSERT_EQ_INT(0, tr.error);
    ASSERT(strcmp(tr.strategy, "line_trimmed") == 0);
    ASSERT(strstr(tr.updated, "a\nZ\nb\n") != NULL);
    free(tr.updated);
}

TEST(tool_text_replace_block_anchor) {
    RkTextReplaceResult tr;
    /* ≥3 行：中间行差异容忍（block_anchor） */
    const char *content =
        "start\n"
        "keep\n"
        "DIFFERENT\n"
        "end\n";
    const char *old =
        "start\n"
        "something else\n"
        "x\n"
        "end\n";
    rk_text_replace(content, strlen(content), old, "REPLACED", 0, &tr);
    ASSERT_EQ_INT(0, tr.error);
    ASSERT(strcmp(tr.strategy, "block_anchor") == 0);
    ASSERT(strstr(tr.updated, "REPLACED") != NULL);
    ASSERT(strstr(tr.updated, "DIFFERENT") == NULL);
    free(tr.updated);
    /* 2 行 old 不能用 block_anchor（降级 not found） */
    rk_text_replace(content, strlen(content), "start\nend\n", "X", 0, &tr);
    ASSERT_EQ_INT(1, tr.error);
}

/* ---------- search / conversation 工具（回调桩） ---------- */

static char *stub_search(const char *query, void *ud) {
    (void)ud;
    char *r = (char *)malloc(256);
    snprintf(r, 256,
             "{\"items\":[{\"title\":\"result for %s\",\"url\":\"http://x/1\"}]}",
             query);
    return r;
}

static char *stub_recent_chats(int limit, void *ud) {
    (void)ud;
    char *r = (char *)malloc(128);
    snprintf(r, 128, "[{\"id\":\"c1\",\"title\":\"t1\",\"limit\":%d}]", limit);
    return r;
}

static char *stub_conv_search(const char *query, void *ud) {
    (void)ud;
    char *r = (char *)malloc(128);
    snprintf(r, 128, "[{\"title\":\"found [%s]\",\"date\":\"2026-08-01\"}]", query);
    return r;
}

TEST(tool_search_and_conversation) {
    RkToolRegistry r;
    rk_tools_init(&r);
    RkToolEnv env = {0};
    env.web_search = stub_search;
    env.recent_chats = stub_recent_chats;
    env.conversation_search = stub_conv_search;
    rk_tools_register_builtin(&r, &env);
    char *result = NULL;
    /* search_web */
    const RkTool *sw = rk_tools_find(&r, "search_web");
    ASSERT_NOT_NULL(sw);
    ASSERT_EQ_INT(0, rk_tool_call(sw, "{\"query\":\"news\"}", &env, &result));
    ASSERT(strstr(result, "result for news") != NULL);
    free(result);
    result = NULL;
    /* recent_chats */
    const RkTool *rc = rk_tools_find(&r, "recent_chats");
    ASSERT_NOT_NULL(rc);
    ASSERT_EQ_INT(0, rk_tool_call(rc, "{\"limit\":5}", &env, &result));
    ASSERT(strstr(result, "\"limit\":5") != NULL);
    free(result);
    result = NULL;
    /* conversation_search */
    const RkTool *cs = rk_tools_find(&r, "conversation_search");
    ASSERT_NOT_NULL(cs);
    ASSERT_EQ_INT(0, rk_tool_call(cs, "{\"query\":\"apk\"}", &env, &result));
    ASSERT(strstr(result, "found [apk]") != NULL);
    free(result);
    result = NULL;
    /* 无回调 → 不注册 */
    RkToolRegistry r2;
    rk_tools_init(&r2);
    RkToolEnv env2 = {0};
    rk_tools_register_builtin(&r2, &env2);
    ASSERT_NULL(rk_tools_find(&r2, "search_web"));
    ASSERT_NULL(rk_tools_find(&r2, "recent_chats"));
    rk_tools_destroy(&r2);
    rk_tools_destroy(&r);
}

int run_tool_suite(void) {
    const RikkaTest tests[] = {
        RIKKA_TEST_REGISTER(tool, tool_registry),
        RIKKA_TEST_REGISTER(tool, tool_whitelist),
        RIKKA_TEST_REGISTER(tool, tool_ask_user_passthrough),
        RIKKA_TEST_REGISTER(tool, tool_time_info),
        RIKKA_TEST_REGISTER(tool, tool_workspace_files),
        RIKKA_TEST_REGISTER(tool, tool_workspace_shell),
        RIKKA_TEST_REGISTER(tool, tool_memory),
        RIKKA_TEST_REGISTER(tool, tool_use_skill),
        RIKKA_TEST_REGISTER(tool, tool_arg_helpers),
        RIKKA_TEST_REGISTER(tool, tool_text_replace_exact),
        RIKKA_TEST_REGISTER(tool, tool_text_replace_line_trimmed),
        RIKKA_TEST_REGISTER(tool, tool_text_replace_block_anchor),
        RIKKA_TEST_REGISTER(tool, tool_search_and_conversation),
    };
    return run_suite("tool", tests, sizeof(tests) / sizeof(tests[0]));
}
