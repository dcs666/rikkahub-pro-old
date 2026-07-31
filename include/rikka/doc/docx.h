#ifndef RIKKA_DOC_DOCX_H
#define RIKKA_DOC_DOCX_H

#include <stddef.h>
#include <stdint.h>

/*
 * DOCX 解析：zip 解压 + word/document.xml 文本提取。
 * 简化实现：扫描 zip local file header，inflate，XML 标签剥离。
 */

typedef struct {
    char *text;      /* 提取的文本（malloc，调用方 free） */
    size_t len;
} DocxContent;

/* 解析 docx 数据，返回 0 成功 */
int docx_parse(const uint8_t *data, size_t len, DocxContent *out);
void docx_content_free(DocxContent *c);

#endif /* RIKKA_DOC_DOCX_H */
