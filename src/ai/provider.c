#define _POSIX_C_SOURCE 200809L
#include "rikka/ai/provider.h"
#include "rikka/http/http.h"
#include "rikka/http/sse.h"
#include "rikka/json/json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
                if (p->len == 0) { first = first; continue; }
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
    RikkaStream *out;
    RikkaSessionStats stats;
    Buf type_buf;
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

static void sink_text(void *ctx, const char *data, size_t len) {
    RikkaStreamSession *ss = (RikkaStreamSession *)ctx;
    rstream_append_text(ss->out, data, len);
    ss->stats.text_chunks++;
}

static void sink_reason(void *ctx, const char *data, size_t len) {
    RikkaStreamSession *ss = (RikkaStreamSession *)ctx;
    rstream_append_reasoning(ss->out, data, len);
    ss->stats.reasoning_chunks++;
}

static void sink_type(void *ctx, const char *data, size_t len) {
    RikkaStreamSession *ss = (RikkaStreamSession *)ctx;
    buf_append(&ss->type_buf, data, len);
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

/* base_url 解析（host/port/tls/prefix） */
static int parse_base(const char *url, char *host, size_t host_cap, uint16_t *port,
                      int *tls, char *prefix, size_t prefix_cap) {
    const char *p = url;
    *tls = 0;
    if (strncmp(p, "https://", 8) == 0) { *tls = 1; p += 8; }
    else if (strncmp(p, "http://", 7) == 0) { p += 7; }
    else return -1;
    const char *he = p;
    while (*he && *he != ':' && *he != '/') he++;
    size_t hn = (size_t)(he - p);
    if (hn >= host_cap) return -1;
    memcpy(host, p, hn);
    host[hn] = '\0';
    *port = *tls ? 443 : 80;
    if (*he == ':') {
        const char *ps = he + 1;
        long po = 0;
        while (*ps >= '0' && *ps <= '9') po = po * 10 + (*ps - '0'), ps++;
        if (po > 0 && po < 65536) *port = (uint16_t)po;
        he = ps;
    }
    if (*he == '/') {
        size_t pn = strlen(he);
        if (pn >= prefix_cap) return -1;
        memcpy(prefix, he, pn + 1);
    } else {
        snprintf(prefix, prefix_cap, "/");
    }
    return 0;
}

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
    return ss;
}

void rp_session_destroy(RikkaStreamSession *ss) {
    if (!ss) return;
    if (ss->conn) rhttp_close(ss->conn);
    if (ss->sse) rsse_destroy(ss->sse);
    if (ss->js_type) rjson_stream_destroy(ss->js_type);
    if (ss->js_text) rjson_stream_destroy(ss->js_text);
    if (ss->js_reason) rjson_stream_destroy(ss->js_reason);
    buf_free(&ss->type_buf);
    free(ss);
}

int rp_stream_start(RikkaStreamSession *ss, const char *path,
                    const char *body, size_t body_len,
                    RikkaStream *out, int timeout_ms, int *http_status) {
    if (!ss || !out) return -1;
    ss->out = out;
    char host[256], prefix[512], full[1024];
    uint16_t port;
    int tls;
    if (parse_base(ss->cfg.base_url, host, sizeof(host), &port, &tls,
                   prefix, sizeof(prefix)) != 0)
        return -1;
    if (!path) {
        if (ss->cfg.id == RIKKA_PROVIDER_GOOGLE) {
            snprintf(full, sizeof(full), "%s%s", prefix, default_chat_path(ss->cfg.id));
            /* 替换 %s 为 model */
            char model_path[1024];
            snprintf(model_path, sizeof(model_path), default_chat_path(ss->cfg.id),
                     ss->cfg.model ? ss->cfg.model : "");
            snprintf(full, sizeof(full), "%s%s", prefix, model_path);
        } else {
            snprintf(full, sizeof(full), "%s%s", prefix, default_chat_path(ss->cfg.id));
        }
        path = full;
    }
    ss->conn = rhttp_connect(host, port, tls, timeout_ms);
    if (!ss->conn) return -1;

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

    if (rhttp_send(ss->conn, "POST", path, hdrs, body, body_len) != 0) return -1;
    RHttpResp resp;
    if (rhttp_read_headers(ss->conn, &resp, timeout_ms) != 0) return -1;
    if (http_status) *http_status = resp.status;
    if (resp.status < 200 || resp.status >= 300) return -2; /* 非 2xx */

    ss->sse = rsse_create(on_sse_event, ss);
    ss->js_type = rjson_stream_create(P_CLAUDE_TYPE, sink_type, ss);
    ss->js_text = rjson_stream_create(P_OAI_CONTENT, sink_text, ss);
    ss->js_reason = rjson_stream_create(P_OAI_REASON, sink_reason, ss);
    switch (ss->cfg.id) {
        case RIKKA_PROVIDER_OPENAI:
            rjson_stream_set_path(ss->js_text, P_OAI_CONTENT);
            rjson_stream_set_path(ss->js_reason, P_OAI_REASON);
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
    return 0;
}

const RikkaSessionStats *rp_session_stats(const RikkaStreamSession *ss) {
    return ss ? &ss->stats : NULL;
}

int rp_chat_stream(const RikkaProviderCfg *cfg,
                   const RikkaMessage *const *msgs, size_t n,
                   RikkaStream *out, int timeout_ms,
                   RikkaSessionStats *stats_out) {
    Buf body;
    buf_init(&body);
    if (rp_build_request(cfg, msgs, n, 1, &body) != 0) { buf_free(&body); return -1; }
    RikkaStreamSession *ss = rp_session_create(cfg);
    if (!ss) { buf_free(&body); return -1; }
    int status = 0;
    int rc = rp_stream_start(ss, NULL, (const char *)body.data, body.len, out, timeout_ms, &status);
    if (rc == 0) rc = rp_stream_pump(ss, timeout_ms);
    if (stats_out && ss) *stats_out = *rp_session_stats(ss);
    buf_free(&body);
    rp_session_destroy(ss);
    return rc;
}
