#ifndef RIKKA_AI_PROVIDER_H
#define RIKKA_AI_PROVIDER_H

#include <stddef.h>
#include <stdint.h>
#include "rikka/core/message.h"
#include "rikka/core/buffer.h"

/*
 * Provider 层（A3 统一流式传输 + B3 错误中间件的基础）：
 *  OpenAI / Claude / Google 三家请求构建 + 统一 SSE 流式管线。
 *
 * 流式管线（对标 JVM GenerationHandler + ChatCompletions + EventSource）：
 *   conn 读线程 → RsseParser（事件）→ RJsonStream（按 provider/事件提取 delta）
 *   → sink 累积到 RikkaStream（零拷贝）→ 完成 freeze。
 */

typedef enum {
    RIKKA_PROVIDER_OPENAI = 0,
    RIKKA_PROVIDER_CLAUDE = 1,
    RIKKA_PROVIDER_GOOGLE = 2,
} RikkaProviderId;

typedef struct {
    RikkaProviderId id;
    const char *base_url;   /* 如 "https://api.openai.com/v1" */
    const char *api_key;
    const char *model;
    int max_tokens;         /* Claude 必需；OpenAI/Google 可选（0=不发送） */
    int enable_cache_control; /* Claude cache_control 断点（B 级） */
} RikkaProviderCfg;

/* 构建 chat completion 请求体（stream=1 时含 stream:true）。返回 0 成功 */
int rp_build_request(const RikkaProviderCfg *cfg,
                     const RikkaMessage *const *msgs, size_t n,
                     int stream, Buf *out);

/* ---------- 统一流式会话 ---------- */

typedef struct RikkaStreamSession RikkaStreamSession;

/* 事件统计（供调试/可观测性） */
typedef struct {
    uint64_t events;
    uint64_t text_chunks;
    uint64_t reasoning_chunks;
    uint64_t error_events;
    uint64_t bytes_received;
} RikkaSessionStats;

RikkaStreamSession *rp_session_create(const RikkaProviderCfg *cfg);
void rp_session_destroy(RikkaStreamSession *ss);

/*
 * 发起流式请求。body 为构建好的请求 JSON；out 为累积目标（每 token 零拷贝）。
 * 内部：连接 → 发送 → 读响应头（检查 status）。
 * 返回 0 成功（HTTP 2xx），负值错误；*http_status 输出状态码。
 */
int rp_stream_start(RikkaStreamSession *ss, const char *path,
                    const char *body, size_t body_len,
                    RikkaStream *out, int timeout_ms, int *http_status);

/*
 * 泵送事件直到 EOF/错误。text 累积到 out 的 text part，
 * reasoning（reasoning_content / thinking_delta）累积到 reasoning part。
 * 返回 0 正常完成，-1 网络/协议错误。
 */
int rp_stream_pump(RikkaStreamSession *ss, int timeout_ms);

const RikkaSessionStats *rp_session_stats(const RikkaStreamSession *ss);

/* 异步泵送（S5）：读线程(rhttp_read_body→SPSC) + 提取线程(SPSC→SSE→累积) 并行。
 * 与 rp_stream_pump 语义相同，但读/解析在不同线程（吞吐=阶段并行）。 */
int rp_stream_pump_async(RikkaStreamSession *ss, int timeout_ms);

/* 便捷：全流程（构建 + 流式 + 累积）。返回 0 成功 */
int rp_chat_stream(const RikkaProviderCfg *cfg,
                   const RikkaMessage *const *msgs, size_t n,
                   RikkaStream *out, int timeout_ms,
                   RikkaSessionStats *stats_out);

#endif /* RIKKA_AI_PROVIDER_H */
