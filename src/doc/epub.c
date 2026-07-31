#define _POSIX_C_SOURCE 200809L
#include "rikka/doc/epub.h"
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#define ZIP_LOCAL_SIG 0x04034b50

/* 从 zip 中找所有匹配后缀的文件，返回文件名列表 + 数据指针 */
typedef struct {
    char name[256];
    const uint8_t *data;
    size_t comp_size, uncomp_size;
    int method;
} ZipEntry;

static size_t zip_find_all(const uint8_t *data, size_t len, const char *suffix,
                           ZipEntry *entries, size_t max_entries) {
    size_t off = 0, count = 0;
    size_t suffix_len = strlen(suffix);
    while (off + 30 <= len && count < max_entries) {
        uint32_t sig = data[off] | (data[off+1] << 8) | (data[off+2] << 16) | (data[off+3] << 24);
        if (sig != ZIP_LOCAL_SIG) break;
        uint16_t comp_method = data[off+8] | (data[off+9] << 8);
        uint32_t comp_sz = data[off+18] | (data[off+19] << 8) | (data[off+20] << 16) | (data[off+21] << 24);
        uint32_t uncomp_sz = data[off+22] | (data[off+23] << 8) | (data[off+24] << 16) | (data[off+25] << 24);
        uint16_t fname_len = data[off+26] | (data[off+27] << 8);
        uint16_t extra_len = data[off+28] | (data[off+29] << 8);
        if (off + 30 + fname_len > len) break;
        const char *fname = (const char *)data + off + 30;
        size_t data_off = off + 30 + fname_len + extra_len;
        /* 检查后缀 */
        if (fname_len >= suffix_len &&
            memcmp(fname + fname_len - suffix_len, suffix, suffix_len) == 0) {
            ZipEntry *e = &entries[count++];
            size_t copy_len = fname_len < 255 ? fname_len : 255;
            memcpy(e->name, fname, copy_len);
            e->name[copy_len] = '\0';
            e->data = data + data_off;
            e->comp_size = comp_sz;
            e->uncomp_size = uncomp_sz;
            e->method = comp_method;
        }
        off = data_off + comp_sz;
    }
    return count;
}

/* XHTML 标签剥离：提取文本 */
static char *xhtml_extract_text(const char *html, size_t len, size_t *out_len) {
    char *out = (char *)malloc(len + 1);
    if (!out) return NULL;
    size_t out_off = 0;
    size_t i = 0;
    int in_tag = 0;
    int in_script = 0;
    while (i < len && out_off < len) {
        if (html[i] == '<') {
            /* 检查 script/style 标签 */
            if (i + 7 < len && (memcmp(html + i, "<script", 7) == 0 || memcmp(html + i, "<style", 6) == 0)) {
                in_script = 1;
            } else if (i + 8 < len && (memcmp(html + i, "</script>", 9) == 0 || memcmp(html + i, "</style>", 8) == 0)) {
                in_script = 0;
            }
            in_tag = 1;
            /* 块级标签加换行 */
            if (i + 3 < len && (memcmp(html + i, "<p", 2) == 0 || memcmp(html + i, "<h", 2) == 0 ||
                                memcmp(html + i, "<div", 4) == 0 || memcmp(html + i, "<br", 3) == 0)) {
                if (out_off > 0 && out_off < len) out[out_off++] = '\n';
            }
            i++;
        } else if (html[i] == '>') {
            in_tag = 0;
            i++;
        } else if (!in_tag && !in_script) {
            /* 文本内容 */
            if (html[i] == '&') {
                /* HTML 实体 */
                if (i + 4 < len && memcmp(html + i, "&lt;", 4) == 0) { out[out_off++] = '<'; i += 4; }
                else if (i + 4 < len && memcmp(html + i, "&gt;", 4) == 0) { out[out_off++] = '>'; i += 4; }
                else if (i + 5 < len && memcmp(html + i, "&amp;", 5) == 0) { out[out_off++] = '&'; i += 5; }
                else if (i + 6 < len && memcmp(html + i, "&nbsp;", 6) == 0) { out[out_off++] = ' '; i += 6; }
                else out[out_off++] = html[i++];
            } else {
                out[out_off++] = html[i++];
            }
        } else {
            i++;
        }
    }
    out[out_off] = '\0';
    *out_len = out_off;
    return out;
}

/* 解压 zip entry */
static char *inflate_entry(const ZipEntry *e, size_t *out_len) {
    char *out = NULL;
    if (e->method == 0) {
        out = (char *)malloc(e->comp_size + 1);
        if (!out) return NULL;
        memcpy(out, e->data, e->comp_size);
        out[e->comp_size] = '\0';
        *out_len = e->comp_size;
    } else if (e->method == 8) {
        out = (char *)malloc(e->uncomp_size + 1);
        if (!out) return NULL;
        z_stream strm;
        memset(&strm, 0, sizeof(strm));
        strm.next_in = (Bytef *)e->data;
        strm.avail_in = e->comp_size;
        strm.next_out = (Bytef *)out;
        strm.avail_out = e->uncomp_size;
        if (inflateInit2(&strm, -15) != Z_OK) {
            free(out);
            return NULL;
        }
        int ret = inflate(&strm, Z_FINISH);
        inflateEnd(&strm);
        if (ret != Z_STREAM_END && ret != Z_OK) {
            free(out);
            return NULL;
        }
        *out_len = strm.total_out;
        out[*out_len] = '\0';
    }
    return out;
}

int epub_parse(const uint8_t *data, size_t len, EpubContent *out) {
    if (!data || !out) return -1;
    out->text = NULL;
    out->len = 0;
    /* 找所有 .xhtml 和 .html 文件 */
    ZipEntry entries[64];
    size_t count_xhtml = zip_find_all(data, len, ".xhtml", entries, 64);
    size_t count_html = zip_find_all(data, len, ".html", entries + count_xhtml, 64 - count_xhtml);
    size_t total = count_xhtml + count_html;
    if (total == 0) return -1;
    /* 合并所有文件的文本 */
    char *merged = (char *)malloc(1024 * 1024); /* 1MB 初始 */
    if (!merged) return -1;
    size_t merged_len = 0;
    size_t merged_cap = 1024 * 1024;
    for (size_t i = 0; i < total; i++) {
        size_t html_len = 0;
        char *html = inflate_entry(&entries[i], &html_len);
        if (!html) continue;
        size_t text_len = 0;
        char *text = xhtml_extract_text(html, html_len, &text_len);
        free(html);
        if (!text) continue;
        /* 合并 */
        if (merged_len + text_len + 2 > merged_cap) {
            size_t new_cap = merged_cap * 2;
            char *new_merged = (char *)realloc(merged, new_cap);
            if (!new_merged) { free(text); break; }
            merged = new_merged;
            merged_cap = new_cap;
        }
        memcpy(merged + merged_len, text, text_len);
        merged_len += text_len;
        merged[merged_len++] = '\n';
        free(text);
    }
    if (merged_len == 0) {
        free(merged);
        return -1;
    }
    merged[merged_len] = '\0';
    out->text = merged;
    out->len = merged_len;
    return 0;
}

void epub_content_free(EpubContent *c) {
    if (c) {
        free(c->text);
        c->text = NULL;
        c->len = 0;
    }
}
