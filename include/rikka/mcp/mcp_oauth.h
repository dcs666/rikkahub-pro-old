#ifndef RIKKA_MCP_OAUTH_H
#define RIKKA_MCP_OAUTH_H

#include <stddef.h>
#include <stdint.h>

/*
 * MCP OAuth 2.1 授权客户端（规范 2025-11-25 basic/authorization）。
 *
 * 环节（对齐 JVM 版 McpOAuthClient）：
 *   1. 受保护资源元数据发现（RFC 9728）：请求资源，401 的
 *      WWW-Authenticate: resource_metadata="..." 定位元数据；退回到
 *      resource/.well-known/mcp-resource-metadata。
 *   2. 授权服务器元数据发现（RFC 8414）：authorization/token/registration endpoint。
 *   3. 动态客户端注册（RFC 7591，token_endpoint_auth_method=none）。
 *   4. PKCE S256 授权码流程。
 *   5. 令牌刷新。
 *
 * 所有返回 malloc 的字符串归调用方（rk_mcp_oauth_free 释放全部字段）。
 * 非线程安全。
 */

typedef struct {
    /* 发现结果 */
    char *resource;                /* 受保护资源标识 */
    char *authorization_endpoint;  /* 授权端点（用户授权页） */
    char *token_endpoint;
    char *registration_endpoint;
    /* 客户端 */
    char *client_id;
    char *client_secret;           /* 可能为 NULL（auth_method=none） */
    /* 令牌 */
    char *access_token;
    char *refresh_token;
    char *scope;
    int64_t expires_at;            /* epoch 秒（0 = 未知） */
    int64_t expires_in;            /* 服务器给的秒数（0 = 未知） */
} RkMcpOAuth;

void rk_mcp_oauth_free(RkMcpOAuth *o);

/* 1. 元数据发现（资源 + 授权服务器）。返回 0 成功。 */
int rk_mcp_oauth_discover(RkMcpOAuth *o, const char *resource_url, int timeout_ms);

/* 2. 动态客户端注册。redirect_uri 必填；scope 可为 NULL。
 *    返回 0 成功（o->client_id 填充）。 */
int rk_mcp_oauth_register(RkMcpOAuth *o, const char *redirect_uri,
                          const char *scope, int timeout_ms);

/* 3. PKCE：生成 verifier（43 字符）与 S256 challenge（43 字符，base64url 无 padding）。
 *    返回 0 成功。 */
int rk_mcp_oauth_pkce(char verifier[64], char challenge[64]);

/* 4. 构造授权 URL（含 PKCE 与 state）。返回 malloc 字符串（调用方 free）。 */
char *rk_mcp_oauth_authorize_url(RkMcpOAuth *o, const char *redirect_uri,
                                 const char *verifier, const char *state);

/* 解析回调 URI 的 code 与 state（?code=..&state=..）。返回 0 成功。 */
int rk_mcp_oauth_parse_callback(const char *uri,
                                char *code, size_t code_sz,
                                char *state, size_t state_sz);

/* 5. 授权码换令牌（PKCE verifier）。成功填充 access/refresh token。 */
int rk_mcp_oauth_exchange(RkMcpOAuth *o, const char *redirect_uri,
                          const char *code, const char *verifier, int timeout_ms);

/* 6. 刷新令牌。返回 0 成功。 */
int rk_mcp_oauth_refresh(RkMcpOAuth *o, int timeout_ms);

/* 工具：URL 编码（malloc）。 */
char *rk_oauth_urlencode(const char *s);
/* 工具：base64url（无 padding，malloc）；decode 时 len 输出为解码长度（-1 非法）。 */
char *rk_oauth_b64url_encode(const uint8_t *data, size_t len);
uint8_t *rk_oauth_b64url_decode(const char *s, size_t *len_out);

#endif /* RIKKA_MCP_OAUTH_H */
