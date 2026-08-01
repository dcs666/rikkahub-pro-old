#ifndef RIKKA_GATEWAY_GATEWAY_H
#define RIKKA_GATEWAY_GATEWAY_H

#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

/*
 * 服务端网关：HTTP 服务器 + AI 请求代理。
 * 对标 JVM 版 Ktor Web 服务器。
 * 简化实现：epoll 事件循环，POST /chat 代理 AI 请求。
 */

typedef struct {
    char name[64];
    char api_key[256];
    char base_url[256];
} RkProviderConfig;

/* 连接池条目 */
typedef struct {
    char host[256];
    int port;
    int tls;
    void *conn;  /* RHttpConn* */
    int in_use;
} RkPoolConn;

typedef struct {
    int fd;              /* listen socket */
    int port;
    int running;
    RkProviderConfig providers[16];
    size_t provider_count;
    /* 连接池 */
    RkPoolConn pool[32];
    size_t pool_count;
    pthread_mutex_t pool_mutex;
} RkGateway;

/* 初始化网关（监听端口） */
int rk_gateway_init(RkGateway *g, int port);

/* 添加 provider */
int rk_gateway_add_provider(RkGateway *g, const char *name, const char *api_key, const char *base_url);

/* 运行网关（阻塞，epoll 事件循环） */
int rk_gateway_run(RkGateway *g);

/* 停止网关 */
void rk_gateway_stop(RkGateway *g);

#endif /* RIKKA_GATEWAY_GATEWAY_H */
