/*
 * 文本替换三级策略（对标 JVM 版 TextReplacers.kt）。
 *
 * 第一级 exact：精确匹配、非重叠计数（String.replace 语义）。
 * 第二级 line_trimmed：逐行 trim 后窗口比较，容忍缩进/行尾空白/CRLF 差异；
 *   命中后以窗口首行的真实缩进重排 new_text。
 * 第三级 block_anchor：old_text ≥3 行时仅用首尾行做锚点，容忍中间行差异。
 *
 * 主流程：逐级尝试，第一个产生匹配的级别生效；非 replace_all 时匹配
 * 数必须恰为 1；全部级别无匹配报 "not found"。
 */
#define _POSIX_C_SOURCE 200809L
#include "rikka/ai/tool.h"
#include "rikka/core/buffer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 行：内容区间 [start, end)（不含 \r\n） */
typedef struct {
    size_t start, end;
} RkLine;

static int split_lines(const char *content, size_t len, RkLine *lines, size_t cap,
                       size_t *n_out) {
    size_t start = 0;
    size_t n = 0;
    for (size_t i = 0; i < len; i++) {
        if (content[i] == '\n') {
            size_t end = (i > start && content[i - 1] == '\r') ? i - 1 : i;
            if (n < cap) {
                lines[n].start = start;
                lines[n].end = end;
            }
            n++;
            start = i + 1;
        }
    }
    if (n < cap) {
        lines[n].start = start;
        lines[n].end = len;
    }
    n++;
    *n_out = n;
    return n <= cap ? 0 : -1; /* 容量不足 */
}

/* old_text 拆行（去尾空行）；trimmed 版本写入 trims */
typedef struct {
    RkLine *lines;      /* 原始行区间（相对 old_text） */
    char **trimmed;     /* trim 后副本（malloc） */
    size_t count;
    char first_indent[128]; /* old 首行原始缩进（reindent 用） */
} OldLines;

static void old_lines_free(OldLines *o) {
    free(o->lines);
    for (size_t i = 0; i < o->count; i++) free(o->trimmed[i]);
    free(o->trimmed);
}

static int old_lines_build(const char *old_text, OldLines *o) {
    memset(o, 0, sizeof(*o));
    size_t olen = strlen(old_text);
    size_t cap = 64;
    o->lines = (RkLine *)malloc(cap * sizeof(RkLine));
    o->trimmed = (char **)malloc(cap * sizeof(char *));
    if (!o->lines || !o->trimmed) return -1;
    size_t n = 0;
    size_t start = 0;
    for (size_t i = 0; i < olen; i++) {
        if (old_text[i] == '\n') {
            size_t end = (i > start && old_text[i - 1] == '\r') ? i - 1 : i;
            if (n == cap) {
                cap *= 2;
                RkLine *nl = (RkLine *)realloc(o->lines, cap * sizeof(RkLine));
                char **nt = (char **)realloc(o->trimmed, cap * sizeof(char *));
                if (!nl || !nt) return -1;
                o->lines = nl;
                o->trimmed = nt;
            }
            o->lines[n].start = start;
            o->lines[n].end = end;
            n++;
            start = i + 1;
        }
    }
    /* 最后一段 */
    if (n == cap) {
        cap *= 2;
        RkLine *nl = (RkLine *)realloc(o->lines, cap * sizeof(RkLine));
        char **nt = (char **)realloc(o->trimmed, cap * sizeof(char *));
        if (!nl || !nt) return -1;
        o->lines = nl;
        o->trimmed = nt;
    }
    o->lines[n].start = start;
    o->lines[n].end = olen;
    n++;
    /* 去尾空行（"foo\n" 语义一行；new_text 同步处理） */
    if (n > 1 && o->lines[n - 1].start == olen) {
        n--;
    }
    o->count = n;
    for (size_t i = 0; i < n; i++) {
        size_t s = o->lines[i].start, e = o->lines[i].end;
        while (s < e && (old_text[s] == ' ' || old_text[s] == '\t' || old_text[s] == '\r')) s++;
        while (e > s && (old_text[e - 1] == ' ' || old_text[e - 1] == '\t' || old_text[e - 1] == '\r')) e--;
        size_t tlen = e - s;
        o->trimmed[i] = (char *)malloc(tlen + 1);
        if (!o->trimmed[i]) return -1;
        memcpy(o->trimmed[i], old_text + s, tlen);
        o->trimmed[i][tlen] = '\0';
    }
    /* 首行原始缩进 */
    {
        size_t s = o->lines[0].start, e = o->lines[0].end;
        size_t k = 0;
        while (s + k < e && k + 1 < sizeof(o->first_indent) &&
               (old_text[s + k] == ' ' || old_text[s + k] == '\t')) {
            k++;
        }
        memcpy(o->first_indent, old_text + s, k);
        o->first_indent[k] = '\0';
    }
    return 0;
}

static int line_blank(const char *s) {
    for (; *s; s++) {
        if (*s != ' ' && *s != '\t' && *s != '\r') return 0;
    }
    return 1;
}

/* 行 trim 副本（malloc）；len 输出 */
static char *line_trimmed(const char *content, size_t start, size_t end, size_t *len_out) {
    while (start < end && (content[start] == ' ' || content[start] == '\t')) start++;
    while (end > start && (content[end - 1] == ' ' || content[end - 1] == '\t')) end--;
    size_t tlen = end - start;
    char *t = (char *)malloc(tlen + 1);
    if (!t) return NULL;
    memcpy(t, content + start, tlen);
    t[tlen] = '\0';
    if (len_out) *len_out = tlen;
    return t;
}

/* 行首缩进 */
static void indent_of(const char *content, size_t start, size_t end, char *out, size_t out_sz) {
    size_t n = 0;
    while (start + n < end && n + 1 < out_sz &&
           (content[start + n] == ' ' || content[start + n] == '\t')) {
        n++;
    }
    memcpy(out, content + start, n);
    out[n] = '\0';
}

/* reindent：把 new_text 每行的 old_indent 前缀替换为 new_indent（空白行保留） */
static void reindent_text(const char *new_text, const char *old_indent,
                          const char *new_indent, Buf *out) {
    const char *p = new_text;
    size_t olen = strlen(old_indent);
    size_t nlen = strlen(new_indent);
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t llen = nl ? (size_t)(nl - p) : strlen(p);
        if (line_blank(p)) {
            buf_append(out, p, llen); /* 空白行保留原样 */
        } else if (olen > 0 && llen >= olen && memcmp(p, old_indent, olen) == 0) {
            buf_append(out, new_indent, nlen);
            buf_append(out, p + olen, llen - olen);
        } else {
            buf_append(out, p, llen);
        }
        if (nl) {
            buf_append_byte(out, '\n');
            p = nl + 1;
        } else {
            break;
        }
    }
}

/* 应用匹配并构建结果文本 */
static void apply_matches(const char *content, size_t content_len,
                          const RkLine *matches, size_t n_matches,
                          const char *replacement, char **updated) {
    Buf out;
    buf_init(&out);
    size_t cursor = 0;
    for (size_t i = 0; i < n_matches; i++) {
        buf_append(&out, content + cursor, matches[i].start - cursor);
        buf_append_str(&out, replacement);
        cursor = matches[i].end;
    }
    buf_append(&out, content + cursor, content_len - cursor);
    char *r = (char *)malloc(out.len + 1);
    if (r) {
        memcpy(r, out.data, out.len);
        r[out.len] = '\0';
    }
    buf_free(&out);
    *updated = r;
}

/* 单级行窗口匹配（level=2 全行比较；level=3 首尾锚点）。
 * 返回 malloc 的 matches 数组（调用方 free）与计数。 */
static RkLine *match_line_window(const char *content, size_t content_len, int level,
                                 const OldLines *old, const char *new_text,
                                 size_t *count_out, char **replacement_out) {
    size_t ncl = 0;
    RkLine *cl = (RkLine *)malloc((content_len + 1) * sizeof(RkLine));
    if (!cl) return NULL;
    if (split_lines(content, content_len, cl, content_len + 1, &ncl) != 0) {
        free(cl);
        return NULL;
    }
    size_t oldn = old->count;
    size_t cap = 32, count = 0;
    RkLine *matches = (RkLine *)malloc(cap * sizeof(RkLine));
    if (!matches) { free(cl); return NULL; }
    Buf repl;
    buf_init(&repl);
    size_t index = 0;
    while (index + oldn <= ncl) {
        int hit = 0;
        if (level == 2) {
            hit = 1;
            for (size_t k = 0; k < oldn; k++) {
                char *wt = line_trimmed(content, cl[index + k].start, cl[index + k].end, NULL);
                if (!wt || strcmp(wt, old->trimmed[k]) != 0) {
                    hit = 0;
                    free(wt);
                    break;
                }
                free(wt);
            }
        } else { /* level 3: 首尾锚点（oldn>=3 已由调用方保证） */
            char *w1 = line_trimmed(content, cl[index].start, cl[index].end, NULL);
            char *w2 = line_trimmed(content, cl[index + oldn - 1].start,
                                    cl[index + oldn - 1].end, NULL);
            if (w1 && w2 && strcmp(w1, old->trimmed[0]) == 0 &&
                strcmp(w2, old->trimmed[oldn - 1]) == 0) {
                hit = 1;
            }
            free(w1);
            free(w2);
        }
        if (hit) {
            if (count == cap) {
                cap *= 2;
                RkLine *nm = (RkLine *)realloc(matches, cap * sizeof(RkLine));
                if (!nm) { free(matches); free(cl); buf_free(&repl); return NULL; }
                matches = nm;
            }
            matches[count].start = cl[index].start;
            matches[count].end = cl[index + oldn - 1].end;
            if (count == 0 && replacement_out) {
                /* 替换文本 = reindent(new_text, old 首行缩进, 窗口首行缩进) */
                char ni[128];
                indent_of(content, cl[index].start, cl[index].end, ni, sizeof(ni));
                reindent_text(new_text, old->first_indent, ni, &repl);
            }
            count++;
            index += oldn; /* 非重叠 */
        } else {
            index++;
        }
    }
    free(cl);
    *count_out = count;
    if (replacement_out) {
        *replacement_out = repl.len > 0 ? strndup((const char *)repl.data, repl.len) : NULL;
        buf_free(&repl);
    }
    return matches;
}

void rk_text_replace(const char *content, size_t content_len,
                     const char *old_text, const char *new_text, int replace_all,
                     RkTextReplaceResult *out) {
    memset(out, 0, sizeof(*out));
    if (!content || !old_text || !new_text || old_text[0] == '\0') {
        out->error = 1;
        snprintf(out->errmsg, sizeof(out->errmsg), "old_text must not be empty");
        return;
    }
    /* ---- 第一级：精确匹配（非重叠） ---- */
    {
        size_t olen = strlen(old_text);
        RkLine *matches = NULL;
        size_t cap = 64, count = 0;
        matches = (RkLine *)malloc(cap * sizeof(RkLine));
        if (!matches) { out->error = 1; snprintf(out->errmsg, sizeof(out->errmsg), "oom"); return; }
        size_t pos = 0;
        while (pos + olen <= content_len) {
            if (memcmp(content + pos, old_text, olen) == 0) {
                if (count == cap) {
                    cap *= 2;
                    RkLine *nm = (RkLine *)realloc(matches, cap * sizeof(RkLine));
                    if (!nm) { free(matches); out->error = 1; snprintf(out->errmsg, sizeof(out->errmsg), "oom"); return; }
                    matches = nm;
                }
                matches[count].start = pos;
                matches[count].end = pos + olen;
                count++;
                pos += olen; /* 非重叠 */
            } else {
                pos++;
            }
        }
        if (count > 0) {
            out->strategy = "exact";
            out->occurrences = count;
            if (!replace_all && count != 1) {
                out->error = 2;
                snprintf(out->errmsg, sizeof(out->errmsg),
                         "old_text matches %zu locations (strategy: exact); "
                         "add more surrounding context to make it unique, or set replace_all=true",
                         count);
                free(matches);
                return;
            }
            apply_matches(content, content_len, matches, count, new_text, &out->updated);
            out->replacements = count;
            free(matches);
            return;
        }
        free(matches);
    }
    /* ---- 第二/三级：行窗口匹配 ---- */
    {
        OldLines old;
        if (old_lines_build(old_text, &old) != 0) {
            out->error = 1;
            snprintf(out->errmsg, sizeof(out->errmsg), "oom");
            return;
        }
        /* old 全空白行 → 禁用宽松匹配 */
        int has_nonblank = 0;
        for (size_t i = 0; i < old.count; i++) {
            if (!line_blank(old.trimmed[i])) { has_nonblank = 1; break; }
        }
        for (int level = 2; level <= 3 && has_nonblank; level++) {
            if (level == 3 && !(old.count >= 3 && old.trimmed[0][0] &&
                                old.trimmed[old.count - 1][0])) {
                continue; /* 第三级前提：≥3 行且首尾非空 */
            }
            size_t count = 0;
            char *repl_text = NULL;
            RkLine *matches = match_line_window(content, content_len, level, &old,
                                                new_text, &count, &repl_text);
            if (!matches) { old_lines_free(&old); out->error = 1; snprintf(out->errmsg, sizeof(out->errmsg), "oom"); return; }
            if (count > 0) {
                const char *sname = level == 2 ? "line_trimmed" : "block_anchor";
                out->strategy = sname;
                out->occurrences = count;
                if (!replace_all && count != 1) {
                    out->error = 2;
                    snprintf(out->errmsg, sizeof(out->errmsg),
                             "old_text matches %zu locations (strategy: %s); "
                             "add more surrounding context to make it unique, or set replace_all=true",
                             count, sname);
                    free(matches);
                    free(repl_text);
                    old_lines_free(&old);
                    return;
                }
                /* replacement 文本（reindent 后；NULL = 无变化用 new_text） */
                const char *rep = repl_text ? repl_text : new_text;
                apply_matches(content, content_len, matches, count, rep, &out->updated);
                out->replacements = count;
                free(matches);
                free(repl_text);
                old_lines_free(&old);
                return;
            }
            free(matches);
            free(repl_text);
        }
        old_lines_free(&old);
    }
    out->error = 1;
    snprintf(out->errmsg, sizeof(out->errmsg),
             "old_text was not found, even with whitespace-tolerant matching; "
             "read the file again and copy old_text exactly from its current content");
}
