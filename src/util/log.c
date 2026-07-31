#define _POSIX_C_SOURCE 200809L
#include "rikka/util/log.h"
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define RIKKA_LOG_CAPACITY 100   /* 最近 100 条，对标 app 端 MAX_RECENT_LOGS */
#define RIKKA_LOG_MSG_MAX  511

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static RikkaLogLevel   g_level = RIKKA_LOG_INFO;
static int             g_quiet = 0;
static RikkaLogEntry   g_ring[RIKKA_LOG_CAPACITY];
static size_t          g_count = 0;   /* 已写入条数 */
static size_t          g_head  = 0;   /* 下一条写入位置 */

static const char *level_name(RikkaLogLevel l) {
    switch (l) {
        case RIKKA_LOG_TRACE: return "TRACE";
        case RIKKA_LOG_DEBUG: return "DEBUG";
        case RIKKA_LOG_INFO:  return "INFO";
        case RIKKA_LOG_WARN:  return "WARN";
        case RIKKA_LOG_ERROR: return "ERROR";
    }
    return "?";
}

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

void rikka_log_set_level(RikkaLogLevel level) {
    pthread_mutex_lock(&g_lock);
    g_level = level;
    pthread_mutex_unlock(&g_lock);
}

void rikka_log_set_quiet(int quiet) {
    pthread_mutex_lock(&g_lock);
    g_quiet = quiet;
    pthread_mutex_unlock(&g_lock);
}

void rikka_log_write(RikkaLogLevel level, const char *fmt, ...) {
    if (level < g_level) return; /* 非精确读取可接受 */
    char msg[RIKKA_LOG_MSG_MAX + 1];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    msg[RIKKA_LOG_MSG_MAX] = '\0';

    pthread_mutex_lock(&g_lock);
    RikkaLogEntry *e = &g_ring[g_head % RIKKA_LOG_CAPACITY];
    e->ts_ms = now_ms();
    e->level = level;
    snprintf(e->msg, sizeof(e->msg), "%s", msg);
    g_head++;
    g_count++;
    pthread_mutex_unlock(&g_lock);

    if (!g_quiet) {
        fprintf(stderr, "[%s] %s\n", level_name(level), msg);
    }
}

size_t rikka_log_recent(RikkaLogEntry *entries, size_t count) {
    if (!entries || count == 0) return 0;
    pthread_mutex_lock(&g_lock);
    size_t n = g_count < RIKKA_LOG_CAPACITY ? g_count : RIKKA_LOG_CAPACITY;
    if (count > n) count = n;
    size_t last = (g_head - 1 + RIKKA_LOG_CAPACITY) % RIKKA_LOG_CAPACITY; /* 最新位置 */
    for (size_t i = 0; i < count; i++) {
        entries[i] = g_ring[(last + RIKKA_LOG_CAPACITY - i) % RIKKA_LOG_CAPACITY];
    }
    pthread_mutex_unlock(&g_lock);
    return count;
}

void rikka_log_clear(void) {
    pthread_mutex_lock(&g_lock);
    g_head = 0;
    g_count = 0;
    memset(g_ring, 0, sizeof(g_ring));
    pthread_mutex_unlock(&g_lock);
}
