#ifndef RIKKA_UTIL_ARENA_H
#define RIKKA_UTIL_ARENA_H

#include <stddef.h>
#include <stdint.h>

/*
 * Arena: 区域分配器。
 * 流式管线"无 GC 停顿"的基础：所有每 token 临时对象从 arena 分配，
 * 生成结束时一次 arena_reset 批量回收，块内存保留复用，零碎片。
 * 非线程安全（每个流水线阶段各持一个）。
 */
typedef struct Arena Arena;

Arena *arena_create(size_t block_size);           /* 0 = 默认 64KB */
void  *arena_alloc(Arena *a, size_t align, size_t size);
void  *arena_alloc0(Arena *a, size_t align, size_t size);
void   arena_reset(Arena *a);                     /* 逻辑清空，块保留复用 */
size_t arena_used(const Arena *a);                /* 当前已用字节（跨块） */
void   arena_destroy(Arena *a);

#endif /* RIKKA_UTIL_ARENA_H */
