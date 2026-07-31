#include "rikka/util/arena.h"
#include <stdlib.h>
#include <string.h>

#define ARENA_DEFAULT_BLOCK 65536

typedef struct Block {
    struct Block *next;
    size_t        used;
    size_t        cap;
    /* 数据紧随块头 */
} Block;

struct Arena {
    Block *head;       /* 当前分配块 */
    Block *first;      /* 首块（reset 时从此复用） */
    size_t block_size;
};

Arena *arena_create(size_t block_size) {
    if (block_size == 0) block_size = ARENA_DEFAULT_BLOCK;
    Arena *a = (Arena *)calloc(1, sizeof(Arena));
    if (!a) return NULL;
    a->block_size = block_size;
    return a;
}

static Block *alloc_block(Arena *a) {
    size_t blk = sizeof(Block) + a->block_size;
    Block *b = (Block *)malloc(blk);
    if (!b) return NULL;
    b->next = NULL;
    b->used = sizeof(Block);
    b->cap  = blk;
    return b;
}

void *arena_alloc(Arena *a, size_t align, size_t size) {
    if (!a) return NULL;
    if (align < 8) align = 8;
    if (size == 0) size = 1;
    if (!a->head) {
        a->head = alloc_block(a);
        if (!a->head) return NULL;
        a->first = a->head;
    }
    Block *b = a->head;
    /* 对齐当前指针 */
    uintptr_t p = (uintptr_t)b + b->used;
    uintptr_t aligned = (p + align - 1) & ~((uintptr_t)align - 1);
    size_t off = (size_t)(aligned - (uintptr_t)b);
    if (off + size > b->cap) {
        /* 新块；若首块之后已有更大块，优先复用（reset 后） */
        Block *nb = NULL;
        for (Block *it = b->next; it; it = it->next) {
            uintptr_t ip = (uintptr_t)it + it->used;
            uintptr_t ia = (ip + align - 1) & ~((uintptr_t)align - 1);
            if ((size_t)(ia - (uintptr_t)it) + size <= it->cap) {
                nb = it;
                break;
            }
        }
        if (!nb) {
            nb = alloc_block(a);
            if (!nb) return NULL;
            nb->next = a->head->next; /* 插入为 head，保持首块顺序 */
            a->head->next = nb;
        }
        b = nb;
        a->head = b;
    }
    uintptr_t base = (uintptr_t)b + b->used;
    uintptr_t out  = (base + align - 1) & ~((uintptr_t)align - 1);
    b->used = (size_t)(out - (uintptr_t)b) + size;
    return (void *)out;
}

void *arena_alloc0(Arena *a, size_t align, size_t size) {
    void *p = arena_alloc(a, align, size);
    if (p) memset(p, 0, size);
    return p;
}

void arena_reset(Arena *a) {
    if (!a) return;
    a->head = a->first;
    for (Block *b = a->first; b; b = b->next) b->used = sizeof(Block);
}

size_t arena_used(const Arena *a) {
    if (!a) return 0;
    size_t total = 0;
    for (const Block *b = a->first; b; b = b->next) {
        total += b->used - sizeof(Block);
    }
    return total;
}

void arena_destroy(Arena *a) {
    if (!a) return;
    Block *b = a->first;
    while (b) {
        Block *n = b->next;
        free(b);
        b = n;
    }
    free(a);
}
