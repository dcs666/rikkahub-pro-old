/*
 * 聊天编排循环实现（见 chat.h）。
 */
#define _POSIX_C_SOURCE 200809L
#include "rikka/ai/chat.h"
#include "rikka/core/buffer.h"
#include "rikka/util/arena.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

static const char *TAG_OPEN = "<think>";
static const char *TAG_CLOSE = "</think>";

void rk_chat_think_feed(RkThinkState *st, const char *data, size_t len,
                        void (*out)(void *ud, int kind, const char *data, size_t len),
                        void *ud) {
    Buf out_text, out_reason;
    buf_init(&out_text);
    buf_init(&out_reason);
    for (size_t i = 0; i < len; i++) {
        char ch = data[i];
        const char *target = st->in_think ? TAG_CLOSE : TAG_OPEN;
        size_t tlen = strlen(target);
        /* 前缀匹配推进 */
        if (st->tag_len < tlen && ch == target[st->tag_len]) {
            st->tag_buf[st->tag_len++] = ch;
            if (st->tag_len == tlen) {
                st->in_think = !st->in_think;
                st->tag_len = 0;
            }
            continue; /* 标签字符不进入输出 */
        }
        /* 不匹配：先把已积累的标签前缀作为普通字符输出 */
        if (st->tag_len > 0) {
            if (st->in_think) {
                buf_append(&out_reason, st->tag_buf, st->tag_len);
            } else {
                buf_append(&out_text, st->tag_buf, st->tag_len);
            }
            st->tag_len = 0;
            /* 重新处理当前字符（可能开始新前缀） */
            i--;
            continue;
        }
        if (st->in_think) {
            buf_append(&out_reason, &ch, 1);
        } else {
            buf_append(&out_text, &ch, 1);
        }
    }
    if (out) {
        if (out_text.len > 0) out(ud, 0, (const char *)out_text.data, out_text.len);
        if (out_reason.len > 0) out(ud, 1, (const char *)out_reason.data, out_reason.len);
    }
    buf_free(&out_text);
    buf_free(&out_reason);
}

/* 增量回调桥：rk_chat 的 cb → provider delta（含流式 think_tag 状态机） */
typedef struct {
    RkChatCallbacks *cb;
    const RkChatConfig *cfg;
    RkThinkState th;        /* think_tag 流式状态 */
    Buf out_text;           /* 本块分流的 text 段 */
    Buf out_reason;         /* 本块分流的 reasoning 段 */
} ChatBridge;

static void bridge_emit(void *ud, int kind, const char *data, size_t len) {
    ChatBridge *b = (ChatBridge *)ud;
    if (kind == 1) {
        buf_append(&b->out_reason, data, len);
    } else {
        buf_append(&b->out_text, data, len);
    }
}

static void bridge_flush(ChatBridge *b) {
    if (b->out_text.len > 0 && b->cb->on_delta) {
        b->cb->on_delta(b->cb->ud, 0, (const char *)b->out_text.data, b->out_text.len);
        buf_reset(&b->out_text);
    }
    if (b->out_reason.len > 0 && b->cb->on_delta) {
        b->cb->on_delta(b->cb->ud, 1, (const char *)b->out_reason.data, b->out_reason.len);
        buf_reset(&b->out_reason);
    }
}

static void delta_bridge(void *ud, int kind, const char *data, size_t len) {
    ChatBridge *b = (ChatBridge *)ud;
    if (!b->cfg->use_visual_think_tag || kind != 0) {
        if (b->cb->on_delta) b->cb->on_delta(b->cb->ud, kind, data, len);
        return;
    }
    rk_chat_think_feed(&b->th, data, len, bridge_emit, b);
    bridge_flush(b);
}/* 追加工具结果消息（TOOL part + TOOL_RESULT part） */
static void append_tool_result(RkMsgList *work, Arena *a, const char *tool_name,
                               const char *tool_id, const char *result) {
    RikkaMessage *m = rk_msgl_add(work, RIKKA_ROLE_TOOL);
    if (!m) return;
    RikkaPart *p = rk_msgl_add_part(work, m, RIKKA_PART_TOOL_RESULT);
    if (!p) return;
    size_t nlen = strlen(tool_name);
    char *name_copy = (char *)arena_alloc(a, 1, nlen + 1);
    if (name_copy) {
        memcpy(name_copy, tool_name, nlen);
        name_copy[nlen] = '\0';
    }
    p->tool_name = name_copy;
    if (tool_id) {
        size_t ilen = strlen(tool_id);
        char *id_copy = (char *)arena_alloc(a, 1, ilen + 1);
        if (id_copy) {
            memcpy(id_copy, tool_id, ilen);
            id_copy[ilen] = '\0';
        }
        p->tool_id = id_copy;
    }
    size_t rlen = strlen(result);
    char *res_copy = (char *)arena_alloc(a, 1, rlen + 1);
    if (res_copy) {
        memcpy(res_copy, result, rlen);
        res_copy[rlen] = '\0';
    }
    p->data = res_copy;
    p->len = rlen;
}

/* 提取 assistant 消息文本（TEXT parts 拼接）→ malloc */
static char *extract_text(const RikkaMessage *m) {
    if (!m) return strdup("");
    size_t total = 0;
    for (size_t i = 0; i < m->part_count; i++) {
        if (m->parts[i].type == RIKKA_PART_TEXT) total += m->parts[i].len;
    }
    char *out = (char *)malloc(total + 1);
    if (!out) return NULL;
    size_t off = 0;
    for (size_t i = 0; i < m->part_count; i++) {
        const RikkaPart *p = &m->parts[i];
        if (p->type == RIKKA_PART_TEXT && p->data) {
            memcpy(out + off, p->data, p->len);
            off += p->len;
        }
    }
    out[total] = '\0';
    return out;
}

/* JSON 字符串转义（工具定义生成用）: 引号/反斜杠/全部控制字符 */
static void jstrz_buf(Buf *out, const char *s) {
    buf_append_byte(out, '"');
    for (const char *p = s; *p; p++) {
        unsigned char ch = (unsigned char)*p;
        switch (ch) {
            case '"':  buf_append_str(out, "\\\""); break;
            case '\\': buf_append_str(out, "\\\\"); break;
            case '\n': buf_append_str(out, "\\n"); break;
            case '\r': buf_append_str(out, "\\r"); break;
            case '\t': buf_append_str(out, "\\t"); break;
            case '\b': buf_append_str(out, "\\b"); break;
            case '\f': buf_append_str(out, "\\f"); break;
            default:
                if (ch < 0x20) {
                    char hex[8];
                    snprintf(hex, sizeof(hex), "\\u%04x", ch);
                    buf_append_str(out, hex);
                } else {
                    buf_append_byte(out, ch);
                }
        }
    }
    buf_append_byte(out, '"');
}

int rk_chat_run(const RkChatConfig *cfg, RkChatCallbacks *cb,
                const RikkaMessage *const *msgs, size_t n,
                char **final_text_out, char **error_out,
                RikkaSessionStats *stats_out) {
    if (final_text_out) *final_text_out = NULL;
    if (error_out) *error_out = NULL;
    if (!cfg || !msgs || n == 0) return -1;

    Arena *a = arena_create(0);
    RkMsgList work;
    rk_msgl_init(&work, a);
    rk_msgl_from(&work, a, msgs, n);

    /* 输入变换 */
    if (cfg->transform_input) {
        cfg->transform_input(&work, cfg->transform_ud);
    }

    int max_rounds = cfg->max_tool_rounds > 0 ? cfg->max_tool_rounds : 8;
    int timeout = cfg->timeout_ms > 0 ? cfg->timeout_ms : 60000;
    int rc = 0;
    char *final_text = NULL;
    char *err = NULL;

    /* 工具定义 JSON（OpenAI tools 数组）——从注册表生成 */
    RikkaProviderCfg pcfg = cfg->provider;
    Buf tools_buf;
    buf_init(&tools_buf);
    if (cfg->tools) {
        buf_append_str(&tools_buf, "[");
        size_t tn = rk_tools_count(cfg->tools);
        for (size_t i = 0; i < tn; i++) {
            const RkTool *t = rk_tools_at(cfg->tools, i);
            if (i > 0) buf_append_str(&tools_buf, ",");
            buf_append_str(&tools_buf, "{\"type\":\"function\",\"function\":{\"name\":");
            jstrz_buf(&tools_buf, t->name ? t->name : "");
            buf_append_str(&tools_buf, ",\"description\":");
            jstrz_buf(&tools_buf, t->description ? t->description : "");
            if (t->input_schema) {
                buf_append_str(&tools_buf, ",\"parameters\":");
                buf_append_str(&tools_buf, t->input_schema);
            }
            buf_append_str(&tools_buf, "}}");
        }
        buf_append_str(&tools_buf, "]");
        buf_append_byte(&tools_buf, '\0'); /* provider 以 strlen 消费，须 NUL 结尾 */
        pcfg.tools_json = (const char *)tools_buf.data;
    }

    /* 冻结消息（freeze 转移来的 malloc 缓冲）统一释放 */
    RikkaMessage *owned[16];
    size_t n_owned = 0;

    for (int round = 0; round <= max_rounds; round++) {
        if (cfg->cancel_flag && *cfg->cancel_flag) {
            rc = -1;
            err = strdup("cancelled");
            break;
        }
        /* 请求（变换后列表快照） */
        const RikkaMessage **arr = (const RikkaMessage **)arena_alloc(
            a, sizeof(void *), work.count * sizeof(*arr));
        if (!arr) { rc = -1; err = strdup("oom"); break; }
        for (size_t i = 0; i < work.count; i++) arr[i] = work.items[i];

        RikkaStream out;
        rstream_init(&out, a, RIKKA_ROLE_ASSISTANT);
        ChatBridge br;
        memset(&br, 0, sizeof(br));
        br.cb = cb;
        br.cfg = cfg;
        buf_init(&br.out_text);
        buf_init(&br.out_reason);
        char *detail = NULL;
        rc = rp_chat_stream_cb(&pcfg, arr, work.count, &out, timeout,
                               delta_bridge, &br, cfg->cancel_flag, stats_out,
                               &detail);
        buf_free(&br.out_text);
        buf_free(&br.out_reason);
        if (rc != 0) {
            rstream_destroy(&out);
            if (detail && detail[0]) {
                err = detail; /* 具体原因（TLS/连接/HTTP 错误） */
            } else {
                free(detail);
                err = strdup("provider request failed");
            }
            break;
        }
        free(detail);
        rstream_freeze(&out);
        if (n_owned < 16) owned[n_owned++] = out.msg; /* 结束时统一 free bufs */

        /* 输出变换（finish） */
        if (cfg->transform_output) {
            cfg->transform_output(&work, out.msg, cfg->transform_ud);
        }

        /* 检查工具调用 */
        size_t tc_count = 0;
        for (size_t i = 0; i < out.msg->part_count; i++) {
            if (out.msg->parts[i].type == RIKKA_PART_TOOL_CALL) tc_count++;
        }
        if (tc_count == 0) {
            final_text = extract_text(out.msg);
            rk_msgl_from(&work, a, (const RikkaMessage *const *)&out.msg, 1);
            break;
        }
        if (round >= max_rounds) {
            final_text = extract_text(out.msg);
            rk_msgl_from(&work, a, (const RikkaMessage *const *)&out.msg, 1);
            break;
        }
        /* assistant 消息（含 tool_calls）入列表 */
        rk_msgl_from(&work, a, (const RikkaMessage *const *)&out.msg, 1);

        /* 执行工具 */
        for (size_t i = 0; i < out.msg->part_count; i++) {
            const RikkaPart *p = &out.msg->parts[i];
            if (p->type != RIKKA_PART_TOOL_CALL) continue;
            const char *name = p->tool_name ? p->tool_name : "";
            const char *args = p->data ? p->data : "{}";
            if (cb && cb->on_tool_call) cb->on_tool_call(cb->ud, name, args);
            char *result = NULL;
            const RkTool *t = cfg->tools ? rk_tools_find(cfg->tools, name) : NULL;
            if (t && cfg->tool_env) {
                (void)rk_tool_call(t, args, cfg->tool_env, &result);
            }
            if (!result) result = rk_tool_result_error("tool not available");
            if (cb && cb->on_tool_result) cb->on_tool_result(cb->ud, name, result);
            append_tool_result(&work, a, name, p->tool_id, result);
            free(result);
        }
        /* 继续下一轮 */
    }

    for (size_t i = 0; i < n_owned; i++) rmsg_free_bufs(owned[i]); /* 消息结构在 arena，须先释放 malloc 缓冲 */
    arena_destroy(a);
    buf_free(&tools_buf);
    if (rc != 0) {
        if (error_out) {
            *error_out = err ? err : strdup("chat failed"); /* 所有权转移 */
        } else {
            free(err);
        }
        return -1;
    }
    free(err);
    if (final_text_out) *final_text_out = final_text ? final_text : strdup("");
    else free(final_text);
    return 0;
}
