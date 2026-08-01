#define _DEFAULT_SOURCE
#include "test.h"
#include "rikka/data/chats.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

TEST(chats_upsert_recent) {
    RkChatIndex *ci = rk_chats_create();
    ASSERT_NOT_NULL(ci);
    /* 3 条会话：排序期望 pinned 优先 + 时间降序 */
    ASSERT_EQ_INT(0, rk_chats_upsert(ci, "c1", "First", "hello world", 3000, 0));
    ASSERT_EQ_INT(0, rk_chats_upsert(ci, "c2", "Pinned", "pinned chat", 1000, 1));
    ASSERT_EQ_INT(0, rk_chats_upsert(ci, "c3", "Newest", "latest talk", 5000, 0));
    char *recent = rk_chats_recent(ci, 10);
    ASSERT_NOT_NULL(recent);
    /* c2(pinned) → c3(5000) → c1(3000) */
    ASSERT(strstr(recent, "\"id\":\"c2\"") < strstr(recent, "\"id\":\"c3\""));
    ASSERT(strstr(recent, "\"id\":\"c3\"") < strstr(recent, "\"id\":\"c1\""));
    ASSERT(strstr(recent, "\"title\":\"Pinned\"") != NULL);
    ASSERT(strstr(recent, "\"last_chat\":\"1970-01-01\"") != NULL);
    free(recent);
    /* limit */
    recent = rk_chats_recent(ci, 2);
    ASSERT(strstr(recent, "\"id\":\"c1\"") == NULL);
    free(recent);
    /* upsert 更新（时间变化 → 排序变化） */
    ASSERT_EQ_INT(0, rk_chats_upsert(ci, "c1", "First!", "hello world again", 9000, 0));
    recent = rk_chats_recent(ci, 10);
    /* pinned 优先：c2 仍最前；然后 c1(9000) → c3(5000) */
    ASSERT(strstr(recent, "\"id\":\"c2\"") < strstr(recent, "\"id\":\"c1\""));
    ASSERT(strstr(recent, "\"id\":\"c1\"") < strstr(recent, "\"id\":\"c3\""));
    ASSERT(strstr(recent, "First!") != NULL);
    free(recent);
    /* remove */
    ASSERT_EQ_INT(0, rk_chats_remove(ci, "c2"));
    ASSERT_EQ_INT(-1, rk_chats_remove(ci, "nope"));
    recent = rk_chats_recent(ci, 10);
    ASSERT(strstr(recent, "\"id\":\"c2\"") == NULL);
    free(recent);
    rk_chats_destroy(ci);
}

TEST(chats_search) {
    RkChatIndex *ci = rk_chats_create();
    ASSERT_NOT_NULL(ci);
    ASSERT_EQ_INT(0, rk_chats_upsert(ci, "a", "About Rikka",
                                     "discussing the rikkahub engine design", 1000, 0));
    ASSERT_EQ_INT(0, rk_chats_upsert(ci, "b", "Other",
                                     "nothing about music here", 2000, 0));
    /* AND 搜索命中 a */
    char *res = rk_chats_search(ci, "rikkahub");
    ASSERT_NOT_NULL(res);
    ASSERT(strstr(res, "\"id\":\"a\"") != NULL);
    ASSERT(strstr(res, "[rikkahub]") != NULL);  /* snippet 高亮 */
    ASSERT(strstr(res, "\"id\":\"b\"") == NULL);
    free(res);
    /* 无命中 → [] */
    res = rk_chats_search(ci, "zzzz");
    ASSERT(strcmp(res, "[]") == 0);
    free(res);
    /* 大小写不敏感 */
    res = rk_chats_search(ci, "RIKKAHUB");
    ASSERT(strstr(res, "\"id\":\"a\"") != NULL);
    free(res);
    /* 更新后索引重建 */
    ASSERT_EQ_INT(0, rk_chats_upsert(ci, "a", "About Rikka", "now about music", 3000, 0));
    res = rk_chats_search(ci, "music");
    ASSERT(strstr(res, "\"id\":\"a\"") != NULL);
    free(res);
    res = rk_chats_search(ci, "rikkahub");
    ASSERT(strcmp(res, "[]") == 0); /* 旧索引已移除 */
    free(res);
    rk_chats_destroy(ci);
}

TEST(chats_snapshot) {
    RkChatIndex *ci = rk_chats_create();
    ASSERT_EQ_INT(0, rk_chats_upsert(ci, "c1", "T1", "alpha beta gamma", 1000, 0));
    ASSERT_EQ_INT(0, rk_chats_upsert(ci, "c2", "T2", "delta", 2000, 1));
    char path[256];
    snprintf(path, sizeof(path), "/tmp/rk_chats_%d.bin", (int)getpid());
    ASSERT_EQ_INT(0, rk_chats_save_file(ci, path));
    rk_chats_destroy(ci);
    /* 加载并校验（含索引重建） */
    RkChatIndex *ci2 = rk_chats_create();
    ASSERT_EQ_INT(0, rk_chats_load_file(ci2, path));
    char *recent = rk_chats_recent(ci2, 10);
    ASSERT(strstr(recent, "\"id\":\"c2\"") != NULL); /* pinned 优先 */
    free(recent);
    char *res = rk_chats_search(ci2, "gamma");
    ASSERT(strstr(res, "\"id\":\"c1\"") != NULL);
    free(res);
    unlink(path);
    rk_chats_destroy(ci2);
}

int run_chats_suite(void) {
    const RikkaTest tests[] = {
        RIKKA_TEST_REGISTER(chats, chats_upsert_recent),
        RIKKA_TEST_REGISTER(chats, chats_search),
        RIKKA_TEST_REGISTER(chats, chats_snapshot),
    };
    return run_suite("chats", tests, sizeof(tests) / sizeof(tests[0]));
}
