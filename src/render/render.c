#define _POSIX_C_SOURCE 200809L
#include "rikka/render/render.h"
#include "rikka/markdown/md.h"
#include <stdlib.h>
#include <string.h>

int rk_render_markdown(const char *md, size_t len, RkRenderDoc *out) {
    if (!md || !out) return -1;
    out->blocks = NULL;
    out->count = 0;
    /* 解析 Markdown AST */
    size_t md_count = 0;
    RikkaMdBlock *md_blocks = rmd_parse_all(md, len, &md_count);
    if (!md_blocks && md_count > 0) return -1;
    /* 转换为渲染块 */
    RkRenderBlock *blocks = (RkRenderBlock *)calloc(md_count, sizeof(RkRenderBlock));
    if (!blocks && md_count > 0) {
        rmd_blocks_free(md_blocks, md_count);
        return -1;
    }
    size_t out_count = 0;
    for (size_t i = 0; i < md_count; i++) {
        RikkaMdBlock *mb = &md_blocks[i];
        RkRenderBlock *rb = &blocks[out_count];
        rb->text = NULL;
        rb->len = 0;
        rb->level = 0;
        rb->lang = NULL;
        rb->hl_tokens = NULL;
        rb->hl_count = 0;
        switch (mb->type) {
            case RIKKA_MD_HEADING:
                rb->type = RK_BLOCK_HEADING;
                rb->level = mb->level;
                rb->text = strndup(mb->text, mb->len);
                rb->len = mb->len;
                break;
            case RIKKA_MD_PARAGRAPH:
                rb->type = RK_BLOCK_TEXT;
                rb->text = strndup(mb->text, mb->len);
                rb->len = mb->len;
                break;
            case RIKKA_MD_CODE_BLOCK:
                rb->type = RK_BLOCK_CODE;
                rb->text = strndup(mb->text, mb->len);
                rb->len = mb->len;
                /* 提取语言（fence 行后的语言标识） */
                /* 简化：从 mb->lang 提取（如果有） */
                /* md 解析器目前不存 lang，简化为空 */
                rb->lang = NULL;
                /* 代码高亮 */
                if (mb->len > 0 && mb->len < 100000) { /* 限制大小 */
                    rb->hl_tokens = (RikkaHlToken *)malloc(mb->len * sizeof(RikkaHlToken));
                    if (rb->hl_tokens) {
                        rb->hl_count = rikka_hl_tokenize(
                            rb->lang ? rb->lang : "text",
                            rb->text, rb->len,
                            rb->hl_tokens, mb->len);
                    }
                }
                break;
            case RIKKA_MD_QUOTE:
                rb->type = RK_BLOCK_QUOTE;
                rb->text = strndup(mb->text, mb->len);
                rb->len = mb->len;
                break;
            case RIKKA_MD_LIST_ITEM:
                rb->type = RK_BLOCK_LIST_ITEM;
                rb->text = strndup(mb->text, mb->len);
                rb->len = mb->len;
                break;
            default:
                continue;
        }
        out_count++;
    }
    rmd_blocks_free(md_blocks, md_count);
    out->blocks = blocks;
    out->count = out_count;
    return 0;
}

void rk_render_doc_free(RkRenderDoc *doc) {
    if (!doc) return;
    for (size_t i = 0; i < doc->count; i++) {
        free(doc->blocks[i].text);
        free(doc->blocks[i].lang);
        free(doc->blocks[i].hl_tokens);
    }
    free(doc->blocks);
    doc->blocks = NULL;
    doc->count = 0;
}
