#ifndef RIKKA_MCP_MCP_H
#define RIKKA_MCP_MCP_H

#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

/*
 * MCP（Model Context Protocol）客户端。
 * 支持两种传输：
 *  - stdio：fork+exec MCP 服务器，管道 JSON-RPC（单行响应模型）
 *  - SSE：HTTP 长连接事件流（MCP 2025-03-26 SSE transport）
 * 支持：tools/list、tools/call。
 * 注意：非线程安全——rk_mcp_disconnect 必须在所有 rk_mcp_* 调用结束后调用，
 * 且同一时刻只允许一个线程发起调用（SSE 的挂起请求链表依赖此约定）。
 */

/* 不透明类型前向声明（避免引入 http.h 头文件耦合） */
typedef struct RHttpConn RHttpConn;

/* SSE 传输的挂起请求节点（内部结构，定义在 mcp.c） */
typedef struct PendingMcpResp PendingMcpResp;

typedef struct {
    int fd_read, fd_write;   /* stdio pipes */
    int pid;
    int next_id;
    /* ---- SSE 传输状态（rk_mcp_connect_sse 初始化） ---- */
    int is_sse;              /* 1 = SSE 传输模式 */
    pthread_t sse_thread;    /* SSE 读线程（连接归线程所有） */
    pthread_mutex_t lock;    /* 保护 pending / stopped / sse_conn */
    pthread_cond_t cond;     /* 响应到达 / 传输断开通知 */
    PendingMcpResp *pending; /* 未完成请求链表 */
    int stopped;             /* disconnect 标志 */
    RHttpConn *sse_conn;     /* SSE 读连接（线程退出时置 NULL） */
    char endpoint_host[256]; /* message endpoint（POST 目标） */
    uint16_t endpoint_port;
    int endpoint_tls;
    char endpoint_path[512];
} RkMcpClient;

typedef struct {
    char *name;
    char *description;
    char *input_schema;  /* JSON schema 字符串 */
} RkMcpTool;

/* 连接 MCP 服务器（fork+exec command args，stdio 传输） */
int rk_mcp_connect(RkMcpClient *c, const char *command, char *const *args);

/* 连接 MCP 服务器（SSE 传输，HTTP 长连接） */
int rk_mcp_connect_sse(RkMcpClient *c, const char *url);

void rk_mcp_disconnect(RkMcpClient *c);

/* 列出工具 */
int rk_mcp_list_tools(RkMcpClient *c, RkMcpTool **tools, size_t *count);
void rk_mcp_tools_free(RkMcpTool *tools, size_t count);

/* 调用工具（args_json 是 JSON 对象字符串，result 返回 JSON 字符串） */
int rk_mcp_call_tool(RkMcpClient *c, const char *name, const char *args_json, char **result);

#endif /* RIKKA_MCP_MCP_H */
