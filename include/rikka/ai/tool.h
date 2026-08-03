#ifndef RIKKA_AI_TOOL_H
#define RIKKA_AI_TOOL_H

#include <stddef.h>
#include <stdint.h>

/*
 * 工具系统（对标 JVM 版 data/ai/tools + data/ai/tools/local）。
 *
 * 设计：
 *  - 注册表：name → RkTool（schema + 执行函数）；
 *  - 内置纯 C 工具：get_time_info、workspace_read_file/write_file/edit_file/shell、
 *    memory_tool、use_skill；
 *  - 设备/UI 工具（日历/屏幕时间/剪贴板/TTS/JS/AskUser/搜索）经 RkToolEnv 回调
 *    由壳层提供，NULL = 该工具不可用（不注册）；
 *  - 所有工具：args 为 JSON 对象字符串，result 返回 JSON 字符串（malloc，
 *    调用方 free）。返回 0 成功；-1 失败（此时 result 可能被设为
 *    {"error":"..."} 错误 JSON，调用方同样需要 free）。
 *
 * 安全：workspace_* 的路径一律解析到 workspace_root 内（防 ../ 逃逸）。
 */

typedef struct RkTool RkTool;
typedef struct RkToolEnv RkToolEnv;

struct RkToolEnv {
    /* workspace 沙箱 */
    const char *workspace_root;  /* rootfs 根（路径穿越防护基准） */
    const char *workspace_cwd;   /* shell 默认工作目录（可 NULL） */
    /* 记忆存储（对标 AssistantMemory） */
    char *(*memory_read_all)(void *ud);              /* 全部记忆 JSON 数组（malloc） */
    char *(*memory_create)(const char *content, void *ud); /* 返回 JSON 对象（malloc） */
    char *(*memory_edit)(int64_t id, const char *content, void *ud);
    int (*memory_delete)(int64_t id, void *ud);
    /* 设备/UI 工具（壳层实现；NULL = 工具不可注册） */
    char *(*ask_user)(const char *question, void *ud);
    char *(*calendar_query)(const char *args_json, void *ud);
    char *(*calendar_create)(const char *args_json, void *ud);
    char *(*screen_time_query)(const char *args_json, void *ud);
    int (*clipboard_write)(const char *text, void *ud);
    char *(*clipboard_read)(void *ud);                 /* 剪贴板文本（malloc） */
    int (*tts_speak)(const char *text, void *ud);
    char *(*javascript_eval)(const char *code, void *ud);
    char *(*web_search)(const char *query, void *ud);
    /* 外部工具(JVM tools_json 定义; 注册表未命中时执行; NULL=不支持)。
     * 返回 malloc JSON 字符串(工具结果), NULL=工具不存在。 */
    char *(*on_external_tool)(const char *name, const char *args, void *ud);
    /* skills 根目录（use_skill 读取；NULL = 默认 /skills） */
    const char *skills_root;
    /* 本地工具白名单（JSON 数组字符串；NULL/空 = 全部注册，仅过滤设备工具） */
    const char *tool_whitelist;
    /* 会话查询（壳层实现；NULL = 工具不可注册） */
    char *(*recent_chats)(int limit, void *ud);                 /* JSON 数组（malloc） */
    char *(*conversation_search)(const char *query, void *ud);  /* JSON 数组（malloc） */
    void *ud;
};

struct RkTool {
    const char *name;
    const char *description;
    const char *input_schema;   /* JSON Schema 字符串 */
    int (*call)(const RkTool *t, const char *args_json, const RkToolEnv *env,
                char **result);
};

/* ---------- 注册表 ---------- */

typedef struct {
    const RkTool **tools;
    size_t count, cap;
} RkToolRegistry;

void rk_tools_init(RkToolRegistry *r);
void rk_tools_destroy(RkToolRegistry *r);
int rk_tools_add(RkToolRegistry *r, const RkTool *t); /* 重名拒绝 */
const RkTool *rk_tools_find(const RkToolRegistry *r, const char *name);
size_t rk_tools_count(const RkToolRegistry *r);
const RkTool *rk_tools_at(const RkToolRegistry *r, size_t idx);

/* 注册内置纯 C 工具 + env 回调对应的设备工具（回调非 NULL 才注册） */
void rk_tools_register_builtin(RkToolRegistry *r, const RkToolEnv *env);

/* 执行工具：0 成功（*result malloc JSON），-1 失败 */
int rk_tool_call(const RkTool *t, const char *args_json, const RkToolEnv *env,
                 char **result);

/* ---------- JSON 参数辅助 ---------- */

/* 取字符串参数（复制到 out）；不存在返回 -1 */
int rk_tool_arg_str(const char *args_json, const char *key, char *out, size_t out_sz);
int64_t rk_tool_arg_i64(const char *args_json, const char *key, int64_t dflt);
int rk_tool_arg_bool(const char *args_json, const char *key, int dflt);
/* 工具结果 JSON 字符串构造（malloc）：{"key":"value"} 等 */
char *rk_tool_result_json(const char *key, const char *value);
char *rk_tool_result_error(const char *message);

/* ---------- 文本替换（三级策略，对标 JVM TextReplacers） ---------- */

typedef struct {
    char *updated;        /* malloc 结果（error 时为 NULL） */
    size_t replacements;  /* 实际替换数 */
    size_t occurrences;   /* 匹配位置数 */
    const char *strategy; /* "exact" | "line_trimmed" | "block_anchor" */
    int error;            /* 0 成功；1 无匹配；2 多匹配且非 replace_all */
    char errmsg[256];     /* error 时的人类可读信息 */
} RkTextReplaceResult;

/* 三级替换：精确 → 行 trim 窗口 → 块锚点（首尾行）。
 * 结果 malloc，调用方 free out->updated。 */
void rk_text_replace(const char *content, size_t content_len,
                     const char *old_text, const char *new_text, int replace_all,
                     RkTextReplaceResult *out);

#endif /* RIKKA_AI_TOOL_H */
