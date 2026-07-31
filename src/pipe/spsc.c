#include "rikka/pipe/spsc.h"
#include <stdlib.h>
#include <string.h>

/*
 * head/tail 单调递增（不取模）；环形写位置 = head & mask（cap 为 2 的幂）。
 * 容量 = head - tail（无下溢）。数据块 = u32 长度前缀 + 内容。
 */

#define HDR 4

void rk_spsc_init(RkSpsc *q, size_t cap) {
    size_t c = 1;
    while (c < cap) c <<= 1;
    if (c < 64) c = 64;
    q->buf = (uint8_t *)malloc(c);
    q->cap = c;
    q->head = 0;
    q->tail = 0;
    q->closed = 0;
}

void rk_spsc_destroy(RkSpsc *q) {
    free(q->buf);
    q->buf = NULL;
    q->cap = 0;
}

int rk_spsc_push(RkSpsc *q, const void *data, size_t len) {
    if (q->closed) return -1;
    if (len == 0) return -1; /* 空块禁止：pop 返回 0 的语义是"关闭且空" */
    size_t need = HDR + len;
    if (need > q->cap) return -1;
    size_t used = q->head - __atomic_load_n(&q->tail, __ATOMIC_ACQUIRE);
    if (used + need > q->cap) return -1;
    size_t mask = q->cap - 1;
    size_t h = q->head & mask;
    size_t hw = q->head; /* 单调写位 */
    uint32_t L = (uint32_t)len;
    if (h + need <= q->cap) {
        memcpy(q->buf + h, &L, HDR);
        if (len) memcpy(q->buf + h + HDR, data, len);
    } else {
        size_t p1 = q->cap - h;
        memcpy(q->buf + h, &L, p1 < HDR ? p1 : HDR);
        if (p1 < HDR) memcpy(q->buf, (const uint8_t *)&L + p1, HDR - p1);
        size_t body = (h + HDR) & mask;
        size_t b1 = q->cap - body;
        if (b1 >= len) {
            if (len) memcpy(q->buf + body, data, len);
        } else {
            if (len) {
                memcpy(q->buf + body, data, b1);
                memcpy(q->buf, (const uint8_t *)data + b1, len - b1);
            }
        }
    }
    /* release：数据写完再发布 head */
    __atomic_store_n(&q->head, hw + need, __ATOMIC_RELEASE);
    return 0;
}

ssize_t rk_spsc_pop(RkSpsc *q, void *out, size_t cap) {
    /* 先读 closed 再读 head：closed 的 release 蕴含此前所有 push 的 release，
     * 故 closed=1 时 head 必为最新（不会误判空丢数据） */
    int closed = __atomic_load_n(&q->closed, __ATOMIC_ACQUIRE);
    size_t head = __atomic_load_n(&q->head, __ATOMIC_ACQUIRE);
    size_t used = head - q->tail;
    if (used == 0) return closed ? 0 : -1;
    size_t mask = q->cap - 1;
    size_t t = q->tail & mask;
    uint32_t len;
    if (t + HDR <= q->cap) {
        memcpy(&len, q->buf + t, HDR);
    } else {
        size_t p1 = q->cap - t;
        memcpy(&len, q->buf + t, p1);
        memcpy((uint8_t *)&len + p1, q->buf, HDR - p1);
    }
    if (len > cap) return -1;
    size_t body = (t + HDR) & mask;
    size_t b1 = q->cap - body;
    if (b1 >= len) {
        if (len) memcpy(out, q->buf + body, len);
    } else {
        if (len) {
            memcpy(out, q->buf + body, b1);
            memcpy((uint8_t *)out + b1, q->buf, len - b1);
        }
    }
    __atomic_store_n(&q->tail, q->tail + HDR + len, __ATOMIC_RELEASE);
    return (ssize_t)len;
}

void rk_spsc_close(RkSpsc *q) {
    __atomic_store_n(&q->closed, 1, __ATOMIC_RELEASE);
}

size_t rk_spsc_used(const RkSpsc *q) {
    return q->head - q->tail;
}
