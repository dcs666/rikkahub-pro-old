#include "test.h"
#include "rikka/core/message.h"
#include "rikka/util/arena.h"
#include <string.h>

TEST(msg_basic) {
    Arena *a = arena_create(0);
    RikkaMessage *m = rmsg_new(a, RIKKA_ROLE_ASSISTANT);
    ASSERT_NOT_NULL(m);
    ASSERT_EQ_INT(RIKKA_ROLE_ASSISTANT, m->role);
    RikkaPart *p = rmsg_add_part(a, m, RIKKA_PART_TEXT);
    ASSERT_NOT_NULL(p);
    p->data = "hello";
    p->len = 5;
    ASSERT_EQ_SIZE(1, m->part_count);
    ASSERT_EQ_INT(RIKKA_PART_TEXT, m->parts[0].type);
    arena_destroy(a);
}

TEST(stream_text_accumulate) {
    Arena *a = arena_create(0);
    RikkaStream s;
    rstream_init(&s, a, RIKKA_ROLE_ASSISTANT);
    rstream_append_text(&s, "Hello", 5);
    rstream_append_text(&s, " ", 1);
    rstream_append_text(&s, "world", 5);
    ASSERT_EQ_SIZE(11, s.msg->parts[0].len);
    ASSERT(memcmp(s.msg->parts[0].data, "Hello world", 11) == 0);
    rstream_freeze(&s);
    ASSERT(s.msg->frozen);
    ASSERT_EQ_SIZE(11, s.msg->parts[0].len);
    rstream_destroy(&s);
    rmsg_free_bufs(s.msg);
    arena_destroy(a);
}

TEST(stream_text_reasoning_mix) {
    Arena *a = arena_create(0);
    RikkaStream s;
    rstream_init(&s, a, RIKKA_ROLE_ASSISTANT);
    rstream_append_reasoning(&s, "think", 5);
    rstream_append_text(&s, "answer", 6);
    rstream_append_reasoning(&s, "more", 4);
    /* parts: reasoning(think), text(answer), reasoning(think+more) */
    ASSERT_EQ_SIZE(3, s.msg->part_count);
    ASSERT_EQ_INT(RIKKA_PART_REASONING, s.msg->parts[0].type);
    ASSERT_EQ_SIZE(5, s.msg->parts[0].len);
    ASSERT_EQ_INT(RIKKA_PART_TEXT, s.msg->parts[1].type);
    ASSERT_EQ_SIZE(6, s.msg->parts[1].len);
    ASSERT_EQ_INT(RIKKA_PART_REASONING, s.msg->parts[2].type);
    ASSERT_EQ_SIZE(9, s.msg->parts[2].len);
    ASSERT(memcmp(s.msg->parts[2].data, "thinkmore", 9) == 0);
    rstream_freeze(&s);
    rstream_destroy(&s);
    rmsg_free_bufs(s.msg);
    arena_destroy(a);
}

TEST(stream_large_correctness) {
    Arena *a = arena_create(0);
    RikkaStream s;
    rstream_init(&s, a, RIKKA_ROLE_ASSISTANT);
    /* 10 万 token，每 token 32 字节模式字符 */
    char token[32];
    for (int i = 0; i < 32; i++) token[i] = (char)('a' + (i % 26));
    for (int i = 0; i < 100000; i++) rstream_append_text(&s, token, sizeof(token));
    ASSERT_EQ_SIZE(3200000, s.msg->parts[0].len);
    /* 抽查首尾 */
    ASSERT(s.msg->parts[0].data[0] == 'a');
    ASSERT(s.msg->parts[0].data[3199999] == token[31]);
    rstream_freeze(&s);
    /* 冻结后数据仍有效（owned_buf 转移） */
    ASSERT_EQ_SIZE(3200000, s.msg->parts[0].len);
    ASSERT(s.msg->parts[0].data[100] == token[100 % 32]);
    rstream_destroy(&s);
    rmsg_free_bufs(s.msg);
    arena_destroy(a);
}

TEST(cow_fork_shared_prefix) {
    Arena *a = arena_create(0);
    RConversation c;
    rconv_init(&c, a);

    RikkaMessage *m1 = rmsg_new(a, RIKKA_ROLE_USER);
    RikkaPart *p1 = rmsg_add_part(a, m1, RIKKA_PART_TEXT);
    p1->data = "q1"; p1->len = 2;
    RNode *n1 = rconv_append(&c, m1);

    RikkaMessage *m2 = rmsg_new(a, RIKKA_ROLE_ASSISTANT);
    RikkaPart *p2 = rmsg_add_part(a, m2, RIKKA_PART_TEXT);
    p2->data = "a1"; p2->len = 2;
    RNode *n2 = rconv_append(&c, m2);

    /* 从 n1 处 regenerate：fork 共享前缀 */
    RNode *n3 = rconv_regenerate(&c, n1);
    ASSERT_NOT_NULL(n3);
    ASSERT(n3->parent == n1);
    ASSERT_EQ_SIZE(2, n1->child_count); /* n1 现有 n2 + n3 两个子 */
    /* n1 的 children 应包含 n2 和 n3 */
    ASSERT(n1->children[0] == n2 || n1->children[1] == n2);
    ASSERT(n1->children[0] == n3 || n1->children[1] == n3);
    ASSERT(c.active == n3);
    ASSERT(n2->active == 0);
    ASSERT(n3->active == 1);
    /* 旧分支消息不变（COW 零复制） */
    ASSERT(rnode_message(n2)->parts[0].data[0] == 'a');

    /* 切换回 n2 */
    rconv_set_active(&c, n2);
    ASSERT(c.active == n2);
    ASSERT(n2->active == 1);
    ASSERT(n3->active == 0);
    rconv_destroy(&c);
    arena_destroy(a);
}

TEST(active_messages_chain) {
    Arena *a = arena_create(0);
    RConversation c;
    rconv_init(&c, a);
    for (int i = 0; i < 5; i++) {
        RikkaMessage *m = rmsg_new(a, RIKKA_ROLE_USER);
        RikkaPart *p = rmsg_add_part(a, m, RIKKA_PART_TEXT);
        p->data = "m"; p->len = 1;
        rconv_append(&c, m);
    }
    const RikkaMessage *out[16];
    size_t n = rconv_active_messages(&c, out, 16);
    ASSERT_EQ_SIZE(5, n);
    for (size_t i = 0; i < n; i++) ASSERT_NOT_NULL(out[i]);
    rconv_destroy(&c);
    arena_destroy(a);
}

TEST(freeze_reasoning_survives_destroy) {
    /* 回归: freeze 后 destroy, reasoning part 数据不得悬垂 */
    Arena *a = arena_create(0);
    RikkaStream s;
    rstream_init(&s, a, RIKKA_ROLE_ASSISTANT);
    rstream_append_reasoning(&s, "think-step-1", 12);
    rstream_append_text(&s, "answer", 6);
    rstream_freeze(&s);
    rstream_destroy(&s);
    int found = 0;
    for (size_t i = 0; i < s.msg->part_count; i++) {
        RikkaPart *p = &s.msg->parts[i];
        if (p->type == RIKKA_PART_REASONING) {
            found = 1;
            ASSERT_EQ_SIZE(12, p->len);
            ASSERT(memcmp(p->data, "think-step-1", 12) == 0);
        }
    }
    ASSERT(found);
    rmsg_free_bufs(s.msg); /* 断言结束后再释放 owned 缓冲 */
    arena_destroy(a);
}

TEST(freeze_empty_text_removed) {
    Arena *a = arena_create(0);
    RikkaStream s;
    rstream_init(&s, a, RIKKA_ROLE_ASSISTANT);
    rstream_append_text(&s, "x", 1);
    rstream_freeze(&s);
    /* 非空保留 */
    ASSERT_EQ_SIZE(1, s.msg->part_count);
    rstream_destroy(&s);
    rmsg_free_bufs(s.msg);

    RikkaStream s2;
    rstream_init(&s2, a, RIKKA_ROLE_ASSISTANT);
    rstream_append_text(&s2, "", 0); /* 空 append 不建 part */
    rstream_freeze(&s2);
    ASSERT_EQ_SIZE(0, s2.msg->part_count);
    rstream_destroy(&s2);
    rmsg_free_bufs(s2.msg);
    arena_destroy(a);
}

int run_message_suite(void) {
    const RikkaTest tests[] = {
        RIKKA_TEST_REGISTER(message, msg_basic),
        RIKKA_TEST_REGISTER(message, stream_text_accumulate),
        RIKKA_TEST_REGISTER(message, stream_text_reasoning_mix),
        RIKKA_TEST_REGISTER(message, stream_large_correctness),
        RIKKA_TEST_REGISTER(message, cow_fork_shared_prefix),
        RIKKA_TEST_REGISTER(message, active_messages_chain),
        RIKKA_TEST_REGISTER(message, freeze_reasoning_survives_destroy),
        RIKKA_TEST_REGISTER(message, freeze_empty_text_removed),
    };
    return run_suite("message", tests, sizeof(tests) / sizeof(tests[0]));
}
