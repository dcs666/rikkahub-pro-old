#ifndef RIKKA_HIGHLIGHT_HIGHLIGHT_H
#define RIKKA_HIGHLIGHT_HIGHLIGHT_H

#include <stddef.h>

/*
 * 代码高亮引擎（S3 增量解析的一部分）。
 * 手写 lexer 直接 tokenize 源码，零解释器（对比 JVM 版 QuickJS 解释执行
 * highlight.js 564KB JS，快 1-2 个数量级）。
 * token 用 offset 而非指针：调用方缓冲可复用/变化。
 */

typedef enum {
    RIKKA_HL_PLAIN = 0,
    RIKKA_HL_KEYWORD,
    RIKKA_HL_STRING,
    RIKKA_HL_COMMENT,
    RIKKA_HL_NUMBER,
    RIKKA_HL_TYPE,
    RIKKA_HL_FUNC,
    RIKKA_HL_OPERATOR,
    RIKKA_HL_PREPROC,
    RIKKA_HL_BUILTIN,
    RIKKA_HL_TAG,       /* HTML 标签 */
    RIKKA_HL_ATTR,      /* HTML 属性名 */
} RikkaHlType;

typedef struct {
    size_t start, len;
    RikkaHlType type;
} RikkaHlToken;

/*
 * 按语言 tokenize 源码。out 容量 cap；返回 token 数（可能截断于 cap）。
 * 未知语言按纯文本（单 token）。
 */
size_t rikka_hl_tokenize(const char *lang, const char *code, size_t len,
                         RikkaHlToken *out, size_t cap);

/* 支持的语言数量与名称（调试/能力查询） */
size_t rikka_hl_lang_count(void);
const char *rikka_hl_lang_name(size_t idx);

#endif /* RIKKA_HIGHLIGHT_HIGHLIGHT_H */
