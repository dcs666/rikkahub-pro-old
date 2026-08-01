#define _DEFAULT_SOURCE
#include "test.h"
#include "rikka/mcp/mcp.h"
#include "rikka/mcp/mcp_oauth.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "test_server.h"
#include <openssl/sha.h> /* start_mock_server / stop_mock_server / g_port */

/* python echo MCP 服务器脚本 */
static const char *MCP_SERVER_PY =
    "import sys, json\n"
    "for line in sys.stdin:\n"
    "    req = json.loads(line)\n"
    "    if req['method'] == 'tools/list':\n"
    "        resp = {'jsonrpc':'2.0','id':req['id'],'result':{'tools':[{'name':'echo','description':'Echo tool','inputSchema':{'type':'object'}}]}}\n"
    "    elif req['method'] == 'tools/call':\n"
    "        text = req['params']['arguments'].get('text','')\n"
    "        resp = {'jsonrpc':'2.0','id':req['id'],'result':{'content':[{'type':'text','text':'echo: ' + text}]}}\n"
    "    else:\n"
    "        resp = {'jsonrpc':'2.0','id':req['id'],'error':{'code':-32601,'message':'not found'}}\n"
    "    print(json.dumps(resp), flush=True)\n";

TEST(mcp_connect_and_list) {
    /* CI 环境可能限制 fork/exec，跳过 */
    if (getenv("CI")) {
        printf("  [skip: CI environment]\n");
        return;
    }
    /* 检查 python3 可用性 */
    if (system("which python3 >/dev/null 2>&1") != 0) {
        printf("  [skip: python3 not found]\n");
        return;
    }
    /* 写 python 脚本到 /tmp */
    FILE *f = fopen("/tmp/mcp_echo.py", "w");
    ASSERT_NOT_NULL(f);
    fputs(MCP_SERVER_PY, f);
    fclose(f);
    RkMcpClient c;
    char *args[] = {(char *)"python3", (char *)"/tmp/mcp_echo.py", NULL};
    int rc = rk_mcp_connect(&c, "python3", args);
    ASSERT_EQ_INT(0, rc);
    RkMcpTool *tools = NULL;
    size_t count = 0;
    rc = rk_mcp_list_tools(&c, &tools, &count);
    ASSERT_EQ_INT(0, rc);
    ASSERT_EQ_SIZE(1, count);
    ASSERT(strcmp(tools[0].name, "echo") == 0);
    ASSERT(strcmp(tools[0].description, "Echo tool") == 0);
    rk_mcp_tools_free(tools, count);
    rk_mcp_disconnect(&c);
}

TEST(mcp_call_tool) {
    if (getenv("CI")) {
        printf("  [skip: CI environment]\n");
        return;
    }
    if (system("which python3 >/dev/null 2>&1") != 0) {
        printf("  [skip: python3 not found]\n");
        return;
    }
    FILE *f = fopen("/tmp/mcp_echo.py", "w");
    ASSERT_NOT_NULL(f);
    fputs(MCP_SERVER_PY, f);
    fclose(f);
    RkMcpClient c;
    char *args[] = {(char *)"python3", (char *)"/tmp/mcp_echo.py", NULL};
    int rc = rk_mcp_connect(&c, "python3", args);
    ASSERT_EQ_INT(0, rc);
    char *result = NULL;
    rc = rk_mcp_call_tool(&c, "echo", "{\"text\":\"hello\"}", &result);
    ASSERT_EQ_INT(0, rc);
    ASSERT_NOT_NULL(result);
    ASSERT(strstr(result, "echo: hello") != NULL);
    free(result);
    rk_mcp_disconnect(&c);
}

/* SSE 传输：相对 endpoint 主路径（list + call + disconnect） */
TEST(mcp_sse_connect_and_list) {
    if (system("which python3 >/dev/null 2>&1") != 0) {
        printf("  [skip: python3 not found]\n");
        return;
    }
    start_mock_server();
    char url[160];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/mcp/sse", g_port);
    RkMcpClient c;
    int rc = rk_mcp_connect_sse(&c, url);
    ASSERT_EQ_INT(0, rc);
    RkMcpTool *tools = NULL;
    size_t count = 0;
    rc = rk_mcp_list_tools(&c, &tools, &count);
    ASSERT_EQ_INT(0, rc);
    ASSERT_EQ_SIZE(1, count);
    ASSERT(strcmp(tools[0].name, "echo") == 0);
    ASSERT(strcmp(tools[0].description, "Echo tool") == 0);
    ASSERT_NOT_NULL(tools[0].input_schema);
    rk_mcp_tools_free(tools, count);
    /* 连续两次调用：验证 pending 复用 + 顺序响应匹配 */
    char *r1 = NULL, *r2 = NULL;
    rc = rk_mcp_call_tool(&c, "echo", "{\"text\":\"sse1\"}", &r1);
    ASSERT_EQ_INT(0, rc);
    ASSERT_NOT_NULL(r1);
    ASSERT(strstr(r1, "echo: sse1") != NULL);
    rc = rk_mcp_call_tool(&c, "echo", "{\"text\":\"sse2\"}", &r2);
    ASSERT_EQ_INT(0, rc);
    ASSERT_NOT_NULL(r2);
    ASSERT(strstr(r2, "echo: sse2") != NULL);
    free(r1);
    free(r2);
    rk_mcp_disconnect(&c);
    stop_mock_server();
}

/* SSE 传输：绝对 endpoint URL（服务器发完整 URL） */
TEST(mcp_sse_absolute_endpoint) {
    if (system("which python3 >/dev/null 2>&1") != 0) {
        printf("  [skip: python3 not found]\n");
        return;
    }
    start_mock_server();
    char url[160];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/mcp/sse_abs", g_port);
    RkMcpClient c;
    int rc = rk_mcp_connect_sse(&c, url);
    ASSERT_EQ_INT(0, rc);
    RkMcpTool *tools = NULL;
    size_t count = 0;
    rc = rk_mcp_list_tools(&c, &tools, &count);
    ASSERT_EQ_INT(0, rc);
    ASSERT_EQ_SIZE(1, count);
    ASSERT(strcmp(tools[0].name, "echo") == 0);
    rk_mcp_tools_free(tools, count);
    rk_mcp_disconnect(&c);
    stop_mock_server();
}

/* SSE 传输：JSON-RPC error 响应 → rc=-1 且 result 不被设置（与 stdio 语义一致） */
TEST(mcp_sse_error_response) {
    if (system("which python3 >/dev/null 2>&1") != 0) {
        printf("  [skip: python3 not found]\n");
        return;
    }
    start_mock_server();
    char url[160];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/mcp/sse", g_port);
    RkMcpClient c;
    int rc = rk_mcp_connect_sse(&c, url);
    ASSERT_EQ_INT(0, rc);
    char *r1 = (char *)0x1; /* 哨兵：验证 error 路径不碰 result */
    rc = rk_mcp_call_tool(&c, "echo", "{\"text\":\"boom\"}", &r1);
    ASSERT_EQ_INT(-1, rc);
    ASSERT(r1 == (char *)0x1);
    rk_mcp_disconnect(&c);
    stop_mock_server();
}

/* SSE 传输：message endpoint POST 失败（404）→ 调用立即失败 */
TEST(mcp_sse_post_failure) {
    if (system("which python3 >/dev/null 2>&1") != 0) {
        printf("  [skip: python3 not found]\n");
        return;
    }
    start_mock_server();
    char url[160];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/mcp/sse_bad", g_port);
    RkMcpClient c;
    int rc = rk_mcp_connect_sse(&c, url);
    ASSERT_EQ_INT(0, rc);
    RkMcpTool *tools = NULL;
    size_t count = 0;
    rc = rk_mcp_list_tools(&c, &tools, &count);
    ASSERT_EQ_INT(-1, rc);
    ASSERT_NULL(tools);
    rk_mcp_disconnect(&c);
    stop_mock_server();
}

/* SSE 传输：服务器无心跳时，客户端读超时后必须继续等待而非断开传输
 * （回归：读线程曾把超时误当退出条件，5s 空闲即杀连接） */
TEST(mcp_sse_idle_no_heartbeat) {
    if (system("which python3 >/dev/null 2>&1") != 0) {
        printf("  [skip: python3 not found]\n");
        return;
    }
    start_mock_server();
    char url[160];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/mcp/sse_nohb", g_port);
    RkMcpClient c;
    int rc = rk_mcp_connect_sse(&c, url);
    ASSERT_EQ_INT(0, rc);
    usleep(6500000); /* 超过读线程 5s 超时阈值 */
    char *r1 = NULL;
    rc = rk_mcp_call_tool(&c, "echo", "{\"text\":\"alive\"}", &r1);
    ASSERT_EQ_INT(0, rc);
    ASSERT_NOT_NULL(r1);
    ASSERT(strstr(r1, "alive") != NULL);
    free(r1);
    rk_mcp_disconnect(&c);
    stop_mock_server();
}

/* CLI MCP 模式端到端：SSE 传输在真实 CLI 进程中跑通（list + call） */
TEST(mcp_cli_e2e_sse) {
    if (access("build/rikkahub", R_OK) != 0) {
        printf("  [skip: build/rikkahub not built]\n");
        return;
    }
    if (system("which python3 >/dev/null 2>&1") != 0) {
        printf("  [skip: python3 not found]\n");
        return;
    }
    start_mock_server();
    char url[160];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/mcp/sse", g_port);
    char cmd[1024];
    char buf[8192];
    /* --mcp-list */
    snprintf(cmd, sizeof(cmd), "build/rikkahub --mcp %s --mcp-list 2>&1", url);
    FILE *f = popen(cmd, "r");
    ASSERT_NOT_NULL(f);
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    int rc = pclose(f);
    ASSERT_EQ_INT(0, rc);
    ASSERT(strstr(buf, "echo") != NULL);
    ASSERT(strstr(buf, "Echo tool") != NULL);
    /* --mcp-call with args */
    snprintf(cmd, sizeof(cmd),
             "build/rikkahub --mcp %s --mcp-call echo --mcp-args '{\"text\":\"cli-e2e\"}' 2>&1",
             url);
    f = popen(cmd, "r");
    ASSERT_NOT_NULL(f);
    n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    rc = pclose(f);
    ASSERT_EQ_INT(0, rc);
    ASSERT(strstr(buf, "echo: cli-e2e") != NULL);
    stop_mock_server();
}

TEST(mcp_streamable_direct) {
    if (getenv("CI")) {
        printf("  [skip: CI environment]\n");
        return;
    }
    if (system("which python3 >/dev/null 2>&1") != 0) {
        printf("  [skip: python3 not found]\n");
        return;
    }
    start_mock_server();
    char url[160];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/mcp/stream", g_port);
    RkMcpClient c;
    ASSERT_EQ_INT(0, rk_mcp_connect_streamable(&c, url));
    /* list tools（首次请求：服务器签发 session id） */
    RkMcpTool *tools = NULL;
    size_t count = 0;
    ASSERT_EQ_INT(0, rk_mcp_list_tools(&c, &tools, &count));
    ASSERT_EQ_INT(1, (int)count);
    ASSERT(strcmp(tools[0].name, "echo") == 0);
    rk_mcp_tools_free(tools, count);
    /* call tool（第二次请求：session id 必须正确传递，否则 404） */
    char *result = NULL;
    ASSERT_EQ_INT(0, rk_mcp_call_tool(&c, "echo", "{\"text\":\"s1\"}", &result));
    ASSERT_NOT_NULL(result);
    ASSERT(strstr(result, "echo: s1") != NULL);
    free(result);
    rk_mcp_disconnect(&c);
    stop_mock_server();
}

TEST(mcp_streamable_202) {
    if (getenv("CI")) {
        printf("  [skip: CI environment]\n");
        return;
    }
    if (system("which python3 >/dev/null 2>&1") != 0) {
        printf("  [skip: python3 not found]\n");
        return;
    }
    start_mock_server();
    char url[160];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/mcp/stream202", g_port);
    RkMcpClient c;
    ASSERT_EQ_INT(0, rk_mcp_connect_streamable(&c, url));
    char *result = NULL;
    ASSERT_EQ_INT(0, rk_mcp_call_tool(&c, "echo", "{\"text\":\"s202\"}", &result));
    ASSERT_NOT_NULL(result);
    ASSERT(strstr(result, "echo: s202") != NULL);
    free(result);
    rk_mcp_disconnect(&c);
    stop_mock_server();
}

TEST(mcp_streamable_bad_session) {
    /* 手工构造：先发一个请求拿 session，然后伪造错误 session → 404 → error */
    if (getenv("CI")) {
        printf("  [skip: CI environment]\n");
        return;
    }
    if (system("which python3 >/dev/null 2>&1") != 0) {
        printf("  [skip: python3 not found]\n");
        return;
    }
    start_mock_server();
    char url[160];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/mcp/stream", g_port);
    RkMcpClient c;
    ASSERT_EQ_INT(0, rk_mcp_connect_streamable(&c, url));
    /* 正常请求拿 session */
    char *result = NULL;
    ASSERT_EQ_INT(0, rk_mcp_call_tool(&c, "echo", "{\"text\":\"a\"}", &result));
    free(result);
    /* 伪造错误 session（外部不可见——直接破坏内部状态模拟） */
    pthread_mutex_lock(&c.lock);
    snprintf(c.stream_session_id, sizeof(c.stream_session_id), "wrong-sess");
    pthread_mutex_unlock(&c.lock);
    result = NULL;
    ASSERT_EQ_INT(-1, rk_mcp_call_tool(&c, "echo", "{\"text\":\"b\"}", &result));
    ASSERT_NULL(result);
    rk_mcp_disconnect(&c);
    stop_mock_server();
}

TEST(mcp_oauth_full_flow) {
    if (getenv("CI")) {
        printf("  [skip: CI environment]\n");
        return;
    }
    if (system("which python3 >/dev/null 2>&1") != 0) {
        printf("  [skip: python3 not found]\n");
        return;
    }
    start_mock_server();
    char resource[160];
    snprintf(resource, sizeof(resource), "http://127.0.0.1:%d/oauth/resource", g_port);
    RkMcpOAuth o;
    memset(&o, 0, sizeof(o));
    /* 1. 发现（401 → resource_metadata → authorization server well-known） */
    ASSERT_EQ_INT(0, rk_mcp_oauth_discover(&o, resource, 15000));
    ASSERT_NOT_NULL(o.authorization_endpoint);
    ASSERT_NOT_NULL(o.token_endpoint);
    ASSERT_NOT_NULL(o.registration_endpoint);
    ASSERT(strstr(o.authorization_endpoint, "/oauth/authorize") != NULL);
    /* 2. 注册 */
    ASSERT_EQ_INT(0, rk_mcp_oauth_register(&o, "rikkahub://callback", "tools.read", 15000));
    ASSERT_NOT_NULL(o.client_id);
    ASSERT(strcmp(o.client_id, "client-1") == 0);
    /* 3. PKCE */
    char verifier[64], challenge[64];
    ASSERT_EQ_INT(0, rk_mcp_oauth_pkce(verifier, challenge));
    ASSERT_EQ_INT(43, (int)strlen(verifier));
    ASSERT_EQ_INT(43, (int)strlen(challenge));
    ASSERT(strstr(challenge, "=") == NULL); /* base64url 无 padding */
    /* 4. 授权 URL */
    char *auth_url = rk_mcp_oauth_authorize_url(&o, "rikkahub://callback", verifier, "st-1");
    ASSERT_NOT_NULL(auth_url);
    ASSERT(strstr(auth_url, "response_type=code") != NULL);
    ASSERT(strstr(auth_url, "client_id=client-1") != NULL);
    ASSERT(strstr(auth_url, "code_challenge=") != NULL);
    ASSERT(strstr(auth_url, "code_challenge_method=S256") != NULL);
    ASSERT(strstr(auth_url, "state=st-1") != NULL);
    free(auth_url);
    /* 5. 回调解析 */
    char code[128], state[128];
    ASSERT_EQ_INT(0, rk_mcp_oauth_parse_callback("rikkahub://callback?code=code-1&state=st-1",
                                                 code, sizeof(code), state, sizeof(state)));
    ASSERT(strcmp(code, "code-1") == 0);
    ASSERT(strcmp(state, "st-1") == 0);
    /* 6. 换令牌 */
    ASSERT_EQ_INT(0, rk_mcp_oauth_exchange(&o, "rikkahub://callback", code, verifier, 15000));
    ASSERT_NOT_NULL(o.access_token);
    ASSERT(strcmp(o.access_token, "at-1") == 0);
    ASSERT_NOT_NULL(o.refresh_token);
    ASSERT(strcmp(o.refresh_token, "rt-1") == 0);
    ASSERT(o.expires_in > 0);
    /* 7. 刷新 */
    ASSERT_EQ_INT(0, rk_mcp_oauth_refresh(&o, 15000));
    ASSERT(strcmp(o.access_token, "at-2") == 0);
    rk_mcp_oauth_free(&o);
    stop_mock_server();
}

TEST(mcp_oauth_tools) {
    /* base64url roundtrip */
    const uint8_t data[] = {0xfb, 0xff, 0xef, 0x00, 0x01, 0x02};
    char *b64 = rk_oauth_b64url_encode(data, sizeof(data));
    ASSERT_NOT_NULL(b64);
    /* fb ff ef → 111110|111111|111111|101111 → -__v ; 00 01 02 → AAEC */
    ASSERT(strcmp(b64, "-__vAAEC") == 0);
    size_t len = 0;
    uint8_t *dec = rk_oauth_b64url_decode(b64, &len);
    ASSERT_NOT_NULL(dec);
    ASSERT_EQ_INT((int)sizeof(data), (int)len);
    ASSERT(memcmp(dec, data, sizeof(data)) == 0);
    free(dec);
    free(b64);
    /* PKCE 向量：RFC 7636 附录 B（verifier → challenge 已知值） */
    char v[64], ch[64];
    ASSERT_EQ_INT(0, rk_mcp_oauth_pkce(v, ch));
    /* verifier → challenge 一致性：重算 */
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256((const unsigned char *)v, strlen(v), digest);
    char *expect = rk_oauth_b64url_encode(digest, SHA256_DIGEST_LENGTH);
    ASSERT_NOT_NULL(expect);
    ASSERT(strcmp(ch, expect) == 0);
    free(expect);
    /* urlencode */
    char *enc = rk_oauth_urlencode("a b+c/é");
    ASSERT_NOT_NULL(enc);
    ASSERT(strstr(enc, "%20") != NULL);
    ASSERT(strstr(enc, "%2B") != NULL);
    free(enc);
}

int run_mcp_suite(void) {
    const RikkaTest tests[] = {
        RIKKA_TEST_REGISTER(mcp, mcp_connect_and_list),
        RIKKA_TEST_REGISTER(mcp, mcp_call_tool),
        RIKKA_TEST_REGISTER(mcp, mcp_sse_connect_and_list),
        RIKKA_TEST_REGISTER(mcp, mcp_sse_absolute_endpoint),
        RIKKA_TEST_REGISTER(mcp, mcp_sse_error_response),
        RIKKA_TEST_REGISTER(mcp, mcp_sse_post_failure),
        RIKKA_TEST_REGISTER(mcp, mcp_sse_idle_no_heartbeat),
        RIKKA_TEST_REGISTER(mcp, mcp_cli_e2e_sse),
        RIKKA_TEST_REGISTER(mcp, mcp_streamable_direct),
        RIKKA_TEST_REGISTER(mcp, mcp_streamable_202),
        RIKKA_TEST_REGISTER(mcp, mcp_streamable_bad_session),
        RIKKA_TEST_REGISTER(mcp, mcp_oauth_full_flow),
        RIKKA_TEST_REGISTER(mcp, mcp_oauth_tools),
    };
    return run_suite("mcp", tests, sizeof(tests) / sizeof(tests[0]));
}
