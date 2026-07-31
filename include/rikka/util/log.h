#ifndef RIKKA_UTIL_LOG_H
#define RIKKA_UTIL_LOG_H

#include <stddef.h>
#include <stdint.h>

/*
 * 环形日志（对标 app 端 Logging：最近 N 条，可查询）。
 * C 版零开销：固定环形缓冲 + 级别过滤 + 可选时间戳。
 * 线程安全：单锁覆盖写入与查询（低频路径，无锁化留给流水线阶段内）。
 */

typedef enum {
    RIKKA_LOG_TRACE = 0,
    RIKKA_LOG_DEBUG = 1,
    RIKKA_LOG_INFO  = 2,
    RIKKA_LOG_WARN  = 3,
    RIKKA_LOG_ERROR = 4,
} RikkaLogLevel;

/* 条目查询（对标 Logging.getRecentLogs） */
typedef struct {
    uint64_t     ts_ms;      /* epoch ms */
    RikkaLogLevel level;
    char         msg[512];   /* 截断后的消息 */
} RikkaLogEntry;

void rikka_log_set_level(RikkaLogLevel level);
void rikka_log_set_quiet(int quiet); /* 1 = 只记录环、不写 stderr（测试/生产静默） */
void rikka_log_write(RikkaLogLevel level, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

/* 最近 count 条（新的在前），返回实际条数；entries 容量须 >= count */
size_t rikka_log_recent(RikkaLogEntry *entries, size_t count);
void   rikka_log_clear(void);

#define RIKKA_LOG_TRACE_(...) rikka_log_write(RIKKA_LOG_TRACE, __VA_ARGS__)
#define RIKKA_LOG_DEBUG_(...) rikka_log_write(RIKKA_LOG_DEBUG, __VA_ARGS__)
#define RIKKA_LOG_INFO_(...)  rikka_log_write(RIKKA_LOG_INFO,  __VA_ARGS__)
#define RIKKA_LOG_WARN_(...)  rikka_log_write(RIKKA_LOG_WARN,  __VA_ARGS__)
#define RIKKA_LOG_ERROR_(...) rikka_log_write(RIKKA_LOG_ERROR, __VA_ARGS__)

#endif /* RIKKA_UTIL_LOG_H */
