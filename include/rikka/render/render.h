#ifndef RIKKA_RENDER_RENDER_H
#define RIKKA_RENDER_RENDER_H

#include <stddef.h>
#include <stdint.h>
#include "rikka/highlight/highlight.h"

/*
 * S1 渲染排版块输出：Markdown AST → 排版块。
 * 对标 JVM 版 Compose MarkdownBlock 渲染。
 * 输出：文本块 + 样式 + 代码块（高亮）。
 */

typedef enum {
    RK_BLOCK_TEXT,
    RK_BLOCK_CODE,
    RK_BLOCK_HEADING,
    RK_BLOCK_QUOTE,
    RK_BLOCK_LIST_ITEM,
} RkBlockType;

typedef struct {
    RkBlockType type;
    char *text;          /* 文本内容（malloc） */
    size_t len;
    int level;           /* heading level (1-6) */
    char *lang;          /* code block language (malloc) */
    RikkaHlToken *hl_tokens;  /* 代码高亮 tokens (malloc) */
    size_t hl_count;
} RkRenderBlock;

typedef struct {
    RkRenderBlock *blocks;
    size_t count;
} RkRenderDoc;

/* Markdown → 渲染块 */
int rk_render_markdown(const char *md, size_t len, RkRenderDoc *out);
void rk_render_doc_free(RkRenderDoc *doc);

#endif /* RIKKA_RENDER_RENDER_H */
