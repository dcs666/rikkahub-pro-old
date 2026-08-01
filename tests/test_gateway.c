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
#include <unistd.h>

static void *gateway_thread(void *arg) {
    RkGateway *g = (RkGateway *)arg;
    rk_gateway_run(g);
    return NULL;
}

TEST(gateway_init_and_404) {
    RkGateway g;
    int port = 18080 + (getpid() % 1000);
    ASSERT_EQ_INT(0, rk_gateway_init(&g, port));
    ASSERT_EQ_INT(0, rk_gateway_add_provider(&g, "openai", "test-key", "https://api.openai.com/v1"));
    /* 启动网关线程 */
    pthread_t tid;
    pthread_create(&tid, NULL, gateway_thread, &g);
    usleep(100000); /* 等网关启动 */
    /* 简单 HTTP 请求：GET / → 404 */
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT(fd >= 0);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    ASSERT_EQ_INT(0, connect(fd, (struct sockaddr *)&addr, sizeof(addr)));
    const char *req = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    ssize_t w = write(fd, req, strlen(req));
    ASSERT(w > 0);
    char resp[1024];
    ssize_t n = read(fd, resp, sizeof(resp) - 1);
    ASSERT(n > 0);
    resp[n] = '\0';
    ASSERT(strstr(resp, "404") != NULL);
    close(fd);
    /* 停止网关 */
    rk_gateway_stop(&g);
    pthread_join(tid, NULL);
}

int run_gateway_suite(void) {
    const RikkaTest tests[] = {
        RIKKA_TEST_REGISTER(gateway, gateway_init_and_404),
    };
    return run_suite("gateway", tests, sizeof(tests) / sizeof(tests[0]));
}
