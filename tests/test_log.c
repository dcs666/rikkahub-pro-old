#include "test.h"
#include "rikka/util/log.h"
#include <string.h>

TEST(level_filter) {
    rikka_log_clear();
    rikka_log_set_level(RIKKA_LOG_WARN);
    rikka_log_write(RIKKA_LOG_INFO, "should be filtered");
    rikka_log_write(RIKKA_LOG_ERROR, "visible error");
    RikkaLogEntry entries[8];
    size_t n = rikka_log_recent(entries, 8);
    ASSERT_EQ_SIZE(1, n);
    ASSERT(strstr(entries[0].msg, "visible error") != NULL);
    rikka_log_set_level(RIKKA_LOG_TRACE);
}

TEST(ring_overflow_keeps_recent) {
    rikka_log_clear();
    rikka_log_set_level(RIKKA_LOG_TRACE);
    for (int i = 0; i < 250; i++) rikka_log_write(RIKKA_LOG_INFO, "msg %d", i);
    RikkaLogEntry entries[120];
    size_t n = rikka_log_recent(entries, 120);
    /* 环形容量 100，最多返回 100 条；最新的一条应是 msg 249 */
    ASSERT_EQ_SIZE(100, n);
    ASSERT(strstr(entries[0].msg, "249") != NULL);
    rikka_log_clear();
    ASSERT_EQ_SIZE(0, rikka_log_recent(entries, 8));
}

TEST(truncation) {
    rikka_log_clear();
    rikka_log_set_level(RIKKA_LOG_TRACE);
    char big[2048];
    memset(big, 'a', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    rikka_log_write(RIKKA_LOG_WARN, "%s", big);
    RikkaLogEntry e;
    ASSERT_EQ_SIZE(1, rikka_log_recent(&e, 1));
    ASSERT(strlen(e.msg) <= 511);
}

int run_log_suite(void) {
    rikka_log_set_quiet(1); /* 测试静默，避免刷屏 */
    const RikkaTest tests[] = {
        RIKKA_TEST_REGISTER(log, level_filter),
        RIKKA_TEST_REGISTER(log, ring_overflow_keeps_recent),
        RIKKA_TEST_REGISTER(log, truncation),
    };
    return run_suite("log", tests, sizeof(tests) / sizeof(tests[0]));
}
