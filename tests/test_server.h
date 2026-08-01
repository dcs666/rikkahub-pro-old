#ifndef RIKKA_TEST_SERVER_H
#define RIKKA_TEST_SERVER_H

/*
 * 本地 mock server 管理（http/provider 测试共用）：
 * 随机端口 + 端口可用性探测 + 就绪轮询 + SIGKILL 清理。
 * 根治测试间端口竞态（残留 server/TIME_WAIT/垂死进程）。
 */
#define _POSIX_C_SOURCE 200809L
/* 调用方可能已定义（test_gateway_e2e.c 顶部），避免 redefined warning */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#include <arpa/inet.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "test.h"

static void msleep(long ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static pid_t g_server_pid = -1;
static int g_port = 18888;

static int port_free(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 0;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)port);
    int rc = bind(fd, (struct sockaddr *)&addr, sizeof(addr));
    close(fd);
    return rc == 0;
}

static int can_connect(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 0;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)port);
    int rc = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    close(fd);
    return rc == 0;
}

static void stop_mock_server(void) {
    if (g_server_pid > 0) {
        kill(g_server_pid, SIGKILL); /* 立即释放端口 */
        waitpid(g_server_pid, NULL, 0);
        g_server_pid = -1;
    }
}

static void start_mock_server(void) {
    g_port = 20000 + (int)(getpid() % 10000);
    for (int attempt = 0; attempt < 50 && !port_free(g_port); attempt++)
        g_port = 20000 + (int)((getpid() + attempt * 137 + 1) % 10000);
    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%d", g_port);
    g_server_pid = fork();
    if (g_server_pid == 0) {
        execlp("python3", "python3", "tests/mock_sse_server.py", portstr, (char *)NULL);
        _exit(1);
    }
    ASSERT(g_server_pid > 0);
    atexit(stop_mock_server);
    /* 轮询等就绪（最多 10s） */
    int ready = 0;
    for (int i = 0; i < 100; i++) {
        if (can_connect(g_port)) { ready = 1; break; }
        msleep(100);
    }
    ASSERT(ready);
}

#endif /* RIKKA_TEST_SERVER_H */
