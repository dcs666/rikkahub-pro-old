#ifndef RIKKA_DATA_LRU_H
#define RIKKA_DATA_LRU_H

#include <stddef.h>
#include <stdint.h>

/*
 * 统一两级缓存（A2）：内存 LRU（哈希表 + 双向链表，访问顺序）。
 * 缓存内容为字节块（markdown 解析结果 / 高亮 token / 文件内容 / HTTP 响应）。
 * put 拷贝值，get 返回内部指针（读后失效需重新 get 或拷贝）。
 */

typedef struct RkLru RkLru;

RkLru *rk_lru_create(size_t max_entries, size_t max_bytes);
void rk_lru_destroy(RkLru *l);
void rk_lru_clear(RkLru *l);

/* 插入/更新。返回 0 成功；-1 值过大。 */
int rk_lru_put(RkLru *l, const void *key, size_t key_len,
               const void *value, size_t value_len);

/* 查询（命中则提升为最新）。返回内部指针（NULL = miss）；*value_len 输出。 */
const void *rk_lru_get(RkLru *l, const void *key, size_t key_len, size_t *value_len);

size_t rk_lru_count(const RkLru *l);
size_t rk_lru_bytes(const RkLru *l);

#endif /* RIKKA_DATA_LRU_H */
