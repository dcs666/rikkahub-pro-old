# 代码约定（C 引擎）

新代码必须遵守，评审按此检查。

## 返回值约定

| API 类别 | 约定 | 示例 |
|---|---|---|
| 生命周期/操作 API | `0` = 成功，`-1` = 失败 | `rk_mcp_connect`、`rk_gateway_init`、`rhttp_send` |
| 连接/资源创建 | 返回 `NULL` 表示失败 | `rhttp_connect`、`arena_create` |
| 解析器 | 返回结果指针 + `size_t *err`（出错字节偏移，0 表示无错误） | `rjson_parse` |
| 读操作 | `>0` 字节数，`0` = EOF/断开，`-1` = 超时 | `rhttp_read_body` |
| 线程 API | 遵循 pthread 返回码（0 = 成功） | `pthread_create` |

**禁止**混用：操作 API 不要返回 `NULL`，解析器不要返回 `-1`。

## 所有权约定

- `malloc`/`realloc` 出来的缓冲：调用方 `free`（函数注释里写明 "caller frees"）。
- Arena 分配的内存：随 arena 一起释放，**不得**传出 arena 生命周期之外。
- `RJsonOut.buf`：调用方 `free`（内部 realloc 实现）。

## 并发约定

- 跨线程共享状态必须用 `pthread_mutex_t` 保护；条件等待用 `pthread_cond_t`。
- `stop`/`disconnect` 类 API：只做"信号"（置位标志 + 唤醒），**禁止**跨线程
  close(fd)/销毁 mutex——资源释放统一放在事件循环/工作线程的退出路径
  （教训：gateway.c 曾因 stop 跨线程 close 触发 TSan 数据竞争）。
- 单线程约定要写进头文件注释（如 mcp.h 的非线程安全说明）。

## URL 解析

**只有一份实现**：`rhttp_parse_url`（src/http/http.c）。禁止在各模块内联
重复解析（gateway/provider/mcp 已统一）。语义：必须带 `http://` 或
`https://` scheme；无路径时输出 `/`。

## 编译门禁

- 零 warning：`-Wall -Wextra -Wpedantic` 下不得出现任何 warning。
- 提交前必须 `make clean && make check`（clean 防 .o 污染）。
- 改公共头文件后全量重编。
- sanitizer 验证走 `make ubsan/asan/lsan/tsan`（CI 全量跑）。
