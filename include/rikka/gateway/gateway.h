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
    /* 生命周期：活跃请求计数（stop 后 run 退出路径等待归零再清理） */
    pthread_mutex_t life_mutex;
    pthread_cond_t life_cond;
    int active_handlers;
} RkGateway;

/* 初始化网关（监听端口） */
int rk_gateway_init(RkGateway *g, int port);

/* 添加 provider */
int rk_gateway_add_provider(RkGateway *g, const char *name, const char *api_key, const char *base_url);

/* 运行网关（阻塞，epoll 事件循环）。
 * 退出时等待所有在途请求结束并清理连接池/监听 fd；
 * 因此 stop 只做信号，资源释放全部发生在 run 返回前。 */
int rk_gateway_run(RkGateway *g);

/*
 * 多实例运行：n 个 worker 线程各自独立 epoll 事件循环，
 * 通过 SO_REUSEPORT 共享端口（内核负载均衡），连接池跨 worker 共享。
 * 语义与 rk_gateway_run 相同：阻塞直到 stop，退出时统一清理。
 */
int rk_gateway_run_multi(RkGateway *g, int n);

/* 多实例辅助：为额外 worker 创建共享端口监听 socket（SO_REUSEPORT）。
 * 返回 fd（调用方负责关闭）或 -1。 */
int rk_gateway_listen_extra(int port);

/* 停止网关（仅置位停止标志；事件循环 ≤1s 内退出）。
 * 非线程安全约束：stop 与 run 可并发，但 stop 后必须 join run 线程
 * 才能安全释放/复用 RkGateway 内存。 */
void rk_gateway_stop(RkGateway *g);

#endif /* RIKKA_GATEWAY_GATEWAY_H */
