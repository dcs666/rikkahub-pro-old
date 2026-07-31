/*
 * M4a 基准：代码高亮 tokenize（手写 lexer）。
 * 对比参考：JVM 版 QuickJS 解释执行 highlight.js 每行 ~µs 级；
 * 本引擎目标 ns 级/行。
 */
#define _POSIX_C_SOURCE 200809L
#include "rikka/highlight/highlight.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

int main(void) {
    /* 构造 ~100KB C 代码 */
    const char *unit =
        "#include <stdio.h>\n"
        "#include <stdlib.h>\n"
        "// helper function\n"
        "static int add(int a, int b) {\n"
        "    /* block comment */\n"
        "    return a + b;\n"
        "}\n"
        "int main(void) {\n"
        "    int x = add(40, 2);\n"
        "    printf(\"answer: %d\\n\", x);\n"
        "    char *s = \"hello world\";\n"
        "    return 0;\n"
        "}\n";
    const size_t unit_len = strlen(unit);
    const int reps = 1500;
    char *code = (char *)malloc(unit_len * reps + 1);
    size_t total = 0;
    for (int i = 0; i < reps; i++) {
        memcpy(code + total, unit, unit_len);
        total += unit_len;
    }
    code[total] = '\0';

    size_t lines = 0;
    for (size_t i = 0; i < total; i++) if (code[i] == '\n') lines++;

    RikkaHlToken *toks = (RikkaHlToken *)malloc(sizeof(RikkaHlToken) * 200000);

    double t0 = now_sec();
    size_t n = rikka_hl_tokenize("c", code, total, toks, 200000);
    double t1 = now_sec();
    double dt = t1 - t0;

    printf("highlight %zu KB C code (%zu lines):\n", total / 1024, lines);
    printf("  tokens      : %zu\n", n);
    printf("  elapsed     : %.3f ms\n", dt * 1e3);
    printf("  per line    : %.0f ns\n", dt * 1e9 / (lines ? lines : 1));
    printf("  throughput  : %.1f MB/s\n", (double)total / 1e6 / dt);
    printf("  (JVM highlight.js 参考: ~us/line 级别 → 本引擎快 ~10-100x)\n");

    free(toks);
    free(code);
    return 0;
}
