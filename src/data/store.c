/*
 * 轻量记录存储实现（见 store.h 的槽位约定与快照格式）。
 *
 * 内存布局：每类型一个动态数组（插入序）；唯一索引（ref_key / relative_path）
 * 线性扫描（规模假设 <1 万条）。字符串归存储所有（insert/load 时复制）。
 */
#define _POSIX_C_SOURCE 200809L
#include "rikka/data/store.h"
#include "rikka/core/buffer.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define STORE_MAGIC "RKSTORE"
#define STORE_VERSION 1

typedef struct {
    RkEnt *items;
    size_t count, cap;
} EntArray;

struct RkStore {
    EntArray arr[RK_ENT_COUNT];
    int64_t next_id; /* 自增 id 分配器（跨类型共用，对齐 JVM 每表自增即可） */
};

RkStore *rk_store_create(void) {
    RkStore *s = (RkStore *)calloc(1, sizeof(RkStore));
    if (!s) return NULL;
    s->next_id = 1;
    return s;
}

static void ent_free(RkEnt *e) {
    for (int i = 0; i < 8; i++) {
        free((void *)e->s[i]);
        e->s[i] = NULL;
    }
}

void rk_store_destroy(RkStore *s) {
    if (!s) return;
    for (int t = 0; t < RK_ENT_COUNT; t++) {
        for (size_t i = 0; i < s->arr[t].count; i++) ent_free(&s->arr[t].items[i]);
        free(s->arr[t].items);
    }
    free(s);
}

static int arr_push(EntArray *a, const RkEnt *e) {
    if (a->count == a->cap) {
        size_t nc = a->cap ? a->cap * 2 : 16;
        if (nc < a->cap || nc > SIZE_MAX / sizeof(RkEnt)) return -1;
        RkEnt *ni = (RkEnt *)realloc(a->items, nc * sizeof(RkEnt));
        if (!ni) return -1;
        a->items = ni;
        a->cap = nc;
    }
    a->items[a->count] = *e; /* 浅拷贝 */
    /* 深拷贝字符串 */
    for (int i = 0; i < 8; i++) {
        if (e->s[i]) {
            char *copy = strdup(e->s[i]);
            if (!copy) {
                for (int j = 0; j < i; j++) free((void *)a->items[a->count].s[j]);
                a->items[a->count].s[i] = NULL;
                return -1;
            }
            a->items[a->count].s[i] = copy;
        }
    }
    a->count++;
    return 0;
}

/* 唯一约束检查：s 槽位值在类型内唯一（排除 exclude_id；id<=0 表示新记录） */
static int find_str_slot(const RkStore *s, RkEntType t, int slot, const char *val,
                         int64_t exclude_id, size_t *idx_out) {
    const EntArray *a = &s->arr[t];
    for (size_t i = 0; i < a->count; i++) {
        const RkEnt *e = &a->items[i];
        if (e->id == exclude_id) continue;
        if (e->s[slot] && val && strcmp(e->s[slot], val) == 0) {
            if (idx_out) *idx_out = i;
            return 1;
        }
    }
    return 0;
}

int64_t rk_store_insert(RkStore *s, const RkEnt *e) {
    if (!s || !e || e->type < 0 || e->type >= RK_ENT_COUNT) return -1;
    /* 唯一约束 */
    if (e->type == RK_ENT_FAVORITE && e->s[RK_ENT_FAV_REF_KEY]) {
        if (find_str_slot(s, e->type, RK_ENT_FAV_REF_KEY, e->s[RK_ENT_FAV_REF_KEY], 0, NULL)) {
            return -1;
        }
    }
    if (e->type == RK_ENT_MANAGED_FILE && e->s[RK_ENT_FILE_REL_PATH]) {
        if (find_str_slot(s, e->type, RK_ENT_FILE_REL_PATH, e->s[RK_ENT_FILE_REL_PATH], 0, NULL)) {
            return -1;
        }
    }
    RkEnt copy = *e;
    copy.id = s->next_id++;
    if (arr_push(&s->arr[e->type], &copy) != 0) return -1;
    return copy.id;
}

const RkEnt *rk_store_get(RkStore *s, RkEntType t, int64_t id) {
    if (!s || t < 0 || t >= RK_ENT_COUNT) return NULL;
    const EntArray *a = &s->arr[t];
    for (size_t i = 0; i < a->count; i++) {
        if (a->items[i].id == id) return &a->items[i];
    }
    return NULL;
}

void rk_store_ent_free(RkEnt *e) {
    if (!e) return;
    for (int i = 0; i < 8; i++) {
        free((void *)e->s[i]);
        e->s[i] = NULL;
    }
}

int rk_store_get_copy(RkStore *s, RkEntType t, int64_t id, RkEnt *out) {
    const RkEnt *src = rk_store_get(s, t, id);
    if (!src || !out) return -1;
    *out = *src;
    for (int k = 0; k < 8; k++) {
        out->s[k] = src->s[k] ? strdup(src->s[k]) : NULL;
    }
    return 0;
}

int rk_store_update(RkStore *s, const RkEnt *e) {
    if (!s || !e || e->type < 0 || e->type >= RK_ENT_COUNT || e->id <= 0) return -1;
    /* 先深拷贝（e 可能引用存储内部，后续检查/替换期间旧内存安全） */
    RkEnt copy = *e;
    for (int k = 0; k < 8; k++) {
        copy.s[k] = e->s[k] ? strdup(e->s[k]) : NULL;
    }
    /* 唯一约束（排除自身） */
    if (e->type == RK_ENT_FAVORITE && e->s[RK_ENT_FAV_REF_KEY] &&
        find_str_slot(s, e->type, RK_ENT_FAV_REF_KEY, copy.s[RK_ENT_FAV_REF_KEY], e->id, NULL)) {
        goto fail;
    }
    if (e->type == RK_ENT_MANAGED_FILE && e->s[RK_ENT_FILE_REL_PATH] &&
        find_str_slot(s, e->type, RK_ENT_FILE_REL_PATH, copy.s[RK_ENT_FILE_REL_PATH], e->id, NULL)) {
        goto fail;
    }
    EntArray *a = &s->arr[e->type];
    for (size_t i = 0; i < a->count; i++) {
        if (a->items[i].id == e->id) {
            ent_free(&a->items[i]);
            a->items[i] = copy;
            return 0;
        }
    }
fail:
    for (int k = 0; k < 8; k++) free((void *)copy.s[k]);
    return -1;
}

int rk_store_delete(RkStore *s, RkEntType t, int64_t id) {
    if (!s || t < 0 || t >= RK_ENT_COUNT) return -1;
    EntArray *a = &s->arr[t];
    for (size_t i = 0; i < a->count; i++) {
        if (a->items[i].id == id) {
            ent_free(&a->items[i]);
            a->items[i] = a->items[a->count - 1];
            a->count--;
            return 0;
        }
    }
    return -1;
}

size_t rk_store_count(RkStore *s, RkEntType t) {
    if (!s || t < 0 || t >= RK_ENT_COUNT) return 0;
    return s->arr[t].count;
}

const RkEnt *rk_store_at(RkStore *s, RkEntType t, size_t idx) {
    if (!s || t < 0 || t >= RK_ENT_COUNT || idx >= s->arr[t].count) return NULL;
    return &s->arr[t].items[idx];
}

const RkEnt *rk_store_find_str(RkStore *s, RkEntType t, int slot, const char *val) {
    if (!s || t < 0 || t >= RK_ENT_COUNT || slot < 0 || slot >= 8 || !val) return NULL;
    const EntArray *a = &s->arr[t];
    for (size_t i = 0; i < a->count; i++) {
        if (a->items[i].s[slot] && strcmp(a->items[i].s[slot], val) == 0) {
            return &a->items[i];
        }
    }
    return NULL;
}

size_t rk_store_query_i64(RkStore *s, RkEntType t, int slot,
                          int64_t lo, int64_t hi, const RkEnt **out, size_t cap) {
    if (!s || t < 0 || t >= RK_ENT_COUNT || slot < 0 || slot >= 6) return 0;
    const EntArray *a = &s->arr[t];
    size_t n = 0;
    for (size_t i = 0; i < a->count; i++) {
        int64_t v = a->items[i].i[slot];
        if (v >= lo && v <= hi) {
            if (out && n < cap) out[n] = &a->items[i];
            n++;
        }
    }
    return n;
}

/* ---------- 快照 ---------- */

static void wr_u8(Buf *b, uint8_t v) { buf_append_byte(b, v); }
static void wr_u16(Buf *b, uint16_t v) {
    uint8_t t[2] = {(uint8_t)(v & 0xff), (uint8_t)((v >> 8) & 0xff)};
    buf_append(b, t, 2);
}
static void wr_u32(Buf *b, uint32_t v) {
    uint8_t t[4] = {(uint8_t)(v & 0xff), (uint8_t)((v >> 8) & 0xff),
                    (uint8_t)((v >> 16) & 0xff), (uint8_t)((v >> 24) & 0xff)};
    buf_append(b, t, 4);
}
static void wr_u64(Buf *b, uint64_t v) {
    uint8_t t[8];
    for (int i = 0; i < 8; i++) t[i] = (uint8_t)(v >> (8 * i));
    buf_append(b, t, 8);
}
static void wr_bytes(Buf *b, const char *s, size_t n) {
    wr_u32(b, (uint32_t)n);
    if (n) buf_append(b, s, n);
}

typedef struct {
    const uint8_t *p;
    size_t len;
    size_t off;
    int err;
} Rd;

static uint8_t rd_u8(Rd *r) {
    if (r->off + 1 > r->len) { r->err = 1; return 0; }
    return r->p[r->off++];
}
static uint16_t rd_u16(Rd *r) {
    if (r->off + 2 > r->len) { r->err = 1; return 0; }
    uint16_t v = (uint16_t)(r->p[r->off] | (r->p[r->off + 1] << 8));
    r->off += 2;
    return v;
}
static uint32_t rd_u32(Rd *r) {
    if (r->off + 4 > r->len) { r->err = 1; return 0; }
    uint32_t v = (uint32_t)r->p[r->off] | ((uint32_t)r->p[r->off + 1] << 8) |
                 ((uint32_t)r->p[r->off + 2] << 16) | ((uint32_t)r->p[r->off + 3] << 24);
    r->off += 4;
    return v;
}
static int64_t rd_i64(Rd *r) {
    if (r->off + (size_t)8 > r->len) { r->err = 1; return 0; }
    uint64_t v = 0;
    for (size_t i = 0; i < 8; i++) v |= (uint64_t)r->p[r->off + i] << (8 * i);
    r->off += 8;
    return (int64_t)v;
}
static const char *rd_bytes(Rd *r, size_t *n_out) {
    uint32_t n = rd_u32(r);
    if (r->err || r->off + n > r->len) { r->err = 1; return NULL; }
    const char *p = (const char *)r->p + r->off;
    r->off += n;
    if (n_out) *n_out = n;
    return p;
}

int rk_store_save_file(const RkStore *s, const char *path) {
    if (!s || !path) return -1;
    Buf b;
    buf_init(&b);
    buf_append(&b, STORE_MAGIC, 8);
    wr_u32(&b, STORE_VERSION);
    uint32_t mask = 0;
    for (int t = 0; t < RK_ENT_COUNT; t++) {
        if (s->arr[t].count > 0) mask |= (1u << t);
    }
    wr_u32(&b, mask);
    for (int t = 0; t < RK_ENT_COUNT; t++) {
        const EntArray *a = &s->arr[t];
        if (a->count == 0) continue;
        wr_u8(&b, (uint8_t)t);
        wr_u32(&b, (uint32_t)a->count);
        for (size_t i = 0; i < a->count; i++) {
            const RkEnt *e = &a->items[i];
            wr_u64(&b, (uint64_t)e->id);
            uint16_t sm = 0;
            for (int k = 0; k < 8; k++) {
                if (e->s[k]) sm |= (uint16_t)(1u << k);
            }
            for (int k = 0; k < 6; k++) {
                if (e->i[k] != 0) sm |= (uint16_t)(1u << (8 + k));
            }
            wr_u16(&b, sm);
            for (int k = 0; k < 8; k++) {
                if (e->s[k]) wr_bytes(&b, e->s[k], strlen(e->s[k]));
            }
            for (int k = 0; k < 6; k++) {
                if (e->i[k] != 0) wr_u64(&b, (uint64_t)e->i[k]);
            }
        }
    }
    /* 原子写：临时文件 + rename */
    char tmp[1024];
    int n = snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (n <= 0 || (size_t)n >= sizeof(tmp)) { buf_free(&b); return -1; }
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { buf_free(&b); return -1; }
    size_t off = 0;
    while (off < b.len) {
        ssize_t w = write(fd, b.data + off, b.len - off);
        if (w <= 0) { close(fd); unlink(tmp); buf_free(&b); return -1; }
        off += (size_t)w;
    }
    if (fsync(fd) != 0) { close(fd); unlink(tmp); buf_free(&b); return -1; }
    close(fd);
    if (rename(tmp, path) != 0) { unlink(tmp); buf_free(&b); return -1; }
    buf_free(&b);
    return 0;
}

int rk_store_load_file(RkStore *s, const char *path) {
    if (!s || !path) return -1;
    /* 全量替换语义（对齐 JVM 打开应用恢复数据）：清空现有内容 */
    for (int t = 0; t < RK_ENT_COUNT; t++) {
        for (size_t i = 0; i < s->arr[t].count; i++) ent_free(&s->arr[t].items[i]);
        free(s->arr[t].items);
        s->arr[t].items = NULL;
        s->arr[t].count = s->arr[t].cap = 0;
    }
    s->next_id = 1;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < 16 || st.st_size > (off_t)(1 << 30)) {
        close(fd);
        return -1;
    }
    size_t len = (size_t)st.st_size;
    uint8_t *data = (uint8_t *)malloc(len);
    if (!data) { close(fd); return -1; }
    size_t off = 0;
    while (off < len) {
        ssize_t r = read(fd, data + off, len - off);
        if (r <= 0) { free(data); close(fd); return -1; }
        off += (size_t)r;
    }
    close(fd);
    /* 解析 */
    Rd rd = {data, len, 0, 0};
    if (len < 16 || memcmp(data, STORE_MAGIC, 8) != 0) { free(data); return -1; }
    rd.off = 8;
    uint32_t ver = rd_u32(&rd);
    uint32_t mask = rd_u32(&rd);
    if (rd.err || ver != STORE_VERSION) { free(data); return -1; }
    for (int t = 0; t < RK_ENT_COUNT; t++) {
        if (!(mask & (1u << t))) continue;
        uint8_t type = rd_u8(&rd);
        uint32_t count = rd_u32(&rd);
        if (rd.err || type >= RK_ENT_COUNT) { free(data); return -1; }
        for (uint32_t i = 0; i < count; i++) {
            RkEnt e;
            memset(&e, 0, sizeof(e));
            e.type = (RkEntType)type;
            e.id = rd_i64(&rd);
            uint16_t sm = rd_u16(&rd);
            if (rd.err) { free(data); return -1; }
            for (int k = 0; k < 8; k++) {
                if (sm & (1u << k)) {
                    size_t vlen = 0;
                    const char *v = rd_bytes(&rd, &vlen);
                    if (!v || rd.err) { free(data); return -1; }
                    /* 复制（快照数据随后释放；v 非 NUL 结尾，必须用长度） */
                    char *copy = strndup(v, vlen);
                    if (!copy) { free(data); return -1; }
                    e.s[k] = copy;
                }
            }
            for (int k = 0; k < 6; k++) {
                if (sm & (1u << (8 + k))) {
                    e.i[k] = rd_i64(&rd);
                }
            }
            if (rd.err) {
                for (int k = 0; k < 8; k++) free((void *)e.s[k]);
                free(data);
                return -1;
            }
            if (e.id >= s->next_id) s->next_id = e.id + 1;
            if (arr_push(&s->arr[type], &e) != 0) {
                for (int k = 0; k < 8; k++) free((void *)e.s[k]);
                free(data);
                return -1;
            }
            /* arr_push 已深拷贝，释放本地副本（防双重拷贝泄漏） */
            for (int k = 0; k < 8; k++) free((void *)e.s[k]);
        }
    }
    free(data);
    return 0;
}
