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

int run_pipe_suite(void);
int run_gateway_suite(void);
int run_render_suite(void);
int run_audio_suite(void);
int run_workspace_suite(void);
int run_mcp_suite(void);
int run_epub_suite(void);
int run_docx_suite(void);
int run_trace_suite(void);
int run_data_suite(void);
int run_md_suite(void);
int run_highlight_suite(void);
int run_provider_suite(void);
int run_http_suite(void);
int run_message_suite(void);
int run_json_suite(void);
int run_buffer_suite(void);
int run_arena_suite(void);
int run_log_suite(void);

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0); /* 卡死时也能看到进度 */
    int failed = 0;
    failed |= run_buffer_suite();
    failed |= run_arena_suite();
    failed |= run_log_suite();
    failed |= run_json_suite();
    failed |= run_message_suite();
    failed |= run_http_suite();
    failed |= run_provider_suite();
    failed |= run_highlight_suite();
    failed |= run_md_suite();
    failed |= run_data_suite();
    failed |= run_trace_suite();
    failed |= run_docx_suite();
    failed |= run_epub_suite();
    failed |= run_mcp_suite();
    failed |= run_workspace_suite();
    failed |= run_audio_suite();
    failed |= run_render_suite();
    failed |= run_gateway_suite();
    failed |= run_pipe_suite();
    if (failed == 0) {
        printf("\nALL SUITES PASSED\n");
        return 0;
    }
    printf("\nSOME SUITES FAILED\n");
    return 1;
}
