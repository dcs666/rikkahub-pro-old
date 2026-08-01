#include "test.h"
#include <stdio.h>
#include <string.h>

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
int run_gateway_e2e_suite(void);
int run_gateway_suite(void);
int run_render_suite(void);
int run_audio_suite(void);
int run_workspace_suite(void);
int run_mcp_suite(void);
int run_epub_suite(void);
int run_transform_suite(void);
int run_store_suite(void);
int run_tool_suite(void);
int run_prompt_suite(void);
int run_docx_suite(void);
int run_pptx_suite(void);
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

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0); /* 卡死时也能看到进度 */
    /* 可选过滤：./test_runner [suite_name]（定向跑单个套件，如 TSan 局部验证） */
    const char *only = argc > 1 ? argv[1] : NULL;
    struct { const char *name; int (*fn)(void); } suites[] = {
        {"buffer", run_buffer_suite},
        {"arena", run_arena_suite},
        {"log", run_log_suite},
        {"json", run_json_suite},
        {"message", run_message_suite},
        {"http", run_http_suite},
        {"provider", run_provider_suite},
        {"highlight", run_highlight_suite},
        {"md", run_md_suite},
        {"data", run_data_suite},
        {"trace", run_trace_suite},
        {"docx", run_docx_suite},
        {"epub", run_epub_suite},
        {"pptx", run_pptx_suite},
        {"mcp", run_mcp_suite},
        {"transform", run_transform_suite},
        {"store", run_store_suite},
        {"tool", run_tool_suite},
        {"prompt", run_prompt_suite},
        {"workspace", run_workspace_suite},
        {"audio", run_audio_suite},
        {"render", run_render_suite},
        {"gateway", run_gateway_suite},
        {"gateway_e2e", run_gateway_e2e_suite},
        {"pipe", run_pipe_suite},
    };
    int failed = 0, ran = 0;
    for (size_t i = 0; i < sizeof(suites) / sizeof(suites[0]); i++) {
        if (only && strcmp(only, suites[i].name) != 0) continue;
        ran++;
        failed |= suites[i].fn();
    }
    if (only && ran == 0) {
        printf("unknown suite: %s\n", only);
        return 2;
    }
    if (failed == 0) {
        printf("\nALL SUITES PASSED\n");
        return 0;
    }
    printf("\nSOME SUITES FAILED\n");
    return 1;
}
