#include "test.h"
#include "rikka/markdown/md.h"
#include <string.h>

TEST(headings) {
    const char *md = "# Title\n\n## Sub\n\n### Deep\n";
    size_t n = 0;
    RikkaMdBlock *b = rmd_parse_all(md, strlen(md), &n);
    ASSERT_NOT_NULL(b);
    ASSERT_EQ_SIZE(3, n);
    ASSERT_EQ_INT(RIKKA_MD_HEADING, b[0].type);
    ASSERT_EQ_INT(1, b[0].level);
    ASSERT_EQ_SIZE(5, b[0].len);
    ASSERT(memcmp(b[0].text, "Title", 5) == 0);
    ASSERT_EQ_INT(2, b[1].level);
    ASSERT_EQ_INT(3, b[2].level);
    rmd_blocks_free(b, n);
}

TEST(paragraph_inline) {
    const char *md = "Hello **bold** and *italic* and `code` and [link](https://x.com)\n";
    size_t n = 0;
    RikkaMdBlock *b = rmd_parse_all(md, strlen(md), &n);
    ASSERT_NOT_NULL(b);
    ASSERT_EQ_SIZE(1, n);
    ASSERT_EQ_INT(RIKKA_MD_PARAGRAPH, b[0].type);
    /* inline 节点：text/bold/text/italic/text/code/text/link */
    ASSERT(b[0].inline_count >= 8);
    int found_bold = 0, found_italic = 0, found_code = 0, found_link = 0;
    for (size_t i = 0; i < b[0].inline_count; i++) {
        RikkaInline *in = &b[0].inlines[i];
        if (in->type == RIKKA_INLINE_BOLD) {
            found_bold = 1;
            ASSERT_EQ_SIZE(4, in->len);
            ASSERT(memcmp(b[0].text + in->start, "bold", 4) == 0);
        }
        if (in->type == RIKKA_INLINE_ITALIC) {
            found_italic = 1;
            ASSERT_EQ_SIZE(6, in->len);
        }
        if (in->type == RIKKA_INLINE_CODE) {
            found_code = 1;
            ASSERT_EQ_SIZE(4, in->len);
        }
        if (in->type == RIKKA_INLINE_LINK) {
            found_link = 1;
            ASSERT_EQ_SIZE(4, in->len);
            ASSERT_EQ_SIZE(13, in->href_len);
            ASSERT(memcmp(in->href, "https://x.com", 13) == 0);
        }
    }
    ASSERT(found_bold && found_italic && found_code && found_link);
    rmd_blocks_free(b, n);
}

TEST(code_block) {
    const char *md = "```python\nprint(1)\n```\n";
    size_t n = 0;
    RikkaMdBlock *b = rmd_parse_all(md, strlen(md), &n);
    ASSERT_NOT_NULL(b);
    ASSERT_EQ_SIZE(1, n);
    ASSERT_EQ_INT(RIKKA_MD_CODE_BLOCK, b[0].type);
    ASSERT_EQ_SIZE(6, b[0].lang_len);
    ASSERT(memcmp(b[0].lang, "python", 6) == 0);
    ASSERT_EQ_SIZE(9, b[0].len);
    ASSERT(memcmp(b[0].text, "print(1)\n", 9) == 0);
    rmd_blocks_free(b, n);
}

TEST(unclosed_fence) {
    /* 增量中间态：fence 未闭合 */
    const char *md = "```c\nint x;\n";
    size_t n = 0;
    RikkaMdBlock *b = rmd_parse_all(md, strlen(md), &n);
    ASSERT_NOT_NULL(b);
    ASSERT_EQ_SIZE(1, n);
    ASSERT_EQ_INT(RIKKA_MD_CODE_BLOCK, b[0].type);
    ASSERT_EQ_SIZE(1, b[0].lang_len);
    rmd_blocks_free(b, n);
}

TEST(quote_list_hr) {
    const char *md = "> quoted text\n\n- item one\n- item two\n\n---\n";
    size_t n = 0;
    RikkaMdBlock *b = rmd_parse_all(md, strlen(md), &n);
    ASSERT_NOT_NULL(b);
    ASSERT_EQ_SIZE(4, n);
    ASSERT_EQ_INT(RIKKA_MD_QUOTE, b[0].type);
    ASSERT_EQ_SIZE(11, b[0].len); /* "quoted text" */
    ASSERT_EQ_INT(RIKKA_MD_LIST_ITEM, b[1].type);
    ASSERT_EQ_SIZE(8, b[1].len);
    ASSERT(memcmp(b[1].text, "item one", 8) == 0);
    ASSERT_EQ_INT(RIKKA_MD_LIST_ITEM, b[2].type);
    ASSERT_EQ_INT(RIKKA_MD_HR, b[3].type);
    rmd_blocks_free(b, n);
}

TEST(image) {
    const char *md = "![alt text](https://img.png)\n";
    size_t n = 0;
    RikkaMdBlock *b = rmd_parse_all(md, strlen(md), &n);
    ASSERT_NOT_NULL(b);
    int found = 0;
    for (size_t i = 0; i < b[0].inline_count; i++) {
        if (b[0].inlines[i].type == RIKKA_INLINE_IMAGE) {
            found = 1;
            ASSERT_EQ_SIZE(8, b[0].inlines[i].alt_len); /* "alt text" */
            ASSERT_EQ_SIZE(15, b[0].inlines[i].href_len);
        }
    }
    ASSERT(found);
    rmd_blocks_free(b, n);
}

TEST(incremental_feed) {
    /* 模拟流式：逐段 feed，最终结构与一次性解析一致 */
    RikkaMdParser *p = rmd_create();
    const char *chunks[] = {
        "# Stream", "ing Title\n\n", "Para", "graph with ", "**bold**\n\n",
        "```js\n", "console.", "log(1)\n", "```\n",
    };
    for (size_t i = 0; i < sizeof(chunks) / sizeof(chunks[0]); i++)
        rmd_feed(p, chunks[i], strlen(chunks[i]));

    size_t n = 0;
    const RikkaMdBlock *b = rmd_blocks(p, &n);
    ASSERT_EQ_SIZE(3, n);
    ASSERT_EQ_INT(RIKKA_MD_HEADING, b[0].type);
    ASSERT_EQ_SIZE(15, b[0].len);
    ASSERT(memcmp(b[0].text, "Streaming Title", 15) == 0);
    ASSERT_EQ_INT(RIKKA_MD_PARAGRAPH, b[1].type);
    int found_bold = 0;
    for (size_t i = 0; i < b[1].inline_count; i++)
        if (b[1].inlines[i].type == RIKKA_INLINE_BOLD) found_bold = 1;
    ASSERT(found_bold);
    ASSERT_EQ_INT(RIKKA_MD_CODE_BLOCK, b[2].type);
    ASSERT_EQ_SIZE(2, b[2].lang_len);
    ASSERT(memcmp(b[2].lang, "js", 2) == 0);
    ASSERT(memcmp(b[2].text, "console.log(1)\n", 15) == 0);
    rmd_destroy(p);
}

TEST(fence_close_reopen_single_feed) {
    /* 回归: 一次 feed 闭合旧 fence + 重开新 fence（2 行）不得走快速路径 */
    RikkaMdParser *p = rmd_create();
    rmd_feed(p, "```c\nint x;\n", strlen("```c\nint x;\n"));
    rmd_feed(p, "```\n```\n", strlen("```\n```\n")); /* 闭合+重开 */
    rmd_feed(p, "int y;\n", strlen("int y;\n")); /* 应在新的未闭合 fence 内 */
    size_t n = 0;
    const RikkaMdBlock *b = rmd_blocks(p, &n);
    ASSERT_EQ_SIZE(2, n);
    ASSERT_EQ_INT(RIKKA_MD_CODE_BLOCK, b[0].type);
    ASSERT_EQ_INT(RIKKA_MD_CODE_BLOCK, b[1].type);
    ASSERT_EQ_SIZE(7, b[0].len); /* "int x;\n" */
    ASSERT(memcmp(b[0].text, "int x;\n", 7) == 0);
    ASSERT_EQ_SIZE(7, b[1].len); /* "int y;\n" */
    ASSERT(memcmp(b[1].text, "int y;\n", 7) == 0);
    rmd_destroy(p);
}

TEST(incremental_fence_cross_feed) {
    /* fence 跨 feed 边界 */
    RikkaMdParser *p = rmd_create();
    rmd_feed(p, "```c\nint ", strlen("```c\nint "));
    size_t n = 0;
    const RikkaMdBlock *b = rmd_blocks(p, &n);
    ASSERT_EQ_SIZE(1, n);
    ASSERT_EQ_INT(RIKKA_MD_CODE_BLOCK, b[0].type);
    rmd_feed(p, "x = 1;\n```\n", strlen("x = 1;\n```\n"));
    b = rmd_blocks(p, &n);
    ASSERT_EQ_SIZE(1, n);
    ASSERT_EQ_INT(RIKKA_MD_CODE_BLOCK, b[0].type);
    ASSERT(memcmp(b[0].text, "int x = 1;\n", 11) == 0);
    rmd_destroy(p);
}

TEST(incremental_boundary) {
    /* feed 段落中（无空行）→ 1 块；feed 空行 → 新段落 */
    RikkaMdParser *p = rmd_create();
    rmd_feed(p, "first line\n", 11);
    rmd_feed(p, "continued", 9);
    size_t n = 0;
    const RikkaMdBlock *b = rmd_blocks(p, &n);
    ASSERT_EQ_SIZE(1, n); /* 同一段落 */
    ASSERT_EQ_INT(RIKKA_MD_PARAGRAPH, b[0].type);
    ASSERT_EQ_SIZE(20, b[0].len);
    ASSERT(memcmp(b[0].text, "first line\ncontinued", 20) == 0);
    /* 空行分隔 */
    rmd_feed(p, "\n\nsecond", strlen("\n\nsecond"));
    b = rmd_blocks(p, &n);
    ASSERT_EQ_SIZE(2, n);
    ASSERT(memcmp(b[1].text, "second", 6) == 0);
    rmd_destroy(p);
}

int run_md_suite(void) {
    const RikkaTest tests[] = {
        RIKKA_TEST_REGISTER(md, headings),
        RIKKA_TEST_REGISTER(md, paragraph_inline),
        RIKKA_TEST_REGISTER(md, code_block),
        RIKKA_TEST_REGISTER(md, unclosed_fence),
        RIKKA_TEST_REGISTER(md, quote_list_hr),
        RIKKA_TEST_REGISTER(md, image),
        RIKKA_TEST_REGISTER(md, incremental_feed),
        RIKKA_TEST_REGISTER(md, fence_close_reopen_single_feed),
        RIKKA_TEST_REGISTER(md, incremental_fence_cross_feed),
        RIKKA_TEST_REGISTER(md, incremental_boundary),
    };
    return run_suite("md", tests, sizeof(tests) / sizeof(tests[0]));
}
