#include "test.h"
#include "rikka/render/render.h"
#include "rikka/json/json.h"
#include "rikka/util/arena.h"
#include <string.h>

/* JSON 线协议：结构完整 + 合法 JSON + 转义正确 */
TEST(render_json_protocol) {
    const char *md = "# Title\n\nSome **bold** and \"quoted\" text.\n\n```c\nint x = 1;\nreturn x;\n```\n";
    char *json = rk_render_markdown_json(md, strlen(md));
    ASSERT_NOT_NULL(json);
    ASSERT(strstr(json, "\"type\":\"heading\"") != NULL);
    ASSERT(strstr(json, "\"level\":1") != NULL);
    ASSERT(strstr(json, "\"type\":\"code\"") != NULL);
    ASSERT(strstr(json, "\"lang\":\"c\"") != NULL);
    ASSERT(strstr(json, "\"tokens\":[") != NULL);
    ASSERT(strstr(json, "\"type\":\"keyword\"") != NULL);
    /* 文本中的引号必须被转义（JSON 合法） */
    ASSERT(strstr(json, "\\\"quoted\\\"") != NULL);
    /* 整体必须是合法 JSON */
    Arena *a = arena_create(0);
    size_t err = 0;
    RJson *v = rjson_parse(a, json, strlen(json), &err);
    ASSERT_NOT_NULL(v);
    const RJson *blocks = rjson_obj_get(v, "blocks");
    ASSERT_NOT_NULL(blocks);
    ASSERT_EQ_INT(RJSON_ARRAY, blocks->type);
    ASSERT(blocks->u.arr.count >= 3);
    arena_destroy(a);
    free(json);
}

TEST(render_markdown_basic) {
    const char *md = "# Title\n\nParagraph text.\n\n```c\nint x = 1;\n```\n\n> quote\n\n- item";
    RkRenderDoc doc;
    int rc = rk_render_markdown(md, strlen(md), &doc);
    ASSERT_EQ_INT(0, rc);
    ASSERT(doc.count >= 5);
    /* heading */
    ASSERT_EQ_INT(RK_BLOCK_HEADING, doc.blocks[0].type);
    ASSERT_EQ_INT(1, doc.blocks[0].level);
    ASSERT(strcmp(doc.blocks[0].text, "Title") == 0);
    /* paragraph */
    ASSERT_EQ_INT(RK_BLOCK_TEXT, doc.blocks[1].type);
    ASSERT(strstr(doc.blocks[1].text, "Paragraph") != NULL);
    /* code block */
    int found_code = 0;
    for (size_t i = 0; i < doc.count; i++) {
        if (doc.blocks[i].type == RK_BLOCK_CODE) {
            found_code = 1;
            ASSERT(strstr(doc.blocks[i].text, "int x") != NULL);
            /* 高亮 tokens */
            ASSERT(doc.blocks[i].hl_count > 0);
        }
    }
    ASSERT(found_code);
    /* quote */
    int found_quote = 0;
    for (size_t i = 0; i < doc.count; i++) {
        if (doc.blocks[i].type == RK_BLOCK_QUOTE) found_quote = 1;
    }
    ASSERT(found_quote);
    /* list item */
    int found_list = 0;
    for (size_t i = 0; i < doc.count; i++) {
        if (doc.blocks[i].type == RK_BLOCK_LIST_ITEM) found_list = 1;
    }
    ASSERT(found_list);
    rk_render_doc_free(&doc);
}

TEST(render_empty) {
    RkRenderDoc doc;
    int rc = rk_render_markdown("", 0, &doc);
    ASSERT_EQ_INT(0, rc);
    ASSERT_EQ_SIZE(0, doc.count);
    rk_render_doc_free(&doc);
}

int run_render_suite(void) {
    const RikkaTest tests[] = {
        RIKKA_TEST_REGISTER(render, render_markdown_basic),
        RIKKA_TEST_REGISTER(render, render_empty),
        RIKKA_TEST_REGISTER(render, render_json_protocol),
    };
    return run_suite("render", tests, sizeof(tests) / sizeof(tests[0]));
}
