#include "test.h"
#include <stdio.h>

int run_suite(const char *suite_name, const RikkaTest *tests, size_t count) {
    int passed = 0;
    for (size_t i = 0; i < count; i++) {
        printf("[RUN ] %s::%s\n", suite_name, tests[i].name);
        tests[i].fn();
        printf("[ OK ] %s::%s\n", suite_name, tests[i].name);
        passed++;
    }
    printf("suite %s: %d/%zu passed\n", suite_name, passed, count);
    return passed == (int)count ? 0 : 1;
}

int run_message_suite(void);
int run_json_suite(void);
int run_buffer_suite(void);
int run_arena_suite(void);
int run_log_suite(void);

int main(void) {
    int failed = 0;
    failed |= run_buffer_suite();
    failed |= run_arena_suite();
    failed |= run_log_suite();
    failed |= run_json_suite();
    failed |= run_message_suite();
    if (failed == 0) {
        printf("\nALL SUITES PASSED\n");
        return 0;
    }
    printf("\nSOME SUITES FAILED\n");
    return 1;
}
