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

### v0.7.8 追加修复(发布前审查发现)
- **重大: JNI 回调线程修复** — 流式 Delta/工具回调由引擎异步流水线
  (pipe_processor 线程)触发, 原用 nativeChat 调用线程的 JNIEnv → 非 Java 线程
  用 JNIEnv = 未定义行为(流式输出可能从未真正回调到 Kotlin!)
  修: jni_thread_env() = GetEnv 优先 + AttachCurrentThread + 回调后 Detach;
  回调开头 ExceptionClear

### 深度审查修复(发布前第二轮)
- Host 头非默认端口必须带端口(自定义端口代理 400)
- baseUrl 尾斜杠去重(https://x/v1/ → /v1/chat/completions 不再双斜杠)
- SSE 单事件 data 8MB 上限(响应头原有 64KB 上限)
- OCR/会话图片 16MB 限制
- 已知限制: rhttp_parse_url 不支持 IPv6 字面量([::1])(场景罕见)

### 当前进度(2026-08-02 深夜, 等待 CI)
- master 最新 81cb044; CI 全绿基线: 58cd3f2 曾 build-test+sanitizers 双绿,
  随后发布前深度审查又修了: JNI 回调线程(重大)/Host 端口/baseUrl 尾斜杠/
  SSE 8MB 上限/图片 16MB 限制/ExceptionClear —— 均已在 53ed1d6/81cb044 提交
- 待 CI(53ed1d6 → 81cb044)全绿 → 打 tag v0.7.8-ce 发布(版本号已升 0.7.8/708)
- 发布验证: APK 下载后 zipfile 检查 librikka.so/libssl.so/libcrypto.so

### 25轮深度优化(发布前第三批) — 全部已提交, 53a76f4 验证中
**重大功能缺口修复(移植丢失):**
- 多模态图片直传: content://(PhotoPicker 主流)/file:/data:/裸路径 全格式进引擎
  (原只传 http URL → 支持 IMAGE 的模型本地图片全部丢失);
  C 引擎加 image_data data:URI 分支; content:// 复制 cacheDir + 16MB 限制
- OcrTransformer content:// 支持(不支持 IMAGE 模型 OCR 流程补齐)
- DocumentAsPromptTransformer content:// 支持(SAF 文档不再丢失; +KoinComponent)
- system prompt 补齐: assistant.systemPrompt/allowConversationSystemPrompt/enableMemory
  记忆注入(buildMemoryPrompt)/contextMessageLimit 截断(limitContext) — 原 CE 版全丢
- search_web JNI 反调桥(DeviceTools.webSearch → SearchService.runBlocking;
  引擎 TOOL_SEARCH_WEB 早已定义但 env->web_search 反调缺失从未注册)
- memory_tool 反调落库(MemoryRepository; assistant_id 经 providerJson;
  useGlobalMemory → GLOBAL_MEMORY_ID)
- **设备工具注册(重大): ask_user/clipboard_tool/text_to_speech/calendar_query/
  get_screen_time/eval_javascript — 反调桥早已存在但从未注册为工具(模型看不到)**
**健壮性:**
- JSON 转义统一 jesc(6 处循环)补 \t/\b/\f/控制字符 \uXXXX(防 Kotlin 解析失败)
- jni_delta 大块不截断(4096 曾丢流式内容)+ malloc 失败 Detach 防线程泄漏
- baseUrl 尾斜杠去重 / Host 头非默认端口带端口 / SSE data 8MB 上限
**确认/待办:**
- 设备工具 6 个全部反调对齐; calendar_create 反调缺失(待办)
- MCP Android 接线待办(引擎 mcp.c 完备, engine_jni 未接线)
- 建议生成/ProviderConnectionTester 仍走 JVM 路径(C 化待办)
- IPv6 URL 不支持(罕见)

### 25轮深度优化(发布前第四批) — 提交中
- calendar_create 工具回归(DeviceTools.calendarCreate 桥: ISO/epoch 时间解析 +
  WRITE_CALENDAR 检查 + ContentResolver.insert)
- ask_user 参数对齐 turbo(questions 数组含 options; 兼容旧单 question;
  多问题完整 UI 待办)
- calendarQuery/screenTimeQuery 参数对齐(begin/end/range 解析; 原忽略参数)
- clipboard_tool 支持 read(env->clipboard_read + DeviceTools.clipboardRead)
- 停止按钮真正生效: 协程取消 → nativeSetCancel(true)(原阻塞 nativeChat 不响应取消)
- 事件消费滚动超时(130s 无事件保护; 多轮工具循环不再中断)
- 确认: get_time_info 输出对齐 / ChatCallback 签名一致 / 注入管线不重复 /
  workspaceCwd 链路一致 / timeout 每轮 120s 语义

### 待办(发布后)
- 系统代理支持: turbo 用 OkHttp 自动走系统代理, C 引擎直连不走代理 → 需 HTTP CONNECT + Android 系统代理读取(JNI)
- MCP 客户端接线: 引擎 mcp.c 完备但未注册为工具(JNI 桥缺失)
- 多会话 g_cancel 全局互扰(现 UI 单会话生成, 风险低)
- ask_user 多问题 UI(引擎已支持 questions 数组, 壳层 UI 未做多问题展示)

### 25轮深度优化(第五批) — 已提交
- 工具白名单开关: localTools/enableMemory/enabledSkills 真正生效(原 tools 参数被忽略)
- web_search/recent_chats 开关: enableWebSearch/enableRecentChatsReference 条件注册
- use_skill 根目录参数化: Android 读 filesDir/skills(原硬编码 /skills 不存在)
- skills 列表注入 system prompt(对齐 turbo createSkillTools)
- 待办记录: 系统代理(HTTP CONNECT)/MCP 接线/多问题 ask_user UI
- 确认: 引擎无重试(对齐 turbo)/SSE 8MB 上限可接受/权限清单齐全

### 25轮深度优化(第六批) — 已提交
- CI 失败修复: rikkahub.c/test_chat.c/test_provider.c RikkaProviderCfg 初始化器补齐
- engine_jni 修复: rjson_get→rjson_obj_get/rjson_is(NDK 编译错误) + 3 处初始化器
- tenv.tool_whitelist 传递(白名单此前未生效)
- tenv.workspace_cwd 设置(workspace_shell 默认在 workspaceCwd 执行, 原为 /)
- 确认: 采样参数/custom_body 测试 / 白名单→tools JSON 自动 / memory 反调条件

### 25轮深度优化(第七批) — 已提交
- per-call 取消: cancel_id 槽表(16槽)替代全局 g_cancel — 多会话并发互不干扰
- ask_user 完整透传 questions 数组(引擎→UI 多问题 + options 快捷选择)
- HTTP 3xx 重定向跟随(最多3次, 对齐 OkHttp) + mock /redirect + 测试
- CI 失败修复: memoryAction non-local return / DeviceTools import / RikkaProviderCfg 初始化器
- 确认: 重定向 Host 头正确/Authorization 跨域保留(信任场景)/304 不跟随

### 25轮深度优化(第八批) — 已提交
- TLS 主机名校验(SSL_set1_host; x509v3.h NDK 缺失 → 改方案)
- buf_append OOM 越界修复(reserve 失败丢弃追加)
- 确认: header 64KB 上限/chunk 行 64B/SSE 8MB/单退出路径清理完整
- CI 失败修复: memoryAction non-local return

### 25轮深度优化(第九批) — 已提交
- DeviceTools.init 接入 RikkaHubApp(重大: 设备工具反调 context 此前从未初始化, 全部失效)
- 确认: 线程模型(引擎线程→mainHandler桥)/TTS懒加载/工具结果格式

### 25轮深度优化(第十批) — 已提交
- askUser 用 Activity context(Dialog 需窗口 token; RikkaHubApp 生命周期跟踪 currentActivity)
- memory 反调 assistant_id per-call 传递(ud→JniCb.mem_aid; 原全局 jni_ctx_mem_aid 多会话竞态)
- 确认: 工具反调链路(env->ud)/内存/引用清理

### 25轮深度优化(第十一批) — 已提交
- ndk-include shim 补 SSL_set1_host/X509_V_OK(NDK 编译失败修复)
- 确认: NDK 用 shim 头(非系统 openssl)/链接真 OpenSSL 3 so/SONAME 无版本

### 🎉 v0.7.8-ce 已发布(2026-08-03)
- Release: rikkahub-ce-0.7.8-ce.apk(44MB, 1497 条目, 双 ABI arm64+x86_64)
- 验证: zip 完整/签名通过(CI apksigner)/librikka+libssl+libcrypto 齐全
- 含全部修复: 设备工具 context/主机名校验/重定向/per-call 取消/翻译对齐/白名单 等

### TLS 证书验证增强(v0.7.9) — 用户反馈修复
- 用户反馈: v0.7.8 生成报 connect/TLS failed: certificate verify failed(DeepSeek)
- 根因: 手机系统 CA 缺 TrustAsia 2025 新根(系统更新慢); 沙箱系统 CA 验证 OK
- 修复: 加载用户 CA(/data/misc/user/0/cacerts-added) + PARTIAL_CHAIN + X509 错误码透传
- 版本 0.7.9/709

### TLS 精准修复 v0.7.10(bc53443) — 2026-08-03
- 用户反馈错误码 19 (self-signed certificate in certificate chain) = 信任库空/缺根
- 根因: 手机系统 CA 目录加载失败(路径/权限) → VERIFY_PEER + 空信任库 → 全 TLS 失败
- 修复: 内置 DigiCert Global Root G2(DER, d2i_X509)兜底 + VERIFY_PEER 恒启用
- 验证(沙箱): 空 CA 目录 + 内置根 → DeepSeek 握手 PASS; 系统 CA + 内置根共存 → google/deepseek 双 PASS
- 诊断: 握手失败报告 trust store 证书数(用户可见精准定位)
- 顺带: Claude 多 content_block 工具槽(index)/Google 适配新槽/symlink 逃逸防护/BOM 跳过
- 版本 0.7.10/710

### 🎉 v0.7.10-ce 已发布(2026-08-03)
- APK rikkahub-ce-0.7.10-ce.apk(44MB, 1497 条目, 双 ABI)验证通过
- TLS 修复收官: 内置 18 根 + VERIFY_PEER 恒启用 + trust store 计数诊断
- 用户验证: 安装后重试 DeepSeek; 仍失败则日志带 trust store: N certs 可精准定位
- 发布中记录(7223900)
- CI 全绿(build-test/sanitizers/android-build); tag v0.7.10-ce 已推, Release 构建中
- 内置信任根 18 个主流 CA(DigiCert G2/G3/ISRG X1/X2/GlobalSign R3/R6/USERTrust/
  Amazon R1-R4/Starfield/GoDaddy/CFCA/Entrust/GTS R1/R3/R4)
- 模拟空信任库(系统 CA 加载失败)验证: DeepSeek/OpenAI/Anthropic/Google/Moonshot 全 PASS
- 诊断: 握手失败报 trust store 证书数(用户可见)
- 顺带: Claude 多 content_block 工具槽/symlink 逃逸防护/BOM/SSL WANT_*

### v0.7.11 — dlopen 崩溃修复(用户反馈 v0.7.10)
- 崩溃: UnsatisfiedLinkError cannot locate symbol "sk_X509_OBJECT_num" (librikka.so)
- 根因: sk_X509_OBJECT_num 是 OpenSSL 内部宏, libcrypto.so 不导出; shim 声明为函数
  → -z undefs 允许未定义引用 → 运行时 dlopen 失败
- 修复: 诊断改用 CA 源状态(system CA: ok/failed, user CA: ok/none/failed, builtin: N)
- 验证: shim 头编译全引擎文件, nm 审计 22 个未定义 OpenSSL 符号全部导出
- 版本 0.7.11/711

### 🎉 v0.7.11-ce 已发布(2026-08-03)
- APK rikkahub-ce-0.7.11-ce.apk(44MB)验证通过(双 ABI/zip 完整)
- dlopen 崩溃修复(sk_X509_OBJECT_num 未导出符号) + 全引擎符号审计
- 用户验证: 安装后库正常加载; DeepSeek 走内置 18 根
- 后续新提交(1b96f16): 会话索引接线(recent_chats/conversation_search 数据源)
- 发布中记录(de628c0)
- CI 全绿; tag v0.7.11-ce 已推, Release 构建中
- 核心修复: 移除未导出符号 sk_X509_OBJECT_num(用户 v0.7.10 崩溃:
  UnsatisfiedLinkError cannot locate symbol) → 诊断改用 CA 源状态
- 全引擎文件 nm 审计: 22 个未定义 OpenSSL 符号全部导出(防再崩)
- 顺带: JSON 转义全面审计(jstrz_buf/chats_search/tool_result_error/tool_result_json)
- 版本 0.7.11/711

### v0.7.12 候选(f5bf34e, CI 全绿, 未发布)
- 🔴 消息膨胀修复: 用户反馈"一个对话28次回答" — 根因 emitChunk 每次 delta
  构造新 UIMessage(random id) → updateCurrentMessages 快速路径失效 → 慢速路径
  add → 节点内消息版本无限膨胀(24/28 分页 = 28 个版本);
  修复: 每次 generateText 生成稳定 assistantMsgId, 流式期间原地更新
- 会话索引接线(1b96f16): ChatStore.init + saveConversation 索引钩子
  (recent_chats/conversation_search 工具数据源; 此前恒空)
- ChatStore 索引上限(45957e4): 会话 200/消息 500 防 prefs 膨胀

### 🎉 v0.7.12-ce 发布中(e85b241, 2026-08-03)
- CI 全绿(含 Kotlin 编译验证); tag v0.7.12-ce 已推, Release 构建中
- 核心: 消息膨胀修复(稳定 id) + 会话索引接线 + ChatStore 上限
- 新增: 输入变换接线(模式注入/lorebook/模板/OCR/文档/时间提醒全部激活)
  + 输出正则替换接线(assistant.regexes)
- 版本 0.7.12/712

### v0.7.13 候选(性能修复, 用户反馈"生成特别卡/经常卡死")
- 🔴 根因1: 流式 text O(n²) — CE 版每 delta last.copy(text+delta) O(n) 复制整段,
  长回答累计 O(n²) → IO 忙 + GC 压力(对照 turbo 有 [TURBO] O(n²) 缓解, CE 缺);
  修复(2044ac7): StringBuilder 累积 O(1) + 32ms 降频 flush
- 🔴 根因2: 消费循环在 Main 线程(flow 块默认收集线程) → 每 token 处理占主线程;
  修复(4e2f441): flowOn(Dispatchers.IO), Main 只收 30fps sample 结果
- 其余: 工具执行期无事件(有 Tool loading 显示, 可接受);
  引擎 120s×8 轮上限(每轮有事件, 可接受)
- 版本 0.7.13/713 待发

### 🎉 v0.7.13-ce 发布中(c3c70b3, 2026-08-03)
- CI 全绿; tag v0.7.13-ce 已推, Release 构建中
- 性能修复(用户反馈"生成特别卡/经常卡死", 对照 turbo 源码):
  ① 流式 text O(n²) 缓解 — StringBuilder + 32ms 降频 flush(对齐 turbo [TURBO] 优化)
  ② generateText 消费循环 flowOn(IO) — 主线程不再被生成占用
  ③ pipe_reader 5s 轮询 — 停止按钮在无数据期 ≤5s 生效(原最长 120s)
  ④ Gradle 网络超时 180s — 防 jitpack 快照解析超时(2bd4b8c 曾因 jitpack 30s 超时失败)
- 版本 0.7.13/713

### v0.7.14 候选(功能对齐, 2026-08-03)
- 对照 turbo 功能清单全面排查, 发现并修复 4 个差异:
  ① 自定义 HTTP 头(聊天): 引擎新增 RikkaProviderCfg.custom_headers(JSON 对象, 全 provider)
  ② 自定义 HTTP 体(聊天): 引擎扩展 Claude/Google + Kotlin 合并 assistant/model customBodies
  ③ MCP 工具不可用: 外部工具通道(引擎合并 JVM tools 定义 + 注册表未命中
     JNI 反调 DeviceTools.executeTool → Tool.execute) — 修复 MCP/workspace 等 JVM 工具
  ④ workspace_shell 不可用: 引擎内置需 workspace_root(未传) + Kotlin 侧被
     engineBuiltin 排除 → 双重丢失; 修复: 排除列表移除 workspace_*(走 JVM proot 通道)
     + engine_jni 不注册引擎内置 workspace(防重复)
- 待办: 外部工具审批流程(workspace/MCP 需确认)/外部工具超时/工具结果截断/
  streamOutput 非流式

- v0.7.14 追加修复:
  ⑥ Claude/Google 请求 tools 定义(此前仅 OpenAI 发送; Claude {name,description,
     input_schema} / Google {functionDeclarations} 格式转换 + 2 测试)
  ⑦ 外部工具输出截断(workspace_shell/MCP 超长结果, 32KB/4KB 对齐 turbo)
  ⑧ custom_body 字符串转义修复(反斜杠丢失致 Kotlin 语法错误)
  ⑨ workspace 工具修复(双重排除 → JVM proot 通道)
- 测试数: 200 → 205

### v0.7.14 CI 状态(2026-08-03 12:2x, 提交 284988d)
- 引擎 205 测试(零告警)✅ / UBSan ✅ / ASan ✅ / TSan ✅ / NDK librikka.so ✅
- ⏳ Assemble debug APK(Gradle)已 60 分钟未完成 — 网络高峰(jitpack/pnpm),与 v0.7.13 经验一致
- 全绿后: tag v0.7.14-ce → release.yml 自动构建 APK → 验证双 ABI/zip/签名 → 通知用户
- v0.7.14-ce 内容汇总:
  性能(v0.7.13): 流式 O(n²) 修复/StringBuilder+32ms 降频/flowOn IO/pipe_reader 5s 轮询取消
  功能对齐(v0.7.14): 自定义 HTTP 头(引擎 custom_headers 全 provider)/
  自定义 HTTP 体(引擎扩展 Claude+Google)/MCP 工具透传(外部工具通道: 定义合并+
  JNI 反调 DeviceTools.executeTool)/workspace 工具修复(JVM proot 通道)/
  工具输出截断(32KB/4KB 对齐 turbo)/Claude+Google 请求 tools 定义(格式转换)
- 测试数 200 → 205(custom_body×2 + custom_headers + tools_claude + tools_google)
- 踩坑记录: python heredoc 写 Kotlin 字符串时反斜杠会被吃掉(append("\":") 变
  append("":") 致 Kotlin 语法错误 185:56) — 写入后用 od -c 验证字节!
