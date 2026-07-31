#include "rikka/data/lru.h"
#include <stdlib.h>
#include <string.h>

typedef struct RkLruEntry RkLruEntry;
struct RkLruEntry {
    uint64_t hash;
    uint32_t key_len, value_len;
    char *key;
    uint8_t *value;
    RkLruEntry *prev, *next;    /* LRU 链表（head = 最新） */
    RkLruEntry *hnext;          /* 哈希链 */
};

struct RkLru {
    size_t max_entries, max_bytes;
    size_t cur_bytes, count;
    RkLruEntry **buckets;
    size_t nbuckets;
    RkLruEntry *head, *tail;
};

static uint64_t fnv1a(const void *key, size_t len) {
    const uint8_t *p = (const uint8_t *)key;
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

RkLru *rk_lru_create(size_t max_entries, size_t max_bytes) {
    RkLru *l = (RkLru *)calloc(1, sizeof(RkLru));
    if (!l) return NULL;
    l->max_entries = max_entries ? max_entries : 1024;
    l->max_bytes = max_bytes ? max_bytes : (64u << 20);
    l->nbuckets = 256;
    l->buckets = (RkLruEntry **)calloc(l->nbuckets, sizeof(RkLruEntry *));
    if (!l->buckets) { free(l); return NULL; }
    return l;
}

static void entry_free(RkLruEntry *e) {
    free(e->key);
    free(e->value);
    free(e);
}

void rk_lru_clear(RkLru *l) {
    RkLruEntry *e = l->head;
    while (e) {
        RkLruEntry *n = e->next;
        entry_free(e);
        e = n;
    }
    l->head = l->tail = NULL;
    l->count = l->cur_bytes = 0;
    memset(l->buckets, 0, l->nbuckets * sizeof(RkLruEntry *));
}

void rk_lru_destroy(RkLru *l) {
    if (!l) return;
    rk_lru_clear(l);
    free(l->buckets);
    free(l);
}

static void unlink(RkLru *l, RkLruEntry *e) {
    if (e->prev) e->prev->next = e->next; else l->head = e->next;
    if (e->next) e->next->prev = e->prev; else l->tail = e->prev;
}

static void link_front(RkLru *l, RkLruEntry *e) {
    e->prev = NULL;
    e->next = l->head;
    if (l->head) l->head->prev = e;
    l->head = e;
    if (!l->tail) l->tail = e;
}

static void evict_one(RkLru *l) {
    RkLruEntry *e = l->tail;
    if (!e) return;
    unlink(l, e);
    /* 从哈希桶移除 */
    size_t bi = e->hash & (l->nbuckets - 1);
    RkLruEntry **pp = &l->buckets[bi];
    while (*pp && *pp != e) pp = &(*pp)->hnext;
    if (*pp) *pp = e->hnext;
    l->cur_bytes -= e->value_len + e->key_len + sizeof(RkLruEntry);
    l->count--;
    entry_free(e);
}

int rk_lru_put(RkLru *l, const void *key, size_t key_len,
               const void *value, size_t value_len) {
    uint64_t h = fnv1a(key, key_len);
    size_t bi = h & (l->nbuckets - 1);
    /* 已存在则替换 */
    for (RkLruEntry *e = l->buckets[bi]; e; e = e->hnext) {
        if (e->hash == h && e->key_len == key_len &&
            memcmp(e->key, key, key_len) == 0) {
            /* 值替换 */
            l->cur_bytes -= e->value_len;
            uint8_t *nv = (uint8_t *)malloc(value_len ? value_len : 1);
            if (!nv) return -1;
            if (value_len) memcpy(nv, value, value_len);
            free(e->value);
            e->value = nv;
            e->value_len = (uint32_t)value_len;
            l->cur_bytes += value_len;
            /* 提升到最新 */
            if (l->head != e) { unlink(l, e); link_front(l, e); }
            return 0;
        }
    }
    if (value_len > l->max_bytes) return -1;
    RkLruEntry *e = (RkLruEntry *)calloc(1, sizeof(RkLruEntry));
    if (!e) return -1;
    e->hash = h;
    e->key_len = (uint32_t)key_len;
    e->value_len = (uint32_t)value_len;
    e->key = (char *)malloc(key_len ? key_len : 1);
    e->value = (uint8_t *)malloc(value_len ? value_len : 1);
    if (!e->key || !e->value) { entry_free(e); return -1; }
    if (key_len) memcpy(e->key, key, key_len);
    if (value_len) memcpy(e->value, value, value_len);
    e->hnext = l->buckets[bi];
    l->buckets[bi] = e;
    link_front(l, e);
    l->cur_bytes += value_len + key_len + sizeof(RkLruEntry);
    l->count++;
    /* 容量淘汰 */
    while (l->count > l->max_entries || l->cur_bytes > l->max_bytes) evict_one(l);
    return 0;
}

const void *rk_lru_get(RkLru *l, const void *key, size_t key_len, size_t *value_len) {
    uint64_t h = fnv1a(key, key_len);
    size_t bi = h & (l->nbuckets - 1);
    for (RkLruEntry *e = l->buckets[bi]; e; e = e->hnext) {
        if (e->hash == h && e->key_len == key_len &&
            memcmp(e->key, key, key_len) == 0) {
            if (l->head != e) { unlink(l, e); link_front(l, e); }
            if (value_len) *value_len = e->value_len;
            return e->value;
        }
    }
    return NULL;
}

size_t rk_lru_count(const RkLru *l) { return l->count; }
size_t rk_lru_bytes(const RkLru *l) { return l->cur_bytes; }
