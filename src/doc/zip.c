#define _POSIX_C_SOURCE 200809L
#include "zip.h"
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#define ZIP_LOCAL_SIG  0x04034b50
#define ZIP_CENTRAL_SIG 0x02014b50
#define ZIP_EOCD_SIG   0x06054b50

/* 从末尾向前扫 EOCD（zip 允许尾随注释，EOCD 在最后 22+ 字节内） */
static size_t find_eocd(const uint8_t *data, size_t len) {
    if (len < 22) return 0;
    for (size_t i = len - 22; i > 0; i--) {
        if (i + 3 >= len) continue;
        uint32_t sig = data[i] | ((uint32_t)data[i+1] << 8) |
                       ((uint32_t)data[i+2] << 16) | ((uint32_t)data[i+3] << 24);
        if (sig == ZIP_EOCD_SIG) return i;
    }
    return 0;
}

/* 条目匹配回调：返回 1 收集该条目 */
typedef int (*ZipWalkCb)(const char *name, size_t name_len, uint16_t method,
                         uint32_t comp_sz, uint32_t uncomp_sz, void *ctx);

/* 遍历 central directory；匹配条目写入 entries（数据指针一并解析）。 */
static size_t walk_central(const uint8_t *data, size_t len, ZipWalkCb cb, void *ctx,
                           ZipEntry *entries, size_t max_entries) {
    size_t eocd_off = find_eocd(data, len);
    if (eocd_off == 0) return 0;
    uint16_t total_entries = data[eocd_off+10] | ((uint16_t)data[eocd_off+11] << 8);
    uint32_t cd_offset = data[eocd_off+16] | ((uint32_t)data[eocd_off+17] << 8) |
                         ((uint32_t)data[eocd_off+18] << 16) | ((uint32_t)data[eocd_off+19] << 24);
    if (cd_offset >= len) return 0;
    size_t off = cd_offset, count = 0;
    for (uint16_t i = 0; i < total_entries && off + 46 <= len && count < max_entries; i++) {
        uint32_t sig = data[off] | ((uint32_t)data[off+1] << 8) |
                       ((uint32_t)data[off+2] << 16) | ((uint32_t)data[off+3] << 24);
        if (sig != ZIP_CENTRAL_SIG) break;
        uint16_t comp_method = data[off+10] | ((uint16_t)data[off+11] << 8);
        uint32_t comp_sz = data[off+20] | ((uint32_t)data[off+21] << 8) |
                           ((uint32_t)data[off+22] << 16) | ((uint32_t)data[off+23] << 24);
        uint32_t uncomp_sz = data[off+24] | ((uint32_t)data[off+25] << 8) |
                             ((uint32_t)data[off+26] << 16) | ((uint32_t)data[off+27] << 24);
        uint16_t fname_len = data[off+28] | ((uint16_t)data[off+29] << 8);
        uint16_t extra_len = data[off+30] | ((uint16_t)data[off+31] << 8);
        uint16_t comment_len = data[off+32] | ((uint16_t)data[off+33] << 8);
        uint32_t local_off = data[off+42] | ((uint32_t)data[off+43] << 8) |
                             ((uint32_t)data[off+44] << 16) | ((uint32_t)data[off+45] << 24);
        if (fname_len > len - off - 46) break; /* 溢出防护 */
        const char *fname = (const char *)data + off + 46;
        if (cb(fname, fname_len, comp_method, comp_sz, uncomp_sz, ctx)) {
            ZipEntry *e = &entries[count++];
            size_t copy_len = fname_len < sizeof(e->name) - 1 ? fname_len : sizeof(e->name) - 1;
            memcpy(e->name, fname, copy_len);
            e->name[copy_len] = '\0';
            e->comp_size = comp_sz;
            e->uncomp_size = uncomp_sz;
            e->method = comp_method;
            e->data = NULL;
            /* 从 local header 定位压缩数据偏移 */
            if (local_off + 30 <= len) {
                uint16_t l_fn = data[local_off+26] | ((uint16_t)data[local_off+27] << 8);
                uint16_t l_ex = data[local_off+28] | ((uint16_t)data[local_off+29] << 8);
                size_t d = local_off + 30 + l_fn + l_ex;
                if (d <= len && comp_sz <= len - d) e->data = data + d;
            }
        }
        size_t next_off = off + 46 + fname_len + extra_len + comment_len;
        if (next_off <= off) break; /* 溢出防护 */
        off = next_off;
    }
    return count;
}

/* ---- 精确文件名 ---- */

typedef struct {
    const char *name;
    size_t len;
} ExactCtx;

static int exact_cb(const char *name, size_t name_len, uint16_t method,
                    uint32_t comp_sz, uint32_t uncomp_sz, void *v) {
    (void)method; (void)comp_sz; (void)uncomp_sz;
    ExactCtx *c = (ExactCtx *)v;
    return name_len == c->len && memcmp(name, c->name, c->len) == 0;
}

int zip_find(const uint8_t *data, size_t len, const char *name, ZipEntry *out) {
    if (!data || !name || !out) return 0;
    ExactCtx ctx;
    ctx.name = name;
    ctx.len = strlen(name);
    ZipEntry e;
    if (walk_central(data, len, exact_cb, &ctx, &e, 1) == 1) {
        *out = e;
        return 1;
    }
    return 0;
}

/* ---- 谓词匹配 ---- */

typedef struct {
    ZipNameMatch match;
    void *ctx;
} MatchCtx;

static int match_cb(const char *name, size_t name_len, uint16_t method,
                    uint32_t comp_sz, uint32_t uncomp_sz, void *v) {
    (void)method; (void)comp_sz; (void)uncomp_sz;
    MatchCtx *c = (MatchCtx *)v;
    return c->match(name, name_len, c->ctx);
}

size_t zip_find_matching(const uint8_t *data, size_t len, ZipNameMatch match, void *ctx,
                         ZipEntry *entries, size_t max_entries) {
    if (!data || !match || !entries || max_entries == 0) return 0;
    MatchCtx mc;
    mc.match = match;
    mc.ctx = ctx;
    return walk_central(data, len, match_cb, &mc, entries, max_entries);
}

char *zip_inflate(const ZipEntry *e, size_t *out_len) {
    if (!e || !e->data) return NULL;
    char *out = NULL;
    if (e->method == 0) {
        out = (char *)malloc(e->comp_size + 1);
        if (!out) return NULL;
        memcpy(out, e->data, e->comp_size);
        out[e->comp_size] = '\0';
        if (out_len) *out_len = e->comp_size;
    } else if (e->method == 8) {
        out = (char *)malloc(e->uncomp_size + 1);
        if (!out) return NULL;
        z_stream strm;
        memset(&strm, 0, sizeof(strm));
        strm.next_in = (Bytef *)e->data;
        strm.avail_in = e->comp_size;
        strm.next_out = (Bytef *)out;
        strm.avail_out = e->uncomp_size;
        if (inflateInit2(&strm, -15) != Z_OK) { /* raw deflate */
            free(out);
            return NULL;
        }
        int ret = inflate(&strm, Z_FINISH);
        inflateEnd(&strm);
        if (ret != Z_STREAM_END && ret != Z_OK) {
            free(out);
            return NULL;
        }
        size_t n = strm.total_out;
        if (n > e->uncomp_size) { /* 解压大小异常 */
            free(out);
            return NULL;
        }
        out[n] = '\0';
        if (out_len) *out_len = n;
    } else {
        return NULL; /* 不支持的压缩方法 */
    }
    return out;
}
