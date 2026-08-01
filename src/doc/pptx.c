#define _POSIX_C_SOURCE 200809L
#include "rikka/doc/pptx.h"
#include "rikka/core/buffer.h"
#include "zip.h"
#include <stdlib.h>
#include <string.h>

#define PPTX_MAX_SLIDES 256

/* 文件名匹配：ppt/slides/slide*.xml */
static int slide_match(const char *name, size_t name_len, const void *ctx) {
    (void)ctx;
    static const char prefix[] = "ppt/slides/slide";
    static const char suffix[] = ".xml";
    size_t plen = sizeof(prefix) - 1, slen = sizeof(suffix) - 1;
    if (name_len <= plen + slen) return 0;
    if (memcmp(name, prefix, plen) != 0) return 0;
    return memcmp(name + name_len - slen, suffix, slen) == 0;
}

/* 追加一段 slide 文本，解码 XML 实体 */
static void append_decoded(Buf *out, const char *s, size_t n) {
    size_t i = 0;
    while (i < n) {
        if (s[i] == '&' && n - i >= 4) {
            if (n - i >= 5 && memcmp(s + i, "&amp;", 5) == 0)  { buf_append_byte(out, '&');  i += 5; continue; }
            if (n - i >= 4 && memcmp(s + i, "&lt;", 4) == 0)   { buf_append_byte(out, '<');  i += 4; continue; }
            if (n - i >= 4 && memcmp(s + i, "&gt;", 4) == 0)   { buf_append_byte(out, '>');  i += 4; continue; }
            if (n - i >= 6 && memcmp(s + i, "&quot;", 6) == 0) { buf_append_byte(out, '"');  i += 6; continue; }
            if (n - i >= 6 && memcmp(s + i, "&apos;", 6) == 0) { buf_append_byte(out, '\''); i += 6; continue; }
        }
        buf_append_byte(out, s[i++]);
    }
}

/* 可移植子串查找（memmem 是 GNU 扩展） */
static const char *find_bytes(const char *hay, size_t hay_len, const char *needle, size_t nlen) {
    if (nlen == 0 || nlen > hay_len) return NULL;
    for (size_t i = 0; i + nlen <= hay_len; i++) {
        if (memcmp(hay + i, needle, nlen) == 0) return hay + i;
    }
    return NULL;
}

/* 提取 slide XML 文本：<a:t> 内容拼接，</a:p> 换行 */
static void extract_slide_text(const char *xml, size_t len, Buf *out) {
    const char *p = xml;
    const char *end = xml + len;
    while (p < end) {
        const char *t = find_bytes(p, (size_t)(end - p), "<a:t>", 5);
        const char *pp = find_bytes(p, (size_t)(end - p), "</a:p>", 6);
        if (!t && !pp) break;
        if (!pp || (t && t < pp)) {
            const char *te = find_bytes(t + 5, (size_t)(end - t - 5), "</a:t>", 6);
            if (!te) break;
            append_decoded(out, t + 5, (size_t)(te - t - 5));
            p = te + 6;
        } else {
            buf_append_byte(out, '\n');
            p = pp + 6;
        }
    }
}

int pptx_parse(const uint8_t *data, size_t len, PptxContent *out) {
    if (!data || !out) return -1;
    out->text = NULL;
    out->len = 0;
    /* 找所有 slide XML（共享 zip 读取） */
    ZipEntry slides[PPTX_MAX_SLIDES];
    size_t count = zip_find_matching(data, len, slide_match, NULL, slides, PPTX_MAX_SLIDES);
    if (count == 0) return -1;
    /* 合并所有 slide 文本 */
    Buf merged;
    buf_init(&merged);
    for (size_t i = 0; i < count; i++) {
        size_t xml_len = 0;
        char *xml = zip_inflate(&slides[i], &xml_len);
        if (!xml) continue;
        extract_slide_text(xml, xml_len, &merged);
        free(xml);
        if (merged.len > 0) buf_append_byte(&merged, '\n'); /* slide 分隔 */
    }
    if (merged.len == 0) {
        buf_free(&merged);
        return -1;
    }
    buf_append_byte(&merged, '\0');
    out->text = (char *)merged.data;
    out->len = merged.len;
    return 0;
}

void pptx_content_free(PptxContent *c) {
    if (c) {
        free(c->text);
        c->text = NULL;
        c->len = 0;
    }
}
