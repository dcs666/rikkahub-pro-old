#ifndef RIKKA_TEST_H
#define RIKKA_TEST_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    const char *name;
    void      (*fn)(void);
} RikkaTest;

#define TEST(name) static void test_##name(void)
#define RIKKA_TEST_REGISTER(suite, name) { #name, test_##name }

#define ASSERT(cond) do {                                                   \
    if (!(cond)) {                                                          \
        fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
        exit(1);                                                            \
    }                                                                       \
} while (0)

#define ASSERT_EQ_INT(a, b) do {                                            \
    long long _a = (long long)(a), _b = (long long)(b);                     \
    if (_a != _b) {                                                         \
        fprintf(stderr, "  FAIL %s:%d: %s(%lld) != %s(%lld)\n",             \
                __FILE__, __LINE__, #a, _a, #b, _b);                        \
        exit(1);                                                            \
    }                                                                       \
} while (0)

#define ASSERT_EQ_SIZE(a, b) ASSERT_EQ_INT((size_t)(a), (size_t)(b))
#define ASSERT_NULL(p)   ASSERT((p) == NULL)
#define ASSERT_NOT_NULL(p) ASSERT((p) != NULL)
#define ASSERT_TRUE(c)   ASSERT(c)
#define ASSERT_FALSE(c)  ASSERT(!(c))

/* 每 suite 提供 run_suite()：返回失败数（0 成功） */
int run_suite(const char *suite_name, const RikkaTest *tests, size_t count);

#endif /* RIKKA_TEST_H */
