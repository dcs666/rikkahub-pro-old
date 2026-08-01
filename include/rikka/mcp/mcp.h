#ifndef RIKKA_MCP_MCP_H
#define RIKKA_MCP_MCP_H

#include <stddef.h>
#include <stdint.h>

/*
 * MCP（Model Context Protocol）客户端：JSON-RPC over stdio。
 * 简化实现：fork+exec MCP 服务器，stdio 管道通信。
 * 支持：tools/list、tools/call。
 */

typedef struct {
    int fd_read, fd_write;  /* stdio pipes */
    int pid;
    int next_id;
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
