#define _POSIX_C_SOURCE 200809L
#include "rikka/render/render.h"
#include "rikka/markdown/md.h"
#include "rikka/core/buffer.h"
#include "rikka/json/json.h"
#include <stdio.h>
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
                /* md 解析器已存 fence 语言标识（如 ```c → "c"）；
                 * 可能带尾随空白（"```c   "），裁剪后再用 */
                if (mb->lang && mb->lang_len > 0) {
                    size_t ll = mb->lang_len;
                    while (ll > 0 && (mb->lang[ll-1] == ' ' || mb->lang[ll-1] == '\t' ||
                                      mb->lang[ll-1] == '\r')) ll--;
                    if (ll > 0) rb->lang = strndup(mb->lang, ll);
                }
                /* 代码高亮（未知语言退化为纯文本单 token） */
                if (mb->len > 0 && mb->len < 100000) { /* 限制大小 */
                    if (mb->len <= SIZE_MAX / sizeof(RikkaHlToken)) {
                        rb->hl_tokens = (RikkaHlToken *)malloc(mb->len * sizeof(RikkaHlToken));
                    }
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

/* ---------- JSON 线协议（UI 壳） ---------- */

static const char *block_type_name(RkBlockType t) {
    switch (t) {
        case RK_BLOCK_TEXT:     return "text";
        case RK_BLOCK_CODE:     return "code";
        case RK_BLOCK_HEADING:  return "heading";
        case RK_BLOCK_QUOTE:    return "quote";
        case RK_BLOCK_LIST_ITEM:return "list_item";
        default:                return "text";
    }
}

static const char *hl_type_name(RikkaHlType t) {
    switch (t) {
        case RIKKA_HL_PLAIN:    return "plain";
        case RIKKA_HL_KEYWORD:  return "keyword";
        case RIKKA_HL_STRING:   return "string";
        case RIKKA_HL_COMMENT:  return "comment";
        case RIKKA_HL_NUMBER:   return "number";
        case RIKKA_HL_TYPE:     return "type";
        case RIKKA_HL_FUNC:     return "func";
        case RIKKA_HL_OPERATOR: return "operator";
        case RIKKA_HL_PREPROC:  return "preproc";
        case RIKKA_HL_BUILTIN:  return "builtin";
        case RIKKA_HL_TAG:      return "tag";
        case RIKKA_HL_ATTR:     return "attr";
        default:                return "plain";
    }
}

/* 转义 JSON 字符串并带引号写出 */
static void jstr(Buf *b, const char *s, size_t len) {
    buf_append_byte(b, '"');
    RJsonOut tmp;
    rjson_out_init(&tmp);
    rjson_write_escaped(&tmp, s, len);
    buf_append(b, tmp.buf, tmp.len);
    rjson_out_free(&tmp);
    buf_append_byte(b, '"');
}

char *rk_render_markdown_json(const char *md, size_t len) {
    if (!md) return NULL;
    RkRenderDoc doc;
    if (rk_render_markdown(md, len, &doc) != 0) return NULL;
    Buf b;
    buf_init(&b);
    buf_append_str(&b, "{\"blocks\":[");
    for (size_t i = 0; i < doc.count; i++) {
        const RkRenderBlock *blk = &doc.blocks[i];
        if (i) buf_append_byte(&b, ',');
        buf_append_str(&b, "{\"type\":\"");
        buf_append_str(&b, block_type_name(blk->type));
        buf_append_str(&b, "\"");
        if (blk->type == RK_BLOCK_HEADING) {
            char tmp[32];
            int n = snprintf(tmp, sizeof(tmp), ",\"level\":%d", blk->level);
            if (n > 0) buf_append(&b, tmp, (size_t)n);
        }
        if (blk->type == RK_BLOCK_CODE) {
            buf_append_str(&b, ",\"lang\":");
            jstr(&b, blk->lang ? blk->lang : "", blk->lang ? strlen(blk->lang) : 0);
        }
        buf_append_str(&b, ",\"text\":");
        jstr(&b, blk->text ? blk->text : "", blk->len);
        if (blk->type == RK_BLOCK_CODE && blk->hl_count > 0) {
            buf_append_str(&b, ",\"tokens\":[");
            for (size_t t = 0; t < blk->hl_count; t++) {
                if (t) buf_append_byte(&b, ',');
                char tmp[96];
                int n = snprintf(tmp, sizeof(tmp),
                                 "{\"start\":%zu,\"len\":%zu,\"type\":\"%s\"}",
                                 blk->hl_tokens[t].start, blk->hl_tokens[t].len,
                                 hl_type_name(blk->hl_tokens[t].type));
                if (n > 0) buf_append(&b, tmp, (size_t)n);
            }
            buf_append_byte(&b, ']');
        }
        buf_append_byte(&b, '}');
    }
    buf_append_str(&b, "]}");
    buf_append_byte(&b, '\0'); /* NUL 结尾（不计入 len） */
    rk_render_doc_free(&doc);
    return (char *)b.data;
}
