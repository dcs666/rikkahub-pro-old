#ifndef RIKKA_AI_TRANSFORM_H
#define RIKKA_AI_TRANSFORM_H

#include <stddef.h>
#include <stdint.h>
#include "rikka/core/message.h"
#include "rikka/util/arena.h"

/*
 * 消息变换管线（对标 JVM 版 data/ai/transformers）。
 *
 * 设计要点：
 *  1) 纯 C 无外部依赖：正则用 POSIX regcomp，模板为 Pebble 子集。
 *  2) COW 不变式：输入冻结消息只读，变换构建新消息列表（Arena 分配），
 *     文本数据与输入共享（零拷贝），仅被替换的 part 才写新 arena 文本。
 *  3) 输入管线顺序（JVM 版一致）：
 *     time_reminder → prompt_injection → placeholder → document_as_prompt
 *     → ocr → template → workspace_reminder
 *  4) 输出管线：think_tag(visual/finish) → base64_image(finish) → regex(visual)。
 */

/* ---------- 可变换消息列表（Arena 上） ---------- */

typedef struct {
    Arena *arena;
    RikkaMessage **items;
    size_t count, cap;
} RkMsgList;

void rk_msgl_init(RkMsgList *l, Arena *a);
/* 从冻结消息数组复制消息结构（parts 引用原数据，零拷贝共享） */
void rk_msgl_from(RkMsgList *l, Arena *a, const RikkaMessage *const *msgs, size_t n);
RikkaMessage *rk_msgl_add(RkMsgList *l, RikkaRole role);          /* 追加新消息（含 TEXT part 追加接口） */
void rk_msgl_insert(RkMsgList *l, size_t idx, RikkaMessage *m);   /* 插入（复制语义，m 须不在列表中） */
/* 把列表中的元素从 from 移动到 to（add 到尾部后再移到插入点的正确做法，
 * 避免复制插入在尾部产生残留副本） */
void rk_msgl_move(RkMsgList *l, size_t from, size_t to);
RikkaPart *rk_msgl_add_part(RkMsgList *l, RikkaMessage *m, RikkaPartType type);
/* 给消息追加文本（新消息 / 已有 TEXT part 合并，arena 分配） */
void rk_msgl_append_text(RkMsgList *l, RikkaMessage *m, const char *text);

/* ---------- 变换配置（对标 TransformerContext 的快照） ---------- */

typedef struct {
    /* 模型 */
    const char *model_id;            /* 占位符 {{model_id}} */
    const char *model_name;          /* 占位符 {{model_name}} */
    int model_supports_images;       /* inputModalities 含 IMAGE → OCR 跳过 */
    /* 助手 */
    const char *assistant_name;      /* 占位符 {{char}} */
    int enable_time_reminder;        /* TimeReminderTransformer 开关 */
    int allow_conversation_prompt_injection; /* 会话级注入优先 */
    const char *workspace_id;        /* 绑定的 workspace id（非空才注入 workspace 提醒） */
    const char *workspace_cwd;       /* 当前工作目录（附加到 workspace 提醒） */
    /* 设置 */
    const char *user_nickname;       /* 占位符 {{user}}/{{nickname}}；NULL = "user" */
    const char *locale_name;         /* 占位符 {{locale}}；NULL = "unknown" */
    const char *tz_name;             /* 占位符 {{timezone}}；NULL = "UTC" */
    /* 设备（占位符用） */
    const char *device_brand;        /* {{device_info}} 前半；NULL 跳过 */
    const char *device_model;        /* {{device_info}} 后半 */
    const char *os_version;          /* {{system_version}}；NULL = "unknown" */
    int battery_pct;                 /* {{battery_level}}；-1 = 未知（输出 "?"） */
    /* 会话级注入启用集合（NULL 结尾数组；可为 NULL） */
    const char **conv_mode_injection_ids;
    const char **conv_lorebook_ids;
} RkTransformConfig;

/* ---------- 注入规则（对标 PromptInjection/ModeInjection/RegexInjection） ---------- */

typedef enum {
    RK_INJ_BEFORE_SYSTEM_PROMPT = 0,
    RK_INJ_AFTER_SYSTEM_PROMPT,
    RK_INJ_TOP_OF_CHAT,
    RK_INJ_BOTTOM_OF_CHAT,
    RK_INJ_AT_DEPTH,
} RkInjPosition;

typedef struct {
    const char *id;
    const char *name;
    int enabled;
    int priority;               /* 大者先应用 */
    RkInjPosition position;
    const char *content;
    int inject_depth;           /* AT_DEPTH：从最新往前数的深度（>=1） */
    RikkaRole role;             /* 注入消息角色：USER 或 ASSISTANT */
    /* RegexInjection 特有（keyword 为空数组 = ModeInjection） */
    const char **keywords;      /* NULL 结尾数组 */
    int use_regex;
    int case_sensitive;
    int scan_depth;             /* 扫描最近 N 条非 SYSTEM 消息 */
    int constant_active;
} RkInjection;

typedef struct {
    const char *id;
    const char *name;
    int enabled;
    const RkInjection **entries;
    size_t entry_count;
} RkLorebook;

/* ---------- 输出正则（对标 AssistantRegex） ---------- */

typedef struct {
    const char *id;
    const char *name;
    int enabled;
    const char *find_regex;
    const char *replace_string;
    int affects_user;           /* affectingScope 含 USER */
    int affects_assistant;      /* affectingScope 含 ASSISTANT */
    int visual_only;            /* 仅视觉替换（流式显示） */
} RkOutputRegex;

/* ---------- 输入 transformers（按 JVM 版顺序调用） ---------- */

/* 1. 时间提醒：首条用户消息前注入 <time_reminder>；后续与上一条消息间隔
 *    > 3600s 时注入带间隔文本的提醒。now_epoch = 当前 Unix 秒。 */
void rk_transform_time_reminder(RkMsgList *l, int64_t now_epoch);

/* 2. 提示词注入：收集 ModeInjection + Lorebook 触发项，按位置/优先级应用。 */
void rk_transform_prompt_injection(RkMsgList *l, const RkTransformConfig *cfg,
                                   const RkInjection *const *mode_injs, size_t n_mode,
                                   const RkLorebook *const *lorebooks, size_t n_lb);

/* 3. 占位符替换：{{key}} / {key}（大小写不敏感），11 个内置键。 */
void rk_transform_placeholder(RkMsgList *l, const RkTransformConfig *cfg);

/* 4. 文档转提示：DOCUMENT part → <UploadFile> 文本块（放在消息最前）。
 *    read_doc 回调：按 mime 解析文档为文本（NULL 返回则用错误占位）。
 *    resolve_path 回调：返回 workspace 内路径（NULL = 无 path 属性）。 */
typedef const char *(*RkDocReader)(const char *mime, const char *path,
                                   const char *file_name, void *ud);
typedef const char *(*RkDocPathResolver)(const char *path, void *ud);
void rk_transform_document_as_prompt(RkMsgList *l, RkDocReader reader,
                                     RkDocPathResolver resolver, void *ud);

/* 5. OCR：模型不支持图片时，IMAGE part → OCR 文本块。ocr 回调返回识别文本
 *    （NULL = 失败，输出 [Image] 占位）。 */
typedef const char *(*RkOcrCallback)(const char *image_path, void *ud);
void rk_transform_ocr(RkMsgList *l, const RkTransformConfig *cfg,
                      RkOcrCallback ocr, void *ud);

/* 6. 消息模板渲染（Pebble 子集：{{message}} {{role}} {{time}} {{date}}）。
 *    template_text 为模板内容；时间为消息的创建时间（epoch 秒）。 */
void rk_transform_template(RkMsgList *l, const char *template_text, int64_t tz_offset_sec);

/* 7. workspace 提醒：system 消息追加 <workspace> 引导（无 system 则插入）。 */
void rk_transform_workspace_reminder(RkMsgList *l, const RkTransformConfig *cfg);

/* ---------- 输出 transformers ---------- */

/* 8. ThinkTag：文本 part 中 <think>...</think> 拆为 REASONING part。
 *    visual 版本：未闭合（无 </think>）时 reasoning 不带结束时间；
 *    finish 版本：统一补结束时间。 */
void rk_transform_think_tag(RkMsgList *l);          /* visual */
void rk_transform_think_tag_finish(RkMsgList *l, int64_t now_epoch);

/* 9. 正则输出替换：assistant 消息文本/reasoning 按规则替换（visualOnly 匹配）。 */
void rk_transform_regex_output(RkMsgList *l, const RkOutputRegex *const *regexes, size_t n);

/* 10. base64 图片落盘：IMAGE part（data: URI）→ 保存为本地文件并替换 URL。
 *     save 回调：接收 data URI 与 part，返回新本地 URL（arena 或静态）；NULL = 跳过。 */
typedef const char *(*RkImageSaver)(const char *data_uri, const char *file_name_hint, void *ud);
void rk_transform_base64_image(RkMsgList *l, RkImageSaver saver, void *ud);

/* ---------- 正则/文本工具（transform 内部使用，也暴露给工具系统） ---------- */

/* 正则替换（POSIX ERE，$N 组引用；编译失败/替换失败返回原串副本，arena 分配）。
 * icase：大小写不敏感。 */
char *rk_regex_replace(Arena *a, const char *text, const char *pattern,
                       const char *replacement, int icase);
/* 是否包含匹配（Java containsMatchIn 语义：非锚定子串匹配） */
int rk_regex_contains(const char *text, const char *pattern, int icase);
/* 大小写不敏感子串查找（NULL 表示未找到） */
const char *rk_strcasestr(const char *haystack, const char *needle);
/* 模板渲染：替换 {{var}} 为值（不存在的变量替换为空串） */
char *rk_template_render(Arena *a, const char *tpl,
                         const char *const *names, const char *const *values, size_t n);

#endif /* RIKKA_AI_TRANSFORM_H */
