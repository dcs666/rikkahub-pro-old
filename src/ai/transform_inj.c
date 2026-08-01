/*
 * 提示词注入引擎（对标 JVM 版 PromptInjectionTransformer.transformMessages）。
 *
 * 语义对齐点：
 *  - 有效 id 集：cfg->conv_*_ids 非空时按会话级过滤（JVM 的
 *    allowConversationPromptInjection 分支；助手级 id 由调用方预过滤传入）；
 *  - 优先级：priority 降序（稳定），再按位置分组应用；
 *  - 位置语义：before/after_system 并入 system 文本；top/bottom_of_chat 与
 *    at_depth 插入独立消息（按 role 分组合并内容）；
 *  - findSafeInsertIndex：不插入 USER → ASSISTANT(含 tool) 之间
 *    （DeepSeek 等厂商要求 USER 后紧跟带工具 ASSISTANT）。
 */
#define _POSIX_C_SOURCE 200809L
#include "rikka/ai/transform.h"
#include "rikka/core/buffer.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* 注入项（收集后排序） */
typedef struct {
    const RkInjection *inj;
} RkInjItem;

static int id_in_list(const char *const *ids, const char *id) {
    if (!ids || !id) return 0;
    for (size_t i = 0; ids[i]; i++) {
        if (strcmp(ids[i], id) == 0) return 1;
    }
    return 0;
}

/* 消息所有 TEXT part 拼接（JVM toText） */
static char *msg_to_text(Arena *a, const RikkaMessage *m) {
    Buf out;
    buf_init(&out);
    for (size_t i = 0; i < m->part_count; i++) {
        const RikkaPart *p = &m->parts[i];
        if (p->type == RIKKA_PART_TEXT && p->data) {
            buf_append(&out, p->data, p->len);
        }
    }
    char *r = arena_alloc(a, 1, out.len + 1);
    if (r) {
        memcpy(r, out.data, out.len);
        r[out.len] = '\0';
    }
    buf_free(&out);
    return r;
}

/* 最近 scan_depth 条非 SYSTEM 消息的文本拼接（JVM extractContextForMatching） */
static char *extract_context(Arena *a, const RkMsgList *l, int scan_depth) {
    if (scan_depth < 1) scan_depth = 1;
    Buf out;
    buf_init(&out);
    size_t taken = 0;
    size_t i = l->count;
    while (i > 0 && taken < (size_t)scan_depth) {
        i--;
        const RikkaMessage *m = l->items[i];
        if (m->role == RIKKA_ROLE_SYSTEM) continue;
        char *t = msg_to_text(a, m);
        if (out.len > 0) buf_append_byte(&out, '\n');
        buf_append_str(&out, t ? t : "");
        taken++;
    }
    char *r = arena_alloc(a, 1, out.len + 1);
    if (r) {
        memcpy(r, out.data, out.len);
        r[out.len] = '\0';
    }
    buf_free(&out);
    return r;
}

/* RegexInjection 是否触发（JVM isTriggered） */
static int entry_triggered(const RkInjection *e, const char *context) {
    if (!e->enabled) return 0;
    if (e->constant_active) return 1;
    if (!e->keywords) return 0;
    for (size_t i = 0; e->keywords[i]; i++) {
        const char *kw = e->keywords[i];
        if (!kw || !kw[0]) continue;
        if (e->use_regex) {
            if (rk_regex_contains(context, kw, !e->case_sensitive)) return 1;
        } else {
            if (e->case_sensitive) {
                if (strstr(context, kw)) return 1;
            } else {
                if (rk_strcasestr(context, kw)) return 1;
            }
        }
    }
    return 0;
}

/* 收集注入项（mode + lorebook 触发） */
static size_t collect_injections(RkInjItem *out, size_t cap,
                                 const RkMsgList *l, const RkTransformConfig *cfg,
                                 const RkInjection *const *mode_injs, size_t n_mode,
                                 const RkLorebook *const *lorebooks, size_t n_lb) {
    size_t n = 0;
    for (size_t i = 0; i < n_mode && n < cap; i++) {
        const RkInjection *e = mode_injs[i];
        if (!e->enabled) continue;
        if (cfg->conv_mode_injection_ids && !id_in_list(cfg->conv_mode_injection_ids, e->id)) {
            continue;
        }
        out[n++].inj = e;
    }
    for (size_t i = 0; i < n_lb && n < cap; i++) {
        const RkLorebook *lb = lorebooks[i];
        if (!lb->enabled) continue;
        if (cfg->conv_lorebook_ids && !id_in_list(cfg->conv_lorebook_ids, lb->id)) continue;
        for (size_t j = 0; j < lb->entry_count && n < cap; j++) {
            const RkInjection *e = lb->entries[j];
            if (!e->enabled) continue;
            Arena *tmp = arena_create(0);
            char *ctx = extract_context(tmp, l, e->scan_depth);
            int hit = entry_triggered(e, ctx);
            arena_destroy(tmp);
            if (hit) out[n++].inj = e;
        }
    }
    return n;
}

/* 稳定插入排序：priority 降序 */
static void sort_by_priority(RkInjItem *items, size_t n) {
    for (size_t i = 1; i < n; i++) {
        RkInjItem key = items[i];
        size_t j = i;
        while (j > 0 && items[j - 1].inj->priority < key.inj->priority) {
            items[j] = items[j - 1];
            j--;
        }
        items[j] = key;
    }
}

static int has_tool_call(const RikkaMessage *m) {
    for (size_t i = 0; i < m->part_count; i++) {
        if (m->parts[i].type == RIKKA_PART_TOOL_CALL) return 1;
    }
    return 0;
}

/* 安全插入位置（JVM findSafeInsertIndex） */
static size_t find_safe_insert(const RkMsgList *l, size_t idx) {
    if (idx > l->count) idx = l->count;
    while (idx > 0) {
        const RikkaMessage *prev = l->items[idx - 1];
        const RikkaMessage *cur = idx < l->count ? l->items[idx] : NULL;
        int prev_user = prev->role == RIKKA_ROLE_USER;
        int cur_ast_tool = cur && cur->role == RIKKA_ROLE_ASSISTANT && has_tool_call(cur);
        if (prev_user && cur_ast_tool) {
            idx--;
        } else {
            break;
        }
    }
    return idx;
}

/* 按 role 分组合并注入内容并插入（JVM createMergedInjectionMessages） */
static void insert_merged(RkMsgList *l, size_t idx, const RkInjItem *items, size_t n) {
    /* 分组：先 ASSISTANT 后 USER（JVM groupBy 保持出现顺序，按组创建消息） */
    static const RikkaRole order[2] = {RIKKA_ROLE_ASSISTANT, RIKKA_ROLE_USER};
    for (size_t oi = 0; oi < 2; oi++) {
        RikkaRole role = order[oi];
        int first = 1;
        Buf content;
        buf_init(&content);
        for (size_t i = 0; i < n; i++) {
            const RkInjection *e = items[i].inj;
            if ((role == RIKKA_ROLE_ASSISTANT) != (e->role == RIKKA_ROLE_ASSISTANT)) continue;
            if (!first) buf_append_byte(&content, '\n');
            buf_append_str(&content, e->content ? e->content : "");
            first = 0;
        }
        if (!first) {
            RikkaMessage *m = rk_msgl_add(l, role);
            RikkaPart *p = rk_msgl_add_part(l, m, RIKKA_PART_TEXT);
            p->data = arena_alloc(l->arena, 1, content.len + 1);
            if (p->data) {
                memcpy((char *)p->data, content.data, content.len);
                ((char *)p->data)[content.len] = '\0';
                p->len = content.len;
            }
            rk_msgl_move(l, l->count - 1, idx);
            idx++;
        }
        buf_free(&content);
    }
}

/* 一组注入内容 "\n" 连接 */
static char *join_contents(Arena *a, const RkInjItem *items, size_t n) {
    Buf out;
    buf_init(&out);
    for (size_t i = 0; i < n; i++) {
        if (i > 0) buf_append_byte(&out, '\n');
        buf_append_str(&out, items[i].inj->content ? items[i].inj->content : "");
    }
    char *r = arena_alloc(a, 1, out.len + 1);
    if (r) {
        memcpy(r, out.data, out.len);
        r[out.len] = '\0';
    }
    buf_free(&out);
    return r;
}

/* BEFORE/AFTER_SYSTEM_PROMPT：并入 system 消息（无则创建） */
static void apply_system_injections(RkMsgList *l, const RkInjItem *before, size_t nb,
                                    const RkInjItem *after, size_t na) {
    char *bt = join_contents(l->arena, before, nb);
    char *at = join_contents(l->arena, after, na);
    if (!bt[0] && !at[0]) return;
    size_t sys_idx = l->count;
    for (size_t i = 0; i < l->count; i++) {
        if (l->items[i]->role == RIKKA_ROLE_SYSTEM) {
            sys_idx = i;
            break;
        }
    }
    if (sys_idx < l->count) {
        RikkaMessage *m = l->items[sys_idx];
        char *orig = msg_to_text(l->arena, m);
        Buf new_text;
        buf_init(&new_text);
        if (bt[0]) {
            buf_append_str(&new_text, bt);
            buf_append_byte(&new_text, '\n');
        }
        buf_append_str(&new_text, orig);
        if (at[0]) {
            buf_append_byte(&new_text, '\n');
            buf_append_str(&new_text, at);
        }
        /* 替换为单一 Text part（JVM 语义） */
        RikkaPart *np = (RikkaPart *)arena_alloc(l->arena, sizeof(void *), sizeof(RikkaPart));
        if (np) {
            np[0].type = RIKKA_PART_TEXT;
            np[0].data = arena_alloc(l->arena, 1, new_text.len + 1);
            if (np[0].data) {
                memcpy((char *)np[0].data, new_text.data, new_text.len);
                ((char *)np[0].data)[new_text.len] = '\0';
                np[0].len = new_text.len;
            }
            m->parts = np;
            m->part_count = 1;
            m->part_cap = 1;
        }
        buf_free(&new_text);
    } else {
        /* 无 system：合并 before/after 创建一条（JVM 语义：before + "\n" + after） */
        char *combined = join_contents(l->arena, before, nb);
        if (at[0]) {
            Buf tmp;
            buf_init(&tmp);
            buf_append_str(&tmp, combined);
            if (combined[0]) buf_append_byte(&tmp, '\n');
            buf_append_str(&tmp, at);
            combined = arena_alloc(l->arena, 1, tmp.len + 1);
            if (combined) {
                memcpy(combined, tmp.data, tmp.len);
                combined[tmp.len] = '\0';
            }
            buf_free(&tmp);
        }
        if (combined[0]) {
            RikkaMessage *m = rk_msgl_add(l, RIKKA_ROLE_SYSTEM);
            RikkaPart *p = rk_msgl_add_part(l, m, RIKKA_PART_TEXT);
            p->data = combined;
            p->len = strlen(combined);
            rk_msgl_move(l, l->count - 1, 0);
        }
    }
}

void rk_transform_prompt_injection(RkMsgList *l, const RkTransformConfig *cfg,
                                   const RkInjection *const *mode_injs, size_t n_mode,
                                   const RkLorebook *const *lorebooks, size_t n_lb) {
    if (n_mode == 0 && n_lb == 0) return;
    /* 收集 + 排序 */
    RkInjItem items[256];
    size_t n = collect_injections(items, 256, l, cfg, mode_injs, n_mode, lorebooks, n_lb);
    if (n == 0) return;
    sort_by_priority(items, n);
    /* 按位置分组 */
    RkInjItem by_pos[5][256];
    size_t pos_count[5] = {0, 0, 0, 0, 0};
    for (size_t i = 0; i < n; i++) {
        int pos = (int)items[i].inj->position;
        if (pos < 0 || pos > 4) pos = 1; /* 未知位置 → AFTER_SYSTEM */
        by_pos[pos][pos_count[pos]++] = items[i];
    }
    /* BEFORE/AFTER_SYSTEM */
    apply_system_injections(l, by_pos[0], pos_count[0], by_pos[1], pos_count[1]);
    /* TOP_OF_CHAT */
    if (pos_count[2] > 0) {
        size_t idx = l->count;
        for (size_t i = 0; i < l->count; i++) {
            if (l->items[i]->role == RIKKA_ROLE_USER) {
                idx = i;
                break;
            }
        }
        idx = find_safe_insert(l, idx);
        insert_merged(l, idx, by_pos[2], pos_count[2]);
    }
    /* BOTTOM_OF_CHAT */
    if (pos_count[3] > 0) {
        size_t idx = l->count > 0 ? l->count - 1 : 0;
        idx = find_safe_insert(l, idx);
        insert_merged(l, idx, by_pos[3], pos_count[3]);
    }
    /* AT_DEPTH：按深度分组，深度降序处理（避免索引变化） */
    if (pos_count[4] > 0) {
        int depth_seen[64];
        size_t depth_n = 0;
        for (size_t i = 0; i < pos_count[4]; i++) {
            int d = by_pos[4][i].inj->inject_depth;
            if (d < 1) d = 1;
            if (d > 64) d = 64;
            int found = 0;
            for (size_t k = 0; k < depth_n; k++) {
                if (depth_seen[k] == d) { found = 1; break; }
            }
            if (!found && depth_n < 64) depth_seen[depth_n++] = d;
        }
        /* 降序 */
        for (size_t a = 0; a + 1 < depth_n; a++) {
            for (size_t b = a + 1; b < depth_n; b++) {
                if (depth_seen[b] > depth_seen[a]) {
                    int t = depth_seen[a];
                    depth_seen[a] = depth_seen[b];
                    depth_seen[b] = t;
                }
            }
        }
        for (size_t k = 0; k < depth_n; k++) {
            int d = depth_seen[k];
            /* 组内保持 priority 降序 */
            RkInjItem grp[256];
            size_t gn = 0;
            for (size_t i = 0; i < pos_count[4] && gn < 256; i++) {
                int dd = by_pos[4][i].inj->inject_depth;
                if (dd < 1) dd = 1;
                if (dd > 64) dd = 64;
                if (dd == d) grp[gn++] = by_pos[4][i];
            }
            size_t idx = l->count - (size_t)d;
            if (idx > l->count) idx = l->count;
            idx = find_safe_insert(l, idx);
            insert_merged(l, idx, grp, gn);
        }
    }
}
