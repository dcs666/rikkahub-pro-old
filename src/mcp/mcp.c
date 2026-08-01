#define _POSIX_C_SOURCE 200809L
#include "rikka/mcp/mcp.h"
#include "rikka/json/json.h"
#include "rikka/http/http.h"
#include "rikka/http/sse.h"
#include "rikka/util/arena.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <errno.h>

/* ---------- SSE 传输内部结构 ---------- */

/* 挂起请求节点（响应经 SSE 事件流异步到达） */
struct PendingMcpResp {
    int id;
    int done;            /* 1 = 已收到响应（含错误/断开） */
    int error;           /* 1 = JSON-RPC error / POST 失败 / 传输断开 */
    char *data;          /* 响应 JSON（malloc，调用方 free） */
    struct PendingMcpResp *next;
};

/* endpoint 事件收集（connect 阶段） */
typedef struct {
    char endpoint[1024];
    int got_endpoint;
} EndpointCtx;

/* SSE 读线程（定义在下方，connect_sse 先引用） */
static void *sse_read_thread(void *arg);

int rk_mcp_connect(RkMcpClient *c, const char *command, char *const *args) {
    memset(c, 0, sizeof(*c));   /* 防止结构体复用残留（is_sse 等） */
    int pipe_in[2], pipe_out[2];
    if (pipe(pipe_in) != 0) return -1;   /* 客户端写 → 服务器读 */
    if (pipe(pipe_out) != 0) { close(pipe_in[0]); close(pipe_in[1]); return -1; }
    int pid = fork();
    if (pid < 0) {
        close(pipe_in[0]); close(pipe_in[1]);
        close(pipe_out[0]); close(pipe_out[1]);
        return -1;
    }
    if (pid == 0) {
        /* 子进程：MCP 服务器 */
        close(pipe_in[1]);
        close(pipe_out[0]);
        dup2(pipe_in[0], STDIN_FILENO);
        dup2(pipe_out[1], STDOUT_FILENO);
        close(pipe_in[0]);
        close(pipe_out[1]);
        execvp(command, args);
        _exit(127);
    }
    /* 父进程 */
    close(pipe_in[0]);
    close(pipe_out[1]);
    c->fd_read = pipe_out[0];
    c->fd_write = pipe_in[1];
    c->pid = pid;
    c->next_id = 1;
    return 0;
}

/* endpoint 事件回调：收集消息端点 URL */
static void endpoint_cb(void *ctx, const char *event, const char *data, size_t data_len,
                        const char *id, long long retry_ms) {
    (void)id; (void)retry_ms;
    EndpointCtx *e = (EndpointCtx *)ctx;
    if (e->got_endpoint) return;
    if (event && strcmp(event, "endpoint") == 0) {
        size_t n = data_len < sizeof(e->endpoint) - 1 ? data_len : sizeof(e->endpoint) - 1;
        memcpy(e->endpoint, data, n);
        e->endpoint[n] = '\0';
        e->got_endpoint = 1;
    }
}

/* 解析 endpoint 值：绝对 URL 或相对路径（相对时沿用 base 的 host/port/tls） */
static int parse_endpoint(RkMcpClient *c, const char *ep,
                          const char *base_host, uint16_t base_port, int base_tls) {
    if (strncmp(ep, "http://", 7) == 0 || strncmp(ep, "https://", 8) == 0) {
        return rhttp_parse_url(ep, c->endpoint_host, sizeof(c->endpoint_host),
                               &c->endpoint_port, &c->endpoint_tls,
                               c->endpoint_path, sizeof(c->endpoint_path));
    }
    if (ep[0] != '/') return -1; /* 非法相对路径 */
    snprintf(c->endpoint_host, sizeof(c->endpoint_host), "%s", base_host);
    c->endpoint_port = base_port;
    c->endpoint_tls = base_tls;
    snprintf(c->endpoint_path, sizeof(c->endpoint_path), "%s", ep);
    return 0;
}

int rk_mcp_connect_sse(RkMcpClient *c, const char *url) {
    if (!c || !url) return -1;
    char host[256], path[512];
    uint16_t port;
    int tls;
    if (rhttp_parse_url(url, host, sizeof(host), &port, &tls, path, sizeof(path)) != 0)
        return -1;

    /* 建立 SSE 读连接（连接归读线程所有，此处先行握手） */
    RHttpConn *conn = rhttp_connect(host, port, tls, 30000);
    if (!conn) return -1;
    const char *headers[] = {
        "Accept", "text/event-stream",
        "Cache-Control", "no-cache",
        NULL
    };
    if (rhttp_send(conn, "GET", path, headers, NULL, 0) != 0) {
        rhttp_close(conn);
        return -1;
    }
    RHttpResp resp;
    if (rhttp_read_headers(conn, &resp, 30000) != 0) {
        rhttp_close(conn);
        return -1;
    }
    if (resp.status != 200) {
        rhttp_close(conn);
        return -1;
    }

    /* 等 endpoint 事件（服务器握手的第一个事件；30s 超时由 read_body 控制） */
    EndpointCtx ectx;
    memset(&ectx, 0, sizeof(ectx));
    RsseParser *p = rsse_create(endpoint_cb, &ectx);
    char buf[8192];
    while (!ectx.got_endpoint) {
        ssize_t n = rhttp_read_body(conn, buf, sizeof(buf), 30000);
        if (n < 0) break;  /* 超时 */
        if (n == 0) break; /* EOF：服务器未发 endpoint */
        if (rsse_feed(p, buf, (size_t)n) != 0) break;
    }
    rsse_finish(p);
    rsse_destroy(p);
    if (!ectx.got_endpoint) {
        rhttp_close(conn);
        return -1;
    }

    /* 初始化客户端状态 */
    memset(c, 0, sizeof(*c));
    c->fd_read = c->fd_write = -1;
    c->pid = 0;
    c->next_id = 1;
    c->is_sse = 1;
    if (parse_endpoint(c, ectx.endpoint, host, port, tls) != 0) {
        rhttp_close(conn);
        return -1;
    }
    pthread_mutex_init(&c->lock, NULL);
    pthread_cond_init(&c->cond, NULL);
    c->sse_conn = conn;
    if (pthread_create(&c->sse_thread, NULL, sse_read_thread, c) != 0) {
        rhttp_close(conn);
        pthread_mutex_destroy(&c->lock);
        pthread_cond_destroy(&c->cond);
        return -1;
    }
    return 0;
}

/* SSE message 事件回调：解析 JSON-RPC 响应并按 id 匹配挂起请求。
 * 语义与 stdio 路径 rpc_call 对齐：成功时 *result = result 成员序列化；
 * error/无 result 时 result 不设置。 */
static void sse_event_cb(void *ctx, const char *event, const char *data, size_t data_len,
                         const char *id, long long retry_ms) {
    (void)id; (void)retry_ms;
    RkMcpClient *c = (RkMcpClient *)ctx;
    if (!event || strcmp(event, "message") != 0) return;

    Arena *a = arena_create(0);
    size_t err = 0;
    RJson *v = rjson_parse(a, data, data_len, &err);
    if (!v) { arena_destroy(a); return; }
    const RJson *idj = rjson_obj_get(v, "id");
    if (!idj || idj->type != RJSON_NUMBER) { arena_destroy(a); return; }
    int rid = (int)idj->u.number;
    const RJson *errj = rjson_obj_get(v, "error");
    const RJson *resj = errj ? NULL : rjson_obj_get(v, "result");

    pthread_mutex_lock(&c->lock);
    for (PendingMcpResp *r = c->pending; r; r = r->next) {
        if (r->id == rid && !r->done) {
            r->done = 1;
            r->error = errj != NULL;
            if (resj) {
                RJsonOut out;
                rjson_out_init(&out);
                rjson_write_value(&out, resj);
                r->data = out.buf;   /* malloc，所有权转移给调用方 */
            }
            break;
        }
    }
    pthread_cond_broadcast(&c->cond);
    pthread_mutex_unlock(&c->lock);
    arena_destroy(a);
}

/* SSE 读线程：持续消费事件流直到断开/EOF/stopped */
static void *sse_read_thread(void *arg) {
    RkMcpClient *c = (RkMcpClient *)arg;
    RHttpConn *conn = c->sse_conn; /* 线程独占 */
    RsseParser *p = rsse_create(sse_event_cb, c);
    char buf[8192];
    for (;;) {
        ssize_t n = rhttp_read_body(conn, buf, sizeof(buf), 5000);
        if (n < 0) {
            /* 读超时（rhttp_read_body 仅超时返回 -1，硬错误按 EOF 返回 0）：
             * 服务器可能无心跳，继续等；仅 stopped 时退出 */
            pthread_mutex_lock(&c->lock);
            int st = c->stopped;
            pthread_mutex_unlock(&c->lock);
            if (st) break;
            continue;
        }
        if (n == 0) break; /* EOF/断开 */
        if (rsse_feed(p, buf, (size_t)n) != 0) break;
        pthread_mutex_lock(&c->lock);
        int st = c->stopped;
        pthread_mutex_unlock(&c->lock);
        if (st) break;
    }
    rsse_finish(p);
    rsse_destroy(p);
    /* 连接归线程所有：关闭并解除引用 */
    pthread_mutex_lock(&c->lock);
    if (c->sse_conn == conn) c->sse_conn = NULL;
    pthread_mutex_unlock(&c->lock);
    rhttp_close(conn);
    /* 传输断开：所有未完成请求标 error 并唤醒 */
    pthread_mutex_lock(&c->lock);
    for (PendingMcpResp *r = c->pending; r; r = r->next) {
        if (!r->done) { r->done = 1; r->error = 1; }
    }
    pthread_cond_broadcast(&c->cond);
    pthread_mutex_unlock(&c->lock);
    return NULL;
}

/* SSE 模式请求：POST 到 message endpoint，响应经事件流异步返回 */
static int rpc_call_sse(RkMcpClient *c, const char *method, const char *params_json,
                        char **result) {
    char req[32768];
    int id = c->next_id++;
    int n = snprintf(req, sizeof(req),
                     "{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"%s\"%s%s}",
                     id, method,
                     params_json ? ",\"params\":" : "",
                     params_json ? params_json : "");
    if (n < 0 || (size_t)n >= sizeof(req)) return -1;

    /* 注册挂起请求 */
    PendingMcpResp *r = (PendingMcpResp *)calloc(1, sizeof(PendingMcpResp));
    if (!r) return -1;
    r->id = id;
    pthread_mutex_lock(&c->lock);
    if (!c->sse_conn) {
        /* 传输已断开（读线程已退出）：响应不可能到达，立即失败 */
        pthread_mutex_unlock(&c->lock);
        free(r);
        return -1;
    }
    r->next = c->pending;
    c->pending = r;
    pthread_mutex_unlock(&c->lock);

    /* POST 到 message endpoint */
    char url[1024];
    snprintf(url, sizeof(url), "%s://%s:%u%s",
             c->endpoint_tls ? "https" : "http",
             c->endpoint_host, (unsigned)c->endpoint_port, c->endpoint_path);
    const char *hdrs[] = {
        "Content-Type", "application/json",
        "Accept", "application/json, text/event-stream",
        NULL
    };
    int status = 0;
    size_t out_len = 0;
    char *body = rhttp_request_sync(url, hdrs, req, (size_t)n, 30000, &status, &out_len);
    if (!body || status < 200 || status >= 300) {
        /* POST 失败：响应不会到达，直接标 error（等路径立即返回） */
        pthread_mutex_lock(&c->lock);
        if (!r->done) { r->done = 1; r->error = 1; }
        pthread_cond_broadcast(&c->cond);
        pthread_mutex_unlock(&c->lock);
    }
    free(body);

    /* 等响应（含 POST 失败/超时/断开的统一出口） */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 30;
    pthread_mutex_lock(&c->lock);
    while (!r->done) {
        if (pthread_cond_timedwait(&c->cond, &c->lock, &ts) != 0) break; /* 超时 */
    }
    int rc = 0;
    if (r->error) rc = -1;
    if (r->data) {
        if (result) *result = r->data; /* 所有权转移给调用方 */
        else free(r->data);
        r->data = NULL;
    } else if (!r->done) {
        rc = -1; /* 30s 无响应 */
    }
    /* 从链表移除（晚到的响应找不到节点，直接丢弃） */
    PendingMcpResp **pp = &c->pending;
    while (*pp && *pp != r) pp = &(*pp)->next;
    if (*pp) *pp = r->next;
    free(r);
    pthread_mutex_unlock(&c->lock);
    return rc;
}

void rk_mcp_disconnect(RkMcpClient *c) {
    if (!c) return;
    if (c->is_sse) {
        pthread_mutex_lock(&c->lock);
        c->stopped = 1;
        pthread_mutex_unlock(&c->lock);
        /* 唤醒读线程：shutdown 让阻塞 poll/read 立即返回（不释放连接） */
        pthread_mutex_lock(&c->lock);
        RHttpConn *conn = c->sse_conn;
        pthread_mutex_unlock(&c->lock);
        if (conn) {
            int fd = rhttp_get_fd(conn);
            if (fd >= 0) shutdown(fd, SHUT_RDWR);
        }
        pthread_join(c->sse_thread, NULL);
        /* 清理残留挂起请求（线程已退出，无并发） */
        PendingMcpResp *r = c->pending;
        while (r) {
            PendingMcpResp *nx = r->next;
            free(r->data);
            free(r);
            r = nx;
        }
        c->pending = NULL;
        pthread_mutex_destroy(&c->lock);
        pthread_cond_destroy(&c->cond);
        c->is_sse = 0;
        return;
    }
    if (c->fd_read >= 0) close(c->fd_read);
    if (c->fd_write >= 0) close(c->fd_write);
    if (c->pid > 0) {
        kill(c->pid, SIGTERM);
        waitpid(c->pid, NULL, 0);
    }
    c->fd_read = c->fd_write = -1;
    c->pid = 0;
}

/* 发送 JSON-RPC 请求，读响应 */
static int rpc_call(RkMcpClient *c, const char *method, const char *params_json, char **result) {
    if (c->is_sse) return rpc_call_sse(c, method, params_json, result);
    char req[32768];
    int id = c->next_id++;
    int n = snprintf(req, sizeof(req),
                     "{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"%s\"%s%s}\n",
                     id, method,
                     params_json ? ",\"params\":" : "",
                     params_json ? params_json : "");
    if (n < 0 || (size_t)n >= sizeof(req)) return -1;
    /* 写请求 */
    size_t off = 0;
    while (off < (size_t)n) {
        ssize_t w = write(c->fd_write, req + off, (size_t)n - off);
        if (w <= 0) return -1;
        off += (size_t)w;
    }
    /* 读响应（一行 JSON） */
    char resp[65536];
    size_t resp_len = 0;
    while (resp_len < sizeof(resp) - 1) {
        ssize_t r = read(c->fd_read, resp + resp_len, 1);
        if (r <= 0) return -1;
        if (resp[resp_len] == '\n') break;
        resp_len++;
    }
    resp[resp_len] = '\0';
    /* 解析 JSON-RPC 响应 */
    Arena *a = arena_create(0);
    size_t err = 0;
    RJson *v = rjson_parse(a, resp, resp_len, &err);
    if (!v) { arena_destroy(a); return -1; }
    /* 检查 error */
    const RJson *error = rjson_obj_get(v, "error");
    if (error) {
        arena_destroy(a);
        return -1;
    }
    /* 提取 result */
    const RJson *res = rjson_obj_get(v, "result");
    if (res && result) {
        /* 序列化 result 为 JSON 字符串 */
        RJsonOut out;
        rjson_out_init(&out);
        rjson_write_value(&out, res);
        *result = out.buf; /* 调用方 free */
    }
    arena_destroy(a);
    return 0;
}

int rk_mcp_list_tools(RkMcpClient *c, RkMcpTool **tools, size_t *count) {
    char *result = NULL;
    if (rpc_call(c, "tools/list", NULL, &result) != 0) return -1;
    /* 解析 result.tools 数组 */
    Arena *a = arena_create(0);
    size_t err = 0;
    RJson *v = rjson_parse(a, result, strlen(result), &err);
    free(result);
    if (!v) { arena_destroy(a); return -1; }
    const RJson *tools_arr = rjson_obj_get(v, "tools");
    if (!tools_arr || tools_arr->type != RJSON_ARRAY) {
        arena_destroy(a);
        return -1;
    }
    size_t n = tools_arr->u.arr.count;
    RkMcpTool *ts = (RkMcpTool *)calloc(n, sizeof(RkMcpTool));
    if (!ts) { arena_destroy(a); return -1; }
    for (size_t i = 0; i < n; i++) {
        const RJson *t = tools_arr->u.arr.items[i];
        const RJson *name = rjson_obj_get(t, "name");
        const RJson *desc = rjson_obj_get(t, "description");
        const RJson *schema = rjson_obj_get(t, "inputSchema");
        if (name && name->type == RJSON_STRING) {
            ts[i].name = strndup(name->u.str.ptr, name->u.str.len);
        }
        if (desc && desc->type == RJSON_STRING) {
            ts[i].description = strndup(desc->u.str.ptr, desc->u.str.len);
        }
        if (schema) {
            RJsonOut out;
            rjson_out_init(&out);
            rjson_write_value(&out, schema);
            ts[i].input_schema = out.buf;
        }
    }
    *tools = ts;
    *count = n;
    arena_destroy(a);
    return 0;
}

void rk_mcp_tools_free(RkMcpTool *tools, size_t count) {
    if (!tools) return;
    for (size_t i = 0; i < count; i++) {
        free(tools[i].name);
        free(tools[i].description);
        free(tools[i].input_schema);
    }
    free(tools);
}

int rk_mcp_call_tool(RkMcpClient *c, const char *name, const char *args_json, char **result) {
    char params[65536];
    /* name 转义防 JSON 注入 */
    RJsonOut jo;
    rjson_out_init(&jo);
    rjson_write_string(&jo, name, strlen(name));
    int n = snprintf(params, sizeof(params),
                     "{\"name\":%s,\"arguments\":%s}",
                     jo.buf ? jo.buf : "\"\"",
                     args_json ? args_json : "{}");
    rjson_out_free(&jo);
    if (n < 0 || (size_t)n >= sizeof(params)) return -1;
    return rpc_call(c, "tools/call", params, result);
}
