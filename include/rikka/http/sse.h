#ifndef RIKKA_HTTP_SSE_H
#define RIKKA_HTTP_SSE_H

#include <stddef.h>
#include "rikka/core/buffer.h"

/*
 * SSE（Server-Sent Events）解析器：喂字节流 → 完整事件回调。
 * 对标 OkHttp EventSource + kotlinx.serialization SSE。
 * 事件语义：data 多行以 \n 拼接，data: 值前导空格剥离；
 * event/id/retry 字段支持；注释行（: 开头）忽略。
 */

typedef struct RsseParser RsseParser;

typedef void (*RsseEventCb)(void *ctx, const char *event, const char *data, size_t data_len,
                            const char *id, long long retry_ms);

RsseParser *rsse_create(RsseEventCb cb, void *ctx);
void rsse_destroy(RsseParser *p);
void rsse_reset(RsseParser *p);   /* 复用（连接级） */

/* 喂入字节；返回 0 正常，-1 解析错误 */
int rsse_feed(RsseParser *p, const char *data, size_t len);
/* EOF：强制 flush 未结束的事件（无尾空行的最后事件） */
void rsse_finish(RsseParser *p);

#endif /* RIKKA_HTTP_SSE_H */
