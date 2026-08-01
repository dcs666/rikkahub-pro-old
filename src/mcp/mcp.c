#define _POSIX_C_SOURCE 200809L
#include "rikka/mcp/mcp.h"
#include "rikka/json/json.h"
#include "rikka/http/http.h"
#include "rikka/util/arena.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <errno.h>

int rk_mcp_connect(RkMcpClient *c, const char *command, char *const *args) {
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

int rk_mcp_connect_sse(RkMcpClient *c, const char *url) {
    if (!c || !url) return -1;
    /* 解析 URL */
    char host[256], path[512];
    int tls = 0, port = 80;
    const char *u = url;
    if (strncmp(u, "https://", 8) == 0) { tls = 1; port = 443; u += 8; }
    else if (strncmp(u, "http://", 7) == 0) { u += 7; }
    sscanf(u, "%255[^/]%511s", host, path);
    if (path[0] == '\0') strcpy(path, "/");
    /* HTTP 连接 */
    RHttpConn *conn = rhttp_connect(host, (uint16_t)port, tls, 30000);
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
    /* SSE 连接建立，用 fd_read 读事件流 */
    /* 简化：SSE 连接用 HTTP 连接的 fd（需要暴露 fd） */
    /* 简化实现：SSE 连接暂不支持（需要 HTTP 连接暴露 fd） */
    rhttp_close(conn);
    return -1; /* TODO: SSE 传输需要 HTTP 连接暴露 fd */
}

void rk_mcp_disconnect(RkMcpClient *c) {
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
    char req[4096];
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
    char params[8192];
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
