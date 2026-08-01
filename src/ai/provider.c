#define _POSIX_C_SOURCE 200809L
#include "rikka/ai/provider.h"
#include "rikka/http/http.h"
#include "rikka/http/sse.h"
#include "rikka/json/json.h"
#include "rikka/pipe/spsc.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void pm_msleep(long ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

/* ================= 请求构建 ================= */

static void jstr(Buf *out, const char *s, size_t len) {
    /* 转义字符串（复用 json 序列化器） */
    RJsonOut o;
    rjson_out_init(&o);
    rjson_write_string(&o, s, len);
    buf_append(out, o.buf, o.len);
    rjson_out_free(&o);
}

static void jstrz(Buf *out, const char *s) { jstr(out, s, strlen(s)); }

static void part_text(Buf *out, const RikkaPart *p) { jstr(out, p->data, p->len); }

/* 消息的纯文本内容（若无 text part 返回 0） */
static int msg_text(const RikkaMessage *m, const char **text, size_t *len) {
    for (size_t i = 0; i < m->part_count; i++) {
        if (m->parts[i].type == RIKKA_PART_TEXT) {
            *text = m->parts[i].data;
            *len = m->parts[i].len;
            return 1;
        }
    }
    return 0;
}

/* ---------- OpenAI ---------- */

static void openai_msg(Buf *out, const RikkaMessage *m) {
    buf_append_str(out, "{\"role\":");
    switch (m->role) {
        case RIKKA_ROLE_USER:      jstrz(out, "user"); break;
        case RIKKA_ROLE_ASSISTANT: jstrz(out, "assistant"); break;
        case RIKKA_ROLE_SYSTEM:    jstrz(out, "system"); break;
        case RIKKA_ROLE_TOOL:      jstrz(out, "tool"); break;
    }
    if (m->role == RIKKA_ROLE_TOOL) {
        /* tool 消息：{role, tool_call_id, content} */
        const RikkaPart *tp = NULL;
        for (size_t i = 0; i < m->part_count; i++)
            if (m->parts[i].type == RIKKA_PART_TOOL_RESULT) { tp = &m->parts[i]; break; }
        if (tp) {
            buf_append_str(out, ",\"tool_call_id\":");
            jstrz(out, tp->tool_id ? tp->tool_id : "");
            buf_append_str(out, ",\"content\":");
            part_text(out, tp);
        } else {
            buf_append_str(out, ",\"tool_call_id\":\"\",\"content\":\"\"");
        }
        buf_append_str(out, "}");
        return;
    }
    /* 检查 tool_calls */
    int has_tc = 0;
    for (size_t i = 0; i < m->part_count; i++)
        if (m->parts[i].type == RIKKA_PART_TOOL_CALL) { has_tc = 1; break; }
    if (has_tc) {
        buf_append_str(out, ",\"content\":null,\"tool_calls\":[");
        int first = 1;
        for (size_t i = 0; i < m->part_count; i++) {
            if (m->parts[i].type != RIKKA_PART_TOOL_CALL) continue;
            const RikkaPart *tc = &m->parts[i];
            if (!first) buf_append_str(out, ",");
            first = 0;
            buf_append_str(out, "{\"id\":");
            jstrz(out, tc->tool_id ? tc->tool_id : "");
            buf_append_str(out, ",\"type\":\"function\",\"function\":{\"name\":");
            jstrz(out, tc->tool_name ? tc->tool_name : "");
            buf_append_str(out, ",\"arguments\":");
            /* arguments 是 JSON 字符串 */
            jstr(out, tc->data, tc->len);
            buf_append_str(out, "}}");
        }
        buf_append_str(out, "]}");
        return;
    }
    /* 纯文本或 image blocks */
    buf_append_str(out, ",\"content\":");
    int has_image = 0;
    for (size_t i = 0; i < m->part_count; i++)
        if (m->parts[i].type == RIKKA_PART_IMAGE) { has_image = 1; break; }
    if (!has_image) {
        const char *t; size_t tl;
        if (msg_text(m, &t, &tl)) jstr(out, t, tl);
        else buf_append_str(out, "\"\"");
    } else {
        buf_append_str(out, "[");
        int first = 1;
        for (size_t i = 0; i < m->part_count; i++) {
            const RikkaPart *p = &m->parts[i];
            if (p->type == RIKKA_PART_IMAGE) {
                if (!first) buf_append_str(out, ",");
                first = 0;
                buf_append_str(out, "{\"type\":\"image_url\",\"image_url\":{\"url\":");
                jstr(out, p->data, p->len);
                buf_append_str(out, "}}");
            } else if (p->type == RIKKA_PART_TEXT && p->len > 0) {
                if (!first) buf_append_str(out, ",");
                first = 0;
                buf_append_str(out, "{\"type\":\"text\",\"text\":");
                part_text(out, p);
                buf_append_str(out, "}");
            }
        }
        buf_append_str(out, "]");
    }
    buf_append_str(out, "}");
}

/* ---------- Claude ---------- */

static void claude_content(Buf *out, const RikkaMessage *m) {
    /* 单纯文本：字符串；否则 blocks */
    int has_block = 0;
    for (size_t i = 0; i < m->part_count; i++) {
        RikkaPartType t = m->parts[i].type;
        if (t == RIKKA_PART_IMAGE || t == RIKKA_PART_TOOL_CALL || t == RIKKA_PART_TOOL_RESULT) has_block = 1;
    }
    if (!has_block) {
        const char *t; size_t tl;
        if (msg_text(m, &t, &tl)) jstr(out, t, tl);
        else buf_append_str(out, "\"\"");
        return;
    }
    buf_append_str(out, "[");
    int first = 1;
    for (size_t i = 0; i < m->part_count; i++) {
        const RikkaPart *p = &m->parts[i];
        if (!first) buf_append_str(out, ",");
        switch (p->type) {
            case RIKKA_PART_TEXT:
                if (p->len == 0) continue;
                buf_append_str(out, "{\"type\":\"text\",\"text\":");
                part_text(out, p);
                buf_append_str(out, "}");
                break;
            case RIKKA_PART_IMAGE:
                buf_append_str(out, "{\"type\":\"image\",\"source\":{\"type\":\"url\",\"url\":");
                jstr(out, p->data, p->len);
                buf_append_str(out, "}}");
                break;
            case RIKKA_PART_TOOL_CALL:
                buf_append_str(out, "{\"type\":\"tool_use\",\"id\":");
                jstrz(out, p->tool_id ? p->tool_id : "");
                buf_append_str(out, ",\"name\":");
                jstrz(out, p->tool_name ? p->tool_name : "");
                buf_append_str(out, ",\"input\":");
                /* input 是对象：数据本身是 JSON 对象文本 */
                if (p->len > 0) buf_append(out, p->data, p->len);
                else buf_append_str(out, "{}");
                buf_append_str(out, "}");
                break;
            case RIKKA_PART_TOOL_RESULT:
                buf_append_str(out, "{\"type\":\"tool_result\",\"tool_use_id\":");
                jstrz(out, p->tool_id ? p->tool_id : "");
                buf_append_str(out, ",\"content\":");
                part_text(out, p);
                buf_append_str(out, "}");
                break;
            default:
                break;
        }
        first = 0;
    }
    buf_append_str(out, "]");
}

/* ---------- Google ---------- */

static void google_part(Buf *out, const RikkaPart *p) {
    buf_append_str(out, "{\"text\":");
    jstr(out, p->data, p->len);
    buf_append_str(out, "}");
}

static void google_content(Buf *out, const RikkaMessage *m) {
    buf_append_str(out, "{\"role\":");
    if (m->role == RIKKA_ROLE_ASSISTANT) jstrz(out, "model");
    else jstrz(out, "user");
    buf_append_str(out, ",\"parts\":[");
    int first = 1;
    for (size_t i = 0; i < m->part_count; i++) {
        if (m->parts[i].type != RIKKA_PART_TEXT) continue;
        if (!first) buf_append_str(out, ",");
        google_part(out, &m->parts[i]);
        first = 0;
    }
    buf_append_str(out, "]}");
}

/* ---------- 入口 ---------- */

int rp_build_request(const RikkaProviderCfg *cfg,
                     const RikkaMessage *const *msgs, size_t n,
                     int stream, Buf *out) {
    if (!cfg || !out) return -1;
    buf_reset(out);
    switch (cfg->id) {
    case RIKKA_PROVIDER_OPENAI: {
        buf_append_str(out, "{\"model\":");
        jstrz(out, cfg->model ? cfg->model : "");
        buf_append_str(out, ",\"stream\":");
        buf_append_str(out, stream ? "true" : "false");
        if (cfg->max_tokens > 0) {
            char tmp[64];
            int k = snprintf(tmp, sizeof(tmp), ",\"max_tokens\":%d", cfg->max_tokens);
            buf_append(out, tmp, (size_t)k);
        }
        buf_append_str(out, ",\"messages\":[");
        for (size_t i = 0; i < n; i++) {
            if (i) buf_append_str(out, ",");
            openai_msg(out, msgs[i]);
        }
        buf_append_str(out, "]}");
        if (cfg->tools_json && cfg->tools_json[0]) {
            buf_append_str(out, ",\"tools\":");
            buf_append_str(out, cfg->tools_json);
        }
        break;
    }
    case RIKKA_PROVIDER_CLAUDE: {
        /* system 提取 */
        const char *sys = NULL; size_t sys_len = 0;
        for (size_t i = 0; i < n; i++) {
            if (msgs[i]->role == RIKKA_ROLE_SYSTEM && msg_text(msgs[i], &sys, &sys_len)) break;
        }
        buf_append_str(out, "{\"model\":");
        jstrz(out, cfg->model ? cfg->model : "");
        buf_append_str(out, ",\"max_tokens\":");
        char tmp[32];
        int k = snprintf(tmp, sizeof(tmp), "%d", cfg->max_tokens > 0 ? cfg->max_tokens : 4096);
        buf_append(out, tmp, (size_t)k);
        buf_append_str(out, ",\"stream\":");
        buf_append_str(out, stream ? "true" : "false");
        if (sys) {
            buf_append_str(out, ",\"system\":[{\"type\":\"text\",\"text\":");
            jstr(out, sys, sys_len);
            if (cfg->enable_cache_control)
                buf_append_str(out, ",\"cache_control\":{\"type\":\"ephemeral\"}");
            buf_append_str(out, "}]");
        }
        buf_append_str(out, ",\"messages\":[");
        int first = 1;
        for (size_t i = 0; i < n; i++) {
            if (msgs[i]->role == RIKKA_ROLE_SYSTEM) continue;
            if (!first) buf_append_str(out, ",");
            first = 0;
            buf_append_str(out, "{\"role\":");
            jstrz(out, msgs[i]->role == RIKKA_ROLE_ASSISTANT ? "assistant" : "user");
            buf_append_str(out, ",\"content\":");
            claude_content(out, msgs[i]);
            buf_append_str(out, "}");
        }
        buf_append_str(out, "]}");
        break;
    }
    case RIKKA_PROVIDER_GOOGLE: {
        const char *sys = NULL; size_t sys_len = 0;
        for (size_t i = 0; i < n; i++) {
            if (msgs[i]->role == RIKKA_ROLE_SYSTEM && msg_text(msgs[i], &sys, &sys_len)) break;
        }
        buf_append_str(out, "{");
        int need_comma = 0;
        if (sys) {
            buf_append_str(out, "\"systemInstruction\":{\"parts\":[{\"text\":");
            jstr(out, sys, sys_len);
            buf_append_str(out, "}]}");
            need_comma = 1;
        }
        if (need_comma) buf_append_str(out, ",");
        buf_append_str(out, "\"contents\":[");
        int first = 1;
        for (size_t i = 0; i < n; i++) {
            if (msgs[i]->role == RIKKA_ROLE_SYSTEM || msgs[i]->role == RIKKA_ROLE_TOOL) continue;
            if (!first) buf_append_str(out, ",");
            first = 0;
            google_content(out, msgs[i]);
        }
        buf_append_str(out, "]");
        if (cfg->max_tokens > 0) {
            char t2[96];
            int k2 = snprintf(t2, sizeof(t2), ",\"generationConfig\":{\"maxOutputTokens\":%d}", cfg->max_tokens);
            buf_append(out, t2, (size_t)k2);
        }
        buf_append_str(out, "}");
        break;
    }
    default:
        return -1;
    }
    return 0;
}

/* ================= 统一流式管线 ================= */

struct RikkaStreamSession {
    RikkaProviderCfg cfg;
    RHttpConn *conn;
    RsseParser *sse;
    RJsonStream *js_type;    /* Claude delta.type → type_buf */
    RJsonStream *js_text;    /* content/text → text part */
    RJsonStream *js_reason;  /* reasoning/thinking → reasoning part */
    RJsonStream *js_tc_name; /* OpenAI tool_calls[0].function.name */
    RJsonStream *js_tc_args; /* OpenAI tool_calls[0].function.arguments */
    RJsonStream *js_tc_id;   /* OpenAI tool_calls[0].id */
    RikkaStream *out;
    RikkaSessionStats stats;
    Buf type_buf;
    Buf tc_name_buf;
    Buf tc_args_buf;
    Buf tc_id_buf;
    char *last_error;        /* 最近一次非 2xx 错误详情（malloc，take 转移所有权） */
    RkStreamDeltaCb delta_cb; /* 增量回调（rp_chat_stream_cb 用，可 NULL） */
    void *delta_ud;
};

/* 提取路径表 */
static const RJsonStreamPathElem P_OAI_CONTENT[] = {
    {0, {.key = "choices"}}, {1, {.index = 0}}, {0, {.key = "delta"}}, {0, {.key = "content"}}, {-2, {0}}};
static const RJsonStreamPathElem P_OAI_REASON[] = {
    {0, {.key = "choices"}}, {1, {.index = 0}}, {0, {.key = "delta"}}, {0, {.key = "reasoning_content"}}, {-2, {0}}};
static const RJsonStreamPathElem P_CLAUDE_TYPE[] = {
    {0, {.key = "delta"}}, {0, {.key = "type"}}, {-2, {0}}};
static const RJsonStreamPathElem P_CLAUDE_TEXT[] = {
    {0, {.key = "delta"}}, {0, {.key = "text"}}, {-2, {0}}};
static const RJsonStreamPathElem P_CLAUDE_THINK[] = {
    {0, {.key = "delta"}}, {0, {.key = "thinking"}}, {-2, {0}}};
static const RJsonStreamPathElem P_GOOG_TEXT[] = {
    {0, {.key = "candidates"}}, {1, {.index = 0}}, {0, {.key = "content"}},
    {0, {.key = "parts"}}, {1, {.index = 0}}, {0, {.key = "text"}}, {-2, {0}}};
/* OpenAI tool_calls delta（index 0 单调用；并行多 index 打磨期扩展） */
static const RJsonStreamPathElem P_OAI_TC_NAME[] = {
    {0, {.key = "choices"}}, {1, {.index = 0}}, {0, {.key = "delta"}},
    {0, {.key = "tool_calls"}}, {1, {.index = 0}}, {0, {.key = "function"}},
    {0, {.key = "name"}}, {-2, {0}}};
static const RJsonStreamPathElem P_OAI_TC_ARGS[] = {
    {0, {.key = "choices"}}, {1, {.index = 0}}, {0, {.key = "delta"}},
    {0, {.key = "tool_calls"}}, {1, {.index = 0}}, {0, {.key = "function"}},
    {0, {.key = "arguments"}}, {-2, {0}}};
static const RJsonStreamPathElem P_OAI_TC_ID[] = {
    {0, {.key = "choices"}}, {1, {.index = 0}}, {0, {.key = "delta"}},
    {0, {.key = "tool_calls"}}, {1, {.index = 0}}, {0, {.key = "id"}}, {-2, {0}}};

static void sink_text(void *ctx, const char *data, size_t len) {
    RikkaStreamSession *ss = (RikkaStreamSession *)ctx;
    rstream_append_text(ss->out, data, len);
    ss->stats.text_chunks++;
    if (ss->delta_cb) ss->delta_cb(ss->delta_ud, 0, data, len);
}

static void sink_reason(void *ctx, const char *data, size_t len) {
    RikkaStreamSession *ss = (RikkaStreamSession *)ctx;
    rstream_append_reasoning(ss->out, data, len);
    ss->stats.reasoning_chunks++;
    if (ss->delta_cb) ss->delta_cb(ss->delta_ud, 1, data, len);
}

static void sink_type(void *ctx, const char *data, size_t len) {
    RikkaStreamSession *ss = (RikkaStreamSession *)ctx;
    buf_append(&ss->type_buf, data, len);
}

/* tool_calls delta：name/id 每次新块覆盖（index 0），arguments 累积拼接 */
static void sink_tc_name(void *ctx, const char *data, size_t len) {
    RikkaStreamSession *ss = (RikkaStreamSession *)ctx;
    buf_reset(&ss->tc_name_buf);
    buf_append(&ss->tc_name_buf, data, len);
}

static void sink_tc_args(void *ctx, const char *data, size_t len) {
    RikkaStreamSession *ss = (RikkaStreamSession *)ctx;
    buf_append(&ss->tc_args_buf, data, len);
}

static void sink_tc_id(void *ctx, const char *data, size_t len) {
    RikkaStreamSession *ss = (RikkaStreamSession *)ctx;
    buf_reset(&ss->tc_id_buf);
    buf_append(&ss->tc_id_buf, data, len);
}

static void run_extract(RJsonStream *js, const char *data, size_t len) {
    rjson_stream_reset(js);
    rjson_stream_feed(js, data, len);
    rjson_stream_finish(js);
}

static void on_sse_event(void *ctx, const char *event, const char *data, size_t len,
                         const char *id, long long retry_ms) {
    (void)id; (void)retry_ms;
    RikkaStreamSession *ss = (RikkaStreamSession *)ctx;
    ss->stats.events++;
    ss->stats.bytes_received += len;
    if (!data || len == 0) return;
    switch (ss->cfg.id) {
    case RIKKA_PROVIDER_OPENAI:
        if (strcmp(event, "message") == 0) {
            /* 双提取：content 与 reasoning_content 各进各的 part（零拷贝分流） */
            run_extract(ss->js_text, data, len);
            run_extract(ss->js_reason, data, len);
            /* tool_calls delta（index 0 单调用） */
            { /* 直接对比路径: 手工 reset+feed */
                rjson_stream_reset(ss->js_tc_name);
                rjson_stream_feed(ss->js_tc_name, data, len);
                rjson_stream_finish(ss->js_tc_name);
                    rjson_stream_reset(ss->js_tc_name);
            }
            run_extract(ss->js_tc_name, data, len);
            run_extract(ss->js_tc_args, data, len);
            run_extract(ss->js_tc_id, data, len);
        }
        break;
    case RIKKA_PROVIDER_CLAUDE:
        if (strcmp(event, "content_block_delta") == 0) {
            buf_reset(&ss->type_buf);
            run_extract(ss->js_type, data, len);
            int is_thinking = ss->type_buf.len >= 8 &&
                              memcmp(ss->type_buf.data, "thinking", 8) == 0;
            run_extract(is_thinking ? ss->js_reason : ss->js_text, data, len);
        } else if (strcmp(event, "error") == 0) {
            ss->stats.error_events++;
        }
        break;
    case RIKKA_PROVIDER_GOOGLE:
        if (strcmp(event, "message") == 0) {
            run_extract(ss->js_text, data, len);
        }
        break;
    }
}

/* base_url 解析已统一到 rhttp_parse_url（rikka/http/http.h） */
static const char *default_chat_path(RikkaProviderId id) {
    switch (id) {
        case RIKKA_PROVIDER_OPENAI: return "/chat/completions";
        case RIKKA_PROVIDER_CLAUDE: return "/v1/messages";
        case RIKKA_PROVIDER_GOOGLE: return "/v1beta/models/%s:streamGenerateContent?alt=sse";
        default: return "/";
    }
}

RikkaStreamSession *rp_session_create(const RikkaProviderCfg *cfg) {
    RikkaStreamSession *ss = (RikkaStreamSession *)calloc(1, sizeof(RikkaStreamSession));
    if (!ss) return NULL;
    ss->cfg = *cfg;
    buf_init(&ss->type_buf);
    buf_init(&ss->tc_name_buf);
    buf_init(&ss->tc_args_buf);
    buf_init(&ss->tc_id_buf);
    return ss;
}

void rp_session_destroy(RikkaStreamSession *ss) {
    if (!ss) return;
    if (ss->conn) rhttp_close(ss->conn);
    if (ss->sse) rsse_destroy(ss->sse);
    if (ss->js_type) rjson_stream_destroy(ss->js_type);
    if (ss->js_text) rjson_stream_destroy(ss->js_text);
    if (ss->js_reason) rjson_stream_destroy(ss->js_reason);
    if (ss->js_tc_name) rjson_stream_destroy(ss->js_tc_name);
    if (ss->js_tc_args) rjson_stream_destroy(ss->js_tc_args);
    if (ss->js_tc_id) rjson_stream_destroy(ss->js_tc_id);
    buf_free(&ss->type_buf);
    buf_free(&ss->tc_name_buf);
    buf_free(&ss->tc_args_buf);
    buf_free(&ss->tc_id_buf);
    free(ss->last_error);
    free(ss);
}

/* ---------- 重试中间件（P4a） ---------- */

/* 读非 2xx 响应体（≤64KB）并提取 provider 的 {"error":{"message":...}}。
 * 三家 provider（OpenAI/Claude/Google）错误结构同构，单一提取即可。 */
static void capture_error_detail(RikkaStreamSession *ss, RHttpConn *conn) {
    char buf[8192];
    Buf body;
    buf_init(&body);
    while (body.len < 65536) {
        ssize_t n = rhttp_read_body(conn, buf, sizeof(buf), 3000);
        if (n <= 0) break;
        buf_append(&body, buf, (size_t)n);
    }
    if (body.len > 0) {
        Arena *a = arena_create(0);
        size_t err = 0;
        RJson *v = rjson_parse(a, (const char *)body.data, body.len, &err);
        if (v) {
            const RJson *e = rjson_obj_get(v, "error");
            const RJson *m = e ? rjson_obj_get(e, "message") : NULL;
            if (m && m->type == RJSON_STRING) {
                free(ss->last_error);
                ss->last_error = strndup(m->u.str.ptr, m->u.str.len);
            }
        }
        arena_destroy(a);
    }
    buf_free(&body);
}

/* 单次尝试：解析 URL → 连接 → 发送 → 读响应头。
 * 返回 0 成功（conn 归 session 所有），-1 网络错误，-2 非 2xx
 * （此时已捕获错误详情并关闭连接）。 */
static int start_once(RikkaStreamSession *ss, const char *path,
                      const char *body, size_t body_len,
                      int timeout_ms, int *http_status) {
    char host[256], prefix[512], full[1024];
    uint16_t port;
    int tls;
    if (rhttp_parse_url(ss->cfg.base_url, host, sizeof(host), &port, &tls,
                        prefix, sizeof(prefix)) != 0)
        return -1;
    if (!path) {
        if (ss->cfg.id == RIKKA_PROVIDER_GOOGLE) {
            /* 字面量格式串：model 作为 %.240s 参数（防 model 含 % 被当格式符） */
            snprintf(full, sizeof(full),
                     "%s/v1beta/models/%.240s:streamGenerateContent?alt=sse",
                     prefix, ss->cfg.model ? ss->cfg.model : "");
        } else {
            snprintf(full, sizeof(full), "%s%s", prefix, default_chat_path(ss->cfg.id));
        }
        path = full;
    }
    RHttpConn *conn = rhttp_connect(host, port, tls, timeout_ms);
    if (!conn) return -1;

    char auth[512];
    const char *hdrs[8];
    int nh = 0;
    if (ss->cfg.api_key && ss->cfg.id != RIKKA_PROVIDER_GOOGLE) {
        snprintf(auth, sizeof(auth), "Bearer %s", ss->cfg.api_key);
        hdrs[nh++] = "Authorization";
        hdrs[nh++] = auth;
    } else if (ss->cfg.api_key) {
        hdrs[nh++] = "x-goog-api-key";
        hdrs[nh++] = ss->cfg.api_key;
    }
    hdrs[nh++] = "Content-Type";
    hdrs[nh++] = "application/json";
    hdrs[nh++] = "Accept";
    hdrs[nh++] = "text/event-stream";
    hdrs[nh] = NULL;

    if (rhttp_send(conn, "POST", path, hdrs, body, body_len) != 0) {
        rhttp_close(conn);
        return -1;
    }
    RHttpResp resp;
    if (rhttp_read_headers(conn, &resp, timeout_ms) != 0) {
        rhttp_close(conn);
        return -1;
    }
    if (http_status) *http_status = resp.status;
    if (resp.status < 200 || resp.status >= 300) {
        capture_error_detail(ss, conn);
        rhttp_close(conn);
        return -2;
    }
    ss->conn = conn;
    return 0;
}

int rp_stream_start(RikkaStreamSession *ss, const char *path,
                    const char *body, size_t body_len,
                    RikkaStream *out, int timeout_ms, int *http_status) {
    if (!ss || !out) return -1;
    ss->out = out;
    free(ss->last_error);
    ss->last_error = NULL;

    int retries = ss->cfg.retry.max_retries;
    if (retries < 0) retries = 0;
    long base = ss->cfg.retry.base_delay_ms > 0 ? ss->cfg.retry.base_delay_ms : 100;
    long maxd = ss->cfg.retry.max_delay_ms > 0 ? ss->cfg.retry.max_delay_ms : 2000;

    int status = 0;
    int attempt = 0;
    for (;;) {
        int rc = start_once(ss, path, body, body_len, timeout_ms, &status);
        if (rc == 0) {
            /* 成功：重试过程中捕获的临时错误详情不留存 */
            free(ss->last_error);
            ss->last_error = NULL;
            break;
        }
        int retryable = (rc == -1) || (status == 429 || status >= 500);
        if (!retryable || attempt >= retries) {
            if (http_status) *http_status = status;
            return rc;
        }
        /* 指数退避：base << attempt，封顶 maxd */
        long delay = base << attempt;
        if (delay > maxd) delay = maxd;
        if (delay > 0) pm_msleep(delay);
        attempt++;
    }
    if (http_status) *http_status = status;

    ss->sse = rsse_create(on_sse_event, ss);
    ss->js_type = rjson_stream_create(P_CLAUDE_TYPE, sink_type, ss);
    ss->js_text = rjson_stream_create(P_OAI_CONTENT, sink_text, ss);
    ss->js_reason = rjson_stream_create(P_OAI_REASON, sink_reason, ss);
    ss->js_tc_name = rjson_stream_create(P_OAI_TC_NAME, sink_tc_name, ss);
    ss->js_tc_args = rjson_stream_create(P_OAI_TC_ARGS, sink_tc_args, ss);
    ss->js_tc_id = rjson_stream_create(P_OAI_TC_ID, sink_tc_id, ss);
    if (!ss->sse || !ss->js_type || !ss->js_text || !ss->js_reason ||
        !ss->js_tc_name || !ss->js_tc_args || !ss->js_tc_id) return -1; /* OOM 防御 */
    switch (ss->cfg.id) {
        case RIKKA_PROVIDER_OPENAI:
            rjson_stream_set_path(ss->js_text, P_OAI_CONTENT);
            rjson_stream_set_path(ss->js_reason, P_OAI_REASON);
            rjson_stream_set_path(ss->js_tc_name, P_OAI_TC_NAME);
            rjson_stream_set_path(ss->js_tc_args, P_OAI_TC_ARGS);
            rjson_stream_set_path(ss->js_tc_id, P_OAI_TC_ID);
            break;
        case RIKKA_PROVIDER_CLAUDE:
            rjson_stream_set_path(ss->js_type, P_CLAUDE_TYPE);
            rjson_stream_set_path(ss->js_text, P_CLAUDE_TEXT);
            rjson_stream_set_path(ss->js_reason, P_CLAUDE_THINK);
            break;
        case RIKKA_PROVIDER_GOOGLE:
            rjson_stream_set_path(ss->js_text, P_GOOG_TEXT);
            break;
    }
    return 0;
}

char *rp_take_error_detail(RikkaStreamSession *ss) {
    if (!ss) return NULL;
    char *e = ss->last_error;
    ss->last_error = NULL;
    return e;
}

/* 流结束后：解析到 tool_calls delta 则构造 TOOL_CALL part（index 0 单调用） */
static void finalize_tool_calls(RikkaStreamSession *ss) {
    if (!ss->out || !ss->out->msg || ss->tc_name_buf.len == 0) return;
    Arena *a = ss->out->arena;
    RikkaPart *p = rmsg_add_part(a, ss->out->msg, RIKKA_PART_TOOL_CALL);
    if (!p) return;
    p->tool_name = (const char *)arena_alloc(a, 1, ss->tc_name_buf.len + 1);
    if (p->tool_name && ss->tc_name_buf.len > 0) {
        memcpy((void *)p->tool_name, ss->tc_name_buf.data, ss->tc_name_buf.len);
        ((char *)p->tool_name)[ss->tc_name_buf.len] = '\0';
    }
    if (ss->tc_id_buf.len > 0) {
        p->tool_id = (const char *)arena_alloc(a, 1, ss->tc_id_buf.len + 1);
        if (p->tool_id) {
            memcpy((void *)p->tool_id, ss->tc_id_buf.data, ss->tc_id_buf.len);
            ((char *)p->tool_id)[ss->tc_id_buf.len] = '\0';
        }
    }
    p->data = (const char *)arena_alloc(a, 1, ss->tc_args_buf.len + 1);
    if (p->data && ss->tc_args_buf.len > 0) {
        memcpy((void *)p->data, ss->tc_args_buf.data, ss->tc_args_buf.len);
        ((char *)p->data)[ss->tc_args_buf.len] = '\0';
    }
    p->len = ss->tc_args_buf.len;
}

int rp_stream_pump(RikkaStreamSession *ss, int timeout_ms) {
    if (!ss || !ss->conn) return -1;
    char buf[16384];
    for (;;) {
        ssize_t n = rhttp_read_body(ss->conn, buf, sizeof(buf), timeout_ms);
        if (n < 0) return -1;
        if (n == 0) break;
        if (rsse_feed(ss->sse, buf, (size_t)n) != 0) return -1;
    }
    rsse_finish(ss->sse);
    finalize_tool_calls(ss);
    return 0;
}

/* ---------- S5 异步流水线：读线程 + 提取线程 ---------- */

typedef struct {
    RkSpsc q;
    RikkaStreamSession *ss;
    int timeout_ms;
    int reader_rc;
    int proc_rc;
    volatile int *cancel;
} PipeIO;

static void *pipe_reader(void *v) {
    PipeIO *io = (PipeIO *)v;
    RikkaStreamSession *ss = io->ss;
    char buf[16384];
    for (;;) {
        if (io->cancel && *io->cancel) {
            /* 取消：关闭连接让读立即返回 */
            rhttp_close(ss->conn);
            ss->conn = NULL;
            io->reader_rc = -1;
            break;
        }
        ssize_t n = rhttp_read_body(ss->conn, buf, sizeof(buf), io->timeout_ms);
        if (n < 0) { io->reader_rc = -1; break; }
        if (n == 0) break;
        while (rk_spsc_push(&io->q, buf, (size_t)n) != 0) {
            if (io->cancel && *io->cancel) {
                rhttp_close(ss->conn);
                ss->conn = NULL;
                io->reader_rc = -1;
                rk_spsc_close(&io->q);
                return NULL;
            }
            pm_msleep(1);
        }
    }
    rk_spsc_close(&io->q);
    return NULL;
}

static void *pipe_processor(void *v) {
    PipeIO *io = (PipeIO *)v;
    RikkaStreamSession *ss = io->ss;
    char buf[16384];
    for (;;) {
        ssize_t n = rk_spsc_pop(&io->q, buf, sizeof(buf));
        if (n == 0) break;
        if (n < 0) { pm_msleep(1); continue; }
        /* 出错后继续消费（排空队列让 reader 完成），不再喂解析器 */
        if (io->proc_rc == 0 && rsse_feed(ss->sse, buf, (size_t)n) != 0)
            io->proc_rc = -1;
    }
    if (io->proc_rc == 0) rsse_finish(ss->sse);
    return NULL;
}

int rp_stream_pump_async(RikkaStreamSession *ss, int timeout_ms) {
    return rp_stream_pump_async_cancel(ss, timeout_ms, NULL);
}

int rp_stream_pump_async_cancel(RikkaStreamSession *ss, int timeout_ms,
                                volatile int *cancel) {
    if (!ss || !ss->conn) return -1;
    PipeIO io;
    io.ss = ss;
    io.timeout_ms = timeout_ms;
    io.reader_rc = 0;
    io.proc_rc = 0;
    io.cancel = cancel;
    rk_spsc_init(&io.q, 1 << 20);
    pthread_t rt, pt;
    pthread_create(&rt, NULL, pipe_reader, &io);
    pthread_create(&pt, NULL, pipe_processor, &io);
    pthread_join(rt, NULL);
    pthread_join(pt, NULL);
    rk_spsc_destroy(&io.q);
    finalize_tool_calls(ss);
    if (cancel && *cancel) return -1; /* 取消优先 */
    return io.reader_rc != 0 ? io.reader_rc : io.proc_rc;
}

const RikkaSessionStats *rp_session_stats(const RikkaStreamSession *ss) {
    return ss ? &ss->stats : NULL;
}

/* B3 重试中间件：仅对"连接失败/5xx"重试（start 阶段，未开始累积无重复风险）；
 * 4xx 与 pump 中途失败不重试（部分内容已累积）。 */
#define RIKKA_MAX_RETRIES 3
int rp_chat_stream_cb(const RikkaProviderCfg *cfg,
                      const RikkaMessage *const *msgs, size_t n,
                      RikkaStream *out, int timeout_ms,
                      RkStreamDeltaCb delta_cb, void *delta_ud,
                      volatile int *cancel,
                      RikkaSessionStats *stats_out) {
    Buf body;
    buf_init(&body);
    if (rp_build_request(cfg, msgs, n, 1, &body) != 0) { buf_free(&body); return -1; }
    int rc = -1;
    int status = 0;
    for (int attempt = 0; attempt < RIKKA_MAX_RETRIES; attempt++) {
        if (cancel && *cancel) break; /* 取消 */
        RikkaStreamSession *ss = rp_session_create(cfg);
        if (!ss) break;
        ss->delta_cb = delta_cb;
        ss->delta_ud = delta_ud;
        rc = rp_stream_start(ss, NULL, (const char *)body.data, body.len, out,
                             timeout_ms, &status);
        if (rc == 0) {
            rc = rp_stream_pump_async_cancel(ss, timeout_ms, cancel);
            if (stats_out) *stats_out = *rp_session_stats(ss);
            rp_session_destroy(ss);
            break;
        }
        rp_session_destroy(ss);
        if (status >= 400 && status < 500) break; /* 4xx 不重试 */
        if (attempt + 1 < RIKKA_MAX_RETRIES) pm_msleep(100L << attempt); /* 指数退避 */
    }
    buf_free(&body);
    return rc;
}

int rp_chat_stream(const RikkaProviderCfg *cfg,
                   const RikkaMessage *const *msgs, size_t n,
                   RikkaStream *out, int timeout_ms,
                   RikkaSessionStats *stats_out) {
    return rp_chat_stream_cb(cfg, msgs, n, out, timeout_ms, NULL, NULL, NULL, stats_out);
}
