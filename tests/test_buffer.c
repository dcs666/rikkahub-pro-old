#include "test.h"
#include "rikka/core/buffer.h"
#include <string.h>

TEST(init_empty) {
    Buf b;
    buf_init(&b);
    ASSERT_EQ_SIZE(0, b.len);
    ASSERT_EQ_SIZE(0, b.cap);
    ASSERT_NULL(b.data);
    buf_free(&b);
}

TEST(append_and_read) {
    Buf b;
    buf_init(&b);
    buf_append_str(&b, "hello ");
    buf_append_str(&b, "world");
    ASSERT_EQ_SIZE(11, b.len);
    ASSERT(memcmp(b.data, "hello world", 11) == 0);
    buf_free(&b);
}

TEST(grow_realloc) {
    Buf b;
    buf_init(&b);
    char chunk[1000];
    memset(chunk, 'x', sizeof(chunk));
    for (int i = 0; i < 100; i++) buf_append(&b, chunk, sizeof(chunk));
    ASSERT_EQ_SIZE(100000, b.len);
    ASSERT(b.cap >= b.len);
    /* 抽查首尾字节 */
    ASSERT(b.data[0] == 'x' && b.data[99999] == 'x');
    buf_free(&b);
}

TEST(reset_reuses_capacity) {
    Buf b;
    buf_init(&b);
    buf_append_str(&b, "1234567890");
    size_t cap = b.cap;
    buf_reset(&b);
    ASSERT_EQ_SIZE(0, b.len);
    ASSERT_EQ_SIZE(cap, b.cap);  /* 容量保留 = 复用 */
    ASSERT_NOT_NULL(b.data);
    buf_append_str(&b, "abc");
    ASSERT_EQ_SIZE(3, b.len);
    buf_free(&b);
}

TEST(byte_append) {
    Buf b;
    buf_init(&b);
    for (int i = 0; i < 256; i++) buf_append_byte(&b, (uint8_t)i);
    ASSERT_EQ_SIZE(256, b.len);
    ASSERT(b.data[255] == 255);
    buf_free(&b);
}

TEST(equal) {
    Buf a, b;
    buf_init(&a); buf_init(&b);
    buf_append_str(&a, "same");
    buf_append_str(&b, "same");
    ASSERT(buf_equal(&a, &b));
    buf_append_byte(&b, '!');
    ASSERT(!buf_equal(&a, &b));
    buf_free(&a); buf_free(&b);
}

int run_buffer_suite(void) {
    const RikkaTest tests[] = {
        RIKKA_TEST_REGISTER(buffer, init_empty),
        RIKKA_TEST_REGISTER(buffer, append_and_read),
        RIKKA_TEST_REGISTER(buffer, grow_realloc),
        RIKKA_TEST_REGISTER(buffer, reset_reuses_capacity),
        RIKKA_TEST_REGISTER(buffer, byte_append),
        RIKKA_TEST_REGISTER(buffer, equal),
    };
    return run_suite("buffer", tests, sizeof(tests) / sizeof(tests[0]));
}
