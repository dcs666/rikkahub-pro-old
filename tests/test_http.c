#define _POSIX_C_SOURCE 200809L
#include "test.h"
#include "rikka/http/http.h"
#include "rikka/http/sse.h"
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* 事件收集 */
#define MAX_EV 16
typedef struct { char event[64]; char data[512]; } Captured;
static Captured g_caps[MAX_EV];
static int g_ncap = 0;

static void sse_cb(void *ctx, const char *event, const char *data, size_t data_len,
                   const char *id, long long retry_ms) {
    (void)ctx; (void)id; (void)retry_ms;
    if (g_ncap < MAX_EV) {
        snprintf(g_caps[g_ncap].event, sizeof(g_caps[g_ncap].event), "%s", event);
        size_t n = data_len < sizeof(g_caps[g_ncap].data) - 1 ? data_len : sizeof(g_caps[g_ncap].data) - 1;
        memcpy(g_caps[g_ncap].data, data, n);
        g_caps[g_ncap].data[n] = '\0';
    }
    g_ncap++;
}

static pid_t g_server_pid = -1;
static int g_port = 18888;

static void stop_mock_server(void) {
    if (g_server_pid > 0) {
        kill(g_server_pid, SIGTERM);
        waitpid(g_server_pid, NULL, 0);
        g_server_pid = -1;
    }
}

/* 随机端口避免冲突 + atexit 清理（断言失败也清理） */
static void start_mock_server(void) {
    g_port = 20000 + (int)(getpid() % 10000);
    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%d", g_port);
    g_server_pid = fork();
    if (g_server_pid == 0) {
        execlp("python3", "python3", "tests/mock_sse_server.py", portstr, (char *)NULL);
        _exit(1);
    }
    ASSERT(g_server_pid > 0);
    atexit(stop_mock_server);
    sleep(1); /* 等服务器就绪 */
}

TEST(http_sse_stream) {
    start_mock_server();
    RHttpConn *c = rhttp_connect("127.0.0.1", (uint16_t)g_port, 0, 5000);
    ASSERT_NOT_NULL(c);

    const char *hdrs[] = {"Accept", "text/event-stream", NULL};
    ASSERT_EQ_INT(0, rhttp_send(c, "GET", "/sse", hdrs, NULL, 0));

    RHttpResp resp;
    ASSERT_EQ_INT(0, rhttp_read_headers(c, &resp, 5000));
    ASSERT_EQ_INT(200, resp.status);
    ASSERT_EQ_INT(1, resp.chunked);

    /* 流式读取 + SSE 解析 */
    g_ncap = 0;
    RsseParser *p = rsse_create(sse_cb, NULL);
    char buf[4096];
    for (;;) {
        ssize_t n = rhttp_read_body(c, buf, sizeof(buf), 3000);
        if (n <= 0) break;
        ASSERT_EQ_INT(0, rsse_feed(p, buf, (size_t)n));
    }
    rsse_finish(p);
    rsse_destroy(p);
    rhttp_close(c);
    stop_mock_server();

    ASSERT_EQ_INT(3, g_ncap);
    ASSERT(strcmp(g_caps[0].event, "message") == 0);
    ASSERT(strcmp(g_caps[0].data, "Hello") == 0);
    ASSERT(strcmp(g_caps[1].event, "message") == 0);
    ASSERT(strcmp(g_caps[1].data, "world") == 0);
    ASSERT(strcmp(g_caps[2].event, "done") == 0);
    ASSERT_EQ_SIZE(0, strlen(g_caps[2].data));
}

TEST(http_sse_split_events) {
    /* 事件跨 TCP 分片：SSE 解析器应正确重组（/slow 无长度流式） */
    start_mock_server();
    RHttpConn *c = rhttp_connect("127.0.0.1", (uint16_t)g_port, 0, 5000);
    ASSERT_NOT_NULL(c);
    ASSERT_EQ_INT(0, rhttp_send(c, "GET", "/slow", NULL, NULL, 0));
    RHttpResp resp;
    ASSERT_EQ_INT(0, rhttp_read_headers(c, &resp, 5000));
    ASSERT_EQ_INT(200, resp.status);

    g_ncap = 0;
    RsseParser *p = rsse_create(sse_cb, NULL);
    char buf[8]; /* 小缓冲强制多次 read */
    for (;;) {
        ssize_t n = rhttp_read_body(c, buf, sizeof(buf), 3000);
        if (n <= 0) break;
        ASSERT_EQ_INT(0, rsse_feed(p, buf, (size_t)n));
    }
    rsse_finish(p);
    rsse_destroy(p);
    rhttp_close(c);
    stop_mock_server();

    ASSERT_EQ_INT(5, g_ncap);
    ASSERT(strcmp(g_caps[0].data, "tick0") == 0);
    ASSERT(strcmp(g_caps[4].data, "tick4") == 0);
}

TEST(http_sync_json) {
    start_mock_server();
    char url[128];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/json", g_port);
    int status = 0;
    size_t len = 0;
    char *body = rhttp_request_sync(url, NULL, NULL, 0, 5000, &status, &len);
    ASSERT_NOT_NULL(body);
    ASSERT_EQ_INT(200, status);
    ASSERT(strstr(body, "\"ok\":true") != NULL);
    ASSERT(strstr(body, "\"value\":42") != NULL);
    free(body);
    stop_mock_server();
}

TEST(http_404) {
    start_mock_server();
    char url[128];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/nope", g_port);
    int status = 0;
    size_t len = 0;
    char *body = rhttp_request_sync(url, NULL, NULL, 0, 5000, &status, &len);
    ASSERT_NOT_NULL(body);
    ASSERT_EQ_INT(404, status);
    free(body);
    stop_mock_server();
}

int run_http_suite(void) {
    const RikkaTest tests[] = {
        RIKKA_TEST_REGISTER(http, http_sse_stream),
        RIKKA_TEST_REGISTER(http, http_sse_split_events),
        RIKKA_TEST_REGISTER(http, http_sync_json),
        RIKKA_TEST_REGISTER(http, http_404),
    };
    return run_suite("http", tests, sizeof(tests) / sizeof(tests[0]));
}
