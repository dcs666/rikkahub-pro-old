#include "test.h"
#include "rikka/mcp/mcp.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

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
    char *args[] = {"python3", "/tmp/mcp_echo.py", NULL};
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
    char *args[] = {"python3", "/tmp/mcp_echo.py", NULL};
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

int run_mcp_suite(void) {
    const RikkaTest tests[] = {
        RIKKA_TEST_REGISTER(mcp, mcp_connect_and_list),
        RIKKA_TEST_REGISTER(mcp, mcp_call_tool),
    };
    return run_suite("mcp", tests, sizeof(tests) / sizeof(tests[0]));
}
