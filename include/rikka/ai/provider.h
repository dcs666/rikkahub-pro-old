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
    /* OpenAI tools 数组 JSON（可选；rk_chat 从注册表自动生成）。
     * 格式: [{"type":"function","function":{"name":..,"parameters":..}}] */
    const char *tools_json;
    /* 重试策略（HTTP 层中间件）：max_retries=0 不重试（默认）。
     * 可重试条件：网络/连接错误、HTTP 429、5xx；其余 4xx 立即失败。
     * 退避：base_delay_ms << attempt，封顶 max_delay_ms。 */
    struct {
        int max_retries;
        int base_delay_ms;  /* 0 = 用默认 100 */
        int max_delay_ms;   /* 0 = 用默认 2000 */
    } retry;
    /* 思考模式（DeepSeek 等）: reasoning_effort="low"/"high"/"max"(NULL=不写);
     * thinking_enabled=1 时写 thinking:{type:"enabled"} (DeepSeek/Moonshot) */
    const char *reasoning_effort;
    int thinking_enabled;
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
    /* token 用量(流式 usage 解析; 0=未上报) */
    int prompt_tokens;
    int completion_tokens;
} RikkaSessionStats;

RikkaStreamSession *rp_session_create(const RikkaProviderCfg *cfg);
void rp_session_destroy(RikkaStreamSession *ss);

/*
 * 发起流式请求（含重试中间件）。body 为构建好的请求 JSON；out 为累积目标。
 * 内部：连接 → 发送 → 读响应头（检查 status）；非 2xx 时按 retry 策略
 * 指数退避重试（网络错误 / 429 / 5xx），其余 4xx 立即失败。
 * 返回 0 成功（HTTP 2xx），-1 网络错误（重试耗尽），-2 非 2xx（重试耗尽或不可重试）；
 * *http_status 输出最后一次状态码。
 */
int rp_stream_start(RikkaStreamSession *ss, const char *path,
                    const char *body, size_t body_len,
                    RikkaStream *out, int timeout_ms, int *http_status);

/*
 * 取最近一次非 2xx 响应的错误详情（从响应体提取 provider 的
 * {"error":{"message":...}}）。返回 malloc 字符串（调用方 free），
 * 无错误体/无法解析返回 NULL。所有权转移后 session 内部不再持有。
 */
char *rp_take_error_detail(RikkaStreamSession *ss);

/*
 * 泵送事件直到 EOF/错误。text 累积到 out 的 text part，
 * reasoning（reasoning_content / thinking_delta）累积到 reasoning part。
 * 返回 0 正常完成，-1 网络/协议错误。
 */
int rp_stream_pump(RikkaStreamSession *ss, int timeout_ms);
/* 异步 pump 变体：可传入取消标志（置 1 立即中断） */
int rp_stream_pump_async_cancel(RikkaStreamSession *ss, int timeout_ms,
                                volatile int *cancel);

const RikkaSessionStats *rp_session_stats(const RikkaStreamSession *ss);

/* 异步泵送（S5）：读线程(rhttp_read_body→SPSC) + 提取线程(SPSC→SSE→累积) 并行。
 * 与 rp_stream_pump 语义相同，但读/解析在不同线程（吞吐=阶段并行）。 */
int rp_stream_pump_async(RikkaStreamSession *ss, int timeout_ms);

/* 流式增量回调（kind: 0=text, 1=reasoning） */
typedef void (*RkStreamDeltaCb)(void *ud, int kind, const char *data, size_t len);

/* 便捷：全流程（构建 + 流式 + 累积）。返回 0 成功 */
int rp_chat_stream(const RikkaProviderCfg *cfg,
                   const RikkaMessage *const *msgs, size_t n,
                   RikkaStream *out, int timeout_ms,
                   RikkaSessionStats *stats_out);

/* 同 rp_chat_stream，另带流式增量回调（可 NULL）——rk_chat 编排循环用。
 * cancel: 非 NULL 时流式期间周期检查，置 1 则立即中断（关闭连接，返回 -1）。 */
int rp_chat_stream_cb(const RikkaProviderCfg *cfg,
                      const RikkaMessage *const *msgs, size_t n,
                      RikkaStream *out, int timeout_ms,
                      RkStreamDeltaCb delta_cb, void *delta_ud,
                      volatile int *cancel,
                      RikkaSessionStats *stats_out,
                      char **err_detail);

#endif /* RIKKA_AI_PROVIDER_H */
