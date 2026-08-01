# RikkaHub Core Engine (C)

RikkaHub AI 客户端核心运行时的 **纯 C 重写**。目标不是"C 版 Android 应用"，而是**零依赖的高性能 AI 运行时引擎 + 薄 UI 壳**。

## 特性

- **零依赖**：仅系统 C 库 + OpenSSL + zlib（无 libcurl/cmake/第三方框架）
- **高性能**：流式累积比 JVM 版快 **1976x**，打开 1 万消息会话 **2.1ms**（JVM Room+JSON 几百 ms）
- **多 Provider**：OpenAI / Claude / Google 统一流式管线 + **重试中间件**（429/5xx/网络错误指数退避，错误详情提取）
- **无锁并发**：SPSC 无锁队列 13.6 Mops/s
- **增量解析**：Markdown 流式 O(n²)→O(1)（199KB 文档 378ms→24.3ms）
- **MCP 双传输**：stdio + SSE（endpoint 握手/挂起请求/读线程，语义与 stdio 对齐）
- **多实例网关**：SO_REUSEPORT 多 worker 内核负载均衡 + 共享连接池
- **文档解析**：docx / epub / pptx（统一 zip 读取模块）
- **内存安全**：八轮 deep-review + ASan/LSan/TSan/UBSan 全量 CI + valgrind 全量 0 泄漏 + strict 告警门禁

## 构建

```bash
make            # 构建全部
make test       # 单元测试 (173/173)
make check      # 零 warning 门禁 + 全测试
make strict     # 严格告警门禁 (-Wshadow/-Wconversion/...)
make bench      # 性能基准
make check-bench # 基准阈值回归
make cli        # CLI 工具
make fuzz       # 解析器 fuzz (UBSan)
make ubsan      # UBSan 全测试 + fuzz
make asan       # ASan 全测试 + fuzz（CI）
make lsan       # LeakSanitizer 全测试 + fuzz
make tsan       # ThreadSanitizer (并发检测)
```

## CLI 使用

```bash
# 单次对话
rikkahub "hello world"                    # OPENAI_API_KEY 环境变量

# 指定 Provider
rikkahub --provider claude --model claude-3-5-sonnet "hi"
rikkahub --provider google --model gemini-pro "hi"

# 交互模式
rikkahub --interactive

# MCP 工具调试（SSE 传输）
rikkahub --mcp http://127.0.0.1:18888/mcp/sse --mcp-list
rikkahub --mcp http://127.0.0.1:18888/mcp/sse --mcp-call echo --mcp-args '{"text":"hi"}'

# MCP 工具调试（Streamable HTTP 传输）
rikkahub --mcp http://127.0.0.1:18888/mcp --mcp-list

# 性能分析
rikkahub --trace "explain this"
```

## 模块架构

| 模块 | 目录 | 说明 |
|---|---|---|
| 核心 | `core/` | Buf/Arena/消息模型/COW 会话树/流式累积 |
| JSON | `json/` | 值解析/序列化/增量流式提取 |
| Provider | `ai/` | OpenAI/Claude/Google 请求构建 + 统一流式管线 + 重试/错误中间件 |
| HTTP | `http/` | socket+OpenSSL HTTP/1.1 客户端 (chunked/连接池/缓冲读) |
| Markdown | `markdown/` | 块+行内 AST + 增量解析 |
| 高亮 | `highlight/` | 17 语言手写 lexer |
| 数据层 | `data/` | rbin 二进制存储 (mmap 零拷贝)/LRU/倒排索引 |
| 无锁管道 | `pipe/` | SPSC 无锁环形队列 |
| 文档解析 | `doc/` | docx/epub/pptx (统一 zip 读取 + XML 文本提取) |
| MCP | `mcp/` | JSON-RPC 客户端（stdio + SSE 双传输） |
| Workspace | `workspace/` | 直 IO (路径穿越防护) |
| 音频 | `audio/` | WAV 封装/TTS/ASR 客户端 |
| 渲染 | `render/` | Markdown→排版块+代码高亮 + **JSON 线协议**（UI 壳跨语言消费） |
| 可观测性 | `trace/` | span 追踪 |
| 网关 | `gateway/` | epoll HTTP 服务器 + AI 代理（单/多实例） |
| CLI | `tools/` | 命令行客户端 |

## 性能数据

| 指标 | 数值 | 对比 |
|---|---|---|
| 流式累积 | 1976x | JVM 每 token 对象重建 |
| rbin 加载 (1万消息) | 2.1ms | JVM Room+JSON 几百 ms |
| 代码高亮 | 297ns/行 | QuickJS highlight.js µs/行 |
| SPSC 队列 | 13.6 Mops/s | - |
| JSON 增量提取 | 2.8x | 全量解析 |
| MD 流式 per-token | 3.7us | 增量只重解析最后一块 |
| arena 分配复用 | 68x | malloc/free |
| SSE 解析吞吐 | 4.4 M events/s | 1400B 分片 |
| EPUB 解析 (40 章) | 0.12ms | - |

## 质量验证

- **173/173 单元测试** · 零 warning（-Wall/-Wextra/-Wpedantic）+ strict 门禁（-Wshadow/-Wformat=2/-Wundef/-Wconversion/-Wsign-conversion/-Wwrite-strings/-Wpointer-arith/-Wcast-align）
- **fuzz 35 万轮**（随机 + 结构化）UBSan 干净（CI 每轮 2 万）
- **ASan/LSan/TSan/UBSan** 全量 CI 门禁；valgrind 全量 0 errors / 0 leaks
- **压力测试**：100KB 单消息 + 1000 轮对话
- **CI 双 job**：build-test（零 warning + 阈值基准）+ sanitizers（UBSan/ASan/LSan/TSan + fuzz）

## 文档

- [路线图](docs/ROADMAP.md)
- [代码约定](docs/CONVENTIONS.md)
- [内存所有权约定](docs/MEMORY.md)
- [提交前审查清单](docs/CHECKLIST.md)
- [代码地图](code-notes.md)（385 单元全读笔记）

## 开发状态

- M0-M7 全部完成（基础设施→JSON→消息→Provider→增量解析→数据层→无锁管道→trace/文档/MCP/workspace/音频/渲染/网关）
- 2026-08 重构轮：MCP SSE 传输、重试/错误中间件、渲染 JSON 协议、网关多实例、pptx、基准补齐、ASan/LSan 门禁、strict 告警清零、10+ 遗留 bug 修复（TSan race×2、RJsonOut 非 NUL 结尾、冻结消息泄漏、网关连接池残留等）
- 剩余 TODO：audio TTS/ASR 网络测试（需 API key）
