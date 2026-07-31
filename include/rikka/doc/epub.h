#ifndef RIKKA_DOC_EPUB_H
#define RIKKA_DOC_EPUB_H

#include <stddef.h>
#include <stdint.h>

/*
 * EPUB 解析：zip 解压 + XHTML 文本提取。
 * 简化实现：找所有 .xhtml/.html 文件，标签剥离，合并文本。
 */

typedef struct {
    char *text;      /* 提取的文本（malloc，调用方 free） */
    size_t len;
} EpubContent;

/* 解析 epub 数据，返回 0 成功 */
int epub_parse(const uint8_t *data, size_t len, EpubContent *out);
void epub_content_free(EpubContent *c);

#endif /* RIKKA_DOC_EPUB_H */
