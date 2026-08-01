#define _POSIX_C_SOURCE 200809L
#include "rikka/doc/epub.h"
#include "zip.h"
#include <stdlib.h>
#include <string.h>

/* 文件名后缀匹配（.xhtml / .html） */
static int epub_suffix_match(const char *name, size_t name_len, const void *ctx) {
    const char *suffix = (const char *)ctx;
    size_t slen = strlen(suffix);
    return name_len >= slen && memcmp(name + name_len - slen, suffix, slen) == 0;
}

/* XHTML 标签剥离：提取文本（script/style 内容丢弃，块级标签换行，实体解码） */
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

int epub_parse(const uint8_t *data, size_t len, EpubContent *out) {
    if (!data || !out) return -1;
    out->text = NULL;
    out->len = 0;
    /* 找所有 .xhtml 和 .html 文件（共享 zip 读取，central directory） */
    ZipEntry entries[64];
    size_t count_xhtml = zip_find_matching(data, len, epub_suffix_match, ".xhtml", entries, 64);
    size_t count_html = zip_find_matching(data, len, epub_suffix_match, ".html",
                                          entries + count_xhtml, 64 - count_xhtml);
    size_t total = count_xhtml + count_html;
    if (total == 0) return -1;
    /* 合并所有文件的文本 */
    char *merged = (char *)malloc(1024 * 1024); /* 1MB 初始 */
    if (!merged) return -1;
    size_t merged_len = 0;
    size_t merged_cap = 1024 * 1024;
    for (size_t i = 0; i < total; i++) {
        size_t html_len = 0;
        char *html = zip_inflate(&entries[i], &html_len);
        if (!html) continue;
        size_t text_len = 0;
        char *text = xhtml_extract_text(html, html_len, &text_len);
        free(html);
        if (!text) continue;
        /* 合并 */
        if (text_len > merged_cap - merged_len - 2) {
            size_t new_cap = merged_cap * 2;
            if (new_cap < merged_len + text_len + 2) new_cap = merged_len + text_len + 2;
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
