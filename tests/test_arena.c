#include "test.h"
#include "rikka/util/arena.h"
#include <stdint.h>
#include <string.h>

TEST(alloc_basic) {
    Arena *a = arena_create(1024);
    ASSERT_NOT_NULL(a);
    int *p = (int *)arena_alloc(a, 8, sizeof(int) * 4);
    ASSERT_NOT_NULL(p);
    for (int i = 0; i < 4; i++) p[i] = i * i;
    ASSERT_EQ_INT(9, p[3]);
    arena_destroy(a);
}

TEST(alignment) {
    Arena *a = arena_create(256);
    /* 多种对齐请求应都满足 */
    uint8_t *p1 = (uint8_t *)arena_alloc(a, 8, 1);
    uint16_t *p2 = (uint16_t *)arena_alloc(a, 16, 2);
    uint32_t *p3 = (uint32_t *)arena_alloc(a, 32, 4);
    ASSERT_NOT_NULL(p1); ASSERT_NOT_NULL(p2); ASSERT_NOT_NULL(p3);
    ASSERT(((uintptr_t)p2 & 15) == 0);
    ASSERT(((uintptr_t)p3 & 31) == 0);
    arena_destroy(a);
}

TEST(many_allocations) {
    Arena *a = arena_create(128);
    void *ptrs[1000];
    for (int i = 0; i < 1000; i++) ptrs[i] = arena_alloc(a, 8, 16);
    for (int i = 0; i < 1000; i++) ASSERT_NOT_NULL(ptrs[i]);
    /* 跨块后仍有效：写入再读回 */
    for (int i = 0; i < 1000; i++) {
        uint64_t *v = (uint64_t *)ptrs[i];
        v[0] = (uint64_t)i;
        v[1] = (uint64_t)(i * 2);
    }
    for (int i = 0; i < 1000; i++) {
        uint64_t *v = (uint64_t *)ptrs[i];
        ASSERT_EQ_INT((uint64_t)i, v[0]);
    }
    arena_destroy(a);
}

TEST(reset_reuses_memory) {
    Arena *a = arena_create(4096);
    for (int round = 0; round < 10; round++) {
        for (int i = 0; i < 100; i++) {
            void *p = arena_alloc0(a, 8, 64);
            ASSERT_NOT_NULL(p);
        }
        size_t used = arena_used(a);
        arena_reset(a);
        ASSERT_EQ_SIZE(0, arena_used(a));
        ASSERT(used > 0);
    }
    arena_destroy(a);
}

TEST(alloc0_zeroes) {
    Arena *a = arena_create(256);
    uint8_t *p = (uint8_t *)arena_alloc0(a, 8, 100);
    for (int i = 0; i < 100; i++) ASSERT_EQ_INT(0, p[i]);
    arena_destroy(a);
}

int run_arena_suite(void) {
    const RikkaTest tests[] = {
        RIKKA_TEST_REGISTER(arena, alloc_basic),
        RIKKA_TEST_REGISTER(arena, alignment),
        RIKKA_TEST_REGISTER(arena, many_allocations),
        RIKKA_TEST_REGISTER(arena, reset_reuses_memory),
        RIKKA_TEST_REGISTER(arena, alloc0_zeroes),
    };
    return run_suite("arena", tests, sizeof(tests) / sizeof(tests[0]));
}
