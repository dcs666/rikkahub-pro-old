#define _DEFAULT_SOURCE
#include "test.h"
#include "rikka/ai/prompt.h"
#include <stdio.h>
#include <string.h>

TEST(prompt_compress) {
    Arena *a = arena_create(0);
    const char *names[4] = {"target_tokens", "locale", "additional_context", "content"};
    const char *values[4] = {"2000", "zh-CN", "", "user: hi\nassistant: hello"};
    char *out = rk_prompt_fill(a, RK_PROMPT_COMPRESS, names, values, 4);
    ASSERT_NOT_NULL(out);
    ASSERT(strstr(out, "2000 tokens") != NULL);
    ASSERT(strstr(out, "zh-CN language") != NULL);
    ASSERT(strstr(out, "user: hi\nassistant: hello") != NULL);
    ASSERT(strstr(out, "<conversation>") != NULL);
    ASSERT(strstr(out, "{content}") == NULL); /* 占位符都被替换 */
    /* 未知键保留 */
    const char *names2[1] = {"target_tokens"};
    const char *values2[1] = {"500"};
    char *out2 = rk_prompt_fill(a, RK_PROMPT_COMPRESS, names2, values2, 1);
    ASSERT(strstr(out2, "{locale}") != NULL); /* 未知键原样 */
    arena_destroy(a);
}

TEST(prompt_static_templates) {
    /* 无占位符模板直接可用 */
    ASSERT(strstr(RK_PROMPT_LEARNING_MODE, "DO NOT GIVE ANSWERS") != NULL);
    ASSERT(strstr(RK_PROMPT_LEARNING_MODE, "{") == NULL);
    ASSERT(strstr(RK_PROMPT_OCR, "reading order") != NULL);
    ASSERT(strstr(RK_PROMPT_OCR, "{") == NULL);
}

TEST(prompt_suggestion_title_translation) {
    Arena *a = arena_create(0);
    /* suggestion */
    const char *sn[2] = {"locale", "content"};
    const char *sv[2] = {"en", "chat text"};
    char *s = rk_prompt_fill(a, RK_PROMPT_SUGGESTION, sn, sv, 2);
    ASSERT(strstr(s, "en language") != NULL);
    ASSERT(strstr(s, "chat text") != NULL);
    /* title */
    char *tt = rk_prompt_fill(a, RK_PROMPT_TITLE, sn, sv, 2);
    ASSERT(strstr(tt, "10 characters") != NULL);
    ASSERT(strstr(tt, "chat text") != NULL);
    /* translation */
    const char *tn[2] = {"target_lang", "source_text"};
    const char *tv[2] = {"Chinese", "Hello world"};
    char *tr = rk_prompt_fill(a, RK_PROMPT_TRANSLATION, tn, tv, 2);
    ASSERT(strstr(tr, "translate it into Chinese") != NULL);
    ASSERT(strstr(tr, "Hello world") != NULL);
    arena_destroy(a);
}

TEST(prompt_memories) {
    Arena *a = arena_create(0);
    const int64_t ids[3] = {1, 12, 7};
    const char *contents[3] = {"prefers brief replies", "name is \"A\"", "line1\nline2"};
    char *out = rk_prompt_memories(a, ids, contents, 3);
    ASSERT_NOT_NULL(out);
    ASSERT(strstr(out, "**Memories**") != NULL);
    ASSERT(strstr(out, "[{\"id\":1,\"content\":\"prefers brief replies\"}") != NULL);
    ASSERT(strstr(out, "\"id\":12") != NULL);
    ASSERT(strstr(out, "name is \\\"A\\\"") != NULL);   /* 引号转义 */
    ASSERT(strstr(out, "line1\\nline2") != NULL);       /* 换行转义 */
    /* 空列表 */
    char *empty = rk_prompt_memories(a, NULL, NULL, 0);
    ASSERT(strstr(empty, "[]") != NULL);
    arena_destroy(a);
}

int run_prompt_suite(void) {
    const RikkaTest tests[] = {
        RIKKA_TEST_REGISTER(prompt, prompt_compress),
        RIKKA_TEST_REGISTER(prompt, prompt_static_templates),
        RIKKA_TEST_REGISTER(prompt, prompt_suggestion_title_translation),
        RIKKA_TEST_REGISTER(prompt, prompt_memories),
    };
    return run_suite("prompt", tests, sizeof(tests) / sizeof(tests[0]));
}
