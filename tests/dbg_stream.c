#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rikka/mcp/mcp.h"
int main(void) {
    RkMcpClient c;
    if (rk_mcp_connect_streamable(&c, "http://127.0.0.1:18920/mcp/stream") != 0) { printf("connect fail\n"); return 1; }
    char *r = NULL;
    int rc = rk_mcp_call_tool(&c, "echo", "{\"text\":\"hello\"}", &r);
    printf("call1 rc=%d r=%s\n", rc, r ? r : "(null)");
    free(r); r = NULL;
    rc = rk_mcp_call_tool(&c, "echo", "{\"text\":\"world\"}", &r);
    printf("call2 rc=%d r=%s\n", rc, r ? r : "(null)");
    free(r);
    rk_mcp_disconnect(&c);
    return 0;
}
