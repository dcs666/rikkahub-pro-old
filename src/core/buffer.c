#include "rikka/core/buffer.h"
#include <stdlib.h>
#include <string.h>

static size_t grow_cap(size_t need) {
    size_t cap = 64;
    while (cap < need) {
        if (cap > (SIZE_MAX / 2)) return need; /* 防倍增溢出死循环 */
        cap *= 2;
    }
    return cap;
}

void buf_init(Buf *b) {
    b->data = NULL;
    b->len  = 0;
    b->cap  = 0;
}

int buf_reserve(Buf *b, size_t extra) {
    size_t need = b->len + extra;
    if (need <= b->cap) return 0;
    size_t nc = grow_cap(need);
    uint8_t *nd = (uint8_t *)realloc(b->data, nc);
    if (!nd) { /* 保持原状，调用方需处理 */
        return -1;
    }
    b->data = nd;
    b->cap  = nc;
    return 0;
}

void buf_append(Buf *b, const void *src, size_t n) {
    if (n == 0) return;
    if (buf_reserve(b, n) != 0) return; /* OOM: 丢弃本次追加，绝不越界写 */
    memcpy(b->data + b->len, src, n);
    b->len += n;
}

void buf_append_str(Buf *b, const char *s) {
    buf_append(b, s, strlen(s));
}

void buf_append_byte(Buf *b, uint8_t c) {
    buf_reserve(b, 1);
    b->data[b->len++] = c;
}

void buf_reset(Buf *b) {
    b->len = 0;
}

void buf_free(Buf *b) {
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

int buf_equal(const Buf *a, const Buf *b) {
    if (a->len != b->len) return 0;
    return memcmp(a->data, b->data, a->len) == 0;
}
