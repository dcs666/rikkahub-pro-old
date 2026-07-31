#define _POSIX_C_SOURCE 200809L
#include "rikka/data/rbin.h"
#include "rikka/core/buffer.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define RBIN_MAGIC "RIKKABIN"
#define RBIN_VERSION 1

/* ---------- 小端读写 ---------- */

static void wr_u8(Buf *b, uint8_t v) { buf_append_byte(b, v); }
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
static uint32_t rd_u32(Rd *r) {
    if (r->off + 4 > r->len) { r->err = 1; return 0; }
    uint32_t v = (uint32_t)r->p[r->off] | ((uint32_t)r->p[r->off + 1] << 8) |
                 ((uint32_t)r->p[r->off + 2] << 16) | ((uint32_t)r->p[r->off + 3] << 24);
    r->off += 4;
    return v;
}
static uint64_t rd_u64(Rd *r) {
    if (r->off + 8 > r->len) { r->err = 1; return 0; }
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= ((uint64_t)r->p[r->off + i]) << (8 * i);
    r->off += 8;
    return v;
}
static const char *rd_bytes(Rd *r, size_t *n) {
    uint32_t l = rd_u32(r);
    if (r->err || r->off + l > r->len) { r->err = 1; *n = 0; return NULL; }
    const char *s = (const char *)r->p + r->off;
    r->off += l;
    *n = l;
    return s;
}

/* ---------- 序列化 ---------- */

static void wr_part(Buf *b, const RikkaPart *p) {
    wr_u8(b, (uint8_t)p->type);
    if (p->len > 0xFFFFFFFFu) return; /* 超长截断防御（>4GB part 不合法） */
    wr_bytes(b, p->data, p->len);
    wr_u8(b, p->tool_name ? 1 : 0);
    if (p->tool_name) wr_bytes(b, p->tool_name, strlen(p->tool_name));
    wr_u8(b, p->tool_id ? 1 : 0);
    if (p->tool_id) wr_bytes(b, p->tool_id, strlen(p->tool_id));
    wr_u8(b, (uint8_t)p->is_error);
}

int rbin_save(const RConversation *c, Buf *out) {
    if (!c || !out) return -1;
    size_t depth = 0;
    for (RNode *cur = c->active; cur && cur->parent; cur = cur->parent) depth++;
    /* 线程局部复用缓冲（高频保存避免每次 malloc） */
    static _Thread_local const RikkaMessage **msgs = NULL;
    static _Thread_local size_t msgs_cap = 0;
    if (depth > msgs_cap) {
        const RikkaMessage **nm = (const RikkaMessage **)realloc((void *)msgs,
                                          depth * sizeof(RikkaMessage *));
        if (!nm) return -1;
        msgs = nm;
        msgs_cap = depth;
    }
    size_t n = rconv_active_messages(c, msgs, depth);
    buf_reset(out);
    buf_append(out, RBIN_MAGIC, 8);
    wr_u32(out, RBIN_VERSION);
    wr_u32(out, (uint32_t)n);
    for (size_t i = 0; i < n; i++) {
        const RikkaMessage *m = msgs[i];
        wr_u8(out, (uint8_t)m->role);
        wr_u32(out, (uint32_t)m->part_count);
        for (size_t j = 0; j < m->part_count; j++) wr_part(out, &m->parts[j]);
        wr_u8(out, (uint8_t)m->has_usage);
        if (m->has_usage) {
            wr_u64(out, m->prompt_tokens);
            wr_u64(out, m->completion_tokens);
            wr_u64(out, m->total_tokens);
        }
    }
    return 0;
}

/* ---------- 反序列化（零拷贝：parts 指向输入） ---------- */

static int parse_msg(Rd *r, Arena *a, RikkaMessage *m) {
    m->role = (RikkaRole)rd_u8(r);
    uint32_t pc = rd_u32(r);
    if (r->err) return -1;
    for (uint32_t i = 0; i < pc; i++) {
        RikkaPart *p = rmsg_add_part(a, m, RIKKA_PART_TEXT);
        if (!p) return -1;
        p->type = (RikkaPartType)rd_u8(r);
        p->data = rd_bytes(r, &p->len);
        if (rd_u8(r)) {
            size_t nl = 0;
            const char *s = rd_bytes(r, &nl);
            if (nl) { p->tool_name = s; }
        }
        if (rd_u8(r)) {
            size_t nl = 0;
            const char *s = rd_bytes(r, &nl);
            if (nl) { p->tool_id = s; }
        }
        p->is_error = (int)rd_u8(r);
        if (r->err) return -1;
    }
    if (rd_u8(r)) {
        m->has_usage = 1;
        m->prompt_tokens = rd_u64(r);
        m->completion_tokens = rd_u64(r);
        m->total_tokens = rd_u64(r);
    }
    if (r->err) return -1;
    m->frozen = 1;
    return 0;
}

int rbin_parse(const uint8_t *data, size_t len, Arena *arena,
               RikkaMessage ***msgs_out, size_t *count_out) {
    Rd r;
    r.p = data;
    r.len = len;
    r.off = 0;
    r.err = 0;
    if (len < 12 || memcmp(data, RBIN_MAGIC, 8) != 0) return -1;
    r.off = 8;
    if (rd_u32(&r) != RBIN_VERSION) return -1;
    uint32_t n = rd_u32(&r);
    if (r.err) return -1;
    if (n > len / 2) return -1; /* 每条消息至少 ~2 字节：防恶意 count 超大分配 */
    RikkaMessage **msgs = (RikkaMessage **)arena_alloc0(arena, 8, n * sizeof(RikkaMessage *));
    if (!msgs) return -1;
    for (uint32_t i = 0; i < n; i++) {
        RikkaMessage *m = rmsg_new(arena, RIKKA_ROLE_USER);
        if (!m) return -1;
        if (parse_msg(&r, arena, m) != 0) return -1;
        msgs[i] = m;
    }
    if (msgs_out) *msgs_out = msgs;
    if (count_out) *count_out = n;
    return 0;
}

int rbin_load(const uint8_t *data, size_t len, Arena *arena,
              RikkaMessage ***msgs_out, size_t *count_out) {
    return rbin_parse(data, len, arena, msgs_out, count_out);
}

/* ---------- 文件快照 ---------- */

int rbin_save_file(const RConversation *c, const char *path) {
    Buf out;
    buf_init(&out);
    if (rbin_save(c, &out) != 0) { buf_free(&out); return -1; }
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { buf_free(&out); return -1; }
    size_t off = 0;
    while (off < out.len) {
        ssize_t w = write(fd, (const char *)out.data + off, out.len - off);
        if (w <= 0) { close(fd); unlink(tmp); buf_free(&out); return -1; }
        off += (size_t)w;
    }
    if (fsync(fd) != 0) { close(fd); unlink(tmp); buf_free(&out); return -1; }
    close(fd);
    if (rename(tmp, path) != 0) { unlink(tmp); buf_free(&out); return -1; }
    buf_free(&out);
    return 0;
}

int rbin_mmap_file(const char *path, const uint8_t **data_out, size_t *len_out) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0) { close(fd); return -1; }
    void *p = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (p == MAP_FAILED) return -1;
    if (data_out) *data_out = (const uint8_t *)p;
    if (len_out) *len_out = (size_t)st.st_size;
    return 0;
}

void rbin_munmap(const uint8_t *data, size_t len) {
    if (data && len) munmap((void *)data, len);
}
