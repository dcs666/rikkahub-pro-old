# 内存所有权约定

引擎混用三种内存来源，**必须遵守以下所有权规则**（违反 = 悬垂指针 bug）：

## 1. Arena（区域分配，`rikka/util/arena.h`）
- **所有权**：arena 拥有所有块；`arena_destroy` 一次回收。
- **存活期**：arena 必须活得比其分配的任何对象长。
- **非线程安全**：每个线程/流水线阶段各持一个；跨线程共享须自行加锁。
- **reset 语义**：`arena_reset` 逻辑清空块内存复用；**reset 后所有指向该 arena 的指针失效**。
- 大分配（> 块容量 64KB）自动创建专用块（`arena_alloc` 内部处理）。

## 2. Buf / owned_buf（堆，`rikka/core/buffer.h`）
- `RikkaStream` 流式累积缓冲：**freeze 前归 stream**，`rstream_destroy` 释放。
- `rstream_freeze` 后：**所有权转移给消息**（`msg->owned_buf`），`rstream_destroy` 不再释放它。
- **数据指针**（part->data）指向 buf 内部：**buf 存活期间有效**；buf 被 free 后悬垂。
- 会话存续期（如 rbin 快照加载后的 mmap）内，buf 必须保持存活。

## 3. rbin mmap 区（零拷贝加载，`rikka/data/rbin.h`）
- `rbin_mmap_file` 返回的映射区：**parts->data / tool_name / tool_id 直接指向映射区**（零拷贝）。
- **存活期**：映射区必须活得比所有引用它的消息长；`rbin_munmap` 前必须确认消息不再使用。
- 典型模式：mmap → 加载 → 使用 → 释放（munmap）；**不要让 mmap 消息跨出使用范围**。
- 若需要长存：先序列化复制（rbin_save → 内存 Buf）。

## 4. 规则速查
| 内存 | 谁拥有 | 何时释放 | 释放后 |
|---|---|---|---|
| Arena 块 | arena | arena_destroy | 所有 arena 指针失效 |
| stream buf | stream / freeze 后消息 | rstream_destroy | part->data 失效 |
| mmap 区 | 调用方 | rbin_munmap | parts 数据失效 |
| RkLru 缓存值 | lru | rk_lru_destroy / 淘汰 | get 返回指针失效 |

**经验法则**：解析结果（md 块、json 值）引用输入缓冲——输入缓冲的存活期必须覆盖结果使用期。
