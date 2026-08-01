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
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>

int rk_gateway_init(RkGateway *g, int port) {
    if (!g) return -1;
    memset(g, 0, sizeof(RkGateway));
    g->port = port;
    g->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g->fd < 0) return -1;
    int opt = 1;
    setsockopt(g->fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
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
    return 0;
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

/* 释放连接回池 */
static void pool_release_conn(RkGateway *g, RHttpConn *conn) {
    pthread_mutex_lock(&g->pool_mutex);
    for (size_t i = 0; i < g->pool_count; i++) {
        if (g->pool[i].conn == conn) {
            g->pool[i].in_use = 0;
            pthread_mutex_unlock(&g->pool_mutex);
            return;
        }
    }
    pthread_mutex_unlock(&g->pool_mutex);
    /* 不在池中（池满时新建的连接），直接释放 */
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
    handle_request(ctx->g, ctx->client_fd);
    free(ctx);
    return NULL;
}

/* 处理 HTTP 请求 */
static void handle_request(RkGateway *g, int client_fd) {
    char req[65536];
    size_t req_len = 0;
    /* 读取请求头 */
    ssize_t n = read(client_fd, req + req_len, sizeof(req) - req_len - 1);
    if (n <= 0) { close(client_fd); return; }
    req_len += (size_t)n;
    req[req_len] = '\0';
    /* 如果有 Content-Length，继续读 body 直到完整 */
    char *hdr_end = strstr(req, "\r\n\r\n");
    if (hdr_end) {
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
        /* 解析 base_url */
        char host[256], path_prefix[256];
        int tls = 0;
        int port = 80;
        const char *url = provider->base_url;
        if (strncmp(url, "https://", 8) == 0) { tls = 1; port = 443; url += 8; }
        else if (strncmp(url, "http://", 7) == 0) { url += 7; }
        sscanf(url, "%255[^/]%255s", host, path_prefix);
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
            rhttp_close(conn);
            const char *resp = "HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\n\r\n";
            ssize_t _w = write(client_fd, resp, strlen(resp)); (void)_w;
            arena_destroy(a);
            close(client_fd);
            return;
        }
        RHttpResp resp;
        if (rhttp_read_headers(conn, &resp, 30000) != 0) {
            rhttp_close(conn);
            const char *resp_str = "HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\n\r\n";
            ssize_t _w = write(client_fd, resp_str, strlen(resp_str)); (void)_w;
            arena_destroy(a);
            close(client_fd);
            return;
        }
        /* 转发响应 */
        char resp_header[512];
        int hn = snprintf(resp_header, sizeof(resp_header),
                          "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\n\r\n",
                          resp.status, resp.reason);
        ssize_t _w = write(client_fd, resp_header, (size_t)hn); (void)_w;
        /* 转发 body */
        char buf[16384];
        for (;;) {
            ssize_t r = rhttp_read_body(conn, buf, sizeof(buf), 30000);
            if (r <= 0) break;
            ssize_t _w = write(client_fd, buf, (size_t)r); (void)_w;
        }
        pool_release_conn(g, conn);
        arena_destroy(a);
    } else {
        const char *resp = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
        ssize_t _w = write(client_fd, resp, strlen(resp)); (void)_w;
    }
    close(client_fd);
}

int rk_gateway_run(RkGateway *g) {
    if (!g || g->fd < 0) return -1;
    int epfd = epoll_create1(0);
    if (epfd < 0) return -1;
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = g->fd;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, g->fd, &ev) != 0) {
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
            if (events[i].data.fd == g->fd) {
                /* 新连接 */
                struct sockaddr_in client_addr;
                socklen_t addr_len = sizeof(client_addr);
                int client_fd = accept(g->fd, (struct sockaddr *)&client_addr, &addr_len);
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

void rk_gateway_stop(RkGateway *g) {
    if (!g) return;
    __atomic_store_n(&g->running, 0, __ATOMIC_RELAXED);
    if (g->fd >= 0) {
        close(g->fd);
        g->fd = -1;
    }
    /* 清理连接池 */
    pthread_mutex_lock(&g->pool_mutex);
    for (size_t i = 0; i < g->pool_count; i++) {
        if (g->pool[i].conn) rhttp_close(g->pool[i].conn);
    }
    g->pool_count = 0;
    pthread_mutex_unlock(&g->pool_mutex);
    pthread_mutex_destroy(&g->pool_mutex);
}
