#ifndef RIKKA_DOC_PPTX_H
#define RIKKA_DOC_PPTX_H

#include <stddef.h>
#include <stdint.h>

/*
 * PPTX 解析：zip 解压 + slide XML 文本提取。
 * 找 ppt/slides/slideN.xml，提取 <a:t> 文本（</a:p> 换行），
 * 各 slide 之间以换行分隔。
 */

typedef struct {
    char *text;      /* 提取的文本（malloc，调用方 free） */
    size_t len;
} PptxContent;

/* 解析 pptx 数据，返回 0 成功 */
int pptx_parse(const uint8_t *data, size_t len, PptxContent *out);
void pptx_content_free(PptxContent *c);

#endif /* RIKKA_DOC_PPTX_H */
