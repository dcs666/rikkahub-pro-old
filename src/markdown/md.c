#include "rikka/markdown/md.h"
#include "rikka/core/buffer.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ================= 行内解析 ================= */

static void inline_append(RikkaMdBlock *b, const char *text, size_t start, size_t len,
                          RikkaInlineType t, const char *href, size_t href_len,
                          const char *alt, size_t alt_len) {
    (void)text;
    if (len == 0 && t == RIKKA_INLINE_TEXT) return;
    if (b->inline_count == b->inline_cap) {
        size_t nc = b->inline_cap ? b->inline_cap * 2 : 8;
        if (nc > SIZE_MAX / sizeof(RikkaInline)) return; /* 溢出防护 */
        RikkaInline *ni = (RikkaInline *)realloc(b->inlines, nc * sizeof(RikkaInline));
        if (!ni) return;
        b->inlines = ni;
        b->inline_cap = nc;
    }
    RikkaInline *in = &b->inlines[b->inline_count++];
    in->type = t;
    in->start = start;
    in->len = len;
    in->href = href;
    in->href_len = href_len;
    in->alt = alt;
    in->alt_len = alt_len;
}

/* 链接目标提取：[..](..) 中 ) 前的内容 */
static void parse_link_dest(const char *s, size_t len, const char **dest, size_t *dest_len) {
    /* s 指向 '(' 后 */
    size_t i = 0;
    while (i < len && s[i] == ' ') i++;
    size_t start = i;
    while (i < len && s[i] != ')') i++;
    *dest = s + start;
    *dest_len = i - start;
}

/* 行内扫描：文本 [start, end) 生成 inline 节点 */
static void inline_scan(RikkaMdBlock *b, const char *text, size_t start, size_t end) {
    size_t i = start;
    size_t plain_start = start;
    while (i < end) {
        char c = text[i];
        if (c == '`') {
            /* 行内代码：`...` */
            size_t close = end;
            for (size_t j = i + 1; j < end; j++) {
                if (text[j] == '`') { close = j; break; }
            }
            if (close < end) {
                if (i > plain_start)
                    inline_append(b, text, plain_start, i - plain_start, RIKKA_INLINE_TEXT, NULL, 0, NULL, 0);
                inline_append(b, text, i + 1, close - i - 1, RIKKA_INLINE_CODE, NULL, 0, NULL, 0);
                i = close + 1;
                plain_start = i;
                continue;
            }
            i++;
            continue;
        }
        if (c == '*' || c == '_') {
            /* 判断 ** 或 *；也处理 _（简单版） */
            int bold = (i + 1 < end && text[i + 1] == c);
            size_t mark = bold ? 2 : 1;
            if (bold && i + 1 < end && text[i + 1] == c) {
                /* **bold** */
                size_t close = end;
                for (size_t j = i + 2; j + 1 < end; j++) {
                    if (text[j] == c && text[j + 1] == c) { close = j; break; }
                }
                if (close < end) {
                    if (i > plain_start)
                        inline_append(b, text, plain_start, i - plain_start, RIKKA_INLINE_TEXT, NULL, 0, NULL, 0);
                    inline_append(b, text, i + 2, close - i - 2, RIKKA_INLINE_BOLD, NULL, 0, NULL, 0);
                    i = close + 2;
                    plain_start = i;
                    continue;
                }
                i += 2;
                continue;
            }
            /* *italic* */
            size_t close = end;
            for (size_t j = i + 1; j < end; j++) {
                if (text[j] == c) { close = j; break; }
            }
            if (close < end && close > i + 1) {
                if (i > plain_start)
                    inline_append(b, text, plain_start, i - plain_start, RIKKA_INLINE_TEXT, NULL, 0, NULL, 0);
                inline_append(b, text, i + 1, close - i - 1, RIKKA_INLINE_ITALIC, NULL, 0, NULL, 0);
                i = close + 1;
                plain_start = i;
                continue;
            }
            i += mark;
            continue;
        }
        if (c == '!' && i + 1 < end && text[i + 1] == '[') {
            /* ![alt](href) */
            size_t close_bracket = end;
            for (size_t j = i + 2; j < end; j++) {
                if (text[j] == ']') { close_bracket = j; break; }
            }
            if (close_bracket < end && close_bracket + 1 < end && text[close_bracket + 1] == '(') {
                size_t close_paren = end;
                for (size_t j = close_bracket + 2; j < end; j++) {
                    if (text[j] == ')') { close_paren = j; break; }
                }
                if (close_paren < end) {
                    if (i > plain_start)
                        inline_append(b, text, plain_start, i - plain_start, RIKKA_INLINE_TEXT, NULL, 0, NULL, 0);
                    const char *dest; size_t dest_len;
                    parse_link_dest(text + close_bracket + 2, close_paren - close_bracket - 2, &dest, &dest_len);
                    inline_append(b, text, i + 2, close_bracket - i - 2, RIKKA_INLINE_IMAGE,
                                  dest, dest_len, text + i + 2, close_bracket - i - 2);
                    i = close_paren + 1;
                    plain_start = i;
                    continue;
                }
            }
            i++;
            continue;
        }
        if (c == '[') {
            /* [text](href) */
            size_t close_bracket = end;
            for (size_t j = i + 1; j < end; j++) {
                if (text[j] == ']') { close_bracket = j; break; }
            }
            if (close_bracket < end && close_bracket + 1 < end && text[close_bracket + 1] == '(') {
                size_t close_paren = end;
                for (size_t j = close_bracket + 2; j < end; j++) {
                    if (text[j] == ')') { close_paren = j; break; }
                }
                if (close_paren < end) {
                    if (i > plain_start)
                        inline_append(b, text, plain_start, i - plain_start, RIKKA_INLINE_TEXT, NULL, 0, NULL, 0);
                    const char *dest; size_t dest_len;
                    parse_link_dest(text + close_bracket + 2, close_paren - close_bracket - 2, &dest, &dest_len);
                    inline_append(b, text, i + 1, close_bracket - i - 1, RIKKA_INLINE_LINK,
                                  dest, dest_len, NULL, 0);
                    i = close_paren + 1;
                    plain_start = i;
                    continue;
                }
            }
            i++;
            continue;
        }
        i++;
    }
    if (plain_start < end)
        inline_append(b, text, plain_start, end - plain_start, RIKKA_INLINE_TEXT, NULL, 0, NULL, 0);
}

/* ================= 块级解析 ================= */

typedef struct {
    const char *text;   /* 源文本 */
    size_t len;
    RikkaMdBlock *blocks;
    size_t count, cap;
} MdCtx;

static RikkaMdBlock *md_add_block(MdCtx *c, RikkaMdBlockType t, size_t start, size_t end) {
    if (c->count == c->cap) {
        size_t nc = c->cap ? c->cap * 2 : 8;
        RikkaMdBlock *nb = (RikkaMdBlock *)realloc(c->blocks, nc * sizeof(RikkaMdBlock));
        if (!nb) return NULL;
        c->blocks = nb;
        c->cap = nc;
    }
    RikkaMdBlock *b = &c->blocks[c->count++];
    memset(b, 0, sizeof(RikkaMdBlock));
    b->type = t;
    b->text = c->text + start;
    b->len = end - start;
    /* 行起点（增量重解析从行起点开始，含标记如 # ） */
    {
        size_t lo = start;
        while (lo > 0 && c->text[lo - 1] != '\n') lo--;
        b->line_off = lo;
    }
    return b;
}

/* 行 trim 后是否以 prefix 开头（prefix 长度 plen），返回前缀后偏移 */
static const char *line_has_prefix(const char *line, size_t len, const char *prefix, size_t plen) {
    size_t i = 0;
    while (i < len && (line[i] == ' ' || line[i] == '\t')) i++;
    if (len - i >= plen && memcmp(line + i, prefix, plen) == 0) return line + i + plen;
    return NULL;
}

/* 块解析：从偏移 start 到 end（多行） */
static void md_parse_blocks(MdCtx *c, size_t start, size_t end) {
    size_t i = start;
    while (i < end) {
        /* 找行 [i, line_end)（不含 \n） */
        size_t line_end = i;
        while (line_end < end && c->text[line_end] != '\n') line_end++;
        size_t line_len = line_end - i;
        if (line_len > 0 && c->text[line_end - 1] == '\r') line_len--;
        const char *line = c->text + i;

        /* 空行 → 跳过（段落间分隔） */
        size_t j = 0;
        while (j < line_len && (line[j] == ' ' || line[j] == '\t')) j++;
        if (j == line_len) { i = line_end + 1; continue; }

        /* # heading */
        if (line[j] == '#') {
            int level = 0;
            while (j + level < line_len && line[j + level] == '#') level++;
            size_t hstart = j + level;
            while (hstart < line_len && line[hstart] == ' ') hstart++;
            RikkaMdBlock *b = md_add_block(c, RIKKA_MD_HEADING, i + hstart, i + line_len);
            if (b) b->level = level;
            inline_scan(b, c->text, i + hstart, i + line_len);
            i = line_end + 1;
            continue;
        }

        /* 代码 fence ``` 或 ~~~ */
        const char *fence = line_has_prefix(line, line_len, "```", 3);
        const char *fence2 = NULL;
        if (!fence) fence2 = line_has_prefix(line, line_len, "~~~", 3);
        if (fence || fence2) {
            const char *f = fence ? fence : fence2;
            /* lang = fence 后到行尾 */
            const char *lang = f;
            size_t lang_len = (size_t)(line + line_len - f);
            size_t block_start = i;
            /* 找闭合 fence */
            size_t k = line_end + 1;
            size_t close = end;
            while (k < end) {
                size_t kend = k;
                while (kend < end && c->text[kend] != '\n') kend++;
                size_t klen = kend - k;
                const char *cl = c->text + k;
                            if (line_has_prefix(cl, klen, "```", 3) || line_has_prefix(cl, klen, "~~~", 3)) {
                    close = k; /* 闭合 fence 行起点 */
                    break;
                }
                k = kend + 1;
            }
            if (close < end) {
                            /* 有闭合：块内容 [line_end+1, close) */
                RikkaMdBlock *b = md_add_block(c, RIKKA_MD_CODE_BLOCK, block_start, close);
                if (b) {
                    b->lang = lang;
                    b->lang_len = lang_len;
                    b->text = c->text + line_end + 1;
                    b->len = close > line_end + 1 ? close - line_end - 1 : 0;
                }
                /* 跳到闭合 fence 行之后 */
                i = close;
                while (i < end && c->text[i] != '\n') i++;
                i++;
            } else {
                /* 未闭合（增量中间态）：内容到末尾 */
                RikkaMdBlock *b = md_add_block(c, RIKKA_MD_CODE_BLOCK, block_start, end);
                if (b) {
                    b->lang = lang;
                    b->lang_len = lang_len;
                    b->text = c->text + line_end + 1;
                    b->len = end > line_end + 1 ? end - line_end - 1 : 0;
                }
                i = end;
            }
            continue;
        }

        /* 引用 > */
        const char *qt = line_has_prefix(line, line_len, ">", 1);
        if (qt) {
            while (qt < line + line_len && (*qt == ' ' || *qt == '	')) qt++;
            RikkaMdBlock *b = md_add_block(c, RIKKA_MD_QUOTE, i, i + line_len);
            if (b) {
                b->text = qt;
                b->len = (size_t)(line + line_len - qt);
                inline_scan(b, c->text, (size_t)(qt - c->text), i + line_len);
            }
            i = line_end + 1;
            continue;
        }

        /* 列表项 - + * */
        const char *li = line_has_prefix(line, line_len, "- ", 2);
        if (!li) li = line_has_prefix(line, line_len, "* ", 2);
        if (!li) li = line_has_prefix(line, line_len, "+ ", 2);
        if (li) {
            RikkaMdBlock *b = md_add_block(c, RIKKA_MD_LIST_ITEM, i, i + line_len);
            if (b) {
                b->text = li;
                b->len = (size_t)(line + line_len - li);
                inline_scan(b, c->text, (size_t)(li - c->text), i + line_len);
            }
            i = line_end + 1;
            continue;
        }

        /* HR --- */
        if (line_len >= 3 && line[j] == '-' && line[j+1] == '-' && line[j+2] == '-') {
            int all_dash = 1;
            for (size_t k = j; k < line_len; k++) if (line[k] != '-' && line[k] != ' ') { all_dash = 0; break; }
            if (all_dash) {
                md_add_block(c, RIKKA_MD_HR, i, i + line_len);
                i = line_end + 1;
                continue;
            }
        }

        /* 段落：合并连续非空行（含缩进续行） */
        size_t pstart = i;
        size_t pend = line_end < end ? line_end + 1 : end;
        size_t k = line_end + 1;
        while (k < end) {
            size_t kend = k;
            while (kend < end && c->text[kend] != '\n') kend++;
            size_t klen = kend - k;
            const char *kl = c->text + k;
            size_t kj = 0;
            while (kj < klen && (kl[kj] == ' ' || kl[kj] == '\t')) kj++;
            if (kj == klen) break; /* 空行结束段落 */
            /* 结构行结束段落 */
            if (kl[kj] == '#') break;
            if (line_has_prefix(kl, klen, "```", 3) || line_has_prefix(kl, klen, "~~~", 3)) break;
            if (line_has_prefix(kl, klen, ">", 1)) break;
            if (line_has_prefix(kl, klen, "- ", 2) || line_has_prefix(kl, klen, "* ", 2) ||
                line_has_prefix(kl, klen, "+ ", 2)) break;
            pend = kend < end ? kend + 1 : end; /* 最后一行无换行 */
            k = kend + 1;
        }
        /* 段落文本 [pstart, pend)，去掉末尾 \n 和尾部空白 */
        size_t pend_trim = pend;
        while (pend_trim > pstart && (c->text[pend_trim-1] == '\n' || c->text[pend_trim-1] == ' ' ||
                                      c->text[pend_trim-1] == '\t' || c->text[pend_trim-1] == '\r'))
            pend_trim--;
        RikkaMdBlock *b = md_add_block(c, RIKKA_MD_PARAGRAPH, pstart, pend_trim);
        inline_scan(b, c->text, pstart, pend_trim);
        i = pend;
    }
}

/* ================= 公共 API ================= */

RikkaMdBlock *rmd_parse_all(const char *text, size_t len, size_t *count) {
    MdCtx c;
    memset(&c, 0, sizeof(c));
    c.text = text;
    c.len = len;
    md_parse_blocks(&c, 0, len);
    if (count) *count = c.count;
    return c.blocks;
}

void rmd_blocks_free(RikkaMdBlock *blocks, size_t count) {
    if (!blocks) return;
    for (size_t i = 0; i < count; i++) free(blocks[i].inlines);
    free(blocks);
}

/* ================= 增量解析 ================= */

struct RikkaMdParser {
    Buf text;
    RikkaMdBlock *blocks;   /* 已解析块 */
    size_t count, cap;
    size_t parse_from;      /* 重解析起点（文本偏移） */
    size_t open_block_off;  /* 未以换行结束的开放块起点；SIZE_MAX = 无 */
    int in_fence;           /* 文本末尾在未闭合代码块内 */
    size_t fence_line_off;  /* 开 fence 行起点 */
    size_t fence_content_off; /* 代码块内容起点（开 fence 行后） */
    size_t fence_scan_off;  /* fence 增量扫描位置（上次完整行边界） */
};

/* 找最后块边界（从后向前）。
 * - 未闭合 fence → 开 fence 行
 * - 闭合 fence 行 → 配对的开 fence（代码块整体重解析）
 * - 空行后 / 结构行（heading/quote/list/hr）起点
 * - 普通文本行 → 同一段落，继续向前
 */
/* 增量边界查找：用 in_fence 状态替代全文 fence 扫描；从 new_len 向前到 parse_from */
static size_t find_boundary_inc(RikkaMdParser *p, size_t n) {
    const char *t = (const char *)p->text.data;
    if (p->in_fence) return p->fence_line_off; /* 未闭合：从开 fence 重解析 */
    size_t stop = p->parse_from; /* 只扫到上次重解析起点 */
    /* 从后向前找块边界（到 stop 为止） */
    size_t line_start = n;
    while (line_start > stop) {
        size_t prev_nl = line_start;
        if (prev_nl > stop && t[prev_nl - 1] == '\n') prev_nl--;
        while (prev_nl > stop && t[prev_nl - 1] != '\n') prev_nl--;
        if (prev_nl <= stop) return stop;
        size_t ls = prev_nl;
        size_t le = line_start;
        if (le > ls && t[le - 1] == '\n') le--;
        if (le > ls && t[le - 1] == '\r') le--;
        size_t ll = le - ls;
        const char *l = t + ls;
        size_t j = 0;
        while (j < ll && (l[j] == ' ' || l[j] == '\t')) j++;
        if (j == ll) {
            return le < n && t[le] == '\n' ? le + 1 : le; /* 空行后 */
        }
        if (l[j] == '#') return ls;
        if (l[j] == '>') return ls;
        if (ll - j >= 2 && (l[j] == '-' || l[j] == '*' || l[j] == '+') && l[j + 1] == ' ')
            return ls;
        if (ll - j >= 3 && l[j] == '-' && l[j + 1] == '-' && l[j + 2] == '-') {
            int all_dash = 1;
            for (size_t k = j; k < ll; k++)
                if (l[k] != '-' && l[k] != ' ') { all_dash = 0; break; }
            if (all_dash) return ls;
        }
        /* 代码块的 ``` 行：返回开 fence 行（代码块起点，fence_line_off 记录） */
        if (ll - j >= 3 && (memcmp(l + j, "```", 3) == 0 || memcmp(l + j, "~~~", 3) == 0))
            return p->fence_line_off;
        line_start = prev_nl;
    }
    return stop;
}

static void rebuild_blocks(RikkaMdParser *p) {
    /* 丢弃 parse_from 之后的旧块，重新解析 */
    size_t keep = 0;
    for (size_t i = 0; i < p->count; i++) {
        /* 块 text 起点 >= parse_from 的丢弃 */
        if ((size_t)(p->blocks[i].text - (const char *)p->text.data) >= p->parse_from) break;
        keep++;
    }
    for (size_t i = keep; i < p->count; i++) {
        free(p->blocks[i].inlines);
        p->blocks[i].inlines = NULL;
        p->blocks[i].inline_count = p->blocks[i].inline_cap = 0;
    }
    p->count = keep;

    MdCtx c;
    memset(&c, 0, sizeof(c));
    c.text = (const char *)p->text.data;
    c.len = p->text.len;
    md_parse_blocks(&c, p->parse_from, p->text.len);
    /* 合并新块 */
    for (size_t i = 0; i < c.count; i++) {
        if (p->count == p->cap) {
            size_t nc = p->cap ? p->cap * 2 : 8;
            RikkaMdBlock *nb = (RikkaMdBlock *)realloc(p->blocks, nc * sizeof(RikkaMdBlock));
            if (!nb) break;
            p->blocks = nb;
            p->cap = nc;
        }
        p->blocks[p->count++] = c.blocks[i];
    }
    free(c.blocks);
}

RikkaMdParser *rmd_create(void) {
    RikkaMdParser *p = (RikkaMdParser *)calloc(1, sizeof(RikkaMdParser));
    if (!p) return NULL;
    buf_init(&p->text);
    return p;
}

void rmd_destroy(RikkaMdParser *p) {
    if (!p) return;
    for (size_t i = 0; i < p->count; i++) free(p->blocks[i].inlines);
    free(p->blocks);
    buf_free(&p->text);
    free(p);
}

/* 统计文本 [start, end) 内的 fence 行数 */
static int count_fence_lines(const char *t, size_t start, size_t end) {
    int n = 0;
    size_t i = start;
    while (i < end) {
        size_t le = i;
        while (le < end && t[le] != '\n') le++;
        size_t ll = le - i;
        if (ll > 0 && t[le - 1] == '\r') ll--;
        const char *l = t + i;
        size_t j = 0;
        while (j < ll && (l[j] == ' ' || l[j] == '\t')) j++;
        if (ll - j >= 3 && (memcmp(l + j, "```", 3) == 0 || memcmp(l + j, "~~~", 3) == 0))
            n++;
        i = le + 1;
    }
    return n;
}

void rmd_feed(RikkaMdParser *p, const char *text, size_t len) {
    size_t old_len = p->text.len;
    uint8_t *old_data = p->text.data;
    buf_append(&p->text, text, len);
    /* Buf realloc 会移动 data：平移保留块的所有指针（text/inline href/alt） */
    ptrdiff_t delta = (const char *)p->text.data - (const char *)old_data;
    if (delta != 0) {
        for (size_t i = 0; i < p->count; i++) {
            p->blocks[i].text += delta;
            for (size_t j = 0; j < p->blocks[i].inline_count; j++) {
                if (p->blocks[i].inlines[j].href) p->blocks[i].inlines[j].href += delta;
                if (p->blocks[i].inlines[j].alt) p->blocks[i].inlines[j].alt += delta;
            }
        }
    }
    /* 增量 fence 状态：只扫新段 [fence_scan_off, 最后完整行边界) */
    {
        const char *t = (const char *)p->text.data;
        size_t scan_start = p->fence_scan_off;
        size_t scan_end = p->text.len;
        while (scan_end > scan_start && t[scan_end - 1] != '\n') scan_end--;
        size_t i = scan_start;
        while (i < scan_end) {
            size_t le = i;
            while (le < scan_end && t[le] != '\n') le++;
            size_t ll = le - i;
            if (ll > 0 && t[le - 1] == '\r') ll--;
            const char *l = t + i;
            size_t j = 0;
            while (j < ll && (l[j] == ' ' || l[j] == '\t')) j++;
            if (ll - j >= 3 && (memcmp(l + j, "```", 3) == 0 || memcmp(l + j, "~~~", 3) == 0)) {
                p->in_fence = !p->in_fence;
                if (p->in_fence) {
                    p->fence_line_off = i;
                    p->fence_content_off = le + 1;
                }
            }
            i = le + 1;
        }
        p->fence_scan_off = scan_end;
    }
    /* fence 快速路径：仍在代码块内时只追加尾部（不重解析整个块） */
    if (p->in_fence && p->count > 0) {
        int new_fences = count_fence_lines((const char *)p->text.data, old_len, p->text.len);
        RikkaMdBlock *last = &p->blocks[p->count - 1];
        if (new_fences == 0 && last->type == RIKKA_MD_CODE_BLOCK) {
            last->text = (const char *)p->text.data + p->fence_content_off;
            last->len = p->text.len - p->fence_content_off;
            p->open_block_off = SIZE_MAX;
            return;
        }
    }
    /* 段落快速路径：新段无空行且最后块是段落 → 只更新 len（inline 延迟到空行时解析） */
    if (!p->in_fence && p->count > 0) {
        RikkaMdBlock *last = &p->blocks[p->count - 1];
        if (last->type == RIKKA_MD_PARAGRAPH) {
            const char *t = (const char *)p->text.data;
            int has_blank = 0;
            for (size_t i = old_len; i + 1 < p->text.len; i++) {
                if (t[i] == '\n' && t[i + 1] == '\n') { has_blank = 1; break; }
            }
            if (!has_blank) {
                last->len = p->text.len - last->line_off;
                p->open_block_off = last->line_off;
                p->parse_from = last->line_off; /* 回退：段落跨 feed 边界时从起点重解析 */
                return;
            }
        }
    }
    size_t b = find_boundary_inc(p, p->text.len);
    if (p->open_block_off != SIZE_MAX && p->open_block_off < b) b = p->open_block_off;
    if (b < p->parse_from) b = p->parse_from;
    p->parse_from = b;
    rebuild_blocks(p);
    /* 前进到"最后完整块的 end"：未完成块（heading/段落无 \n 结尾）保持起点 */
    {
        const char *t = (const char *)p->text.data;
        size_t last_complete = p->parse_from;
        for (size_t i = 0; i < p->count; i++) {
            RikkaMdBlock *blk = &p->blocks[i];
            size_t blk_end = (size_t)(blk->text - t) + blk->len;
            /* 代码块未完成（in_fence）时不前进；段落以 \n 结尾才完整 */
            if ((blk->type == RIKKA_MD_CODE_BLOCK && !p->in_fence) ||
                (blk->type != RIKKA_MD_CODE_BLOCK && blk_end < p->text.len && t[blk_end] == '\n')) {
                last_complete = blk_end;
            }
        }
        p->parse_from = last_complete;
    }
    /* 更新开放块状态：最后块未以换行结束时记录起点 */
    p->open_block_off = SIZE_MAX;
    if (p->count > 0) {
        RikkaMdBlock *last = &p->blocks[p->count - 1];
        {
            size_t text_off = (size_t)(last->text - (const char *)p->text.data);
            size_t end = text_off + last->len;
            if (end >= p->text.len || p->text.data[end] != '\n')
                p->open_block_off = last->line_off;
        }
    }
}

const RikkaMdBlock *rmd_blocks(RikkaMdParser *p, size_t *count) {
    if (count) *count = p->count;
    return p->blocks;
}

size_t rmd_total(const RikkaMdParser *p) { return p->text.len; }
