#ifndef RIKKA_TRACE_TRACE_H
#define RIKKA_TRACE_TRACE_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/*
 * 可观测性 trace：轻量级 span 追踪（性能分析/调试）。
 * 关键路径埋点：JSON 解析、MD 解析、序列化、流式请求。
 * 零开销设计：disabled 时 begin/end 是 no-op。
 */

typedef struct {
    const char *name;
    uint64_t start_ns;
    uint64_t end_ns;
    int finished;
} RkSpan;

typedef struct {
    RkSpan *spans;
    size_t count, cap;
    int enabled;
} RkTracer;

void rk_trace_init(RkTracer *t);
void rk_trace_destroy(RkTracer *t);
void rk_trace_enable(RkTracer *t, int enabled);

/* 开始 span，返回 span id（disabled 时返回 SIZE_MAX） */
size_t rk_trace_begin(RkTracer *t, const char *name);
void rk_trace_end(RkTracer *t, size_t span_id);

/* 输出 trace 报告（name + duration，按 start 排序） */
void rk_trace_dump(const RkTracer *t, FILE *out);

/* 全局 tracer（可选，关键路径自动埋点） */
void rk_trace_set_global(RkTracer *t);
RkTracer *rk_trace_get_global(void);

/* 便捷宏：自动作用域 span */
#define RK_TRACE_SCOPE(tracer, name) \
    size_t _rk_span = rk_trace_begin(tracer, name); \
    for (int _rk_guard = 1; _rk_guard; _rk_guard = 0, rk_trace_end(tracer, _rk_span))

#endif /* RIKKA_TRACE_TRACE_H */
