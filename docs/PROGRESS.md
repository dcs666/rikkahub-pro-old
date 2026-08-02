# RikkaHub pro-old 进度档案（新对话必读）

> 本文件由开发会话持续维护：每完成一个进展点即追加，保证新对话可无缝接续。
> 最后更新：2026-08-02

## 一、仓库与工作流

- **仓库**: dcs666/rikkahub-pro-old（纯 C 引擎 + Android 壳层 monorepo）
  - 引擎: `src/*/*.c`（15 模块）+ `include/rikka/`
  - Android 壳: `android/`（NDK + Kotlin/Compose，包名 **dev.rikkahub.ce**）
- **工作流**: 改代码 → `make clean && make test`（零警告）→ `make strict`（门禁）→ git commit（中文小写前缀）→ push master 触发 CI
- **CI**: build-test（190 测试 + check-bench 阈值）/ sanitizers（ubsan/asan/lsan/tsan）/ android-build（NDK .so + Gradle APK）
- **发布**: tag `vX.Y-ce` 触发 release.yml → 正式 keystore 签名 APK → GitHub Release
  - keystore: GitHub Secrets（RIKKA_CE_KEYSTORE_B64/PASS/ALIAS/KEY_PASS），本地副本 /workspace/rikkahub-ce.keystore + /workspace/keystore_creds.txt
  - 已发布: v0.1.0-ce / v0.2.0-ce / v0.3.0-ce（新包名+正式签名）
- **用户要求**: ①CI 状态约每 2 分钟查一次（勿长 sleep）；②本地只跑小规模（make test/strict/单套件 valgrind），大规模（UBSan 2 万轮 fuzz/TSan）交 CI；③每完善一点记录到本文件

## 二、引擎完成度（对标 JVM 版 v1.0.0-回退版）

- 190/190 测试，strict 零告警，valgrind 0 errors
- 覆盖: 消息模型/COW 会话树/流式累积、JSON 全家族、provider 三家（OpenAI/Claude/Google）流式 + tool_calls 解析（Claude functionCall、Google functionCall 对象、OpenAI 并行多 index）、重试中间件、HTTP 客户端、Markdown 增量/高亮/SPSC、文档（docx/epub/pptx）、workspace/trace/渲染、MCP（SSE/Streamable HTTP/OAuth 2.1）、transformers 管线（10 个）、工具系统（注册表 + 内置工具 + 设备回调）、prompt 6 模板、OCR（rk_ocr_image）、会话索引（rk_chats）、聊天编排循环（rk_chat，含工具循环/取消/think 标签流式拆分）

## 三、Android 壳层完成度

- 功能: 聊天（流式 + 取消 + 推理内容折叠 + 消息复制/重试）、多会话（自动命名/切换/删除/持久化）、Markdown 渲染（代码块/行内代码/粗体/列表）、设备工具（ask_user 对话框/剪贴板/TTS 朗读回复开关/日历/屏幕时间）、会话工具（recent_chats/conversation_search → ChatStore）、workspace 工具（filesDir 沙箱）、OCR（📷 图片识别 → base64 data URI → rk_ocr_image）、暗色主题、自适应图标
- 架构: Engine.kt（JNI 声明）/ DeviceTools.kt / ChatStore.kt / ui/（ChatViewModel/ChatScreen/MarkdownText）
- JNI: engine_jni.c（nativeChat/nativeSetCancel/nativeOcr + 设备/会话工具反调）

## 四、踩坑地图（高频）

- **openssl shim**: NDK 无 ssl.h 且 r23+ 无 libssl 链接库 → android/ndk-include/openssl/*.h 自备声明 + `-Wl,-z,undefs`（须用全局 CMAKE_SHARED_LINKER_FLAGS，AGP 附加 --no-undefined 会覆盖 target_link_options）
- **clang 特有警告**（NDK 比 GCC 严）: 已清零的实例——自赋值/-Wimplicit-int-conversion（uint16_t 窄化）/sign-conversion（char→unsigned char 需显式转换）
- **memcpy(dst, buf.data, buf.len) 空 Buf UB**（UBSan 抓）: 先 len>0 再 memcpy
- **mock 服务器**: 必须真 chunked 帧；do_POST 读 body 后 do_GET 不能二次读 rfile（死锁）→ body 缓存 self._body；/slow 发 tick0-4（http_sse_split_events 断言 5 个）；/chat/completions 按请求体含 "tools" 分派 tool_calls 回放
- **workspace_shell**: 后台进程命令结束即清理 → 起服务+测试+清理须同一条命令；pkill 会误杀 shell 自身
- **GitHub API**: openssl s_client 发原始 HTTP + 重试；secrets 设置需 libsodium sealed box（apt install python3-nacl）
- **Kotlin**: weight 是 RowScope/ColumnScope 成员扩展勿 import；setAutoTts 类命名冲突属性 setter（改名 updateXxx）；LocalContext 不能进非 Composable 回调

## 五、待办/可打磨项

- [x] 设置页完善: 权限引导（日历/屏幕时间）、朗读回复开关、版本信息（2026-08-02）
- [x] 会话标题 LLM 自动生成（RK_PROMPT_TITLE，首轮回复后异步）（2026-08-02）
- [x] WebView JS 工具（WebView eval 落地，主线程+latch 桥 10s 超时）（2026-08-02）
- [x] 图片消息展示（气泡内显示，2026-08-02）
- [ ] 桌面小部件/深链（可选）
- [ ] 正式 keystore 长期轮换提醒
