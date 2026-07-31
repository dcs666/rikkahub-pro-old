#define _POSIX_C_SOURCE 200809L
#include "rikka/trace/trace.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

static RkTracer *g_tracer = NULL;

void rk_trace_set_global(RkTracer *t) { g_tracer = t; }
RkTracer *rk_trace_get_global(void) { return g_tracer; }

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

void rk_trace_init(RkTracer *t) {
    memset(t, 0, sizeof(RkTracer));
    t->enabled = 0;
}

void rk_trace_destroy(RkTracer *t) {
    free(t->spans);
    t->spans = NULL;
    t->count = t->cap = 0;
}

void rk_trace_enable(RkTracer *t, int enabled) {
    t->enabled = enabled;
}

size_t rk_trace_begin(RkTracer *t, const char *name) {
    if (!t || !t->enabled) return SIZE_MAX;
    if (t->count == t->cap) {
        size_t nc = t->cap ? t->cap * 2 : 64;
        if (nc > SIZE_MAX / sizeof(RkSpan)) return SIZE_MAX;
        RkSpan *ns = (RkSpan *)realloc(t->spans, nc * sizeof(RkSpan));
        if (!ns) return SIZE_MAX;
        t->spans = ns;
        t->cap = nc;
    }
    size_t id = t->count++;
    t->spans[id].name = name;
    t->spans[id].start_ns = now_ns();
    t->spans[id].end_ns = 0;
    t->spans[id].finished = 0;
    return id;
}

void rk_trace_end(RkTracer *t, size_t span_id) {
    if (!t || !t->enabled || span_id == SIZE_MAX || span_id >= t->count) return;
    t->spans[span_id].end_ns = now_ns();
    t->spans[span_id].finished = 1;
}

static int span_cmp(const void *a, const void *b) {
    const RkSpan *sa = (const RkSpan *)a;
    const RkSpan *sb = (const RkSpan *)b;
    if (sa->start_ns < sb->start_ns) return -1;
    if (sa->start_ns > sb->start_ns) return 1;
    return 0;
}

void rk_trace_dump(const RkTracer *t, FILE *out) {
    if (!t->enabled || t->count == 0) return;
    /* 按 start 排序（不修改原数组） */
    RkSpan *sorted = (RkSpan *)malloc(t->count * sizeof(RkSpan));
    if (!sorted) return;
    memcpy(sorted, t->spans, t->count * sizeof(RkSpan));
    qsort(sorted, t->count, sizeof(RkSpan), span_cmp);
    fprintf(out, "=== Trace Report (%zu spans) ===\n", t->count);
    for (size_t i = 0; i < t->count; i++) {
        RkSpan *s = &sorted[i];
        if (s->finished) {
            uint64_t dur_us = (s->end_ns - s->start_ns) / 1000;
            fprintf(out, "  %-30s %8llu us\n", s->name, (unsigned long long)dur_us);
        } else {
            fprintf(out, "  %-30s (unfinished)\n", s->name);
        }
    }
    free(sorted);
}
