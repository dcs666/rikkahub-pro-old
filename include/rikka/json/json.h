#ifndef RIKKA_JSON_JSON_H
#define RIKKA_JSON_JSON_H

#include <stddef.h>
#include <stdint.h>
#include "rikka/util/arena.h"

/*
 * Rikka JSON 模块：一个自包含 JSON 实现，覆盖三种场景：
 *
 *  1) 值解析（tree）：完整 JSON → 值树（arena 分配），对标非流式序列化场景。
 *  2) 序列化：值树/字符串 → JSON 文本，用于 Provider 请求构建。
 *  3) 增量流式提取（JsonStream）：SSE 场景边收字节边提取目标路径的字符串值，
 *     只解析需要的字段、解码一次转义、分片输出 —— 这是"每 token 全量解析"
 *     问题的根解，也是零拷贝流式管线（S2）的解析层。
 */

/* ---------- 1) 值树 ---------- */

typedef enum {
    RJSON_NULL,
    RJSON_BOOL,
    RJSON_NUMBER,
    RJSON_STRING,
    RJSON_ARRAY,
    RJSON_OBJECT,
} RJsonType;

typedef struct RJson RJson;

struct RJson {
    RJsonType type;
    union {
        int        boolean;
        double     number;
        struct { const char *ptr; size_t len; } str;
        struct { RJson **items; size_t count; size_t cap; } arr;
        struct {
            const char **keys;   /* 指向 arena 内拷贝的键字符串 */
            RJson **values;
            size_t count, cap;
        } obj;
    } u;
};

/* 解析完整 JSON 文本到 arena；返回 NULL 表示语法错误（*err_pos 指向出错偏移） */
RJson *rjson_parse(Arena *arena, const char *text, size_t len, size_t *err_pos);

/* 便捷访问器 */
const RJson *rjson_obj_get(const RJson *obj, const char *key);
const RJson *rjson_arr_at(const RJson *arr, size_t idx);
int          rjson_is(const RJson *v, RJsonType t);
const char  *rjson_str(const RJson *v, size_t *len); /* 仅 STRING，否则 NULL */

/* ---------- 2) 序列化 ---------- */

typedef struct {
    char  *buf;
    size_t len, cap;
} RJsonOut;

void rjson_out_init(RJsonOut *o);
void rjson_out_free(RJsonOut *o);
void rjson_write_value(RJsonOut *o, const RJson *v);
void rjson_write_string(RJsonOut *o, const char *s, size_t len); /* 转义输出 */
void rjson_write_escaped(RJsonOut *o, const char *s, size_t len); /* 转义但不加引号 */

/* ---------- 3) 增量流式提取 ---------- */

/*
 * JsonStream：SSE/data 场景的增量路径提取器。
 * 输入分片字节，当解析路径命中目标路径（如 choices[0].delta.content）时，
 * 把该字符串的值分片回调给调用方（解码转义），其余字段完全跳过。
 * 不构建任何对象 —— 零分配，输出直接进调用方累积缓冲。
 */

typedef enum {
    RJSON_STREAM_OK = 0,       /* 继续喂数据 */
    RJSON_STREAM_DONE,         /* 根值已完整结束（可安全复用/重建） */
    RJSON_STREAM_ERROR,        /* 语法错误 */
} RJsonStreamStatus;

typedef struct {
    /* 目标路径元素：键（字符串）或数组索引 */
    int  is_index;
    union { const char *key; size_t index; } u;
} RJsonStreamPathElem;

typedef struct RJsonStream RJsonStream;

typedef void (*RJsonStreamSink)(void *ctx, const char *data, size_t len);

/*
 * 创建提取器。path 元素数组以 { .is_index = -2 } 终结（空路径传 NULL）；
 * sink 在命中目标字符串值时被调用（可能分多次调用，每次一段解码后的文本）。
 * sink 可为 NULL（只验证）。限制：对象键中的 \u 转义不解码（SSE 键均为 ASCII）。
 */
RJsonStream *rjson_stream_create(const RJsonStreamPathElem *path,
                                 RJsonStreamSink sink, void *sink_ctx);
void rjson_stream_destroy(RJsonStream *s);

/* 复用同一 stream 处理下一个 JSON 事件（SSE 场景避免每事件 malloc） */
void rjson_stream_reset(RJsonStream *s);
/* 切换目标路径（事件类型不同时复用同一 stream） */
void rjson_stream_set_path(RJsonStream *s, const RJsonStreamPathElem *path);

/* 喂入一段字节；返回状态。EOF 时调用 rjson_stream_finish 强制收尾。 */
RJsonStreamStatus rjson_stream_feed(RJsonStream *s, const char *data, size_t len);
RJsonStreamStatus rjson_stream_finish(RJsonStream *s);

/* 是否已命中过目标路径（供状态查询/调试） */
int rjson_stream_hit(const RJsonStream *s);

#endif /* RIKKA_JSON_JSON_H */
