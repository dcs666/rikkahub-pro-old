#ifndef RIKKA_DATA_INDEX_H
#define RIKKA_DATA_INDEX_H

#include <stddef.h>
#include <stdint.h>

/*
 * 内存倒排索引（B2 简化版 FTS）：
 * 分词 = ASCII 词 + 中文 bigram（对标 jieba 词典的简化，无 13MB 词典依赖）。
 * 支持文档增删（删除通过标记重建）与 AND 交集搜索。
 */

/* 分词回调（token 不保证 NUL 结尾，用 tlen） */
typedef void (*RkTokenCb)(void *ctx, const char *tok, size_t tlen);
size_t rk_tokenize(const char *text, size_t len, RkTokenCb cb, void *ctx);

typedef struct RkIndex RkIndex;

RkIndex *rk_index_create(void);
void rk_index_destroy(RkIndex *ix);

/* 索引文档（text 中的 token → doc）。返回索引 token 数 */
size_t rk_index_add(RkIndex *ix, uint64_t doc, const char *text, size_t len);

/* 移除文档（posting 删除 + 空 token 节点清理）。O(全部 token × posting) 低频可用 */
void rk_index_remove_doc(RkIndex *ix, uint64_t doc);

/*
 * AND 搜索：query 全部分词都命中的文档。
 * out 按 doc 升序；返回命中数（<= cap）。
 */
size_t rk_index_search(RkIndex *ix, const char *query, size_t len,
                       uint64_t *out, size_t cap);

size_t rk_index_token_count(const RkIndex *ix);
size_t rk_index_doc_count(const RkIndex *ix);

#endif /* RIKKA_DATA_INDEX_H */
