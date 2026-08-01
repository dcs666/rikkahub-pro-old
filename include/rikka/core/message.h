#ifndef RIKKA_CORE_MESSAGE_H
#define RIKKA_CORE_MESSAGE_H

#include <stddef.h>
#include <stdint.h>
#include "rikka/core/buffer.h"
#include "rikka/util/arena.h"

/*
 * 核心消息模型（对标 UIMessage/UIMessagePart/Conversation/MessageNode）。
 *
 * 设计目标（S2 + A1）：
 *  1) 流式累积零拷贝：生成期间文本 append 到可复用 Buf（O(1) 摊销），
 *     part 的 data/len 直接引用缓冲 —— 不再每 token 重建字符串（JVM O(n²) 根因）。
 *  2) 会话分支 COW：节点树共享前缀，regenerate/fork 零复制。
 *  3) 冻结语义：消息完成即不可变，可被多节点/多视图安全共享。
 */

typedef enum {
    RIKKA_ROLE_USER = 0,
    RIKKA_ROLE_ASSISTANT = 1,
    RIKKA_ROLE_SYSTEM = 2,
    RIKKA_ROLE_TOOL = 3,
} RikkaRole;

typedef enum {
    RIKKA_PART_TEXT = 0,
    RIKKA_PART_REASONING = 1,
    RIKKA_PART_IMAGE = 2,       /* 图片 URL 或 base64 data URI */
    RIKKA_PART_TOOL_CALL = 3,
    RIKKA_PART_TOOL_RESULT = 4,
    RIKKA_PART_DOCUMENT = 5,    /* 上传文档（docx/epub/pdf/pptx/文本） */
} RikkaPartType;

typedef struct {
    RikkaPartType type;
    const char *data;      /* 指向缓冲/arena 字节（不拥有） */
    size_t len;
    const char *tool_name; /* TOOL_CALL / TOOL_RESULT */
    const char *tool_id;
    int is_error;          /* TOOL_RESULT 失败标记 */
    const char *doc_mime;  /* DOCUMENT: mime 类型 */
    const char *doc_name;  /* DOCUMENT: 文件名 */
} RikkaPart;

typedef struct {
    RikkaRole role;
    RikkaPart *parts;
    size_t part_count, part_cap;
    /* 消息创建时间（Unix 秒；0 = 未知）——time_reminder/模板变换用 */
    int64_t created_at;
    /* 使用量元数据 */
    int has_usage;
    uint64_t prompt_tokens, completion_tokens, total_tokens;
    /* 流式累积缓冲所有权（freeze 后归消息所有；未 freeze 时归 RikkaStream） */
    Buf *owned_buf;
    Buf *reasoning_owned;   /* reasoning 累积缓冲（同 owned_buf 语义） */
    int frozen;
} RikkaMessage;

/* 创建消息（结构 + parts 数组从 arena 分配） */
RikkaMessage *rmsg_new(Arena *arena, RikkaRole role);
RikkaPart *rmsg_add_part(Arena *arena, RikkaMessage *m, RikkaPartType type);

/*
 * 释放冻结消息持有的流式累积缓冲（owned_buf / reasoning_owned）。
 * 消息结构本身是 arena 分配的（随 arena 释放），但 freeze 转移来的
 * 文本缓冲是 malloc 的——会话树销毁（rconv_destroy）会自动调用；
 * 独立持有冻结消息的调用方必须手动调用，否则泄漏。
 */
void rmsg_free_bufs(RikkaMessage *m);

/* ---------- 流式累积（S2 核心） ---------- */

typedef struct {
    Arena *arena;
    RikkaMessage *msg;       /* 活动消息（可变） */
    Buf text_buf;            /* 文本累积缓冲（part.data 引用其内存） */
    size_t text_part_idx;
    Buf reasoning_buf;       /* 推理累积缓冲 */
    size_t reasoning_part_idx;
    int active;              /* 1 = 未 freeze */
} RikkaStream;

void rstream_init(RikkaStream *s, Arena *arena, RikkaRole role);
void rstream_append_text(RikkaStream *s, const char *data, size_t len);      /* O(1) 摊销 */
void rstream_append_reasoning(RikkaStream *s, const char *data, size_t len);
void rstream_freeze(RikkaStream *s);   /* 完成：buf 所有权转移给消息，标记只读 */
void rstream_destroy(RikkaStream *s);  /* 未 freeze 时释放 buf */

/* ---------- 会话节点树 + COW 分支（A1） ---------- */

typedef struct RNode RNode;
struct RNode {
    const RikkaMessage *msg;  /* 冻结消息；活动（生成中）节点为 NULL */
    RNode *parent;
    RNode **children;
    size_t child_count, child_cap;
    int active;               /* 当前激活分支标记 */
    RikkaStream *stream;      /* 活动节点的流式累积器（生成中，非 NULL） */
};

RNode *rnode_new(RNode *parent);                /* 新节点挂到 parent（NULL = 根） */
RNode *rnode_fork(RNode *at);                   /* 在 at 处 fork：共享前缀零复制 */
void rnode_attach(RNode *n, RikkaMessage *frozen); /* 冻结消息挂载到节点 */
const RikkaMessage *rnode_message(const RNode *n); /* 活动节点返回 NULL */

/* ---------- 会话 ---------- */

typedef struct {
    Arena *arena;
    RNode *root;
    RNode *active;      /* 当前激活叶节点 */
    size_t node_count;
} RConversation;

void rconv_init(RConversation *c, Arena *arena);
/* 释放会话节点树（heap RNode/children/stream）；消息数据在 arena 由调用方释放 */
void rconv_destroy(RConversation *c);
RNode *rconv_append(RConversation *c, RikkaMessage *frozen); /* 追加到 active 之后并激活 */
RNode *rconv_regenerate(RConversation *c, RNode *at);        /* 从 at fork 新分支并激活（返回活动节点） */
void rconv_set_active(RConversation *c, RNode *n);           /* 切换激活分支 */

/* 沿激活链取消息序列（从根到 active，不含活动节点）；out 容量须 >= node_count */
size_t rconv_active_messages(const RConversation *c, const RikkaMessage **out, size_t cap);

#endif /* RIKKA_CORE_MESSAGE_H */
