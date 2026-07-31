#ifndef RIKKA_MARKDOWN_MD_H
#define RIKKA_MARKDOWN_MD_H

#include <stddef.h>

/*
 * Markdown 解析器（S3 增量解析）。
 * 对标 JVM 版 org.intellij.markdown → HTML → Jsoup DOM → Compose 树双重解析；
 * 本实现一次解析成 AST（块 + 行内），零 HTML 中间层。
 *
 * 增量：RikkaMdParser 记录块边界，feed 追加文本时只重解析最后一个块
 * （流式场景每 token 成本 = 最后一段，而非整篇）。
 */

/* ---------- 块级 ---------- */

typedef enum {
    RIKKA_MD_DOC = 0,
    RIKKA_MD_HEADING,
    RIKKA_MD_PARAGRAPH,
    RIKKA_MD_CODE_BLOCK,
    RIKKA_MD_QUOTE,
    RIKKA_MD_LIST_ITEM,
    RIKKA_MD_HR,
} RikkaMdBlockType;

/* ---------- 行内 ---------- */

typedef enum {
    RIKKA_INLINE_TEXT = 0,
    RIKKA_INLINE_BOLD,
    RIKKA_INLINE_ITALIC,
    RIKKA_INLINE_CODE,
    RIKKA_INLINE_LINK,
    RIKKA_INLINE_IMAGE,
} RikkaInlineType;

typedef struct {
    RikkaInlineType type;
    size_t start, len;      /* 在所属文本内的偏移 */
    const char *href;       /* LINK/IMAGE：目标 */
    size_t href_len;
    const char *alt;        /* IMAGE：alt 文本 */
    size_t alt_len;
} RikkaInline;

typedef struct RikkaMdBlock RikkaMdBlock;
struct RikkaMdBlock {
    RikkaMdBlockType type;
    int level;              /* heading: 1-6；list: 缩进 */
    const char *text;       /* 指向源缓冲（零拷贝） */
    size_t len;
    const char *lang;       /* code block: 语言 */
    size_t lang_len;
    RikkaMdBlock **items;   /* quote/list: 子块 */
    size_t item_count, item_cap;
    size_t line_off;        /* 块所在行的起点（增量重解析用） */
    RikkaInline *inlines;   /* 行内节点（text 上） */
    size_t inline_count, inline_cap;
};

/* ---------- 一次性解析 ---------- */

/* 解析完整 markdown 文本；返回 malloc 块数组（*count），调用方 rmd_blocks_free 释放 */
RikkaMdBlock *rmd_parse_all(const char *text, size_t len, size_t *count);
void rmd_blocks_free(RikkaMdBlock *blocks, size_t count);

/* ---------- 增量解析 ---------- */

typedef struct RikkaMdParser RikkaMdParser;

RikkaMdParser *rmd_create(void);
void rmd_destroy(RikkaMdParser *p);

/* 追加文本（流式每 token 一次）。内部重解析最后一个块。 */
void rmd_feed(RikkaMdParser *p, const char *text, size_t len);

/* 取当前块（最后一个块可仍在变化）；返回块数与块数组（parser 持有） */
const RikkaMdBlock *rmd_blocks(RikkaMdParser *p, size_t *count);

/* 已喂入总字节数（诊断） */
size_t rmd_total(const RikkaMdParser *p);

#endif /* RIKKA_MARKDOWN_MD_H */
