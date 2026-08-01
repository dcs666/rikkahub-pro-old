#define _POSIX_C_SOURCE 200809L
#include "rikka/doc/docx.h"
#include "zip.h"
#include <stdlib.h>
#include <string.h>

/* XML 标签剥离：提取 <w:t>...</w:t> 之间的文本（每段后换行） */
static char *xml_extract_text(const char *xml, size_t len, size_t *out_len) {
    /* 预估输出大小（XML 文本通常 < 原始大小） */
    char *out = (char *)malloc(len + 1);
    if (!out) return NULL;
    size_t out_off = 0;
    size_t i = 0;
    while (i < len) {
        /* 找 <w:t> 或 <w:t ...> */
        if (xml[i] == '<' && i + 4 < len && memcmp(xml + i, "<w:t", 4) == 0) {
            /* 找 > */
            size_t tag_end = i + 4;
            while (tag_end < len && xml[tag_end] != '>') tag_end++;
            if (tag_end >= len) break;
            tag_end++; /* 跳过 > */
            /* 找 </w:t> */
            size_t text_start = tag_end;
            size_t close = text_start;
            while (close < len && len - close >= 6 && memcmp(xml + close, "</w:t>", 6) != 0) close++;
            if (close >= len || len - close < 6) break;
            /* 复制文本 */
            size_t text_len = close - text_start;
            if (text_len > len - out_off) break; /* 溢出防护 */
            memcpy(out + out_off, xml + text_start, text_len);
            out_off += text_len;
            /* 段落结束加换行 */
            if (out_off > 0 && out_off < len) out[out_off++] = '\n';
            i = close + 6;
        } else {
            i++;
        }
    }
    out[out_off] = '\0';
    *out_len = out_off;
    return out;
}

int docx_parse(const uint8_t *data, size_t len, DocxContent *out) {
    if (!data || !out) return -1;
    out->text = NULL;
    out->len = 0;
    /* 找 word/document.xml（共享 zip 读取） */
    ZipEntry e;
    if (!zip_find(data, len, "word/document.xml", &e)) return -1;
    /* 解压（stored / raw deflate） */
    size_t xml_len = 0;
    char *xml = zip_inflate(&e, &xml_len);
    if (!xml) return -1;
    /* XML 文本提取 */
    out->text = xml_extract_text(xml, xml_len, &out->len);
    free(xml);
    if (!out->text) return -1;
    return 0;
}

void docx_content_free(DocxContent *c) {
    if (c) {
        free(c->text);
        c->text = NULL;
        c->len = 0;
    }
}
