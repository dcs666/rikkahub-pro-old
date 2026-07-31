#include "test.h"
#include "rikka/trace/trace.h"
#include "rikka/data/rbin.h"
#include "rikka/core/message.h"
#include "rikka/util/arena.h"
#include <string.h>
#include <unistd.h>

TEST(trace_basic) {
    RkTracer t;
    rk_trace_init(&t);
    rk_trace_enable(&t, 1);
    size_t s1 = rk_trace_begin(&t, "span1");
    ASSERT(s1 != SIZE_MAX);
    usleep(1000); /* 1ms */
    rk_trace_end(&t, s1);
    size_t s2 = rk_trace_begin(&t, "span2");
    rk_trace_end(&t, s2);
    ASSERT_EQ_SIZE(2, t.count);
    ASSERT(t.spans[0].finished);
    ASSERT(t.spans[0].end_ns > t.spans[0].start_ns);
    /* dump 不崩溃 */
    FILE *devnull = fopen("/dev/null", "w");
    rk_trace_dump(&t, devnull);
    fclose(devnull);
    rk_trace_destroy(&t);
}

TEST(trace_disabled_noop) {
    RkTracer t;
    rk_trace_init(&t);
    /* disabled: begin 返回 SIZE_MAX */
    size_t s = rk_trace_begin(&t, "noop");
    ASSERT_EQ_SIZE(SIZE_MAX, s);
    rk_trace_end(&t, s); /* no-op */
    ASSERT_EQ_SIZE(0, t.count);
    rk_trace_destroy(&t);
}

TEST(trace_rbin_integration) {
    RkTracer t;
    rk_trace_init(&t);
    rk_trace_enable(&t, 1);
    rk_trace_set_global(&t);

    Arena *a = arena_create(0);
    RConversation c;
    rconv_init(&c, a);
    RikkaMessage *m = rmsg_new(a, RIKKA_ROLE_USER);
    RikkaPart *p = rmsg_add_part(a, m, RIKKA_PART_TEXT);
    p->data = "test";
    p->len = 4;
    rconv_append(&c, m);
    Buf bin;
    buf_init(&bin);
    rbin_save(&c, &bin);
    Arena *a2 = arena_create(0);
    RikkaMessage **msgs = NULL;
    size_t n = 0;
    rbin_load(bin.data, bin.len, a2, &msgs, &n);
    /* trace 应记录 rbin_save + rbin_load */
    ASSERT(t.count >= 2);
    int found_save = 0, found_load = 0;
    for (size_t i = 0; i < t.count; i++) {
        if (strcmp(t.spans[i].name, "rbin_save") == 0) found_save = 1;
        if (strcmp(t.spans[i].name, "rbin_load") == 0) found_load = 1;
    }
    ASSERT(found_save && found_load);
    buf_free(&bin);
    rconv_destroy(&c);
    arena_destroy(a);
    arena_destroy(a2);
    rk_trace_set_global(NULL);
    rk_trace_destroy(&t);
}

int run_trace_suite(void) {
    const RikkaTest tests[] = {
        RIKKA_TEST_REGISTER(trace, trace_basic),
        RIKKA_TEST_REGISTER(trace, trace_disabled_noop),
        RIKKA_TEST_REGISTER(trace, trace_rbin_integration),
    };
    return run_suite("trace", tests, sizeof(tests) / sizeof(tests[0]));
}
