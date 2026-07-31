# C 引擎提交前审查清单

每笔 commit 前逐项自检（本清单来自 M0-M6 全部踩坑）。

## 编译与测试
- [ ] `make clean && make test` 全绿（**必须 clean**：增量编译会漏掉头文件变更）
- [ ] 编译输出 **零 warning**（`-Wall -Wextra -Wpedantic`）
- [ ] `make bench` 关键数字无异常回归（对照 docs 基准表）

## 易错点（全部踩过）
1. **snprintf 缓冲区**：`char tmp[N]` 必须 ≥ 格式串 + 参数展开长度；截断时**返回值是"应该写"的长度**——按返回值 append 会越界读栈垃圾。修法：缓冲给足（64+），或改用 `snprintf` 返回值前先查 `n < sizeof`。
2. **手写长度**：字符串长度一律 `strlen()`/`sizeof()-1`，**禁止手数**（本次错 5 次）。
3. **结构体布局**：改 `.h` 后**所有引用它的 .o 必须重编译**——`make clean` 兜底（Makefile 已加 `-MMD -MP`，但新依赖首次仍需 clean）。
4. **内存序（并发）**：共享标志（closed/head/tail）用 `__atomic_*`；**先读 closed 再读 head**（release 传递性防丢数据）；head/tail 单调不取模。
5. **UTF-8**：中文按字符边界处理（bigram 按字符非字节）；`strlen` 中文是多字节。
6. **零拷贝指针**：指向输入缓冲的指针无 NUL 结尾——测试用 `memcmp+len`，别用 `strcmp`。
7. **python 批量改 C 源码**：`\\n` 转义会破坏字符串——改完必须编译验证；优先 `workspace_edit_file` 或原始字符串。
8. **缓冲区溢出**：栈数组容量按最坏情况算（长会话/大文本），写越界由 stack-protector 兜底但别依赖。
9. **测试隔离**：fork 的子进程用 SIGKILL + 随机端口；`pkill -f` 会匹配自身命令行（用 `[x]` 技巧或避免）。
10. **fence/大块流式**：增量解析快速路径只追加尾部，闭合时全量重建（md fence 已实现）。

## 提交前必跑
```
make clean && make test          # 全绿 + 零 warning
make bench                        # 关键数字
./build/fuzz_parsers 20000        # 解析器 fuzz（UBSan 构建）
```
