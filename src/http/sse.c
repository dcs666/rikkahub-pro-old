#include "rikka/http/sse.h"
#include <stdlib.h>
#include <string.h>

#define SSE_LINE_MAX 8192

struct RsseParser {
    RsseEventCb cb;
    void *ctx;

    /* 行缓冲 */
    char *line;
    size_t line_len, line_cap;

    /* 当前事件累积 */
    char event[64];
    Buf data;
    char id[128];
    long long retry_ms;
    int have_retry;
};

RsseParser *rsse_create(RsseEventCb cb, void *ctx) {
    RsseParser *p = (RsseParser *)calloc(1, sizeof(RsseParser));
    if (!p) return NULL;
    p->cb = cb;
    p->ctx = ctx;
    buf_init(&p->data);
    return p;
}

void rsse_destroy(RsseParser *p) {
    if (!p) return;
    free(p->line);
    buf_free(&p->data);
    free(p);
}

void rsse_reset(RsseParser *p) {
    p->line_len = 0;
    p->event[0] = '\0';
    buf_reset(&p->data);
    p->id[0] = '\0';
    p->retry_ms = 0;
    p->have_retry = 0;
}

static void dispatch(RsseParser *p) {
    if (p->cb) {
        p->cb(p->ctx, p->event[0] ? p->event : "message",
              (const char *)p->data.data, p->data.len,
              p->id, p->have_retry ? p->retry_ms : -1);
    }
    /* 复位事件累积（id 保留——SSE 规范：未设 id 沿用上一个） */
    p->event[0] = '\0';
    buf_reset(&p->data);
    p->have_retry = 0;
}

static void handle_line(RsseParser *p, const char *line, size_t len) {
    if (len == 0) { dispatch(p); return; }        /* 空行 = 事件结束 */
    if (line[0] == ':') return;                   /* 注释 */
    /* 找冒号 */
    const char *colon = memchr(line, ':', len);
    const char *field;
    size_t field_len;
    const char *value;
    size_t value_len;
    if (colon) {
        field = line;
        field_len = (size_t)(colon - line);
        value = colon + 1;
        value_len = len - field_len - 1;
        if (value_len > 0 && value[0] == ' ') { value++; value_len--; } /* 剥一个前导空格 */
    } else {
        field = line;
        field_len = len;
        value = line + len;
        value_len = 0;
    }
    if (field_len == 4 && memcmp(field, "data", 4) == 0) {
        if (p->data.len > 0) buf_append_byte(&p->data, '\n'); /* 多行拼接 */
        buf_append(&p->data, value, value_len);
    } else if (field_len == 5 && memcmp(field, "event", 5) == 0) {
        size_t n = value_len < sizeof(p->event) - 1 ? value_len : sizeof(p->event) - 1;
        memcpy(p->event, value, n);
        p->event[n] = '\0';
    } else if (field_len == 2 && memcmp(field, "id", 2) == 0) {
        size_t n = value_len < sizeof(p->id) - 1 ? value_len : sizeof(p->id) - 1;
        memcpy(p->id, value, n);
        p->id[n] = '\0';
    } else if (field_len == 5 && memcmp(field, "retry", 5) == 0) {
        p->retry_ms = 0;
        for (size_t i = 0; i < value_len; i++) {
            if (value[i] < '0' || value[i] > '9') break;
            p->retry_ms = p->retry_ms * 10 + (value[i] - '0');
        }
        p->have_retry = 1;
    }
    /* 未知字段忽略 */
}

int rsse_feed(RsseParser *p, const char *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        char ch = data[i];
        if (p->line_len + 1 > SSE_LINE_MAX) return -1; /* 行超长 */
        if (ch == '\n') {
            /* 行结束：剥离 \r */
            size_t n = p->line_len;
            if (n > 0 && p->line[n - 1] == '\r') n--;
            handle_line(p, p->line, n);
            p->line_len = 0;
        } else {
            if (p->line_len >= p->line_cap) {
                size_t nc = p->line_cap ? p->line_cap * 2 : 256;
                char *nl = (char *)realloc(p->line, nc);
                if (!nl) return -1;
                p->line = nl;
                p->line_cap = nc;
            }
            p->line[p->line_len++] = ch;
        }
    }
    return 0;
}

void rsse_finish(RsseParser *p) {
    /* 无尾空行的最后一行：处理并 dispatch */
    if (p->line_len > 0) {
        size_t n = p->line_len;
        if (n > 0 && p->line[n - 1] == '\r') n--;
        handle_line(p, p->line, n);
        p->line_len = 0;
    }
    if (p->data.len > 0 || p->event[0]) dispatch(p);
    rsse_reset(p);
}
