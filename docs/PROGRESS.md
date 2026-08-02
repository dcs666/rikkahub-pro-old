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
- [x] 会话标题手动编辑（会话列表改名按钮+对话框）（2026-08-02）
- [x] 正式 keystore 长期轮换提醒——密钥已存 GitHub Secrets + 本地副本

## 六、最近提交记录（2026-08-02 夜间）

### UI 全面打磨（用户指示"先把 UI 全都做好，不计时间和 token"）
- 主题系统: 三态切换（跟随系统/浅/深）+ 完整品牌配色（primary/secondary/tertiary/error/container/outline 全套）
- Markdown 增强: 块解析（代码块语言标签+复制/标题1-3/引用竖线/段落）、斜体、链接（蓝色下划线+StringAnnotation）、行内代码背景
- 消息气泡: 工具调用卡片（⚙️ 名称+参数/结果折叠+失败高亮+✓）、时间戳 HH:mm、错误重试按钮、流式闪烁光标（infiniteTransition）、长按菜单（复制/朗读）
- 输入栏: 图片附件预览条（缩略图+✕）、IME 回车发送、OCR 队列化（图文并行发送）
- 会话管理: 侧滑抽屉（ModalNavigationDrawer，列表项时间/当前加粗/✏️改名/🗑删除/新建）
- 设置页: 全屏化（Dialog 全宽）+ 分组（Provider/通用/权限/关于）+ 字体大小滑杆 12-20sp + 清空所有数据（确认框）
- 空状态: 欢迎页（品牌引导+4 个示例快捷发送）
- 踩坑: material3 无 Dialog 符号→用 androidx.compose.ui.window.Dialog；
  import 换行粘连（"BuildConfigimport"）多轮修复；inlineMarkdown 调 @Composable 需参数化颜色

### 提交记录
- 59e415b: import 换行粘连修复
- 0be6e4b/a36ad2d: 流式光标 + 长按菜单
- 496127d: 空状态欢迎页
- 85880d9: Dialog 换 core 库 → **CI 全绿**
- ce23cda→4ec0f5a→8f72cfc: 设置页全屏化 + import 区重建多轮修复
- 11c5ae2: 工具卡片 + 时间戳
- 5525c23: Markdown 渲染增强
- 7f62db1: 主题系统
- 3d8ae1e: 会话重命名
- 27df2f0: WebView JS 工具（DeviceTools.javascriptEval）
- f45f3a1/ffb2120/a66bf32: Kotlin 编译错误逐轮清零（HorizontalDivider material3、
  BuildConfig import、ChatMsg.imagePath 补回、DeviceTools/Context import）
- a7f0aab: 会话标题 LLM 自动生成（nativeGenerateTitle + RK_PROMPT_TITLE）
- e86ebfe: 设置页权限引导 + 版本信息（BuildConfig.VERSION_NAME）
- e878e98: 图片消息展示（ChatMsg.imagePath + BitmapFactory）
- 并行会话（非本会话）: Claude/Google tool_calls、OpenAI 并行调用、
  UI 推理折叠/复制/重试、rk_chat_think_feed、versionCode 3/0.3.0

- 图片直发多模态: parse_history 支持 image_path → base64 data URI → IMAGE part；sendImage 图文一条消息（并行覆盖恢复踩坑: sendImage/MessageBubble vm 参数被覆盖需补回）
- 消息删除（长按菜单）/清空会话确认/切换会话自动滚底
- 踩坑补充: arena_alloc 是 3 参 (a, align, size) 不是 2 参；import 换行粘连（BuildConfigimport）；DropdownMenu 需 material3 import

- **release v0.5.0-ce 发布成功** (2026-08-02): UI 全面打磨版 — 主题三态/Markdown增强/工具卡片/侧滑抽屉/全屏设置/附件预览/流式光标/空状态/图片直发多模态/图片持久化/顶栏⋮菜单


## 🔄 UI 复刻 turbo（用户指示"UI 要和 rikkahub-turbo 一模一样，可以移植"）
- 已整体移植 turbo 多模块工程到 android/（app/ai/search/speech/highlight，AGP 9.3.1/Kotlin 2.4.10/KSP/gradle 9.5.0 wrapper/compose BOM 2026.06.01/material3 1.5.0-alpha25）
- applicationId=dev.rikkahub.ce（与 turbo dev.nebula.turbo 区分），签名走 RIKKA_CE_* secrets
- 纯 C 引擎已集成：CMakeLists/ndk-include/engine_jni.c + JNI 桥（dev.rikkahub.ce.Engine/DeviceTools/ChatStore）+ proguard keep
- **替换点（进行中）**: data/ai/GenerationHandler.kt（629 行 Kotlin 生成循环）→ 内部改 JNI 调 C 引擎（rk_chat 编排循环）
- 旧壳备份: /workspace/android_backup/android_old
- 踩坑: AGP 9.3.1 需 gradle 9.5.0（CI 已改 wrapper）；R8 需 keep JNI 类

- **1a2fda5: android-build 全绿 — turbo UI 工程 + C 引擎集成构建成功!** (2026-08-02)
  - 10 模块 + web-ui 前端 + material-color-utilities submodule 全部就位
  - 待办: GenerationHandler.kt C 化 (JNI 调 rk_chat)

- **4533e5e: GenerationHandler C 化提交** (2026-08-02)
  - generateText 内部改 JNI 调 C 引擎 (rk_chat): 消息/配置序列化 JSON,
    回调事件 channel 桥 → UIMessagePart 组装 → 流式 emit
  - translateText 同步 C 化
  - C 侧 parse_history 支持 image_url (http 图片直传)
  - 待验证: CI 编译 + 真机行为

- **46b845b: CI 全绿 — GenerationHandler C 化完成!** (2026-08-02)
  - generateText/translateText 内部走 JNI → C 引擎 rk_chat
  - turbo UI(5.9万行) + C 引擎完整构建成功
  - v0.6.0-ce 的 Release 失败待重发 (签名流程已改为 local.properties)

- **73a2db8: CI 全绿 — OCR/标题生成 C 化完成** (2026-08-02)
  - OcrTransformer.performOcr → Engine.nativeOcr (rk_ocr_image)
  - ChatService 会话标题 → Engine.nativeGenerateTitle
  - ProviderExt.kt 公共 baseUrlOr/apiKeyOr 扩展
  - v0.7.0-ce 发布中 (turbo UI + C 引擎全链路)

- **v0.7.0-ce 发布成功!** (2026-08-02)
  - turbo UI(5.9万行) + 纯 C 引擎(JNI)全链路 APK
  - 发布流程修: splits 输出 APK 名 (app-universal-release.apk)
  - 下载: https://github.com/dcs666/rikkahub-pro-old/releases/tag/v0.7.0-ce

- **v0.7.1-ce 发布成功!** (2026-08-02) 修复真机崩溃
  - 崩溃: SQLiteException dlopen libsimple.so not found (coil MemoryCacheService 加载 SQLite 扩展)
  - 根因: .gitignore 的 *.so 把 jniLibs/libsimple.so 排除, APK 缺库
  - 修复: 强制提交 libsimple.so (arm64/x86_64) + .gitignore 例外规则

- **1b5c361: CI 全绿 — 助手无回复诊断修复** (2026-08-02)
  - 用户反馈: 消息能发但助手不回复
  - 修复: provider 日志(掩码 apiKey)/nativeChat 返回校验(补发 Finish)/90s 超时/空配置提前报错
  - 待发 v0.7.2 诊断版, 用户复测 + logcat
- 轮询间隔: 140 秒 (用户指定)

- **v0.7.2-ce 发布成功!** (2026-08-02) 助手无回复诊断版
  - provider 日志/返回校验/90s 超时/空配置提前报错
  - 等待用户安装复测 + logcat

- **v0.7.3-ce 发布成功!** (2026-08-02) 诊断日志入 App 内日志页
  - GenerationHandler 日志双写 (android.util.Log + turbo Logging)
  - 用户免 adb: 设置 → 日志 直接查看诊断信息

- **v0.7.4-ce 发布成功!** (2026-08-02) 根因修复版
  - **R8 keep JNI 引擎类 (dev.rikkahub.ce.Engine/ChatCallback)** — 根治"助手不回复 + 标题失败"
    - 根因: release isMinifyEnabled=true 把 Engine 类裁剪 → NoClassDefFoundError
  - 日志页 TextLog 可点击查看详情
  - generateTitle 异常写入 App 日志页

- **v0.7.5-ce 发布成功!** (2026-08-02) APK 内置 OpenSSL 3 — 根治 SSL_new
  - 根因: librikka.so 链接 NDK stub libssl(仅符号), 运行时靠系统 BoringSSL;
    国产 ROM 系统库无 SSL_new → UnsatisfiedLinkError → Engine 类初始化失败
  - 修复: android/scripts/build_openssl.sh — CI 编译 OpenSSL 3.0.13
    (arm64-v8a + x86_64, SONAME 去版本号) 进 jniLibs; release.yml/ci.yml 调用
  - 踩坑: GitHub archive 解压目录名 openssl-openssl-3.0.13; no-docs 是 3.1+ 选项

- **v0.7.7-ce 发布成功!** (2026-08-02) SSL_CTX_set_min_proto_version 宏修复
  - 根因: OpenSSL 3.0 中 set_min/max_proto_version 是宏(SSL_CTRL_SET_MIN_PROTO_VERSION
    =123 → SSL_CTX_ctrl), libssl.so 不导出; shim 头误声明为函数 → dlopen 失败
  - 修复: ndk-include/openssl/ssl.h 声明 SSL_CTX_ctrl + 按官方宏定义
  - 依赖链已完整: R8 keep ✓ + DT_NEEDED libssl/libcrypto ✓ + 符号全部可解析 ✓

### 已知限制(待办)
- engine_jni.c 的 g_cancel 是全局变量: 多会话并发生成时, 取消一个会话会影响另一个
  (v0.7.8 记录; 修复方案: nativeChat 返回会话 token, nativeSetCancel(token, cancel))

### v0.7.8 待发布 — 功能对齐 turbo(思考模式 + 用量统计 + 请求日志)
- 思考模式: GenerationHandler 按 turbo host 分派(DeepSeek XHIGH→max/MEDIUM→high、
  Moonshot thinking、NVIDIA、opencode、默认) → C 引擎 rp_build_request 写入
- Token 用量: C 引擎解析流式 usage(OpenAI 顶层/Anthropic message_delta) +
  cached_tokens(prompt_tokens_details) → JNI 返回 → UIMessage.usage
- 请求日志: stats 加 http_status/duration_ms → JNI request 对象 →
  GenerationHandler 写 Logging(日志页可见)
- 测试: reasoning_body / stream_usage 双协议
- 修: SSE data 非 NUL 结尾 strstr 越界(ASan 抓到)→ contains_usage
- 功能差异已清零(除: Moonshot keep:all 未传、NVIDIA 非 v4 模型简化、reasoning_tokens 未解析)

### 审查发现(2026-08-02 优化轮)
- **重大: OpenAI 请求体 tools 在顶层 } 之后拼接 → 非法 JSON**(潜伏, 真实 provider
  带工具必 400; mock 不校验 JSON 未暴露) → 已修 + 回归测试 build_openai_tools_json_valid
- strstr 对非 NUL 结尾 SSE data 越界读(ASan 抓到) → contains_usage
- reasoningArgs AUTO 与 turbo 不一致(NVIDIA/opencode AUTO 误写 effort) → 已修
- 图片 MIME 魔数探测(jpeg/gif/webp/heic 替代硬编码 png)
- OCR 失败透传引擎错误详情
- 已知低风险: workspace resolve_path 不做 symlink 解析(字符串段级拦截已够用);
  ProviderConnectionTester 走 Kotlin 路径(待办 C 化)

### v0.7.8 发布内容汇总(等待 CI 全绿)
1. 思考模式 reasoning_effort/thinking(host 分派对齐 turbo)
2. Token 用量 usage + cached_tokens(OpenAI/Anthropic 流式解析)
3. 请求日志摘要(status/耗时/tokens)入日志页
4. 错误详情透传(TLS/连接/HTTP/服务端 message)
5. **tools JSON 非法修复(重大潜伏 bug)**
6. strstr 越界修复(ASan)/MIME 魔数/OCR 错误详情/AUTO 修复
7. ci.yml concurrency cancel-in-progress
