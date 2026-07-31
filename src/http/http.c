#define _POSIX_C_SOURCE 200809L
#include "rikka/http/http.h"
#include "rikka/core/buffer.h"
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <strings.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <openssl/ssl.h>
#include <openssl/err.h>

/* ---------- 全局 TLS 上下文 ---------- */

static pthread_once_t g_ssl_once = PTHREAD_ONCE_INIT;
static SSL_CTX *g_ssl_ctx = NULL;

static void ssl_init_once(void) {
    SSL_library_init();
    SSL_load_error_strings();
    g_ssl_ctx = SSL_CTX_new(TLS_client_method());
    if (g_ssl_ctx) {
        SSL_CTX_set_default_verify_paths(g_ssl_ctx);
        SSL_CTX_set_verify(g_ssl_ctx, SSL_VERIFY_PEER, NULL);
        SSL_CTX_set_min_proto_version(g_ssl_ctx, TLS1_2_VERSION);
    }
}

static SSL_CTX *ssl_ctx(void) {
    pthread_once(&g_ssl_once, ssl_init_once);
    return g_ssl_ctx;
}

/* ---------- 连接 ---------- */

struct RHttpConn {
    int fd;
    SSL *ssl;
    int tls;
    char host[256];
    int eof;
    RHttpResp resp;
    /* 读缓冲：头/body 大块读，调用方小量取（避免逐字节 syscall） */
    uint8_t rbuf[16384];
    size_t rbuf_len, rbuf_off;
    /* chunked 解码状态 */
    int chunked;
    int chunk_active;
    long chunk_remain;
    int chunk_done;
    long body_read;    /* content-length 已读 */
};

static int set_nonblock(int fd, int on) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0) return -1;
    return fcntl(fd, F_SETFL, on ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK));
}

static int wait_fd(int fd, short events, int timeout_ms) {
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = events;
    pfd.revents = 0;
    int r;
    do {
        r = poll(&pfd, 1, timeout_ms);
    } while (r < 0 && errno == EINTR);
    if (r <= 0) return -1;
    return 0;
}

RHttpConn *rhttp_connect(const char *host, uint16_t port, int use_tls, int timeout_ms) {
    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%u", (unsigned)port);

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res) return NULL;

    int fd = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        set_nonblock(fd, 1);
        int rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (rc != 0 && errno != EINPROGRESS) { close(fd); fd = -1; continue; }
        if (rc != 0) {
            if (wait_fd(fd, POLLOUT, timeout_ms) != 0) { close(fd); fd = -1; continue; }
            int err = 0;
            socklen_t el = sizeof(err);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) != 0 || err != 0) {
                close(fd); fd = -1; continue;
            }
        }
        break;
    }
    freeaddrinfo(res);
    if (fd < 0) return NULL;
    set_nonblock(fd, 0);

    RHttpConn *c = (RHttpConn *)calloc(1, sizeof(RHttpConn));
    if (!c) { close(fd); return NULL; }
    c->fd = fd;
    c->tls = use_tls;
    snprintf(c->host, sizeof(c->host), "%s", host);
    c->resp.content_length = -1;

    if (use_tls) {
        SSL_CTX *ctx = ssl_ctx();
        if (!ctx) { close(fd); free(c); return NULL; }
        c->ssl = SSL_new(ctx);
        if (!c->ssl) { close(fd); free(c); return NULL; }
        SSL_set_fd(c->ssl, fd);
        SSL_set_tlsext_host_name(c->ssl, host);
        SSL_set_connect_state(c->ssl);
        /* TLS 握手（阻塞；SO_RCVTIMEO 由调用方默认 30s——此处直接依赖内核） */
        for (;;) {
            int rc = SSL_connect(c->ssl);
            if (rc == 1) break;
            int err = SSL_get_error(c->ssl, rc);
            if (err == SSL_ERROR_WANT_READ) {
                if (wait_fd(fd, POLLIN, timeout_ms) != 0) { rhttp_close(c); return NULL; }
                continue;
            }
            if (err == SSL_ERROR_WANT_WRITE) {
                if (wait_fd(fd, POLLOUT, timeout_ms) != 0) { rhttp_close(c); return NULL; }
                continue;
            }
            rhttp_close(c);
            return NULL;
        }
    }
    return c;
}

void rhttp_close(RHttpConn *c) {
    if (!c) return;
    if (c->ssl) { SSL_shutdown(c->ssl); SSL_free(c->ssl); }
    if (c->fd >= 0) close(c->fd);
    free(c);
}

/* ---------- 连接池（B1：短请求 keep-alive 复用） ---------- */

typedef struct RkPoolConn {
    char host[256];
    uint16_t port;
    int tls;
    RHttpConn *conn;
    struct RkPoolConn *next;
} RkPoolConn;

static RkPoolConn *g_pool = NULL;
static pthread_mutex_t g_pool_mutex = PTHREAD_MUTEX_INITIALIZER;

static RHttpConn *pool_take(const char *host, uint16_t port, int tls) {
    pthread_mutex_lock(&g_pool_mutex);
    RkPoolConn **pp = &g_pool;
    while (*pp) {
        if ((*pp)->port == port && (*pp)->tls == tls &&
            strcmp((*pp)->host, host) == 0) {
            RkPoolConn *e = *pp;
            *pp = e->next;
            RHttpConn *c = e->conn;
            free(e);
            pthread_mutex_unlock(&g_pool_mutex);
            return c;
        }
        pp = &(*pp)->next;
    }
    pthread_mutex_unlock(&g_pool_mutex);
    return NULL;
}

static void pool_put(RHttpConn *c, const char *host, uint16_t port, int tls) {
    if (!c) return;
    if (!c->resp.keep_alive || c->eof || c->rbuf_len > c->rbuf_off) {
        rhttp_close(c);
        return;
    }
    RkPoolConn *e = (RkPoolConn *)malloc(sizeof(RkPoolConn));
    if (!e) { rhttp_close(c); return; }
    snprintf(e->host, sizeof(e->host), "%s", host);
    e->port = port;
    e->tls = tls;
    e->conn = c;
    pthread_mutex_lock(&g_pool_mutex);
    e->next = g_pool;
    g_pool = e;
    /* 池上限：超过 32 条淘汰链尾（最旧） */
    {
        int n = 0;
        RkPoolConn *prev = NULL, *last = g_pool;
        for (RkPoolConn *it = g_pool; it; it = it->next) {
            n++;
            if (!it->next) break;
            prev = it;
            last = it->next;
        }
        if (n > 32 && prev) {
            prev->next = NULL;
            rhttp_close(last->conn);
            free(last);
        }
    }
    pthread_mutex_unlock(&g_pool_mutex);
}

/* ---------- 读写原语 ---------- */

/* 底层填充：poll + 读满内部缓冲 */
static ssize_t fill_rbuf(RHttpConn *c, int timeout_ms) {
    if (wait_fd(c->fd, POLLIN, timeout_ms) != 0) return -1;
    for (;;) {
        ssize_t n;
        if (c->ssl) {
            int rc = SSL_read(c->ssl, c->rbuf, sizeof(c->rbuf));
            if (rc <= 0) {
                int err = SSL_get_error(c->ssl, rc);
                if (err == SSL_ERROR_WANT_READ) {
                    if (wait_fd(c->fd, POLLIN, timeout_ms) != 0) return -1;
                    continue;
                }
                return 0; /* EOF 或错误按 EOF */
            }
            n = rc;
        } else {
            n = read(c->fd, c->rbuf, sizeof(c->rbuf));
            if (n < 0) return (errno == EAGAIN || errno == EWOULDBLOCK) ? -1 : 0;
        }
        c->rbuf_len = (size_t)n;
        c->rbuf_off = 0;
        return n;
    }
}

/* 读 cap 字节（尽量）：先取内部缓冲，不足则填充 */
static ssize_t raw_read(RHttpConn *c, char *buf, size_t cap, int timeout_ms) {
    if (c->rbuf_len > c->rbuf_off) {
        size_t avail = c->rbuf_len - c->rbuf_off;
        size_t n = avail < cap ? avail : cap;
        memcpy(buf, c->rbuf + c->rbuf_off, n);
        c->rbuf_off += n;
        if (c->rbuf_off == c->rbuf_len) c->rbuf_len = c->rbuf_off = 0;
        return (ssize_t)n;
    }
    ssize_t n = fill_rbuf(c, timeout_ms);
    if (n <= 0) return (ssize_t)n; /* -1 超时 / 0 EOF */
    size_t take = (size_t)n < cap ? (size_t)n : cap;
    memcpy(buf, c->rbuf, take);
    c->rbuf_off = take;
    return (ssize_t)take;
}

static int raw_write_all(RHttpConn *c, const char *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        if (wait_fd(c->fd, POLLOUT, 30000) != 0) return -1;
        int n;
        if (c->ssl) {
            n = SSL_write(c->ssl, buf + off, (int)(len - off));
            if (n <= 0) {
                int err = SSL_get_error(c->ssl, n);
                if (err == SSL_ERROR_WANT_WRITE) continue;
                return -1;
            }
        } else {
            n = (int)write(c->fd, buf + off, len - off);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
                return -1;
            }
        }
        off += (size_t)n;
    }
    return 0;
}

/* 读单字节（chunk 行用）。返回 0 EOF / 1 有字节 / -1 超时 */
static int read_byte(RHttpConn *c, char *out, int timeout_ms) {
    ssize_t n = raw_read(c, out, 1, timeout_ms);
    if (n < 0) return -1;
    if (n == 0) return 0;
    return 1;
}

/* ---------- 请求/响应 ---------- */

int rhttp_send(RHttpConn *c, const char *method, const char *path,
               const char *const *headers, const char *body, size_t body_len) {
    Buf req;
    buf_init(&req);
    buf_append_str(&req, method);
    buf_append(&req, " ", 1);
    buf_append_str(&req, path);
    buf_append_str(&req, " HTTP/1.1\r\nHost: ");
    buf_append_str(&req, c->host);
    buf_append_str(&req, "\r\nUser-Agent: rikkahub-engine/0.1\r\nAccept: */*\r\n");
    int has_cl = 0, has_conn = 0;
    if (headers) {
        for (const char *const *h = headers; h[0] && h[1]; h += 2) {
            buf_append_str(&req, h[0]);
            buf_append(&req, ": ", 2);
            buf_append_str(&req, h[1]);
            buf_append_str(&req, "\r\n");
            if (strcasecmp(h[0], "Content-Length") == 0) has_cl = 1;
            if (strcasecmp(h[0], "Connection") == 0) has_conn = 1;
        }
    }
    if (!has_cl) {
        char tmp[32];
        int n = snprintf(tmp, sizeof(tmp), "Content-Length: %zu\r\n", body_len);
        buf_append(&req, tmp, (size_t)n);
    }
    if (!has_conn) buf_append_str(&req, "Connection: keep-alive\r\n");
    buf_append_str(&req, "\r\n");
    if (body && body_len > 0) buf_append(&req, body, body_len);

    int rc = raw_write_all(c, (const char *)req.data, req.len);
    buf_free(&req);
    return rc;
}

/* 逐字节读响应头直到空行 */
static int read_headers_raw(RHttpConn *c, Buf *hdr, int timeout_ms) {
    buf_reset(hdr);
    int found = 0;
    for (;;) {
        char ch;
        int r = read_byte(c, &ch, timeout_ms);
        if (r <= 0) return -1;
        buf_append_byte(hdr, (uint8_t)ch);
        /* 检测 \r\n\r\n 或 \n\n */
        size_t n = hdr->len;
        if (n >= 4 && hdr->data[n-1] == '\n' && hdr->data[n-2] == '\r' &&
            hdr->data[n-3] == '\n' && hdr->data[n-4] == '\r') { found = 1; break; }
        if (n >= 2 && hdr->data[n-1] == '\n' && hdr->data[n-2] == '\n') { found = 1; break; }
        if (n > 65536) return -1;
    }
    return found ? 0 : -1;
}

static long parse_content_length(const char *v) {
    long n = 0;
    for (; *v; v++) {
        if (*v < '0' || *v > '9') continue;
        n = n * 10 + (*v - '0');
    }
    return n;
}

int rhttp_read_headers(RHttpConn *c, RHttpResp *resp, int timeout_ms) {
    Buf hdr;
    buf_init(&hdr);
    if (read_headers_raw(c, &hdr, timeout_ms) != 0) {
        buf_free(&hdr);
        return -1;
    }
    /* 状态行 */
    char *text = (char *)hdr.data;
    size_t len = hdr.len;
    int status = 0;
    int http11 = 0; /* HTTP/1.1 默认持久连接（无 Connection 头时） */
    char reason[128] = {0};
    /* HTTP/1.1 200 OK\r\n... */
    const char *p = text;
    const char *end = text + len;
    if (len >= 8 && memcmp(p, "HTTP/1.", 7) == 0) {
        http11 = (p[7] == '1');
        p += 8; /* 跳过 "HTTP/1.x" */
        while (p < end && *p == ' ') p++;
        while (p < end && *p >= '0' && *p <= '9') { status = status * 10 + (*p - '0'); p++; }
        if (p < end && *p == ' ') p++;
        size_t rn = 0;
        while (p < end && *p != '\r' && *p != '\n' && rn < sizeof(reason) - 1) {
            reason[rn++] = *p++;
        }
        reason[rn] = '\0';
    }
    /* 头字段 */
    memset(resp, 0, sizeof(RHttpResp));
    resp->status = status;
    snprintf(resp->reason, sizeof(resp->reason), "%s", reason);
    resp->content_length = -1;
    resp->tls = c->tls;
    resp->keep_alive = http11; /* HTTP/1.1 默认 keep-alive */
    /* 逐行解析 */
    const char *line = text;
    while (line < end) {
        const char *nl = memchr(line, '\n', (size_t)(end - line));
        if (!nl) break;
        const char *line_end = nl;
        if (line_end > line && line_end[-1] == '\r') line_end--;
        const char *colon = memchr(line, ':', (size_t)(line_end - line));
        if (colon) {
            const char *name = line;
            size_t name_len = (size_t)(colon - line);
            const char *val = colon + 1;
            while (val < line_end && *val == ' ') val++;
            size_t val_len = (size_t)(line_end - val);
            if (name_len == 14 && strncasecmp(name, "Content-Length", 14) == 0) {
                char tmp[64];
                size_t n = val_len < sizeof(tmp) - 1 ? val_len : sizeof(tmp) - 1;
                memcpy(tmp, val, n);
                tmp[n] = '\0';
                resp->content_length = parse_content_length(tmp);
            } else if (name_len == 17 && strncasecmp(name, "Transfer-Encoding", 17) == 0) {
                if (val_len >= 7 && strncasecmp(val, "chunked", 7) == 0) resp->chunked = 1;
            } else if (name_len == 10 && strncasecmp(name, "Connection", 10) == 0) {
                if (val_len >= 5 && strncasecmp(val, "close", 5) == 0) resp->keep_alive = 0;
                else resp->keep_alive = 1;
            }
        }
        line = nl + 1;
    }
    buf_free(&hdr);
    /* 初始化解码状态 */
    c->chunked = resp->chunked;
    c->chunk_active = 0;
    c->chunk_remain = 0;
    c->chunk_done = 0;
    c->body_read = 0;
    c->resp = *resp;
    return 0;
}

/* chunk 大小行读取（到 \n 结束） */
static int read_chunk_line(RHttpConn *c, char *out, size_t cap, int timeout_ms) {
    size_t n = 0;
    for (;;) {
        char ch;
        int r = read_byte(c, &ch, timeout_ms);
        if (r <= 0) return -1;
        if (ch == '\n') break;
        if (n < cap - 1) out[n++] = ch;
    }
    out[n] = '\0';
    return (int)n;
}

ssize_t rhttp_read_body(RHttpConn *c, char *buf, size_t cap, int timeout_ms) {
    if (c->eof) return 0;
    if (c->chunked) {
        for (;;) {
            if (c->chunk_done) { c->eof = 1; return 0; }
            if (!c->chunk_active) {
                char line[64];
                if (read_chunk_line(c, line, sizeof(line), timeout_ms) < 0) return -1;
                long size = 0;
                const char *p = line;
                while (*p) {
                    char hc = *p;
                    int hv;
                    if (hc >= '0' && hc <= '9') hv = hc - '0';
                    else if (hc >= 'a' && hc <= 'f') hv = hc - 'a' + 10;
                    else if (hc >= 'A' && hc <= 'F') hv = hc - 'A' + 10;
                    else break;
                    size = size * 16 + hv;
                    p++;
                }
                if (size == 0) {
                    /* 尾头：读空行 */
                    for (;;) {
                        char t[256];
                        int n = read_chunk_line(c, t, sizeof(t), timeout_ms);
                        if (n < 0) return -1;
                        /* 空行（可能含 \r）即尾头结束 */
                        if (n == 0 || (n == 1 && t[0] == '\r')) break;
                    }
                    c->chunk_done = 1;
                    c->eof = 1;
                    return 0;
                }
                c->chunk_active = 1;
                c->chunk_remain = size;
            }
            size_t want = cap;
            if ((long)want > c->chunk_remain) want = (size_t)c->chunk_remain;
            ssize_t n = raw_read(c, buf, want, timeout_ms);
            if (n < 0) return -1;
            if (n == 0) { c->eof = 1; return 0; } /* 服务器提前关闭 */
            c->chunk_remain -= n;
            if (c->chunk_remain == 0) {
                /* 读 chunk 尾 CRLF */
                char crlf[2];
                if (read_byte(c, &crlf[0], timeout_ms) < 0) return -1;
                if (read_byte(c, &crlf[1], timeout_ms) < 0) return -1;
                c->chunk_active = 0;
            }
            return n;
        }
    }
    if (c->resp.content_length >= 0) {
        if (c->body_read >= c->resp.content_length) { c->eof = 1; return 0; }
        size_t want = cap;
        long remain = c->resp.content_length - c->body_read;
        if ((long)want > remain) want = (size_t)remain;
        ssize_t n = raw_read(c, buf, want, timeout_ms);
        if (n < 0) return -1;
        if (n == 0) { c->eof = 1; return 0; }
        c->body_read += n;
        return n;
    }
    /* 无长度：流式读到 EOF */
    ssize_t n = raw_read(c, buf, cap, timeout_ms);
    if (n < 0) return -1;
    if (n == 0) c->eof = 1;
    return n;
}

int rhttp_eof(RHttpConn *c) { return c->eof; }

/* ---------- 同步请求 ---------- */

static int parse_url(const char *url, char *host, size_t host_cap, uint16_t *port,
                     int *tls, char *path, size_t path_cap) {
    const char *p = url;
    *tls = 0;
    if (strncmp(p, "https://", 8) == 0) { *tls = 1; p += 8; }
    else if (strncmp(p, "http://", 7) == 0) { p += 7; }
    else return -1;
    const char *host_end = p;
    while (*host_end && *host_end != ':' && *host_end != '/') host_end++;
    size_t hn = (size_t)(host_end - p);
    if (hn >= host_cap) return -1;
    memcpy(host, p, hn);
    host[hn] = '\0';
    *port = *tls ? 443 : 80;
    if (*host_end == ':') {
        const char *ps = host_end + 1;
        long po = 0;
        while (*ps >= '0' && *ps <= '9') po = po * 10 + (*ps - '0'), ps++;
        if (po > 0 && po < 65536) *port = (uint16_t)po;
        host_end = ps;
    }
    if (*host_end == '/') {
        size_t pn = strlen(host_end);
        if (pn >= path_cap) return -1;
        memcpy(path, host_end, pn + 1);
    } else {
        snprintf(path, path_cap, "/");
    }
    return 0;
}

/* 单次同步请求（连接可复用/新建）；返回 0 成功 */
static int sync_once(RHttpConn *c, const char *path, const char *const *headers,
                     const char *body, size_t body_len, int timeout_ms,
                     RHttpResp *resp, Buf *out) {
    if (rhttp_send(c, "POST", path, headers, body, body_len) != 0) return -1;
    if (rhttp_read_headers(c, resp, timeout_ms) != 0) return -1;
    buf_reset(out);
    char tmp[16384];
    for (;;) {
        ssize_t n = rhttp_read_body(c, tmp, sizeof(tmp), timeout_ms);
        if (n < 0) break;
        if (n == 0) break;
        buf_append(out, tmp, (size_t)n);
    }
    return 0;
}

char *rhttp_request_sync(const char *url, const char *const *headers,
                         const char *body, size_t body_len,
                         int timeout_ms, int *status, size_t *out_len) {
    char host[256], path[2048];
    uint16_t port;
    int tls;
    if (parse_url(url, host, sizeof(host), &port, &tls, path, sizeof(path)) != 0) return NULL;

    RHttpResp resp;
    Buf out;
    buf_init(&out);
    int ok = 0;
    /* 尝试池化连接；失败（失效/新连接失败）则重建重试一次 */
    for (int attempt = 0; attempt < 2 && !ok; attempt++) {
        RHttpConn *c = attempt == 0 ? pool_take(host, port, tls) : NULL;
        int fresh = 0;
        if (!c) {
            c = rhttp_connect(host, port, tls, timeout_ms);
            fresh = 1;
            if (!c) break;
        }
        if (sync_once(c, path, headers, body, body_len, timeout_ms, &resp, &out) == 0) {
            ok = 1;
            pool_put(c, host, port, tls);
        } else {
            rhttp_close(c);
            if (!fresh && attempt == 0) {
                /* 池化连接失效：丢弃重试（下一轮新建） */
                continue;
            }
            break;
        }
    }
    if (!ok) {
        buf_free(&out);
        return NULL;
    }
    if (status) *status = resp.status;
    if (out_len) *out_len = out.len;
    if (!out.data) {
        out.data = (uint8_t *)malloc(1);
        if (out.data) out.data[0] = '\0';
    }
    return (char *)out.data;
}
