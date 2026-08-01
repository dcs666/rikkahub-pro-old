/*
 * 消息变换管线（对标 JVM 版 data/ai/transformers）。
 *
 * 本文件：消息列表 + 正则/模板/文本工具 + 简单 transformers；
 * prompt_injection（注入引擎）在 transform_inj.c。
 *
 * 性能要点：
 *  - 输入冻结消息只读（COW），变换只写 arena 新文本；
 *  - 无变化的 part 保持引用（零拷贝共享）；
 *  - 正则编译每次调用（输入侧调用频率低）；如需热路径可加缓存。
 */
#define _POSIX_C_SOURCE 200809L
#include "rikka/ai/transform.h"
#include "rikka/core/buffer.h"
#include <ctype.h>
#include <regex.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* ================= 消息列表 ================= */

void rk_msgl_init(RkMsgList *l, Arena *a) {
    l->arena = a;
    l->items = NULL;
    l->count = l->cap = 0;
}

static void msgl_grow(RkMsgList *l) {
    size_t nc = l->cap ? l->cap * 2 : 8;
    if (nc < l->cap || nc > SIZE_MAX / sizeof(*l->items)) return; /* 溢出防护 */
    RikkaMessage **ni = (RikkaMessage **)arena_alloc(l->arena, sizeof(void *), nc * sizeof(*ni));
    if (!ni) return;
    if (l->items) memcpy(ni, l->items, l->count * sizeof(*ni));
    l->items = ni;
    l->cap = nc;
}

void rk_msgl_insert(RkMsgList *l, size_t idx, RikkaMessage *m) {
    if (idx > l->count) idx = l->count;
    if (l->count == l->cap) msgl_grow(l);
    if (l->count < l->cap) {
        memmove(&l->items[idx + 1], &l->items[idx], (l->count - idx) * sizeof(*l->items));
        l->items[idx] = m;
        l->count++;
    }
}

void rk_msgl_move(RkMsgList *l, size_t from, size_t to) {
    if (from >= l->count) return;
    if (to > l->count - 1) to = l->count - 1;
    if (from == to) return;
    RikkaMessage *m = l->items[from];
    if (to < from) {
        memmove(&l->items[to + 1], &l->items[to], (from - to) * sizeof(*l->items));
    } else {
        memmove(&l->items[from], &l->items[from + 1], (to - from) * sizeof(*l->items));
    }
    l->items[to] = m;
}

void rk_msgl_from(RkMsgList *l, Arena *a, const RikkaMessage *const *msgs, size_t n) {
    rk_msgl_init(l, a);
    for (size_t i = 0; i < n; i++) {
        const RikkaMessage *src = msgs[i];
        if (!src) continue;
        RikkaMessage *m = rmsg_new(a, src->role);
        if (src->part_count > 0) {
            m->parts = (RikkaPart *)arena_alloc(a, sizeof(void *), src->part_count * sizeof(RikkaPart));
            if (!m->parts) return;
            memcpy(m->parts, src->parts, src->part_count * sizeof(RikkaPart));
            m->part_count = m->part_cap = src->part_count;
        }
        m->created_at = src->created_at;
        m->has_usage = src->has_usage;
        m->prompt_tokens = src->prompt_tokens;
        m->completion_tokens = src->completion_tokens;
        m->total_tokens = src->total_tokens;
        rk_msgl_insert(l, l->count, m);
    }
}

RikkaMessage *rk_msgl_add(RkMsgList *l, RikkaRole role) {
    RikkaMessage *m = rmsg_new(l->arena, role);
    if (!m) return NULL;
    rk_msgl_insert(l, l->count, m);
    return m;
}

RikkaPart *rk_msgl_add_part(RkMsgList *l, RikkaMessage *m, RikkaPartType type) {
    (void)l;
    return rmsg_add_part(l->arena, m, type);
}

void rk_msgl_append_text(RkMsgList *l, RikkaMessage *m, const char *text) {
    size_t tlen = strlen(text);
    /* 合并到最后一个 TEXT part（JVM appendText 语义） */
    for (size_t i = m->part_count; i > 0; i--) {
        RikkaPart *p = &m->parts[i - 1];
        if (p->type == RIKKA_PART_TEXT) {
            size_t nlen = p->len + tlen;
            char *joined = (char *)arena_alloc(l->arena, 1, nlen + 1);
            if (!joined) return;
            memcpy(joined, p->data, p->len);
            memcpy(joined + p->len, text, tlen);
            joined[nlen] = '\0';
            p->data = joined;
            p->len = nlen;
            return;
        }
    }
    RikkaPart *p = rmsg_add_part(l->arena, m, RIKKA_PART_TEXT);
    char *copy = (char *)arena_alloc(l->arena, 1, tlen + 1);
    if (!copy) return;
    memcpy(copy, text, tlen + 1);
    p->data = copy;
    p->len = tlen;
}

/* ================= 文本工具 ================= */

static char *arena_strndup(Arena *a, const char *s, size_t n) {
    char *p = (char *)arena_alloc(a, 1, n + 1);
    if (!p) return NULL;
    if (n > 0) memcpy(p, s, n); /* 空 Buf（data=NULL, len=0）时跳过拷贝（UBSan） */
    p[n] = '\0';
    return p;
}

static char *arena_strdup(Arena *a, const char *s) {
    return arena_strndup(a, s, strlen(s));
}

const char *rk_strcasestr(const char *haystack, const char *needle) {
    if (!needle || !*needle) return haystack;
    size_t nlen = strlen(needle);
    for (const char *p = haystack; *p; p++) {
        size_t i = 0;
        while (i < nlen && tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) i++;
        if (i == nlen) return p;
        if (!p[i]) break;
    }
    return NULL;
}

/* ================= 正则工具 ================= */

int rk_regex_contains(const char *text, const char *pattern, int icase) {
    regex_t re;
    int flags = REG_EXTENDED;
    if (icase) flags |= REG_ICASE;
    if (regcomp(&re, pattern, flags) != 0) return 0; /* 编译失败 = 不匹配（JVM 同语义） */
    int rc = regexec(&re, text, 0, NULL, 0);
    regfree(&re);
    return rc == 0;
}

/* replacement 中 $N / $$ 展开；组不存在或未匹配返回 -1（JVM 抛异常 → 整体失败） */
static int append_replacement(Buf *out, const regmatch_t *m, size_t nm,
                              const char *text, size_t base, const char *repl) {
    for (const char *p = repl; *p; p++) {
        if (*p != '$') {
            buf_append(out, p, 1);
            continue;
        }
        p++;
        if (*p == '$') {
            buf_append(out, "$", 1); /* $$ → $ */
        } else if (*p >= '1' && *p <= '9') {
            size_t gi = (size_t)(*p - '0');
            if (gi >= nm || m[gi].rm_so < 0 || m[gi].rm_eo < 0) return -1; /* 组不存在/未匹配 */
            buf_append(out, text + base + (size_t)m[gi].rm_so,
                       (size_t)(m[gi].rm_eo - m[gi].rm_so));
        } else {
            return -1; /* 非法引用（JVM 抛异常） */
        }
    }
    return 0;
}

char *rk_regex_replace(Arena *a, const char *text, const char *pattern,
                       const char *replacement, int icase) {
    regex_t re;
    int flags = REG_EXTENDED;
    if (icase) flags |= REG_ICASE;
    if (regcomp(&re, pattern, flags) != 0) return arena_strdup(a, text);
    Buf out;
    buf_init(&out);
    const char *p = text;
    regmatch_t m[10];
    int matched = 0;
    while (regexec(&re, p, 10, m, 0) == 0) {
        if (m[0].rm_so < 0) break;
        buf_append(&out, p, (size_t)m[0].rm_so); /* 匹配前文本 */
        if (append_replacement(&out, m, 10, text, (size_t)(p - text), replacement) != 0) {
            buf_free(&out);
            regfree(&re);
            return arena_strdup(a, text); /* 替换失败 → 原串（JVM 同语义） */
        }
        p += m[0].rm_eo;
        matched = 1;
        if (m[0].rm_eo == m[0].rm_so) { /* 零长匹配：前进一个字符防死循环 */
            if (!*p) break;
            buf_append(&out, p, 1);
            p++;
        }
    }
    if (!matched) {
        buf_free(&out);
        regfree(&re);
        return arena_strdup(a, text);
    }
    buf_append(&out, p, strlen(p));
    char *r = arena_strndup(a, (const char *)out.data, out.len);
    buf_free(&out);
    regfree(&re);
    return r;
}

/* ================= 模板渲染（Pebble 子集） ================= */

char *rk_template_render(Arena *a, const char *tpl,
                         const char *const *names, const char *const *values, size_t n) {
    Buf out;
    buf_init(&out);
    const char *p = tpl;
    for (;;) {
        const char *open = strstr(p, "{{");
        if (!open) break;
        buf_append(&out, p, (size_t)(open - p));
        const char *close = strstr(open + 2, "}}");
        if (!close) { /* 未闭合：原样输出剩余 */
            buf_append(&out, open, strlen(open));
            p = open + strlen(open);
            break;
        }
        /* 提取变量名（trim 空白） */
        const char *vn = open + 2;
        size_t vlen = (size_t)(close - vn);
        while (vlen > 0 && isspace((unsigned char)*vn)) { vn++; vlen--; }
        while (vlen > 0 && isspace((unsigned char)vn[vlen - 1])) vlen--;
        const char *val = NULL;
        for (size_t i = 0; i < n; i++) {
            if (names[i] && vlen == strlen(names[i]) && strncmp(vn, names[i], vlen) == 0) {
                val = values[i];
                break;
            }
        }
        if (val) buf_append(&out, val, strlen(val));
        p = close + 2;
    }
    buf_append(&out, p, strlen(p));
    char *r = arena_strndup(a, (const char *)out.data, out.len);
    buf_free(&out);
    return r;
}

/* ================= 1. 时间提醒 ================= */

static void fmt_reminder(Arena *a, Buf *out, int64_t epoch, int64_t gap_secs, int has_gap) {
    (void)a;
    time_t t = (time_t)epoch;
    struct tm tmv;
    localtime_r(&t, &tmv);
    char datebuf[64];
    char wdbuf[16];
    strftime(datebuf, sizeof(datebuf), "%Y-%m-%dT%H:%M:%S", &tmv);
    strftime(wdbuf, sizeof(wdbuf), "%A", &tmv);
    buf_append_str(out, "<time_reminder>Current time: ");
    buf_append_str(out, wdbuf);
    buf_append_str(out, ", ");
    buf_append_str(out, datebuf);
    if (has_gap) {
        char gap[48];
        if (gap_secs < 3600) {
            snprintf(gap, sizeof(gap), " (%ld min since last message)", (long)(gap_secs / 60));
        } else if (gap_secs < 86400) {
            snprintf(gap, sizeof(gap), " (%ld h since last message)", (long)(gap_secs / 3600));
        } else {
            snprintf(gap, sizeof(gap), " (%ld d since last message)", (long)(gap_secs / 86400));
        }
        buf_append_str(out, gap);
    }
    buf_append_str(out, "</time_reminder>");
}

void rk_transform_time_reminder(RkMsgList *l, int64_t now_epoch) {
    int first_user = 0;
    size_t i = 0;
    while (i < l->count) {
        RikkaMessage *m = l->items[i];
        if (m->role != RIKKA_ROLE_USER) {
            i++;
            continue;
        }
        Buf text;
        buf_init(&text);
        if (!first_user) {
            /* 首条用户消息总是注入（JVM 语义）；时间未知用 now */
            int64_t ts = m->created_at;
            if (ts == 0) ts = now_epoch ? now_epoch : (int64_t)time(NULL);
            fmt_reminder(l->arena, &text, ts, 0, 0);
        } else if (i > 0 && m->created_at > 0) {
            const RikkaMessage *prev = l->items[i - 1];
            if (prev->created_at > 0) {
                int64_t gap = m->created_at - prev->created_at;
                if (gap > 3600) fmt_reminder(l->arena, &text, m->created_at, gap, 1);
            }
        }
        if (text.len > 0) {
            char *msg_text = arena_strndup(l->arena, (const char *)text.data, text.len);
            RikkaMessage *rem = rk_msgl_add(l, RIKKA_ROLE_USER);
            rem->created_at = m->created_at;
            if (rem->created_at == 0) rem->created_at = now_epoch ? now_epoch : (int64_t)time(NULL);
            RikkaPart *p = rk_msgl_add_part(l, rem, RIKKA_PART_TEXT);
            p->data = msg_text;
            p->len = text.len;
            rk_msgl_move(l, l->count - 1, i); /* 从尾部移到插入点（复制插入会留尾部残留） */
            i++;                              /* 跳过刚插入的提醒 */
        }
        buf_free(&text);
        first_user = 1;
        i++;
    }
}

/* ================= 3. 占位符 ================= */

static const char *const PLACEHOLDER_KEYS[] = {
    "cur_date", "model_id", "model_name", "locale", "timezone",
    "system_version", "device_info", "battery_level", "nickname", "char", "user",
};

static const char *placeholder_value(const RkTransformConfig *cfg, size_t key_idx) {
    static char datebuf[32];
    static char batt[16];
    static char dev[192];
    switch (key_idx) {
    case 0: { /* cur_date */
        time_t t = time(NULL);
        struct tm tmv;
        localtime_r(&t, &tmv);
        strftime(datebuf, sizeof(datebuf), "%Y-%m-%d", &tmv);
        return datebuf;
    }
    case 1: return cfg->model_id ? cfg->model_id : "";
    case 2: return cfg->model_name ? cfg->model_name : "";
    case 3: return cfg->locale_name ? cfg->locale_name : "unknown";
    case 4: return cfg->tz_name ? cfg->tz_name : "UTC";
    case 5: return cfg->os_version ? cfg->os_version : "unknown";
    case 6: { /* device_info */
        const char *b = cfg->device_brand;
        const char *m = cfg->device_model;
        if (b && m) {
            snprintf(dev, sizeof(dev), "%s %s", b, m);
        } else if (b) {
            snprintf(dev, sizeof(dev), "%s", b);
        } else if (m) {
            snprintf(dev, sizeof(dev), "%s", m);
        } else {
            snprintf(dev, sizeof(dev), "unknown");
        }
        return dev;
    }
    case 7: /* battery_level */
        if (cfg->battery_pct < 0) return "?";
        snprintf(batt, sizeof(batt), "%d", cfg->battery_pct);
        return batt;
    case 8: /* nickname */
        return cfg->user_nickname && cfg->user_nickname[0] ? cfg->user_nickname : "user";
    case 9: /* char */
        return cfg->assistant_name && cfg->assistant_name[0] ? cfg->assistant_name : "assistant";
    case 10: /* user */
        return cfg->user_nickname && cfg->user_nickname[0] ? cfg->user_nickname : "user";
    default: return "";
    }
}

/* 大小写不敏感全替换；无命中返回 NULL */
static char *replace_all_ci(Arena *a, const char *text, const char *key, const char *value) {
    if (!rk_strcasestr(text, key)) return NULL;
    Buf out;
    buf_init(&out);
    const char *p = text;
    size_t klen = strlen(key);
    for (;;) {
        const char *hit = rk_strcasestr(p, key);
        if (!hit) break;
        buf_append(&out, p, (size_t)(hit - p));
        buf_append(&out, value, strlen(value));
        p = hit + klen;
    }
    buf_append(&out, p, strlen(p));
    char *r = arena_strndup(a, (const char *)out.data, out.len);
    buf_free(&out);
    return r;
}

void rk_transform_placeholder(RkMsgList *l, const RkTransformConfig *cfg) {
    for (size_t i = 0; i < l->count; i++) {
        RikkaMessage *m = l->items[i];
        for (size_t j = 0; j < m->part_count; j++) {
            RikkaPart *p = &m->parts[j];
            if (p->type != RIKKA_PART_TEXT) continue;
            const char *cur = p->data;
            int changed = 0;
            for (size_t k = 0; k < sizeof(PLACEHOLDER_KEYS) / sizeof(PLACEHOLDER_KEYS[0]); k++) {
                const char *val = placeholder_value(cfg, k);
                char key1[48], key2[48];
                snprintf(key1, sizeof(key1), "{{%s}}", PLACEHOLDER_KEYS[k]);
                snprintf(key2, sizeof(key2), "{%s}", PLACEHOLDER_KEYS[k]);
                char *r1 = replace_all_ci(l->arena, cur, key1, val);
                if (r1) { cur = r1; changed = 1; }
                char *r2 = replace_all_ci(l->arena, cur, key2, val);
                if (r2) { cur = r2; changed = 1; }
            }
            if (changed) {
                p->data = cur;
                p->len = strlen(cur);
            }
        }
    }
}

/* ================= 4. 文档转提示 ================= */

void rk_transform_document_as_prompt(RkMsgList *l, RkDocReader reader,
                                     RkDocPathResolver resolver, void *ud) {
    for (size_t i = 0; i < l->count; i++) {
        RikkaMessage *m = l->items[i];
        if (m->part_count == 0) continue;
        /* 收集文档 part */
        size_t ndoc = 0;
        for (size_t j = 0; j < m->part_count; j++) {
            if (m->parts[j].type == RIKKA_PART_DOCUMENT) ndoc++;
        }
        if (ndoc == 0) continue;
        /* 构建新 parts：文档提示块（顺序与 JVM add(0) 一致 = 反序）+ 原 parts */
        RikkaPart *np = (RikkaPart *)arena_alloc(l->arena, sizeof(void *),
                                                 (m->part_count + ndoc) * sizeof(RikkaPart));
        if (!np) return;
        size_t ncount = 0;
        for (size_t j = 0; j < m->part_count; j++) {
            RikkaPart *dp = &m->parts[j];
            if (dp->type != RIKKA_PART_DOCUMENT) continue;
            const char *name = dp->doc_name ? dp->doc_name : "document";
            const char *content = reader ? reader(dp->doc_mime, dp->data, name, ud) : NULL;
            const char *wpath = resolver ? resolver(dp->data, ud) : NULL;
            if (!content) {
                char err[256];
                snprintf(err, sizeof(err), "[ERROR, failed to read file: %s]", name);
                content = arena_strdup(l->arena, err);
            }
            char path_attr[512];
            if (wpath && wpath[0]) {
                snprintf(path_attr, sizeof(path_attr), " path=\"%s\"", wpath);
            } else {
                path_attr[0] = '\0';
            }
            /* <UploadFile name=".." path="..">\n```\ncontent\n```\n</UploadFile> */
            size_t cap = strlen(name) + strlen(path_attr) + strlen(content) + 64;
            char *blk = (char *)arena_alloc(l->arena, 1, cap);
            if (!blk) return;
            snprintf(blk, cap, "<UploadFile name=\"%s\"%s>\n```\n%s\n```\n</UploadFile>",
                     name, path_attr, content);
            /* JVM add(0) 语义：每个新块插到最前 → 文档顺序反转 */
            memmove(&np[1], &np[0], ncount * sizeof(RikkaPart));
            np[0].type = RIKKA_PART_TEXT;
            np[0].data = blk;
            np[0].len = strlen(blk);
            ncount++;
        }
        memcpy(&np[ncount], m->parts, m->part_count * sizeof(RikkaPart));
        m->parts = np;
        m->part_count += ncount;
        m->part_cap = m->part_count;
    }
}

/* ================= 5. OCR ================= */

void rk_transform_ocr(RkMsgList *l, const RkTransformConfig *cfg, RkOcrCallback ocr, void *ud) {
    if (cfg->model_supports_images) return; /* 模型原生支持图片：无需 OCR */
    for (size_t i = 0; i < l->count; i++) {
        RikkaMessage *m = l->items[i];
        for (size_t j = 0; j < m->part_count; j++) {
            RikkaPart *p = &m->parts[j];
            if (p->type != RIKKA_PART_IMAGE) continue;
            if (!p->data || strncmp(p->data, "file:", 5) != 0) continue; /* 仅本地文件图片 */
            const char *text = ocr ? ocr(p->data, ud) : NULL;
            if (!text) {
                text = "[Image]"; /* JVM：OCR 模型缺失时的占位 */
            }
            /* 构建 <image_file_ocr> 文本块 */
            size_t cap = strlen(text) + 160;
            char *blk = (char *)arena_alloc(l->arena, 1, cap);
            if (!blk) return;
            int n = snprintf(blk, cap,
                             "<image_file_ocr>\n   %s\n</image_file_ocr>\n"
                             "* The image_file_ocr tag contains a description of an image "
                             "that the user uploaded to you, not the user's prompt.",
                             text);
            if (n > 0 && (size_t)n < cap) {
                p->type = RIKKA_PART_TEXT;
                p->data = blk;
                p->len = (size_t)n;
            }
        }
    }
}

/* ================= 6. 模板 ================= */

void rk_transform_template(RkMsgList *l, const char *template_text, int64_t tz_offset_sec) {
    if (!template_text) return;
    const char *names[4] = {"message", "role", "time", "date"};
    for (size_t i = 0; i < l->count; i++) {
        RikkaMessage *m = l->items[i];
        for (size_t j = 0; j < m->part_count; j++) {
            RikkaPart *p = &m->parts[j];
            if (p->type != RIKKA_PART_TEXT) continue;
            int64_t ts = m->created_at;
            if (ts == 0) ts = (int64_t)time(NULL);
            time_t t = (time_t)(ts + tz_offset_sec);
            struct tm tmv;
            gmtime_r(&t, &tmv);
            static char timebuf[16], datebuf[16];
            strftime(timebuf, sizeof(timebuf), "%H:%M:%S", &tmv);
            strftime(datebuf, sizeof(datebuf), "%Y-%m-%d", &tmv);
            const char *role = m->role == RIKKA_ROLE_USER ? "user"
                             : m->role == RIKKA_ROLE_ASSISTANT ? "assistant"
                             : m->role == RIKKA_ROLE_SYSTEM ? "system" : "tool";
            const char *values[4] = {p->data, role, timebuf, datebuf};
            char *rendered = rk_template_render(l->arena, template_text, names, values, 4);
            if (rendered) {
                p->data = rendered;
                p->len = strlen(rendered);
            }
        }
    }
}

/* ================= 7. workspace 提醒 ================= */

void rk_transform_workspace_reminder(RkMsgList *l, const RkTransformConfig *cfg) {
    if (!cfg->workspace_id || !cfg->workspace_id[0]) return;
    Buf prompt;
    buf_init(&prompt);
    buf_append_str(&prompt,
               "<workspace>\n"
               "You have access to a persistent Linux workspace named \"");
    buf_append_str(&prompt, cfg->workspace_id);
    buf_append_str(&prompt,
               "\", running in a sandboxed proot rootfs environment.\n"
               "- The workspace files area is mounted at `/workspace`. Use it as your working directory; "
               "files written there persist across turns of this conversation.\n"
               "- All paths passed to workspace tools must be absolute and inside the Rootfs "
               "(for example `/workspace/notes.md`).\n"
               "- Available tools:\n"
               "  - `workspace_read_file`: read file contents.\n"
               "  - `workspace_write_file` / `workspace_edit_file`: create files, or make precise "
               "edits to existing files.\n"
               "  - `workspace_shell`: run shell commands (the files area is mounted at /workspace).\n"
               "- Prefer `workspace_shell` for tasks that standard Unix tools handle well, and prefer "
               "`workspace_edit_file` for targeted edits over rewriting whole files.\n"
               "- The skills directory is mounted at `/skills`. Each skill is a subdirectory "
               "`/skills/<skill-name>/` containing a `SKILL.md` (with `name` and `description` frontmatter) "
               "plus any supporting files. Read a skill's `SKILL.md` before using it, and follow its "
               "instructions.\n"
               "- Files the user uploaded are mounted at `/upload`. Treat `/upload` as READ-ONLY: "
               "read uploaded files from `/upload/<file-name>`, but never modify, overwrite, or delete "
               "anything there. If you need to change an uploaded file, copy it into `/workspace` first "
               "and edit the copy.\n");
    if (cfg->workspace_cwd && cfg->workspace_cwd[0]) {
        buf_append_str(&prompt, "- Current working directory: `");
        buf_append_str(&prompt, cfg->workspace_cwd);
        buf_append_str(&prompt, "`.\n");
    }
    buf_append_str(&prompt, "</workspace>");
    /* 找第一条 system 消息并追加；不存在则插入 */
    size_t sys_idx = l->count;
    for (size_t i = 0; i < l->count; i++) {
        if (l->items[i]->role == RIKKA_ROLE_SYSTEM) {
            sys_idx = i;
            break;
        }
    }
    if (sys_idx < l->count) {
        /* Buf 不保 NUL 结尾：先复制到 arena（NUL 结尾）再追加 */
        char *ws_prompt = arena_strndup(l->arena, (const char *)prompt.data, prompt.len);
        rk_msgl_append_text(l, l->items[sys_idx], "\n\n");
        rk_msgl_append_text(l, l->items[sys_idx], ws_prompt);
    } else {
        RikkaMessage *m = rk_msgl_add(l, RIKKA_ROLE_SYSTEM);
        RikkaPart *p = rk_msgl_add_part(l, m, RIKKA_PART_TEXT);
        p->data = arena_strndup(l->arena, (const char *)prompt.data, prompt.len);
        p->len = prompt.len;
        rk_msgl_move(l, l->count - 1, 0); /* 从尾部移到最前 */
    }
    buf_free(&prompt);
}

/* ================= 8. ThinkTag ================= */

/* 扫描文本中的 think 块：
 *  - stripped：移除所有 <think>...</think>（未闭合到结尾）后的文本（buf）
 *  - reasoning：第一个块内容（trim 后，arena 副本；无块为 NULL）
 *  - has_closing：第一个块是否有闭合标签 */
static void scan_think(const char *text, size_t len, Arena *a,
                       Buf *stripped, const char **reasoning, int *has_closing) {
    const char *p = text;
    const char *end = text + len;
    const char *first_rs = NULL, *first_re = NULL;
    int first_closing = 0;
    while (p < end) {
        const char *open = NULL;
        for (const char *q = p; q + 7 <= end; q++) {
            if (memcmp(q, "<think>", 7) == 0) { open = q; break; }
        }
        if (!open) break;
        buf_append(stripped, p, (size_t)(open - p)); /* 块前文本保留 */
        const char *cs = open + 7;
        const char *close = NULL;
        for (const char *q = cs; q + 8 <= end; q++) {
            if (memcmp(q, "</think>", 8) == 0) { close = q; break; }
        }
        if (close) {
            if (!first_rs) {
                first_rs = cs;
                first_re = close;
                first_closing = 1;
            }
            p = close + 8;
        } else {
            if (!first_rs) {
                first_rs = cs;
                first_re = end;
                first_closing = 0;
            }
            p = end; /* 未闭合到结尾 */
        }
    }
    if (p < end) buf_append(stripped, p, (size_t)(end - p));
    if (first_rs && first_re) {
        /* trim 空白 */
        while (first_rs < first_re && isspace((unsigned char)*first_rs)) first_rs++;
        while (first_re > first_rs && isspace((unsigned char)first_re[-1])) first_re--;
        *reasoning = arena_strndup(a, first_rs, (size_t)(first_re - first_rs));
    } else {
        *reasoning = NULL;
    }
    *has_closing = first_closing;
}

void rk_transform_think_tag(RkMsgList *l) {
    for (size_t i = 0; i < l->count; i++) {
        RikkaMessage *m = l->items[i];
        if (m->role != RIKKA_ROLE_ASSISTANT) continue;
        /* 先检查是否有含 think 块的 TEXT part */
        int any = 0;
        for (size_t j = 0; j < m->part_count; j++) {
            RikkaPart *p = &m->parts[j];
            if (p->type == RIKKA_PART_TEXT && p->data) {
                const char *hit = strstr(p->data, "<think>");
                if (hit) { any = 1; break; }
            }
        }
        if (!any) continue;
        /* 构建新 parts：每块 [Reasoning, Text(stripped)] 替换原 Text */
        size_t extra = 0;
        for (size_t j = 0; j < m->part_count; j++) {
            if (m->parts[j].type == RIKKA_PART_TEXT &&
                strstr(m->parts[j].data ? m->parts[j].data : "", "<think>")) {
                extra++;
            }
        }
        size_t ncap = m->part_count + extra;
        RikkaPart *np = (RikkaPart *)arena_alloc(l->arena, sizeof(void *), ncap * sizeof(RikkaPart));
        if (!np) return;
        size_t ncount = 0;
        for (size_t j = 0; j < m->part_count; j++) {
            RikkaPart *p = &m->parts[j];
            if (p->type == RIKKA_PART_TEXT && strstr(p->data ? p->data : "", "<think>")) {
                Buf stripped;
                buf_init(&stripped);
                const char *reasoning = NULL;
                int closing = 0;
                scan_think(p->data, p->len, l->arena, &stripped, &reasoning, &closing);
                np[ncount].type = RIKKA_PART_REASONING;
                np[ncount].data = reasoning ? reasoning : "";
                np[ncount].len = reasoning ? strlen(reasoning) : 0;
                ncount++;
                np[ncount] = *p;
                np[ncount].data = arena_strndup(l->arena, (const char *)stripped.data, stripped.len);
                np[ncount].len = stripped.len;
                ncount++;
                buf_free(&stripped);
                (void)closing; /* finished_at 语义在 C 版不承载（无时间字段） */
            } else {
                np[ncount++] = *p;
            }
        }
        m->parts = np;
        m->part_count = ncount;
        m->part_cap = ncount;
    }
}

void rk_transform_think_tag_finish(RkMsgList *l, int64_t now_epoch) {
    (void)now_epoch;
    rk_transform_think_tag(l); /* C 版 Reasoning part 无 finished_at 字段，两者同实现 */
}

/* ================= 9. 正则输出 ================= */

void rk_transform_regex_output(RkMsgList *l, const RkOutputRegex *const *regexes, size_t n) {
    if (!regexes || n == 0) return;
    for (size_t i = 0; i < l->count; i++) {
        RikkaMessage *m = l->items[i];
        if (m->role != RIKKA_ROLE_ASSISTANT) continue;
        for (size_t j = 0; j < m->part_count; j++) {
            RikkaPart *p = &m->parts[j];
            if (p->type != RIKKA_PART_TEXT && p->type != RIKKA_PART_REASONING) continue;
            const char *cur = p->data;
            int changed = 0;
            for (size_t k = 0; k < n; k++) {
                const RkOutputRegex *r = regexes[k];
                if (!r->enabled || !r->affects_assistant || r->visual_only) continue;
                if (!r->find_regex || !r->find_regex[0]) continue;
                char *out = rk_regex_replace(l->arena, cur, r->find_regex,
                                             r->replace_string ? r->replace_string : "", 0);
                if (strcmp(out, cur) != 0) changed = 1;
                cur = out;
            }
            if (changed) {
                p->data = cur;
                p->len = strlen(cur);
            }
        }
    }
}

/* ================= 10. base64 图片落盘 ================= */

void rk_transform_base64_image(RkMsgList *l, RkImageSaver saver, void *ud) {
    if (!saver) return;
    for (size_t i = 0; i < l->count; i++) {
        RikkaMessage *m = l->items[i];
        for (size_t j = 0; j < m->part_count; j++) {
            RikkaPart *p = &m->parts[j];
            if (p->type != RIKKA_PART_IMAGE) continue;
            if (!p->data || strncmp(p->data, "data:", 5) != 0) continue; /* 仅 data URI */
            const char *url = saver(p->data, p->doc_name, ud);
            if (url && url[0]) {
                p->data = url;
                p->len = strlen(url);
            }
        }
    }
}
