# RikkaHub 核心引擎 C 重写 — 里程碑路线图

原则：每个里程碑 = 可独立编译 + 单元测试 + 基准数字；先立基线再优化；对标 JVM 版指标。

## 2026-08 重构轮次（进行中）

顺序由依赖决定：先安全网 → 动结构 → 功能 → 终审。每阶段独立 commit + make clean && make test 全绿 + 零 warning。

- [x] **P0 基线加固**：Makefile 加 `ubsan/asan/lsan` 目标；CI sanitizers job 改用 make 目标并新增 ASan+LSan
- [x] **P2 MCP SSE 传输**：http 暴露 fd + `rhttp_parse_url` 公开；SSE transport（endpoint 握手/挂起请求/pending 链表/读线程）；语义与 stdio 对齐（result 成员提取、error 不设 result）；修复读线程超时误杀连接 bug；新增 5 个测试（error/POST404/无心跳空闲回归）
- [x] **P1 结构重构**：三份 URL 解析统一为 `rhttp_parse_url`（gateway 内联解析顺带修复 path_prefix 未初始化读；provider 的 parse_base 删除；google 路径合并单 snprintf 消除 -Wformat-truncation）；`docs/CONVENTIONS.md` 固化返回值/所有权/并发/编译约定；未拆大文件——各模块 200-930 行且内聚，拆分的收益小于风险；构建产物 *.d/.build-flags 取消追踪
- [x] **BUG 修复（CI 暴露）**：① gateway `rk_gateway_stop` 跨线程 close(fd) 数据竞争（TSan）→ stop 只置位标志，资源清理移到 run 退出路径 + active_handlers 等待；② `RJsonOut.buf` 不 NUL 结尾 → strlen 读堆垃圾（CI 上 mcp_sse 解析失败，本地靠堆布局侥幸通过）→ out_append 保持 NUL 不变式 + 回归测试；③ 测试 runner 支持 `./test_runner [suite]` 定向运行
- [x] **P3 基准补齐**：md 增量(3.7us/token)/epub(0.12ms)/arena(68x)/SSE(4.4M ev/s) 四类新 bench + check_bench 阈值门禁（CI 跑 make check-bench）
- [x] **P4 功能补齐**：4a provider 重试/错误中间件（429/5xx/网络指数退避 + error.message 提取，测试 500/429 重试成功、4xx 不重试）→ 4b 渲染 JSON 线协议（rk_render_markdown_json + 修复代码块 lang 丢失）→ 4c 网关多实例（SO_REUSEPORT + 共享连接池，8 并发 e2e；修复池死连接 502/double-free/SIGPIPE 三连）→ 4d pptx（统一 zip 模块 docx/epub/pptx 共用）
- [x] **P5 安全终审**：strict 门禁（-Wshadow/-Wformat=2/-Wundef/-Wconversion/-Wsign-conversion/-Wwrite-strings/-Wpointer-arith/-Wcast-align）零警告；CI ASan/LSan 抓出并修复：RJsonOut 非 NUL 结尾、冻结消息 owned_buf 泄漏、MCP disconnect 跨线程 fd race；TSan 全绿
- [x] **P6 文档收尾**：README 更新（126 测试/新特性/性能表/开发状态）；ROADMAP 勾选
- [x] **P7 MCP 端到端 + 边界修复**：CLI MCP 调试模式（--mcp/--mcp-list/--mcp-call/--mcp-args，SSE 传输进程级端到端测试）；网关慢客户端读头循环（SO_RCVTIMEO 5s）；audio multipart 动态 body（去 64KB 上限）；池自赋值 memcpy 重叠修复；测试 read 后补 NUL（valgrind 抓）；128 测试
- [x] **P8 消息变换管线**：对标 JVM transformers 全套——输入 7 步（time_reminder/prompt_injection 5 位置+优先级+safe insert/placeholder 11 键/document_as_prompt/ocr/template/workspace_reminder）+ 输出 3 步（think_tag/regex_output/base64_image）；POSIX ERE + $N 替换；COW 消息列表；29 测试
- [x] **P9 数据实体面**：rk_store 记录存储（Favorite/Folder/GenMedia/ManagedFile，字段槽位 + 唯一约束 + 紧凑二进制快照原子写 + get_copy 安全读写）；4 测试
- [x] **P10 MCP Streamable HTTP + OAuth**：Streamable HTTP 传输（直答 JSON / 202 SSE 流 / mcp-session-id）；OAuth 2.1 全环节（RFC 9728/8414 发现、RFC 7591 注册、PKCE S256、授权码、刷新）；http 层 rhttp_resp_header 复制式 API；5 测试
- [x] **P11 工具系统**：注册表 + 7 个内置工具（get_time_info/workspace 读写编辑/shell 超时/memory_tool/use_skill）+ 设备工具回调接口（日历/屏幕时间/剪贴板/TTS/JS/AskUser/搜索）；路径穿越防护；7 测试；**151 测试**

## M0 基础设施层（本轮）
- [x] Makefile + 目录骨架
- [x] Buf 动态字节缓冲（append/reset 复用容量 = 零分配累积基础）
- [x] Arena 分配器（批量释放、块复用 = 流式管线无 GC 停顿基础）
- [x] 环形日志（级别过滤 + 最近 N 条查询）
- [x] 微型测试框架（断言 + 注册表 + 失败统计）
- [x] 基准框架（clock_gettime，ops/s）
- 验收：make test 全绿；bench 有数字

## M1 JSON 层（S2 基础）
- JSON 值解析器（tree，对应 kotlinx.serialization 非流式场景）
- **增量流式解析器**（SSE 场景只解析 delta，对应每 token 全量解析问题的根解）
- 序列化器（结构体 → JSON，对齐 Provider 请求构建）
- 基准：增量解析 vs 全量解析，ops/s + 分配计数对比

## M2 核心消息模型（S2）
- UIMessage/UIMessagePart 等价物，Arena 上零拷贝 append（对应 appendChunk 的 O(n) 重建 → O(1) 追加）
- groupPartsByToolBoundary / limitContext+alignContextStart / 注入管线（A6）纯逻辑迁移
- 会话分支结构（COW，A1）
- 基准：10 万 token 流式累积分配计数 vs JVM 基线

## M3 Provider 客户端（A3+B1+B3）
- HTTP 客户端（epoll 事件循环/连接复用；检查 libcurl 可用性）
- 统一 SSE 流式传输层（3 Provider + 17 搜索 + TTS/ASR 复用）
- OpenAI / Claude / Google 请求构建（多厂商 reasoning 适配表、cache_control 断点、thinking adaptive）
- 统一重试/错误中间件（parseErrorDetail）
- 验收：Mock 服务器回放真实请求/响应，字节级对比 JVM 版输出

## M4 增量解析引擎（S3）
- tree-sitter 或手写 lexer 高亮（对标 QuickJS+highlight.js，1-2 个数量级目标）
- markdown 增量 AST + 排版块缓存
- 基准：流式每 token 解析成本、高亮延迟对比

## M5 数据层（S4+A2）
- 内存常驻会话 + SQLite 仅持久化（flatbuffers 零解析指针访问）
- 统一两级 LRU（markdown/高亮/文件/HTTP）
- FTS 分词器（jieba 词典 mmap）+ 并行重建
- 基准：打开 1 万消息会话延迟对比

## M6 无锁流水线（S5）
- SSE 读 / 增量 JSON / 渲染 / 落盘 SPSC 无锁队列
- 基准：接收完→UI 更新延迟、吞吐

## M7+ A/B 其余项
- 渲染排版块输出（对接 UI 壳的协议）
- 文档解析（Docx/Epub/Pptx→Markdown，minizip+libxml2）
- 音频管线 / MCP 客户端 / workspace 直 IO
- 可观测性 trace 接入
- 服务端网关化（多实例）

## ✗ 明确不做
功能性 UI、无障碍/IME/文本栈、安全存储（keystore）、图像处理、Web API 静态资源、国际化（ICU）
