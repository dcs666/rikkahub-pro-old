#ifndef RIKKA_DOC_ZIP_H
#define RIKKA_DOC_ZIP_H

#include <stddef.h>
#include <stdint.h>

/*
 * 内部共享：docx/epub/pptx 的 zip 读取（central directory + raw deflate）。
 * 之前三个模块各有一份实现，统一到这里。
 */

typedef struct {
    char name[256];
    const uint8_t *data;      /* 指向源缓冲内的压缩数据 */
    size_t comp_size, uncomp_size;
    int method;               /* 0 = stored, 8 = deflate */
} ZipEntry;

/* 按精确文件名查找（docx 用）。返回 1 找到 / 0 未找到。 */
int zip_find(const uint8_t *data, size_t len, const char *name, ZipEntry *out);

/* 按文件名谓词匹配（epub 后缀 / pptx 路径前缀用）。
 * 返回匹配数（≤ max_entries）。ctx 只读（const）。 */
typedef int (*ZipNameMatch)(const char *name, size_t name_len, const void *ctx);
size_t zip_find_matching(const uint8_t *data, size_t len, ZipNameMatch match, const void *ctx,
                         ZipEntry *entries, size_t max_entries);

/* 解压条目（stored / raw deflate）。返回 malloc 缓冲（调用方 free），
 * *out_len 输出长度；失败返回 NULL。 */
char *zip_inflate(const ZipEntry *e, size_t *out_len);

#endif /* RIKKA_DOC_ZIP_H */
