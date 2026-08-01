#define _DEFAULT_SOURCE
#include "test.h"
#include "rikka/mcp/mcp.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "test_server.h" /* start_mock_server / stop_mock_server / g_port */

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
    };
    return run_suite("mcp", tests, sizeof(tests) / sizeof(tests[0]));
}
