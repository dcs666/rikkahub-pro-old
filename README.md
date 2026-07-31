# rikkahub-pro-old — RikkaHub 核心引擎 C 重写

RikkaHub（rikkahub/rikkahub）AI 客户端核心运行时的 C 重写。目标不是"C 版 Android 应用"，而是**纯 C 的 AI 运行时引擎 + 薄 UI 壳**：

- S 级（换栈质变）：预排版块渲染、零拷贝流式、增量解析、内存常驻数据、无锁流水线
- A 级（引擎化收益）：分支 COW、统一缓存、统一流式传输、进程/内存、网关化、提示词管线
- B 级（顺带）：网络栈、FTS、重试中间件、MCP、音频、workspace、零拷贝协议、文档解析、可观测性、基准
- ✗ 级不碰：功能性 UI、Android 系统能力、安全存储、图像处理、Web API、国际化

来源：/workspace/code-notes.md（385 单元全读代码地图）

## 构建

```bash
make            # 构建全部
make test       # 运行单元测试
make bench      # 运行基准
```

## 目录

```
include/rikka/  # 公共头
src/            # 实现（core/ai/http/util/...）
tests/          # 单元测试（微型框架，无外部依赖）
benchmarks/     # 基准（clock_gettime，输出 ops/s）
docs/           # ROADMAP 等
```
