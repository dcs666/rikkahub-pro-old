#ifndef RIKKA_AI_PROMPT_H
#define RIKKA_AI_PROMPT_H

#include "rikka/util/arena.h"

/*
 * 提示词模板集（对标 JVM 版 data/ai/prompts + GenerationPrompts）。
 *
 * 模板为常量字符串，占位符用单花括号 {key}（与 transform 的 {{var}} 区分）；
 * rk_prompt_fill 只替换已知键，未知键原样保留。
 */

extern const char *const RK_PROMPT_COMPRESS;     /* 占位: {target_tokens} {locale} {additional_context} {content} */
extern const char *const RK_PROMPT_LEARNING_MODE; /* 无占位符 */
extern const char *const RK_PROMPT_OCR;           /* 无占位符 */
extern const char *const RK_PROMPT_SUGGESTION;    /* 占位: {locale} {content} */
extern const char *const RK_PROMPT_TITLE;         /* 占位: {locale} {content} */
extern const char *const RK_PROMPT_TRANSLATION;   /* 占位: {target_lang} {source_text} */

/* 填充模板：{key} → value（未知键保留原样）。返回 arena 分配字符串。 */
char *rk_prompt_fill(Arena *a, const char *tpl,
                     const char *const *names, const char *const *values, size_t n);

/* 记忆注入块（对标 buildMemoryPrompt）：
 * "**Memories**\n... \n[{\"id\":..,\"content\":..},...]\n" 返回 arena 字符串。 */
char *rk_prompt_memories(Arena *a, const int64_t *ids, const char *const *contents, size_t n);

#endif /* RIKKA_AI_PROMPT_H */
