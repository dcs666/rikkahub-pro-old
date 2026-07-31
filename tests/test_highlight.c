#include "test.h"
#include "rikka/highlight/highlight.h"
#include <string.h>

/* 找指定 offset 的 token */
static const RikkaHlToken *tok_at(const RikkaHlToken *t, size_t n, size_t off) {
    for (size_t i = 0; i < n; i++)
        if (t[i].start <= off && off < t[i].start + t[i].len) return &t[i];
    return NULL;
}

TEST(c_lang) {
    const char *code =
        "#include <stdio.h>\n"
        "int main(void) {\n"
        "    // comment\n"
        "    int x = 42;\n"
        "    printf(\"hi %d\", x);\n"
        "    return 0;\n"
        "}\n";
    RikkaHlToken toks[128];
    size_t n = rikka_hl_tokenize("c", code, strlen(code), toks, 128);
    ASSERT(n > 0);
    /* #include → PREPROC（offset 0） */
    const RikkaHlToken *t0 = &toks[0];
    ASSERT_EQ_INT(RIKKA_HL_PREPROC, t0->type);
    /* 行2 "int main(void) {" 起始 19 */
    const RikkaHlToken *ti = tok_at(toks, n, 19);
    ASSERT_NOT_NULL(ti);
    ASSERT_EQ_INT(RIKKA_HL_TYPE, ti->type);      /* int @19 */
    const RikkaHlToken *tm = tok_at(toks, n, 23);
    ASSERT_NOT_NULL(tm);
    ASSERT_EQ_INT(RIKKA_HL_FUNC, tm->type);      /* main @23 */
    /* 行3 "    // comment"，// @40 */
    const RikkaHlToken *tc = tok_at(toks, n, 40);
    ASSERT_NOT_NULL(tc);
    ASSERT_EQ_INT(RIKKA_HL_COMMENT, tc->type);
    /* 行4 "    int x = 42;" 起始 51，int @55，42 @63 */
    const RikkaHlToken *tn = tok_at(toks, n, 63);
    ASSERT_NOT_NULL(tn);
    ASSERT_EQ_INT(RIKKA_HL_NUMBER, tn->type);
    /* 行5 printf @71，字符串 @78 */
    const RikkaHlToken *tp = tok_at(toks, n, 71);
    ASSERT_NOT_NULL(tp);
    ASSERT_EQ_INT(RIKKA_HL_BUILTIN, tp->type);
    const RikkaHlToken *ts = tok_at(toks, n, 78);
    ASSERT_NOT_NULL(ts);
    ASSERT_EQ_INT(RIKKA_HL_STRING, ts->type);
    /* 行6 return @95 */
    const RikkaHlToken *tr = tok_at(toks, n, 95);
    ASSERT_NOT_NULL(tr);
    ASSERT_EQ_INT(RIKKA_HL_KEYWORD, tr->type);
}

TEST(python_lang) {
    const char *code =
        "# comment\n"
        "def hello(name):\n"
        "    print('hi', name)\n"
        "    return len(name)\n";
    RikkaHlToken toks[128];
    size_t n = rikka_hl_tokenize("python", code, strlen(code), toks, 128);
    ASSERT(n > 0);
    ASSERT_EQ_INT(RIKKA_HL_COMMENT, toks[0].type);
    const RikkaHlToken *td = tok_at(toks, n, 10);
    ASSERT_NOT_NULL(td);
    ASSERT_EQ_INT(RIKKA_HL_KEYWORD, td->type); /* def */
    const RikkaHlToken *tp = tok_at(toks, n, 31);
    ASSERT_NOT_NULL(tp);
    ASSERT_EQ_INT(RIKKA_HL_BUILTIN, tp->type); /* print @31 */
    const RikkaHlToken *ts = tok_at(toks, n, 37);
    ASSERT_NOT_NULL(ts);
    ASSERT_EQ_INT(RIKKA_HL_STRING, ts->type); /* 'hi' @37 */
}

TEST(json_lang) {
    const char *code = "{\"key\": true, \"n\": 42.5, \"nil\": null}";
    RikkaHlToken toks[64];
    size_t n = rikka_hl_tokenize("json", code, strlen(code), toks, 64);
    ASSERT(n > 0);
    const RikkaHlToken *tk = tok_at(toks, n, 1);
    ASSERT_NOT_NULL(tk);
    ASSERT_EQ_INT(RIKKA_HL_STRING, tk->type);
    const RikkaHlToken *tb = tok_at(toks, n, 9);
    ASSERT_NOT_NULL(tb);
    ASSERT_EQ_INT(RIKKA_HL_KEYWORD, tb->type); /* true */
    const RikkaHlToken *tn = tok_at(toks, n, 20);
    ASSERT_NOT_NULL(tn);
    ASSERT_EQ_INT(RIKKA_HL_NUMBER, tn->type); /* 42.5 */
}

TEST(html_lang) {
    const char *code = "<!-- c --><div class=\"box\">text</div>";
    RikkaHlToken toks[64];
    size_t n = rikka_hl_tokenize("html", code, strlen(code), toks, 64);
    ASSERT(n > 0);
    ASSERT_EQ_INT(RIKKA_HL_COMMENT, toks[0].type);
    const RikkaHlToken *td = tok_at(toks, n, 10);
    ASSERT_NOT_NULL(td);
    ASSERT_EQ_INT(RIKKA_HL_TAG, td->type); /* <div */
    const RikkaHlToken *ta = tok_at(toks, n, 15);
    ASSERT_NOT_NULL(ta);
    ASSERT_EQ_INT(RIKKA_HL_ATTR, ta->type); /* class */
    const RikkaHlToken *ts = tok_at(toks, n, 22);
    ASSERT_NOT_NULL(ts);
    ASSERT_EQ_INT(RIKKA_HL_STRING, ts->type); /* "box" */
}

TEST(unknown_lang_plain) {
    const char *code = "int x = 1; // anything";
    RikkaHlToken toks[8];
    size_t n = rikka_hl_tokenize("brainfuck", code, strlen(code), toks, 8);
    ASSERT_EQ_SIZE(1, n);
    ASSERT_EQ_INT(RIKKA_HL_PLAIN, toks[0].type);
    ASSERT_EQ_SIZE(strlen(code), toks[0].len);
}

TEST(escape_in_string) {
    const char *code = "char *s = \"a\\\"b\"; // end";
    RikkaHlToken toks[32];
    size_t n = rikka_hl_tokenize("c", code, strlen(code), toks, 32);
    ASSERT(n > 0);
    /* "a\"b" 是一个完整字符串（含转义引号），不应提前结束 */
    const RikkaHlToken *ts = tok_at(toks, n, 11);
    ASSERT_NOT_NULL(ts);
    ASSERT_EQ_INT(RIKKA_HL_STRING, ts->type);
    /* 转义引号位置 13 仍在该字符串内 */
    const RikkaHlToken *te = tok_at(toks, n, 13);
    ASSERT(te == ts);
}

TEST(cap_overflow) {
    /* token 数超过 cap 时安全截断 */
    const char *code = "aaa bbb ccc ddd eee fff";
    RikkaHlToken toks[4];
    size_t n = rikka_hl_tokenize("c", code, strlen(code), toks, 4);
    ASSERT_EQ_SIZE(4, n);
}

TEST(go_backtick) {
    const char *code = "s := `raw string`\n";
    RikkaHlToken toks[32];
    size_t n = rikka_hl_tokenize("go", code, strlen(code), toks, 32);
    ASSERT(n > 0);
    const RikkaHlToken *ts = tok_at(toks, n, 6);
    ASSERT_NOT_NULL(ts);
    ASSERT_EQ_INT(RIKKA_HL_STRING, ts->type);
}

int run_highlight_suite(void) {
    const RikkaTest tests[] = {
        RIKKA_TEST_REGISTER(highlight, c_lang),
        RIKKA_TEST_REGISTER(highlight, python_lang),
        RIKKA_TEST_REGISTER(highlight, json_lang),
        RIKKA_TEST_REGISTER(highlight, html_lang),
        RIKKA_TEST_REGISTER(highlight, unknown_lang_plain),
        RIKKA_TEST_REGISTER(highlight, escape_in_string),
        RIKKA_TEST_REGISTER(highlight, cap_overflow),
        RIKKA_TEST_REGISTER(highlight, go_backtick),
    };
    return run_suite("highlight", tests, sizeof(tests) / sizeof(tests[0]));
}
