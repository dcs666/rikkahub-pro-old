#ifndef RIKKA_AI_CHAT_H
#define RIKKA_AI_CHAT_H

#include <stddef.h>
#include <stdint.h>

#include "rikka/ai/provider.h"
#include "rikka/ai/transform.h"
#include "rikka/ai/tool.h"
#include "rikka/core/message.h"

/*
 * 聊天编排循环（对标 JVM 版 GenerationHandler）。
 *
 * 流程：输入变换（调用方钩子）→ provider 流式生成（增量回调）→
 * 输出变换（调用方钩子）→ 若含工具调用：执行工具（注册表）→
 * 追加 tool 消息 → 重新请求（最多 max_tool_rounds 轮）→ 最终文本。
 *
 * 线程模型：rk_chat_run 阻塞；增量回调在引擎读线程/本线程触发，
 * 调用方不得在回调里长时间阻塞（回调内可拷贝数据后立即返回）。
 */

typedef struct RkChatCallbacks {
    void *ud;
    /* 流式增量（kind: 0=text, 1=reasoning） */
    void (*on_delta)(void *ud, int kind, const char *data, size_t len);
    /* 工具调用与结果（执行前/后各一次） */
    void (*on_tool_call)(void *ud, const char *name, const char *args_json);
    void (*on_tool_result)(void *ud, const char *name, const char *result_json);
} RkChatCallbacks;

typedef struct RkChatConfig {
    RikkaProviderCfg provider;
    /* 变换钩子（可 NULL）：
     * transform_input 在请求前对 work 列表执行（time_reminder/注入等）；
     * transform_output 对刚生成的 assistant 消息执行（think_tag 等 finish 变换）。 */
    void (*transform_input)(RkMsgList *work, void *ud);
    void (*transform_output)(RkMsgList *work, RikkaMessage *assistant, void *ud);
    void *transform_ud;
    /* 流式 think_tag（对标 JVM ThinkTagTransformer visual）：
     * 把流式输出中的 <think>...</think> 路由为 reasoning（kind=1），
     * 其余为 text。开启后 transform_output 不应再调 think_tag（避免重复）。 */
    int use_visual_think_tag;
    const RkToolRegistry *tools;  /* 工具集（NULL = 无工具） */
    const RkToolEnv *tool_env;    /* 工具环境（tools 非 NULL 时必填） */
    int max_tool_rounds;          /* 0 = 默认 8 */
    int timeout_ms;               /* 每轮超时；0 = 默认 60000 */
    /* 取消标志（volatile int*；非 NULL 时生成期间周期检查，置 1 则中断返回 -1）。 */
    volatile int *cancel_flag;
} RkChatConfig;

/* 流式 think_tag 状态机（visual）：把 <think>...</think> 路由为 reasoning。
 * feed 输出经 out 回调（kind: 0=text, 1=reasoning）。标签可跨块切分。 */
typedef struct {
    int in_think;
    char tag_buf[16];   /* 部分匹配 "<think>"/"</think>" 前缀 */
    size_t tag_len;
} RkThinkState;

void rk_chat_think_feed(RkThinkState *st, const char *data, size_t len,
                        void (*out)(void *ud, int kind, const char *data, size_t len),
                        void *ud);

/* 运行一轮对话。msgs 为冻结历史（COW 列表或数组）。
 * 返回 0 成功（final_text_out 为最终文本 malloc，可能为空串）；
 * -1 失败（error_out 为错误详情 malloc，可 NULL）。 */
int rk_chat_run(const RkChatConfig *cfg, RkChatCallbacks *cb,
                const RikkaMessage *const *msgs, size_t n,
                char **final_text_out, char **error_out);

#endif /* RIKKA_AI_CHAT_H */
