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
