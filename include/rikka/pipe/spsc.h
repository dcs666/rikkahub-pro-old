#ifndef RIKKA_PIPE_SPSC_H
#define RIKKA_PIPE_SPSC_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/*
 * SPSC（单生产者单消费者）无锁环形字节队列（S5 无锁流水线基础）。
 * - head 仅生产者写，tail 仅消费者写：无锁无竞争
 * - 内存序：push 用 release（写完数据再发布 head），pop 用 acquire
 * - 固定容量环形缓冲，数据块 = 长度前缀 + 内容
 *
 * 对标 JVM 版协程链串行（SSE 读→JSON→合并→渲染→DB 同线程串行）；
 * 本队列让各阶段独立线程并行，吞吐 = 阶段并行、延迟不叠加。
 */

typedef struct {
    uint8_t *buf;
    size_t cap;      /* 环形缓冲总大小 */
    size_t head;     /* 生产者写位（仅写线程访问） */
    size_t tail;     /* 消费者读位（仅读线程访问） */
    int closed;
} RkSpsc;

void rk_spsc_init(RkSpsc *q, size_t cap);
void rk_spsc_destroy(RkSpsc *q);

/* 入队（写线程）：复制 data 到环形缓冲。返回 0 成功，-1 满/已关闭 */
int rk_spsc_push(RkSpsc *q, const void *data, size_t len);

/* 出队（读线程）：拷贝到 out（cap 容量）。返回 >0 字节数；0 = 已关闭且空；-1 = 空（等待后重试） */
ssize_t rk_spsc_pop(RkSpsc *q, void *out, size_t cap);

/* 关闭（写线程最后调用）：关闭后 pop 读完剩余返回 0 */
void rk_spsc_close(RkSpsc *q);

size_t rk_spsc_used(const RkSpsc *q);

#endif /* RIKKA_PIPE_SPSC_H */
