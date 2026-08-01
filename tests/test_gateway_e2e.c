#define _DEFAULT_SOURCE
#include "test.h"
#include "rikka/gateway/gateway.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <signal.h>
#include <sys/wait.h>

#include "test_server.h" /* start_mock_server / stop_mock_server / g_port */

static void *gateway_thread(void *arg) {
    rk_gateway_run((RkGateway *)arg);
    return NULL;
}

static void *gateway_multi_thread(void *arg) {
    rk_gateway_run_multi((RkGateway *)arg, 2);
    return NULL;
}

/* 并发请求线程：POST /chat 并断言 200 + 内容 */
typedef struct {
    int port;
    int ok;
} ReqCtx;

static void *req_thread(void *arg) {
    ReqCtx *rc = (ReqCtx *)arg;
    rc->ok = 0;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return NULL;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)rc->port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return NULL;
    }
    const char *body = "{\"model\":\"gpt-4o\",\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}";
    char req[2048];
    int n = snprintf(req, sizeof(req),
                     "POST /chat HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\n"
                     "Content-Length: %zu\r\n\r\n%s",
                     strlen(body), body);
    ssize_t w = write(fd, req, (size_t)n);
    if (w <= 0) { close(fd); return NULL; }
    char resp[16384];
    size_t resp_len = 0;
    for (int i = 0; i < 10; i++) {
        ssize_t r = read(fd, resp + resp_len, sizeof(resp) - resp_len - 1);
        if (r <= 0) break;
        resp_len += (size_t)r;
        if (strstr(resp, "\r\n\r\n") && strstr(resp, "Hello")) break;
    }
    resp[resp_len] = '\0';
    rc->ok = (strstr(resp, "200") != NULL) && (strstr(resp, "Hello") != NULL);
    close(fd);
    return NULL;
}

TEST(gateway_multi_worker) {
    /* 2 worker + SO_REUSEPORT：并发请求全部成功，stop 干净退出 */
    start_mock_server();
    RkGateway g;
    int gport = 18000 + (getpid() % 700);
    ASSERT_EQ_INT(0, rk_gateway_init(&g, gport));
    char mock_base[128];
    snprintf(mock_base, sizeof(mock_base), "http://127.0.0.1:%d", g_port);
    ASSERT_EQ_INT(0, rk_gateway_add_provider(&g, "openai", "test-key", mock_base));
    pthread_t tid;
    pthread_create(&tid, NULL, gateway_multi_thread, &g);
    usleep(150000);

    /* 8 个并发请求 */
    ReqCtx ctxs[8];
    pthread_t rts[8];
    for (int i = 0; i < 8; i++) {
        ctxs[i].port = gport;
        ctxs[i].ok = 0;
        pthread_create(&rts[i], NULL, req_thread, &ctxs[i]);
    }
    for (int i = 0; i < 8; i++) pthread_join(rts[i], NULL);
    for (int i = 0; i < 8; i++) ASSERT(ctxs[i].ok);

    rk_gateway_stop(&g);
    pthread_join(tid, NULL);
    stop_mock_server();
}

TEST(gateway_chat_proxy) {
    /* 起 mock provider（OpenAI SSE 回放） */
    start_mock_server();
    /* 网关 */
    RkGateway g;
    int gport = 18000 + (getpid() % 700);
    ASSERT_EQ_INT(0, rk_gateway_init(&g, gport));
    char mock_base[128];
    snprintf(mock_base, sizeof(mock_base), "http://127.0.0.1:%d", g_port);
    ASSERT_EQ_INT(0, rk_gateway_add_provider(&g, "openai", "test-key", mock_base));
    pthread_t tid;
    pthread_create(&tid, NULL, gateway_thread, &g);
    usleep(100000);

    /* 请求网关 /chat */
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT(fd >= 0);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)gport);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    ASSERT_EQ_INT(0, connect(fd, (struct sockaddr *)&addr, sizeof(addr)));
    const char *body = "{\"model\":\"gpt-4o\",\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}";
    char req[2048];
    int n = snprintf(req, sizeof(req),
                     "POST /chat HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\n"
                     "Content-Length: %zu\r\n\r\n%s",
                     strlen(body), body);
    ssize_t w = write(fd, req, (size_t)n);
    ASSERT(w > 0);
    /* 读响应（可能需要多次 read） */
    char resp[16384];
    size_t resp_len = 0;
    for (int i = 0; i < 10; i++) {
        ssize_t r = read(fd, resp + resp_len, sizeof(resp) - resp_len - 1);
        if (r <= 0) break;
        resp_len += (size_t)r;
    }
    resp[resp_len] = '\0';
    ASSERT(strstr(resp, "200") != NULL);
    ASSERT(strstr(resp, "Hello") != NULL); /* mock 回放的流式内容 */
    close(fd);

    rk_gateway_stop(&g);
    pthread_join(tid, NULL);
    stop_mock_server();
}

int run_gateway_e2e_suite(void) {
    const RikkaTest tests[] = {
        RIKKA_TEST_REGISTER(gateway_e2e, gateway_chat_proxy),
        RIKKA_TEST_REGISTER(gateway_e2e, gateway_multi_worker),
    };
    return run_suite("gateway_e2e", tests, sizeof(tests) / sizeof(tests[0]));
}
