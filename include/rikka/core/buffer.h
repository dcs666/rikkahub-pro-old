#ifndef RIKKA_CORE_BUFFER_H
#define RIKKA_CORE_BUFFER_H

#include <stddef.h>
#include <stdint.h>

/*
 * Buf: 可复用动态字节缓冲。
 * 零拷贝流式管线的基础：append 到同一缓冲累积，reset 保留容量复用，
 * 避免流式场景每 token 分配/释放。len/cap 按需增长（倍增），
 * 数据区始终可被外部直接读写（用于增量解析器透传）。
 */
typedef struct {
    uint8_t *data;
    size_t   len;
    size_t   cap;
} Buf;

void  buf_init(Buf *b);
void  buf_reserve(Buf *b, size_t extra);          /* 保证至少可再写 extra 字节 */
void  buf_append(Buf *b, const void *src, size_t n);
void  buf_append_str(Buf *b, const char *s);       /* 不含 NUL */
void  buf_append_byte(Buf *b, uint8_t c);
void  buf_reset(Buf *b);                           /* len=0，保留容量复用 */
void  buf_free(Buf *b);
int   buf_equal(const Buf *a, const Buf *b);

#endif /* RIKKA_CORE_BUFFER_H */
