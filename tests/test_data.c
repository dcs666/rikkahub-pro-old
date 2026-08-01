#include "test.h"
#include "rikka/data/rbin.h"
#include "rikka/data/lru.h"
#include "rikka/data/index.h"
#include "rikka/core/buffer.h"
#include <string.h>
#include <unistd.h>

/* ---------- rbin ---------- */

static RikkaMessage *mk_msg(Arena *a, RikkaRole role, const char *text) {
    RikkaMessage *m = rmsg_new(a, role);
    RikkaPart *p = rmsg_add_part(a, m, RIKKA_PART_TEXT);
    p->data = text;
    p->len = strlen(text);
    return m;
}

TEST(rbin_roundtrip) {
    Arena *a = arena_create(0);
    RConversation c;
    rconv_init(&c, a);
    RikkaMessage *m1 = mk_msg(a, RIKKA_ROLE_USER, "hello 世界");
    rconv_append(&c, m1);
    RikkaMessage *m2 = rmsg_new(a, RIKKA_ROLE_ASSISTANT);
    RikkaPart *p = rmsg_add_part(a, m2, RIKKA_PART_TOOL_CALL);
    p->tool_id = "call_9";
    p->tool_name = "search";
    p->data = "{\"q\":\"x\"}";
    p->len = strlen(p->data);
    rconv_append(&c, m2);
    RikkaMessage *m3 = mk_msg(a, RIKKA_ROLE_TOOL, "result text");
    rconv_append(&c, m3);

    Buf bin;
    buf_init(&bin);
    ASSERT_EQ_INT(0, rbin_save(&c, &bin));

    /* 加载到新 arena */
    Arena *a2 = arena_create(0);
    RikkaMessage **msgs = NULL;
    size_t n = 0;
    ASSERT_EQ_INT(0, rbin_load(bin.data, bin.len, a2, &msgs, &n));
    ASSERT_EQ_SIZE(3, n);
    ASSERT_EQ_INT(RIKKA_ROLE_USER, msgs[0]->role);
    ASSERT_EQ_SIZE(12, msgs[0]->parts[0].len);
    ASSERT(memcmp(msgs[0]->parts[0].data, "hello 世界", 12) == 0);
    ASSERT_EQ_INT(RIKKA_PART_TOOL_CALL, msgs[1]->parts[0].type);
    ASSERT(memcmp(msgs[1]->parts[0].tool_id, "call_9", 6) == 0);
    ASSERT(memcmp(msgs[1]->parts[0].tool_name, "search", 6) == 0);
    ASSERT_EQ_INT(RIKKA_ROLE_TOOL, msgs[2]->role);
    ASSERT_EQ_SIZE(11, msgs[2]->parts[0].len);

    buf_free(&bin);
    rconv_destroy(&c);
    arena_destroy(a);
    arena_destroy(a2);
}

TEST(rbin_truncation_guard) {
    /* 回归: 合法文件每字节截断, 解析必须返回错误不崩溃 */
    Arena *a = arena_create(0);
    RConversation c;
    rconv_init(&c, a);
    for (int i = 0; i < 20; i++) {
        /* RikkaPart.data 零拷贝引用调用方内存——文本必须放入 arena 生命周期 */
        char *s = arena_alloc(a, 1, 32);
        ASSERT_NOT_NULL(s);
        snprintf(s, 32, "msg %d with data", i);
        RikkaMessage *m = mk_msg(a, RIKKA_ROLE_USER, s);
        rconv_append(&c, m);
    }
    Buf bin;
    buf_init(&bin);
    ASSERT_EQ_INT(0, rbin_save(&c, &bin));
    for (size_t cut = 0; cut < bin.len; cut += 3) {
        Arena *a2 = arena_create(0);
        RikkaMessage **msgs = NULL;
        size_t n = 0;
        int rc = rbin_load(bin.data, cut, a2, &msgs, &n);
        ASSERT(rc != 0); /* 截断必须报错 */
        arena_destroy(a2);
    }
    /* 完整加载仍成功 */
    Arena *a3 = arena_create(0);
    RikkaMessage **msgs = NULL;
    size_t n = 0;
    ASSERT_EQ_INT(0, rbin_load(bin.data, bin.len, a3, &msgs, &n));
    ASSERT_EQ_SIZE(20, n);
    buf_free(&bin);
    rconv_destroy(&c);
    arena_destroy(a);
    arena_destroy(a3);
}

TEST(rbin_bad_magic) {
    Arena *a = arena_create(0);
    const char *bad = "NOTRIKKABIN";
    RikkaMessage **msgs = NULL;
    size_t n = 0;
    ASSERT_EQ_INT(-1, rbin_load((const uint8_t *)bad, 11, a, &msgs, &n));
    arena_destroy(a);
}

TEST(rbin_file_snapshot) {
    Arena *a = arena_create(0);
    RConversation c;
    rconv_init(&c, a);
    for (int i = 0; i < 100; i++) {
        /* 零拷贝契约：文本放入 arena */
        char *s = arena_alloc(a, 1, 64);
        ASSERT_NOT_NULL(s);
        snprintf(s, 64, "message number %d", i);
        RikkaMessage *m = mk_msg(a, RIKKA_ROLE_USER, s);
        rconv_append(&c, m);
    }
    const char *path = "/tmp/rbin_test.bin";
    ASSERT_EQ_INT(0, rbin_save_file(&c, path));

    /* mmap 加载（零拷贝） */
    const uint8_t *data = NULL;
    size_t len = 0;
    ASSERT_EQ_INT(0, rbin_mmap_file(path, &data, &len));
    Arena *a2 = arena_create(0);
    RikkaMessage **msgs = NULL;
    size_t n = 0;
    ASSERT_EQ_INT(0, rbin_parse(data, len, a2, &msgs, &n));
    ASSERT_EQ_SIZE(100, n);
    ASSERT(memcmp(msgs[99]->parts[0].data, "message number 99", 17) == 0);
    rbin_munmap(data, len);
    rconv_destroy(&c);
    arena_destroy(a);
    arena_destroy(a2);
    unlink(path);
}

/* ---------- lru ---------- */

TEST(lru_basic) {
    RkLru *l = rk_lru_create(16, 1 << 20);
    const char *v1 = "value-one";
    ASSERT_EQ_INT(0, rk_lru_put(l, "k1", 2, v1, strlen(v1) + 1));
    size_t vlen = 0;
    const void *got = rk_lru_get(l, "k1", 2, &vlen);
    ASSERT_NOT_NULL(got);
    ASSERT(strcmp((const char *)got, "value-one") == 0);
    ASSERT_EQ_SIZE(0, rk_lru_get(l, "missing", 7, NULL) ? 1 : 0);
    /* 覆盖 */
    ASSERT_EQ_INT(0, rk_lru_put(l, "k1", 2, "new", 4));
    got = rk_lru_get(l, "k1", 2, &vlen);
    ASSERT(memcmp(got, "new", 3) == 0);
    rk_lru_destroy(l);
}

TEST(lru_eviction) {
    RkLru *l = rk_lru_create(3, 1 << 20);
    for (int i = 0; i < 5; i++) {
        char k[8], v[8];
        snprintf(k, sizeof(k), "k%d", i);
        snprintf(v, sizeof(v), "v%d", i);
        rk_lru_put(l, k, strlen(k), v, strlen(v) + 1);
    }
    ASSERT_EQ_SIZE(3, rk_lru_count(l));
    /* 最旧的 k0/k1 被淘汰 */
    ASSERT_EQ_SIZE(0, rk_lru_get(l, "k0", 2, NULL) ? 1 : 0);
    ASSERT_EQ_SIZE(0, rk_lru_get(l, "k1", 2, NULL) ? 1 : 0);
    /* 最新的 k2-k4 在 */
    ASSERT_NOT_NULL(rk_lru_get(l, "k2", 2, NULL));
    ASSERT_NOT_NULL(rk_lru_get(l, "k4", 2, NULL));
    /* 访问 k2 提升后，k3 变最旧 → k5 挤掉 k3 */
    rk_lru_get(l, "k2", 2, NULL);
    rk_lru_put(l, "k5", 2, "v5", 3);
    ASSERT_EQ_SIZE(0, rk_lru_get(l, "k3", 2, NULL) ? 1 : 0);
    rk_lru_destroy(l);
}

TEST(lru_lru_order_on_get) {
    RkLru *l = rk_lru_create(2, 1 << 20);
    rk_lru_put(l, "a", 1, "1", 2);
    rk_lru_put(l, "b", 1, "2", 2);
    rk_lru_get(l, "a", 1, NULL);   /* a 提升为最新 */
    rk_lru_put(l, "c", 1, "3", 2); /* 挤掉 b */
    ASSERT_NOT_NULL(rk_lru_get(l, "a", 1, NULL));
    ASSERT_EQ_SIZE(0, rk_lru_get(l, "b", 1, NULL) ? 1 : 0);
    rk_lru_destroy(l);
}

/* ---------- index ---------- */

struct C { int n; };
static void tok_n(void *ctx, const char *t, size_t tl) {
    (void)t; (void)tl;
    ((struct C *)ctx)->n++;
}

TEST(index_tokenize) {
    /* 中文 bigram + ASCII 词 */
    const char *text = "hello世界world";
    int toks = 0;
    struct C c = {0};
    (void)toks;
    rk_tokenize(text, strlen(text), tok_n, &c);
    /* "hello" + "世界"→"世界"(2字节bigram=1) + "world" = 3 */
    ASSERT_EQ_INT(3, c.n);
}

TEST(index_search) {
    RkIndex *ix = rk_index_create();
    rk_index_add(ix, 1, "the quick brown fox", strlen("the quick brown fox"));
    rk_index_add(ix, 2, "the lazy dog", strlen("the lazy dog"));
    rk_index_add(ix, 3, "quick fox jumps", strlen("quick fox jumps"));
    rk_index_add(ix, 4, "你好世界 测试文本", strlen("你好世界 测试文本"));

    uint64_t out[8];
    /* 单词 */
    size_t n = rk_index_search(ix, "quick", 5, out, 8);
    ASSERT_EQ_SIZE(2, n); /* doc 1, 3 */
    ASSERT(out[0] == 1 && out[1] == 3);
    /* AND */
    n = rk_index_search(ix, "quick fox", 9, out, 8);
    ASSERT_EQ_SIZE(2, n); /* doc 1, 3 */
    /* 中文 */
    n = rk_index_search(ix, "世界", strlen("世界"), out, 8);
    ASSERT_EQ_SIZE(1, n);
    ASSERT(out[0] == 4);
    /* remove_doc: 删除后不再命中 */
    rk_index_remove_doc(ix, 1);
    n = rk_index_search(ix, "quick", 5, out, 8);
    ASSERT_EQ_SIZE(1, n);
    ASSERT(out[0] == 3);
    rk_index_remove_doc(ix, 3);
    n = rk_index_search(ix, "quick", 5, out, 8);
    ASSERT_EQ_SIZE(0, n);
    /* 无命中 */
    n = rk_index_search(ix, "nonexistent", 11, out, 8);
    ASSERT_EQ_SIZE(0, n);
    /* 混合：有未索引 token 的 AND 必须空 */
    n = rk_index_search(ix, "quick nonexistent", 17, out, 8);
    ASSERT_EQ_SIZE(0, n);
    rk_index_destroy(ix);
}

int run_data_suite(void) {
    const RikkaTest tests[] = {
        RIKKA_TEST_REGISTER(data, rbin_roundtrip),
        RIKKA_TEST_REGISTER(data, rbin_bad_magic),
        RIKKA_TEST_REGISTER(data, rbin_truncation_guard),
        RIKKA_TEST_REGISTER(data, rbin_file_snapshot),
        RIKKA_TEST_REGISTER(data, lru_basic),
        RIKKA_TEST_REGISTER(data, lru_eviction),
        RIKKA_TEST_REGISTER(data, lru_lru_order_on_get),
        RIKKA_TEST_REGISTER(data, index_tokenize),
        RIKKA_TEST_REGISTER(data, index_search),
    };
    return run_suite("data", tests, sizeof(tests) / sizeof(tests[0]));
}
