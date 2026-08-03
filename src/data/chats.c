/*
 * 会话元数据索引实现（见 chats.h）。
 */
#define _POSIX_C_SOURCE 200809L
#include "rikka/data/chats.h"
#include "rikka/core/buffer.h"
#include "rikka/data/index.h"
#include "rikka/data/store.h"
#include "rikka/json/json.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

struct RkChatIndex {
    RkStore *store;
    RkIndex *index;
};

RkChatIndex *rk_chats_create(void) {
    RkChatIndex *ci = (RkChatIndex *)calloc(1, sizeof(RkChatIndex));
    if (!ci) return NULL;
    ci->store = rk_store_create();
    ci->index = rk_index_create();
    if (!ci->store || !ci->index) {
        if (ci->store) rk_store_destroy(ci->store);
        if (ci->index) rk_index_destroy(ci->index);
        free(ci);
        return NULL;
    }
    return ci;
}

void rk_chats_destroy(RkChatIndex *ci) {
    if (!ci) return;
    rk_store_destroy(ci->store);
    rk_index_destroy(ci->index);
    free(ci);
}

int rk_chats_upsert(RkChatIndex *ci, const char *id, const char *title,
                    const char *content, int64_t updated_at, int pinned) {
    if (!ci || !id) return -1;
    RkEnt e;
    memset(&e, 0, sizeof(e));
    e.type = RK_ENT_CHAT;
    e.s[RK_ENT_CHAT_ID] = id;
    e.s[RK_ENT_CHAT_TITLE] = title ? title : "";
    e.s[RK_ENT_CHAT_CONTENT] = content ? content : "";
    e.i[RK_ENT_CHAT_UPDATED_AT] = updated_at;
    e.i[RK_ENT_CHAT_PINNED] = pinned ? 1 : 0;
    const RkEnt *ex = rk_store_find_str(ci->store, RK_ENT_CHAT, RK_ENT_CHAT_ID, id);
    if (ex) {
        /* 重建全文索引 */
        rk_index_remove_doc(ci->index, (uint64_t)ex->id);
        if (content && content[0]) {
            rk_index_add(ci->index, (uint64_t)ex->id, content, strlen(content));
        }
        e.id = ex->id;
        return rk_store_update(ci->store, &e);
    }
    int64_t nid = rk_store_insert(ci->store, &e);
    if (nid < 0) return -1;
    if (content && content[0]) {
        rk_index_add(ci->index, (uint64_t)nid, content, strlen(content));
    }
    return 0;
}

int rk_chats_remove(RkChatIndex *ci, const char *id) {
    if (!ci || !id) return -1;
    const RkEnt *ex = rk_store_find_str(ci->store, RK_ENT_CHAT, RK_ENT_CHAT_ID, id);
    if (!ex) return -1;
    rk_index_remove_doc(ci->index, (uint64_t)ex->id);
    return rk_store_delete(ci->store, RK_ENT_CHAT, ex->id);
}

/* 排序：pinned 降序 → updated_at 降序 → id 升序 */
static int chat_cmp(const void *pa, const void *pb) {
    const RkEnt *a = *(const RkEnt *const *)pa;
    const RkEnt *b = *(const RkEnt *const *)pb;
    int ap = (int)a->i[RK_ENT_CHAT_PINNED];
    int bp = (int)b->i[RK_ENT_CHAT_PINNED];
    if (ap != bp) return bp - ap;
    int64_t au = a->i[RK_ENT_CHAT_UPDATED_AT];
    int64_t bu = b->i[RK_ENT_CHAT_UPDATED_AT];
    if (au != bu) return bu > au ? 1 : -1;
    return strcmp(a->s[RK_ENT_CHAT_ID], b->s[RK_ENT_CHAT_ID]);
}

char *rk_chats_recent(RkChatIndex *ci, int limit) {
    if (!ci) return NULL;
    if (limit < 1) limit = 1;
    size_t n = rk_store_count(ci->store, RK_ENT_CHAT);
    const RkEnt **arr = (const RkEnt **)malloc((n ? n : 1) * sizeof(*arr));
    if (!arr) return NULL;
    for (size_t i = 0; i < n; i++) arr[i] = rk_store_at(ci->store, RK_ENT_CHAT, i);
    qsort(arr, n, sizeof(*arr), chat_cmp);
    size_t take = n < (size_t)limit ? n : (size_t)limit;
    Buf out;
    buf_init(&out);
    buf_append_byte(&out, '[');
    for (size_t i = 0; i < take; i++) {
        const RkEnt *e = arr[i];
        if (i > 0) buf_append_byte(&out, ',');
        char date[16] = "unknown";
        if (e->i[RK_ENT_CHAT_UPDATED_AT] > 0) {
            time_t t = (time_t)e->i[RK_ENT_CHAT_UPDATED_AT];
            struct tm tmv;
            localtime_r(&t, &tmv);
            strftime(date, sizeof(date), "%Y-%m-%d", &tmv);
        }
        char item[512];
        const char *title = e->s[RK_ENT_CHAT_TITLE];
        if (!title || !title[0]) title = "Untitled";
        int n2 = snprintf(item, sizeof(item),
                          "{\"id\":\"%s\",\"title\":\"%s\",\"last_chat\":\"%s\"}",
                          e->s[RK_ENT_CHAT_ID] ? e->s[RK_ENT_CHAT_ID] : "",
                          title, date);
        if (n2 > 0 && (size_t)n2 < sizeof(item)) buf_append_str(&out, item);
    }
    buf_append_byte(&out, ']');
    char *r = (char *)malloc(out.len + 1);
    if (r) {
        memcpy(r, out.data, out.len);
        r[out.len] = '\0';
    }
    buf_free(&out);
    free(arr);
    return r;
}

/* 大小写不敏感子串查找（chats 内部） */
static const char *ci_strcasestr(const char *haystack, const char *needle) {
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

/* snippet：匹配位置前 40 / 后 60 字符，关键词 [kw] 高亮 */
static void make_snippet(const char *content, const char *query, Buf *out) {
    const char *hit = ci_strcasestr(content, query);
    if (!hit) {
        size_t clen = strlen(content);
        const char *s = clen > 100 ? content + clen - 100 : content;
        buf_append_str(out, s);
        return;
    }
    size_t qlen = strlen(query);
    const char *start = hit;
    for (size_t i = 0; i < 40 && start > content; i++) start--;
    const char *end = hit + qlen;
    for (size_t i = 0; i < 60 && *end; i++) end++;
    if (start > content) buf_append_str(out, "...");
    buf_append(out, start, (size_t)((uintptr_t)hit - (uintptr_t)start));
    buf_append_byte(out, '[');
    buf_append(out, hit, qlen);
    buf_append_byte(out, ']');
    buf_append(out, hit + qlen, (size_t)((uintptr_t)end - (uintptr_t)hit - qlen));
    if (*end) buf_append_str(out, "...");
}

char *rk_chats_search(RkChatIndex *ci, const char *query) {
    if (!ci || !query || !query[0]) return NULL;
    uint64_t docs[64];
    size_t n = rk_index_search(ci->index, query, strlen(query), docs, 64);
    Buf out;
    buf_init(&out);
    buf_append_byte(&out, '[');
    size_t wrote = 0;
    for (size_t i = 0; i < n; i++) {
        const RkEnt *e = rk_store_get(ci->store, RK_ENT_CHAT, (int64_t)docs[i]);
        if (!e) continue;
        if (wrote > 0) buf_append_byte(&out, ',');
        char date[16] = "unknown";
        if (e->i[RK_ENT_CHAT_UPDATED_AT] > 0) {
            time_t t = (time_t)e->i[RK_ENT_CHAT_UPDATED_AT];
            struct tm tmv;
            localtime_r(&t, &tmv);
            strftime(date, sizeof(date), "%Y-%m-%d", &tmv);
        }
        Buf snip;
        buf_init(&snip);
        make_snippet(e->s[RK_ENT_CHAT_CONTENT] ? e->s[RK_ENT_CHAT_CONTENT] : "",
                     query, &snip);
        RJsonOut jo;
        rjson_out_init(&jo);
        rjson_write_string(&jo, e->s[RK_ENT_CHAT_ID] ? e->s[RK_ENT_CHAT_ID] : "",
                           strlen(e->s[RK_ENT_CHAT_ID] ? e->s[RK_ENT_CHAT_ID] : ""));
        buf_append_str(&out, "{\"id\":");
        buf_append(&out, jo.buf, jo.len);
        rjson_write_string(&jo, e->s[RK_ENT_CHAT_TITLE] ? e->s[RK_ENT_CHAT_TITLE] : "",
                           strlen(e->s[RK_ENT_CHAT_TITLE] ? e->s[RK_ENT_CHAT_TITLE] : ""));
        buf_append_str(&out, ",\"title\":");
        buf_append(&out, jo.buf, jo.len);
        buf_append_str(&out, ",\"snippet\":");
        /* snippet 完整转义(控制字符/引号/反斜杠) */
        rjson_write_string(&jo, (const char *)snip.data, snip.len);
        buf_append(&out, jo.buf, jo.len);
        rjson_write_string(&jo, date, strlen(date));
        buf_append_str(&out, ",\"date\":");
        buf_append(&out, jo.buf, jo.len);
        buf_append_str(&out, "}");
        rjson_out_free(&jo);
        buf_free(&snip);
        wrote++;
    }
    buf_append_byte(&out, ']');
    char *r = (char *)malloc(out.len + 1);
    if (r) {
        memcpy(r, out.data, out.len);
        r[out.len] = '\0';
    }
    buf_free(&out);
    return r;
}

int rk_chats_save_file(RkChatIndex *ci, const char *path) {
    if (!ci || !path) return -1;
    return rk_store_save_file(ci->store, path);
}

int rk_chats_load_file(RkChatIndex *ci, const char *path) {
    if (!ci || !path) return -1;
    if (rk_store_load_file(ci->store, path) != 0) return -1;
    /* 重建全文索引 */
    size_t n = rk_store_count(ci->store, RK_ENT_CHAT);
    for (size_t i = 0; i < n; i++) {
        const RkEnt *e = rk_store_at(ci->store, RK_ENT_CHAT, i);
        if (e->s[RK_ENT_CHAT_CONTENT] && e->s[RK_ENT_CHAT_CONTENT][0]) {
            rk_index_add(ci->index, (uint64_t)e->id, e->s[RK_ENT_CHAT_CONTENT],
                         strlen(e->s[RK_ENT_CHAT_CONTENT]));
        }
    }
    return 0;
}
