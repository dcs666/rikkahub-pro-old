# RikkaHub Core Engine (C)

RikkaHub AI 客户端核心运行时的 **纯 C 重写**。目标不是"C 版 Android 应用"，而是**零依赖的高性能 AI 运行时引擎 + 薄 UI 壳**。

## 特性

- **零依赖**：仅系统 C 库 + OpenSSL + zlib（无 libcurl/cmake/第三方框架）
- **高性能**：流式累积比 JVM 版快 **1976x**，打开 1 万消息会话 **2.1ms**（JVM Room+JSON 几百 ms）
- **多 Provider**：OpenAI / Claude / Google 统一流式管线
- **无锁并发**：SPSC 无锁队列 14.4 Mops/s
- **增量解析**：Markdown 流式 O(n²)→O(1)（199KB 文档 378ms→24.3ms）
- **内存安全**：八轮 deep-review，修复 40+ 问题，fuzz 35 万轮 UBSan 干净

## 构建

```bash
make            # 构建全部
make test       # 单元测试 (109/109)
make check      # 零 warning 门禁 + 全测试
make bench      # 性能基准
make cli        # CLI 工具
make fuzz       # 解析器 fuzz (UBSan)
make tsan       # ThreadSanitizer (并发检测)
make check-bench # 基准阈值回归
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

# 性能分析
rikkahub --trace "explain this"
```

## 模块架构

| 模块 | 目录 | 说明 |
|---|---|---|
| 核心 | `core/` | Buf/Arena/消息模型/COW 会话树/流式累积 |
| JSON | `json/` | 值解析/序列化/增量流式提取 |
| Provider | `ai/` | OpenAI/Claude/Google 请求构建 + 统一流式管线 |
| HTTP | `http/` | socket+OpenSSL HTTP/1.1 客户端 (chunked/连接池/缓冲读) |
| Markdown | `markdown/` | 块+行内 AST + 增量解析 |
| 高亮 | `highlight/` | 17 语言手写 lexer |
| 数据层 | `data/` | rbin 二进制存储 (mmap 零拷贝)/LRU/倒排索引 |
| 无锁管道 | `pipe/` | SPSC 无锁环形队列 |
| 文档解析 | `doc/` | docx/epub (zip+XML/XHTML) |
| MCP | `mcp/` | JSON-RPC stdio 客户端 |
| Workspace | `workspace/` | 直 IO (路径穿越防护) |
| 音频 | `audio/` | WAV 封装/TTS/ASR 客户端 |
| 渲染 | `render/` | Markdown→排版块+代码高亮 |
| 可观测性 | `trace/` | span 追踪 |
| 网关 | `gateway/` | epoll HTTP 服务器 + AI 代理 |
| CLI | `tools/` | 命令行客户端 |

## 性能数据

| 指标 | 数值 | 对比 |
|---|---|---|
| 流式累积 | 1976x | JVM 每 token 对象重建 |
| rbin 加载 (1万消息) | 2.1ms | JVM Room+JSON 几百 ms |
| 代码高亮 | 365ns/行 | QuickJS highlight.js µs/行 |
| SPSC 队列 | 14.4 Mops/s | - |
| JSON 增量提取 | 2.8x | 全量解析 |
| MD 流式 (199KB) | 24.3ms (15.5x) | O(n²)→O(1) |

## 质量验证

- **109/109 单元测试** · 零 warning（-Wshadow/-Wformat=2/-Wundef）
- **fuzz 35 万轮**（随机 + 结构化）UBSan 干净
- **泄漏检查**：json/md 0 增长，rbin 平台（glibc 缓存）
- **压力测试**：100KB 单消息 + 1000 轮对话
- **TSan**：无数据竞争（CI）
- **CI 双 job**：build-test（零 warning 门禁 + TLS 冒烟）+ sanitizers（UBSan/fuzz/TSan）

## 文档

- [路线图](docs/ROADMAP.md)
- [内存所有权约定](docs/MEMORY.md)
- [提交前审查清单](docs/CHECKLIST.md)
- [代码地图](code-notes.md)（385 单元全读笔记）

## 开发状态

- M0-M7 全部完成（基础设施→JSON→消息→Provider→增量解析→数据层→无锁管道→trace/文档/MCP/workspace/音频/渲染/网关）
- 八轮 deep-review，修复 40+ 问题
- 剩余 TODO：audio TTS/ASR 网络测试（需 API key）、MCP SSE 完整实现（stdio 已覆盖本地场景，SSE 需 HTTP fd 暴露 + 事件循环）
