#ifndef RIKKA_HTTP_HTTP_H
#define RIKKA_HTTP_HTTP_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/*
 * 最小 HTTP/1.1 客户端（零外部依赖，仅 socket + OpenSSL）。
 * 支持：TLS、keep-alive、chunked 传输解码、流式读体（SSE 场景）。
 * 阻塞 + 超时模型：每流式请求一个连接 + 一个读线程（M6 无锁流水线再引入 epoll）。
 */

typedef struct RHttpConn RHttpConn;

typedef struct {
    int status;             /* 状态码（0 = 未读） */
    char reason[128];       /* 状态行说明 */
    int chunked;            /* Transfer-Encoding: chunked */
    long content_length;    /* Content-Length（-1 = 未知/流式） */
    int keep_alive;
    int tls;
} RHttpResp;

/* 建立连接（use_tls=1 时 TLS 握手；timeout_ms 连接超时）。失败返回 NULL */
RHttpConn *rhttp_connect(const char *host, uint16_t port, int use_tls, int timeout_ms);
void rhttp_close(RHttpConn *c);

/* 发送请求。headers 交替 key/value 以 NULL 结尾。返回 0 成功 */
int rhttp_send(RHttpConn *c, const char *method, const char *path,
               const char *const *headers, const char *body, size_t body_len);

/* 读取并解析响应头。返回 0 成功 */
int rhttp_read_headers(RHttpConn *c, RHttpResp *resp, int timeout_ms);

/*
 * 流式读体（透明解码 chunked）。返回 >0 字节数、0 = EOF、-1 = 超时/错误。
 * 非 chunked 且无 content-length 时读到连接关闭（SSE 常见）。
 */
ssize_t rhttp_read_body(RHttpConn *c, char *buf, size_t cap, int timeout_ms);

/* 连接是否已 EOF（供 SSE 循环判定） */
int rhttp_eof(RHttpConn *c);

/*
 * 获取底层 socket fd。连接仍归 RHttpConn 所有（rhttp_close 负责关闭），
 * 调用方只读（如 MCP SSE 传输需要把 fd 交给外部事件循环/poll）。
 * 失败返回 -1。
 */
int rhttp_get_fd(const RHttpConn *c);

/* 便捷：一次同步请求（非流式），返回 malloc 的 body（*out_len），失败 NULL */
char *rhttp_request_sync(const char *url, const char *const *headers,
                         const char *body, size_t body_len,
                         int timeout_ms, int *status, size_t *out_len);

/*
 * 解析 http(s):// URL 为 host/port/tls/path。支持端口（127.0.0.1:8080/x）。
 * 返回 0 成功；host/path 以 NUL 结尾写入调用方缓冲。
 */
int rhttp_parse_url(const char *url, char *host, size_t host_cap, uint16_t *port,
                    int *tls, char *path, size_t path_cap);

#endif /* RIKKA_HTTP_HTTP_H */
