#define _POSIX_C_SOURCE 200809L
#include "rikka/gateway/gateway.h"
#include "rikka/http/http.h"
#include "rikka/json/json.h"
#include "rikka/util/arena.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <errno.h>

int rk_gateway_init(RkGateway *g, int port) {
    if (!g) return -1;
    /* 网络服务器标准：客户端提前断开时 write 应返回 EPIPE 而非 SIGPIPE
     * 杀进程（handler 线程可能在任何时刻写已关闭的客户端 fd） */
    signal(SIGPIPE, SIG_IGN);
    memset(g, 0, sizeof(RkGateway));
    g->port = port;
    g->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g->fd < 0) return -1;
    int opt = 1;
    setsockopt(g->fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    /* SO_REUSEPORT：多实例模式（rk_gateway_run_multi）下内核负载均衡；
     * 所有共享端口的 socket 都必须设置，因此单实例也统一开启 */
#ifdef SO_REUSEPORT
    setsockopt(g->fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
#endif
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((uint16_t)port);
    if (bind(g->fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(g->fd);
        g->fd = -1;
        return -1;
    }
    if (listen(g->fd, 128) != 0) {
        close(g->fd);
        g->fd = -1;
        return -1;
    }
    __atomic_store_n(&g->running, 1, __ATOMIC_RELAXED);
    g->pool_count = 0;
    pthread_mutex_init(&g->pool_mutex, NULL);
    pthread_mutex_init(&g->life_mutex, NULL);
    pthread_cond_init(&g->life_cond, NULL);
    g->active_handlers = 0;
    return 0;
}

/* 多实例：为第 2..n 个 worker 创建共享端口的监听 socket（SO_REUSEPORT）。
 * 返回 fd（调用方负责关闭）或 -1。 */
int rk_gateway_listen_extra(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#ifdef SO_REUSEPORT
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
#endif
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(fd, 128) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int rk_gateway_add_provider(RkGateway *g, const char *name, const char *api_key, const char *base_url) {
    if (!g || g->provider_count >= 16) return -1;
    RkProviderConfig *p = &g->providers[g->provider_count++];
    snprintf(p->name, sizeof(p->name), "%s", name ? name : "");
    snprintf(p->api_key, sizeof(p->api_key), "%s", api_key ? api_key : "");
    snprintf(p->base_url, sizeof(p->base_url), "%s", base_url ? base_url : "");
    return 0;
}

/* 从连接池获取连接 */
static RHttpConn *pool_get_conn(RkGateway *g, const char *host, int port, int tls) {
    pthread_mutex_lock(&g->pool_mutex);
    for (size_t i = 0; i < g->pool_count; i++) {
        RkPoolConn *pc = &g->pool[i];
        if (!pc->in_use && pc->port == port && pc->tls == tls &&
            strcmp(pc->host, host) == 0) {
            pc->in_use = 1;
            pthread_mutex_unlock(&g->pool_mutex);
            return pc->conn;
        }
    }
    pthread_mutex_unlock(&g->pool_mutex);
    /* 新建连接 */
    RHttpConn *conn = rhttp_connect(host, (uint16_t)port, tls, 30000);
    if (!conn) return NULL;
    pthread_mutex_lock(&g->pool_mutex);
    if (g->pool_count < 32) {
        RkPoolConn *pc = &g->pool[g->pool_count++];
        snprintf(pc->host, sizeof(pc->host), "%s", host);
        pc->port = port;
        pc->tls = tls;
        pc->conn = conn;
        pc->in_use = 1;
    }
    pthread_mutex_unlock(&g->pool_mutex);
    return conn;
}

/* 释放连接回池；已到 EOF 的连接（服务器已关闭）不可复用，移出池并关闭 */
static void pool_release_conn(RkGateway *g, RHttpConn *conn) {
    pthread_mutex_lock(&g->pool_mutex);
    for (size_t i = 0; i < g->pool_count; i++) {
        if (g->pool[i].conn == conn) {
            if (rhttp_eof(conn)) {
                /* 死连接：移出池并关闭，防止被复用/清理时二次释放 */
                if (i != g->pool_count - 1)
                    g->pool[i] = g->pool[g->pool_count - 1]; /* 尾元素覆盖(防自赋值 memcpy 重叠) */
                g->pool_count--;
                pthread_mutex_unlock(&g->pool_mutex);
                rhttp_close(conn);
            } else {
                g->pool[i].in_use = 0;
                pthread_mutex_unlock(&g->pool_mutex);
            }
            return;
        }
    }
    pthread_mutex_unlock(&g->pool_mutex);
    /* 不在池中（池满时新建的连接），直接释放 */
    rhttp_close(conn);
}

/* 丢弃连接：从池中移除条目（防清理二次释放/防复用）并关闭。
 * 用于 send/read 失败路径——此时连接不可信。 */
static void pool_discard(RkGateway *g, RHttpConn *conn) {
    pthread_mutex_lock(&g->pool_mutex);
    for (size_t i = 0; i < g->pool_count; i++) {
        if (g->pool[i].conn == conn) {
            if (i != g->pool_count - 1)
                g->pool[i] = g->pool[g->pool_count - 1]; /* 防自赋值 memcpy 重叠 */
            g->pool_count--;
            break;
        }
    }
    pthread_mutex_unlock(&g->pool_mutex);
    rhttp_close(conn);
}

/* 前向声明 */
static void handle_request(RkGateway *g, int client_fd);

/* 线程包装 */
typedef struct {
    RkGateway *g;
    int client_fd;
} HandleCtx;

static void *handle_request_thread(void *arg) {
    HandleCtx *ctx = (HandleCtx *)arg;
    RkGateway *g = ctx->g;
    /* 活跃计数：run 的退出路径等待其归零后才清理连接池 */
    pthread_mutex_lock(&g->life_mutex);
    g->active_handlers++;
    pthread_mutex_unlock(&g->life_mutex);
    handle_request(g, ctx->client_fd);
    pthread_mutex_lock(&g->life_mutex);
    g->active_handlers--;
    pthread_cond_broadcast(&g->life_cond);
    pthread_mutex_unlock(&g->life_mutex);
    free(ctx);
    return NULL;
}

/* 处理 HTTP 请求 */
static void handle_request(RkGateway *g, int client_fd) {
    char req[65536];
    size_t req_len = 0;
    char *hdr_end = NULL;
    /* 慢客户端保护：读头超时 5s（SO_RCVTIMEO） */
    struct timeval tv;
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    /* 循环读直到头完整（\r\n\r\n）或超时/断开 */
    while (req_len < sizeof(req) - 1) {
        ssize_t n = read(client_fd, req + req_len, sizeof(req) - req_len - 1);
        if (n <= 0) { close(client_fd); return; } /* EOF/超时/错误 */
        req_len += (size_t)n;
        req[req_len] = '\0';
        hdr_end = strstr(req, "\r\n\r\n");
        if (hdr_end) break;
    }
    if (!hdr_end) { close(client_fd); return; } /* 头不完整 */
    /* 如果有 Content-Length，继续读 body 直到完整 */
    char *cl = strstr(req, "Content-Length:");
    if (cl) {
        long content_len = atol(cl + 15);
        size_t hdr_total = (size_t)(hdr_end - req) + 4;
        while (req_len < hdr_total + (size_t)content_len && req_len < sizeof(req) - 1) {
            ssize_t r = read(client_fd, req + req_len, sizeof(req) - req_len - 1);
            if (r <= 0) break;
            req_len += (size_t)r;
        }
        req[req_len] = '\0';
    }
    /* 解析 HTTP 请求行 */
    char method[16], path[256];
    if (sscanf(req, "%15s %255s", method, path) != 2) {
        close(client_fd);
        return;
    }
    /* 路由：POST /chat */
    if (strcmp(method, "POST") == 0 && strcmp(path, "/chat") == 0) {
        /* 找 body（\r\n\r\n 后） */
        char *body = strstr(req, "\r\n\r\n");
        if (!body) { close(client_fd); return; }
        body += 4;
        /* 解析请求 JSON {"model": "...", "messages": [...]} */
        Arena *a = arena_create(0);
        size_t err = 0;
        RJson *v = rjson_parse(a, body, strlen(body), &err);
        if (!v) {
            const char *resp = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n";
            ssize_t _w = write(client_fd, resp, strlen(resp)); (void)_w;
            arena_destroy(a);
            close(client_fd);
            return;
        }
        const RJson *model = rjson_obj_get(v, "model");
        if (!model || model->type != RJSON_STRING) {
            const char *resp = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n";
            ssize_t _w = write(client_fd, resp, strlen(resp)); (void)_w;
            arena_destroy(a);
            close(client_fd);
            return;
        }
        /* 选择 provider（简化：根据 model 名称匹配） */
        RkProviderConfig *provider = NULL;
        for (size_t i = 0; i < g->provider_count; i++) {
            if (strstr(model->u.str.ptr, g->providers[i].name) != NULL) {
                provider = &g->providers[i];
                break;
            }
        }
        if (!provider && g->provider_count > 0) {
            provider = &g->providers[0]; /* 默认第一个 */
        }
        if (!provider) {
            const char *resp = "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\n\r\n";
            ssize_t _w = write(client_fd, resp, strlen(resp)); (void)_w;
            arena_destroy(a);
            close(client_fd);
            return;
        }
        /* 代理请求到 provider（简化：直接转发 body） */
        /* 解析 base_url（必须带 scheme；解析失败视为配置错误 → 502） */
        char host[256], path_prefix[256];
        uint16_t port;
        int tls;
        if (rhttp_parse_url(provider->base_url, host, sizeof(host), &port, &tls,
                            path_prefix, sizeof(path_prefix)) != 0) {
            const char *resp = "HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\n\r\n";
            ssize_t _w = write(client_fd, resp, strlen(resp)); (void)_w;
            arena_destroy(a);
            close(client_fd);
            return;
        }
        /* rhttp_parse_url 无路径时给 "/"；网关语义为无前缀 */
        if (strcmp(path_prefix, "/") == 0) path_prefix[0] = '\0';
        /* 连接 provider（连接池） */
        RHttpConn *conn = pool_get_conn(g, host, port, tls);
        if (!conn) {
            const char *resp = "HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\n\r\n";
            ssize_t _w = write(client_fd, resp, strlen(resp)); (void)_w;
            arena_destroy(a);
            close(client_fd);
            return;
        }
        char auth[512];
        snprintf(auth, sizeof(auth), "Bearer %s", provider->api_key);
        const char *headers[] = {
            "Authorization", auth,
            "Content-Type", "application/json",
            NULL
        };
        char full_path[512];
        snprintf(full_path, sizeof(full_path), "%s/chat/completions", path_prefix);
        if (rhttp_send(conn, "POST", full_path, headers, body, strlen(body)) != 0) {
            pool_discard(g, conn);
            const char *resp = "HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\n\r\n";
            ssize_t _w = write(client_fd, resp, strlen(resp)); (void)_w;
            arena_destroy(a);
            close(client_fd);
            return;
        }
        RHttpResp resp;
        if (rhttp_read_headers(conn, &resp, 30000) != 0) {
            pool_discard(g, conn);
            const char *resp_str = "HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\n\r\n";
            ssize_t _w = write(client_fd, resp_str, strlen(resp_str)); (void)_w;
            arena_destroy(a);
            close(client_fd);
            return;
        }
        if (getenv("GW_DEBUG"))
            fprintf(stderr, "[gw] provider status=%d reason=%.32s\n", resp.status, resp.reason);
        /* 转发响应 */
        char resp_header[512];
        int hn = snprintf(resp_header, sizeof(resp_header),
                          "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\n\r\n",
                          resp.status, resp.reason);
        ssize_t _w = write(client_fd, resp_header, (size_t)hn); (void)_w;
        /* 转发 body */
        char buf[16384];
        int client_gone = 0;
        for (;;) {
            ssize_t r = rhttp_read_body(conn, buf, sizeof(buf), 30000);
            if (r <= 0) break;
            if (write(client_fd, buf, (size_t)r) < 0) { client_gone = 1; break; }
        }
        /* 客户端提前断开时响应体未读完，连接带残留数据不可复用 → 作废 */
        if (client_gone) pool_discard(g, conn);
        else pool_release_conn(g, conn);
        arena_destroy(a);
    } else {
        const char *resp = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
        ssize_t _w = write(client_fd, resp, strlen(resp)); (void)_w;
    }
    close(client_fd);
}

/* 单 worker epoll 事件循环：阻塞直到 running=0。
 * fd 生命周期：worker 0 的主监听 fd 由 gateway_cleanup 关闭；
 * 额外 fd 由调用方（gateway_worker_thread）关闭——此处不关，防双重 close。 */
static int loop_run(RkGateway *g, int fd) {
    if (!g || fd < 0) return -1;
    int epfd = epoll_create1(0);
    if (epfd < 0) return -1;
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = fd;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) != 0) {
        close(epfd);
        return -1;
    }
    struct epoll_event events[64];
    while (__atomic_load_n(&g->running, __ATOMIC_RELAXED)) {
        int n = epoll_wait(epfd, events, 64, 1000);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        for (int i = 0; i < n; i++) {
            if (events[i].data.fd == fd) {
                /* 新连接 */
                struct sockaddr_in client_addr;
                socklen_t addr_len = sizeof(client_addr);
                int client_fd = accept(fd, (struct sockaddr *)&client_addr, &addr_len);
                if (client_fd >= 0) {
                    HandleCtx *ctx = (HandleCtx *)malloc(sizeof(HandleCtx));
                    if (ctx) {
                        ctx->g = g;
                        ctx->client_fd = client_fd;
                        pthread_t tid;
                        if (pthread_create(&tid, NULL, handle_request_thread, ctx) == 0) {
                            pthread_detach(tid);
                        } else {
                            free(ctx);
                            close(client_fd);
                        }
                    } else {
                        close(client_fd);
                    }
                }
            }
        }
    }
    close(epfd);
    return 0;
}

/* 共享退出路径：等在途请求结束 → 清理连接池/生命周期对象 → 关闭主监听 fd。
 * 所有 worker 循环退出后由 run/run_multi 调用一次。 */
static void gateway_cleanup(RkGateway *g) {
    pthread_mutex_lock(&g->life_mutex);
    while (g->active_handlers > 0)
        pthread_cond_wait(&g->life_cond, &g->life_mutex);
    pthread_mutex_unlock(&g->life_mutex);
    /* 清理连接池（此刻无 handler 使用） */
    pthread_mutex_lock(&g->pool_mutex);
    for (size_t i = 0; i < g->pool_count; i++) {
        if (g->pool[i].conn) rhttp_close(g->pool[i].conn);
    }
    g->pool_count = 0;
    pthread_mutex_unlock(&g->pool_mutex);
    pthread_mutex_destroy(&g->pool_mutex);
    pthread_mutex_destroy(&g->life_mutex);
    pthread_cond_destroy(&g->life_cond);
    /* 监听 fd 由事件循环线程独占关闭（stop 不触碰，无跨线程 fd 竞争） */
    close(g->fd);
    g->fd = -1;
}

int rk_gateway_run(RkGateway *g) {
    if (!g || g->fd < 0) return -1;
    int rc = loop_run(g, g->fd);
    gateway_cleanup(g);
    return rc;
}

/* 多实例 worker 包装 */
typedef struct {
    RkGateway *g;
    int fd;
} WorkerCtx;

static void *gateway_worker_thread(void *arg) {
    WorkerCtx *w = (WorkerCtx *)arg;
    loop_run(w->g, w->fd);
    /* 额外监听 fd 由 worker 自行关闭；主 fd（worker 0）归 cleanup */
    if (w->fd != w->g->fd) close(w->fd);
    free(w);
    return NULL;
}

int rk_gateway_run_multi(RkGateway *g, int n) {
    if (!g || g->fd < 0 || n < 1) return -1;
    if (n > 64) n = 64;
    pthread_t tids[64];
    int created = 0;
    for (int i = 0; i < n; i++) {
        int fd;
        if (i == 0) {
            fd = g->fd; /* worker 0 持有主监听 fd */
        } else {
            fd = rk_gateway_listen_extra(g->port);
            if (fd < 0) break; /* 额外监听失败：以已创建的 worker 继续 */
        }
        WorkerCtx *w = (WorkerCtx *)malloc(sizeof(WorkerCtx));
        if (!w) {
            if (i > 0) close(fd);
            break;
        }
        w->g = g;
        w->fd = fd;
        if (pthread_create(&tids[created], NULL, gateway_worker_thread, w) != 0) {
            free(w);
            if (i > 0) close(fd);
            break;
        }
        created++;
    }
    for (int i = 0; i < created; i++) pthread_join(tids[i], NULL);
    gateway_cleanup(g);
    return 0;
}

void rk_gateway_stop(RkGateway *g) {
    if (!g) return;
    /* 仅置位停止标志。事件循环 epoll 超时 ≤1s 后自行退出并清理。
     * 不再 close(fd)/销毁 mutex：跨线程 fd 操作与在途 handler 使用
     * 会构成数据竞争（TSan 曾报 rk_gateway_stop vs rk_gateway_run）。 */
    __atomic_store_n(&g->running, 0, __ATOMIC_RELAXED);
}
