package me.rerere.rikkahub.data.ai

import android.content.Context
import android.util.Log
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.flow
import kotlinx.coroutines.flow.flowOn
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeoutOrNull
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.put
import me.rerere.ai.core.MessageRole
import me.rerere.ai.core.ReasoningLevel
import me.rerere.ai.core.TokenUsage
import me.rerere.ai.core.Tool
import me.rerere.common.android.Logging
import me.rerere.ai.provider.Model
import me.rerere.ai.provider.ProviderManager
import me.rerere.ai.provider.ProviderSetting
import me.rerere.ai.registry.ModelRegistry
import me.rerere.ai.ui.ToolApprovalState
import me.rerere.ai.ui.UIMessage
import me.rerere.ai.ui.UIMessagePart
import me.rerere.ai.ui.limitContext
import me.rerere.rikkahub.data.ai.transformers.InputMessageTransformer
import me.rerere.rikkahub.data.ai.transformers.OutputMessageTransformer
import me.rerere.rikkahub.data.datastore.Settings
import me.rerere.rikkahub.data.datastore.findModelById
import me.rerere.rikkahub.data.datastore.findProvider
import me.rerere.rikkahub.data.repository.MemoryRepository
import me.rerere.rikkahub.utils.applyPlaceholders
import me.rerere.rikkahub.data.model.Assistant
import me.rerere.rikkahub.data.model.AssistantMemory
import me.rerere.rikkahub.data.model.replaceRegexes
import dev.rikkahub.ce.Engine
import dev.rikkahub.ce.ChatCallback
import org.json.JSONArray
import org.json.JSONObject
import java.util.Locale
import java.util.concurrent.atomic.AtomicInteger
import kotlin.uuid.Uuid

private const val TAG = "GenerationHandler(CE)"

/** 日志双写：android.util.Log + turbo Logging（设置页日志页可见） */
private fun logMsg(tag: String, msg: String) {
    android.util.Log.i(tag, msg)
    me.rerere.common.android.Logging.log(tag, msg)
}
private fun logErr(tag: String, msg: String, e: Throwable? = null) {
    android.util.Log.e(tag, msg, e)
    me.rerere.common.android.Logging.log(tag, msg + (e?.let { " | " + it.message } ?: ""))
}

/** [CE] content:// URI 复制到 cacheDir（引擎 fopen 读不了 content 协议）。
 *  返回绝对路径；超大图(>16MB)/解析失败返回 null。 */
private fun resolveContentToFile(context: Context, uri: String): String? = runCatching {
    val u = android.net.Uri.parse(uri)
    val size = context.contentResolver.query(
        u, arrayOf(android.provider.MediaStore.MediaColumns.SIZE), null, null, null,
    )?.use { c -> if (c.moveToFirst()) c.getLong(0) else -1L } ?: -1L
    if (size > 16 * 1024 * 1024) return null
    val f = java.io.File(context.cacheDir, "chat_img_${System.currentTimeMillis()}_${(0..99999).random()}")
    context.contentResolver.openInputStream(u)?.use { input ->
        f.outputStream().use { output -> input.copyTo(output) }
    }
    f.absolutePath
}.getOrNull()

/* C 引擎回调事件（文件级，Kotlin 不允许局部 interface） */
private sealed interface Evt {
    class Delta(val kind: Int, val text: String) : Evt
    class ToolCall(val name: String, val args: String) : Evt
    class ToolResult(val name: String, val result: String) : Evt
    class Finish(val ok: Boolean, val err: String?) : Evt
}

sealed interface GenerationChunk {
    data class Messages(
        val messages: List<UIMessage>
    ) : GenerationChunk
}

/**
 * [CE] 生成处理器：内部实现为 C 引擎（rk_chat 编排循环，JNI 桥）。
 * 保留原签名（ChatService 调用方无感），输入输出与 UI 层模型保持一致。
 */
class GenerationHandler(
    private val context: Context,
    private val providerManager: ProviderManager,
    private val json: Json,
    private val memoryRepo: MemoryRepository,
    private val skillManager: me.rerere.rikkahub.data.files.SkillManager,
) {
    private val cancelSeq = AtomicInteger(0)
    fun generateText(
        settings: Settings,
        model: Model,
        messages: List<UIMessage>,
        inputTransformers: List<InputMessageTransformer> = emptyList(),
        outputTransformers: List<OutputMessageTransformer> = emptyList(),
        assistant: Assistant,
        memories: List<AssistantMemory>? = null,
        tools: List<Tool> = emptyList(),
        maxSteps: Int = 256,
        processingStatus: MutableStateFlow<String?> = MutableStateFlow(null),
        conversationSystemPrompt: String? = null,
        conversationModeInjectionIds: Set<Uuid> = emptySet(),
        conversationLorebookIds: Set<Uuid> = emptySet(),
        workspaceCwd: String? = null,
    ): Flow<GenerationChunk> = flow {
        val provider = model.findProvider(settings.providers) ?: error("Provider not found")
        val baseUrl = provider.baseUrlOr().trimEnd('/')
        val apiKey = provider.apiKeyOr()
        val modelId = model.modelId
        logMsg(TAG, "generateText: model=${model.modelId} messages=${messages.size} " +
            "baseUrl=$baseUrl apiKey=${if (apiKey.isBlank()) "BLANK" else apiKey.take(4) + "***"} " +
            "systemPrompt=${conversationSystemPrompt?.take(50)}")
        if (baseUrl.isBlank()) error("Provider base URL is empty: ${provider.name}")
        if (apiKey.isBlank()) error("Provider API key is empty: ${provider.name}")
        processingStatus.value = "生成中…"

        // ---- 组装 C 引擎输入 ----
        val cancelId = cancelSeq.incrementAndGet().toLong()
        val providerJson = JSONObject()
            .put("base_url", baseUrl)
            .put("api_key", apiKey)
            .put("model", modelId)
            .put("cancel_id", cancelId)
        // skills 根目录(use_skill 工具读取; Android: filesDir/skills)
        providerJson.put(
            "skills_root",
            java.io.File(context.filesDir, "skills").absolutePath,
        )
        // 工具白名单(对齐 turbo 的 localTools/enableMemory/enabledSkills 开关)
        providerJson.put("tool_whitelist", buildToolWhitelist(assistant))
        // 搜索/会话引用开关(对齐 turbo: enableWebSearch / enableRecentChatsReference)
        providerJson.put("enable_web_search", assistant.enableWebSearch)
        providerJson.put("enable_recent_chats", assistant.enableRecentChatsReference)
        // 记忆库目标(对齐 turbo: enableMemory 时 memory_tool 反调落库)
        if (assistant.enableMemory) {
            providerJson.put(
                "assistant_id",
                if (assistant.useGlobalMemory) MemoryRepository.GLOBAL_MEMORY_ID
                else assistant.id.toString(),
            )
        }
        // 思考模式(与 turbo 的 ai 模块逻辑对齐: DeepSeek/Moonshot/NVIDIA/opencode/默认)
        val (effort, thinking) = reasoningArgs(baseUrl, assistant.reasoningLevel)
        if (effort != null) providerJson.put("reasoning_effort", effort)
        if (thinking) providerJson.put("thinking", true)
        // 自定义请求头/体(对齐 turbo: assistant.customHeaders/customBodies + model.customHeaders/customBodies
        // 合并; 引擎按 provider 格式追加到请求)
        val extraHeaders = buildList {
            addAll(assistant.customHeaders)
            addAll(model.customHeaders)
        }
        if (extraHeaders.isNotEmpty()) {
            val hj = JSONObject()
            extraHeaders.forEach { hj.put(it.name, it.value) }
            providerJson.put("custom_headers", hj.toString())
        }
        val extraBodies = buildList {
            addAll(assistant.customBodies)
            addAll(model.customBodies)
        }
        if (extraBodies.isNotEmpty()) {
            val sb = StringBuilder()
            extraBodies.forEachIndexed { i, cb ->
                if (i > 0) sb.append(',')
                sb.append('"').append(cb.key).append("":").append(cb.value)
            }
            providerJson.put("custom_body", sb.toString())
        }

        val history = JSONArray()
        // ---- system prompt(对齐 turbo: assistant.systemPrompt + 会话覆盖 + 记忆注入) ----
        val systemBuilder = StringBuilder()
        val effectiveSystemPrompt =
            if (assistant.allowConversationSystemPrompt && !conversationSystemPrompt.isNullOrBlank())
                conversationSystemPrompt
            else
                assistant.systemPrompt
        if (!effectiveSystemPrompt.isNullOrBlank()) systemBuilder.append(effectiveSystemPrompt)
        if (assistant.enableMemory && !memories.isNullOrEmpty()) {
            systemBuilder.append(buildMemoryPrompt(memories!!))
        }
        // skills 列表注入(对齐 turbo createSkillTools 的 systemPrompt)
        if (assistant.enabledSkills.isNotEmpty()) {
            val available = skillManager.listSkills().filter { it.name in assistant.enabledSkills }
            if (available.isNotEmpty()) {
                systemBuilder.appendLine()
                systemBuilder.append("**Skills**")
                systemBuilder.appendLine()
                systemBuilder.append(
                    "You have access to the following skills. Use the `use_skill` tool to " +
                        "load a skill's instructions when the user's request matches.",
                )
                systemBuilder.appendLine()
                systemBuilder.append("<available_skills>")
                systemBuilder.appendLine()
                available.forEach { skill ->
                    systemBuilder.appendLine("  <skill>")
                    systemBuilder.appendLine("    <name>${skill.name}</name>")
                    systemBuilder.appendLine("    <description>${skill.description}</description>")
                    systemBuilder.append("  </skill>")
                    systemBuilder.appendLine()
                }
                systemBuilder.append("</available_skills>")
            }
        }
        if (systemBuilder.isNotBlank()) {
            history.put(JSONObject().put("role", "system").put("content", systemBuilder.toString()))
        }
        // ---- 输入变换(模式注入/lorebook/模板/workspace 提醒; 对齐 turbo) ----
        var inputMsgs = messages
        if (inputTransformers.isNotEmpty()) {
            val tctx = me.rerere.rikkahub.data.ai.transformers.TransformerContext(
                context = context,
                model = model,
                assistant = assistant,
                settings = settings,
                conversationModeInjectionIds = conversationModeInjectionIds,
                conversationLorebookIds = conversationLorebookIds,
                processingStatus = processingStatus,
                workspaceCwd = workspaceCwd,
            )
            for (t in inputTransformers) {
                inputMsgs = t.transform(tctx, inputMsgs)
            }
        }
        // ---- 上下文截断(limitContext, 对齐 turbo) ----
        val effectiveMessages = inputMsgs.limitContext(assistant.contextMessageLimit)
        for (m in effectiveMessages) {
            if (m.role != MessageRole.USER && m.role != MessageRole.ASSISTANT) continue
            for (part in m.parts) {
                when (part) {
                    is UIMessagePart.Text -> {
                        if (part.text.isNotBlank()) {
                            history.put(
                                JSONObject()
                                    .put("role", if (m.role == MessageRole.USER) "user" else "assistant")
                                    .put("content", part.text),
                            )
                        }
                    }
                    is UIMessagePart.Image -> {
                        val url = part.url
                        when {
                            // 远程图片 URL：直传 provider
                            url.startsWith("http") -> history.put(
                                JSONObject().put("role", "user").put("content", "[图片]")
                                    .put("image_url", url),
                            )
                            // base64 data URI：直通引擎（image_data 分支）
                            url.startsWith("data:") -> history.put(
                                JSONObject().put("role", "user").put("content", "[图片]")
                                    .put("image_data", url),
                            )
                            // content://（PhotoPicker 主流路径）：复制到 cacheDir 再交给引擎读
                            url.startsWith("content:") -> {
                                val path = resolveContentToFile(context, url)
                                if (path != null) history.put(
                                    JSONObject().put("role", "user").put("content", "[图片]")
                                        .put("image_path", path),
                                )
                            }
                            // file:// 或裸路径：引擎直接读文件
                            else -> {
                                val path = url.removePrefix("file://").removePrefix("file:")
                                if (path.isNotBlank()) history.put(
                                    JSONObject().put("role", "user").put("content", "[图片]")
                                        .put("image_path", path),
                                )
                            }
                        }
                    }
                    else -> { /* 工具等中间态由 C 引擎自管理，不进入历史 */ }
                }
            }
        }

        // ---- 回调事件（文件级 Evt） ----
        val channel = Channel<Evt>(Channel.UNLIMITED)
        var nativeUsage: TokenUsage? = null
        val toolSeq = AtomicInteger(0)
        val callback = object : ChatCallback {
            override fun onDelta(kind: Int, text: String) {
                channel.trySend(Evt.Delta(kind, text))
            }
            override fun onToolCall(name: String, args: String) {
                channel.trySend(Evt.ToolCall(name, args))
            }
            override fun onToolResult(name: String, result: String) {
                channel.trySend(Evt.ToolResult(name, result))
            }
            override fun onFinish(ok: Boolean, error: String?) {
                channel.trySend(Evt.Finish(ok, error))
            }
        }

        // ---- C 引擎生成（IO 线程阻塞）+ 事件消费 ----
        kotlinx.coroutines.coroutineScope {
        val job = launch(Dispatchers.IO) {
            try {
                val nativeResult = Engine.nativeChat(
                    providerJson.toString(),
                    history.toString(),
                    workspaceCwd,
                    callback,
                )
                // nativeChat 返回后检查结果（防止回调遗漏导致死等）
                logMsg(TAG, "nativeChat returned: ${nativeResult.take(200)}")
                val parsed = try {
                    JSONObject(nativeResult)
                } catch (_: Exception) {
                    null
                }
                // 请求日志(诊断): C 引擎请求摘要写入 App 日志页(复用 parsed)
                parsed?.optJSONObject("request")?.let { req ->
                    Logging.log(
                        TAG,
                        "request: POST ${baseUrl} -> ${req.optInt("status")} " +
                            "(${req.optLong("duration_ms")}ms)" +
                            (parsed.optJSONObject("usage")?.let {
                                " | tokens: in=${it.optInt("prompt_tokens")} " +
                                    "out=${it.optInt("completion_tokens")} " +
                                    "cached=${it.optInt("cached_tokens")}"
                            } ?: ""),
                    )
                }
                if (parsed != null && !parsed.optBoolean("ok", true)) {
                    channel.trySend(Evt.Finish(false, parsed.optString("error", "engine error")))
                } else if (parsed != null) {
                    // token 用量统计(引擎流式 usage)
                    parsed.optJSONObject("usage")?.let { u ->
                        val p = u.optInt("prompt_tokens", 0)
                        val c = u.optInt("completion_tokens", 0)
                        val ck = u.optInt("cached_tokens", 0)
                        if (p > 0 || c > 0) {
                            nativeUsage = TokenUsage(
                                promptTokens = p,
                                completionTokens = c,
                                cachedTokens = ck,
                                totalTokens = p + c,
                            )
                        }
                    }
                }
            } catch (e: Throwable) {
                logErr(TAG, "nativeChat failed", e)
                channel.trySend(Evt.Finish(false, e.message ?: "engine error"))
            }
        }
        // 协程取消(用户停止)→ 通知引擎(阻塞 nativeChat 不响应协程取消;
        // 按 cancel_id 精准取消本会话, 不误伤其他并发会话)
        job.invokeOnCompletion { cause ->
            if (cause is kotlinx.coroutines.CancellationException) {
                logMsg(TAG, "generation cancelled, signalling engine")
                Engine.nativeSetCancel(cancelId, true)
            }
        }

        // ---- 消费事件 → 组装 UI 消息（滚动超时：130s 无事件保护；
        // 引擎侧每轮 120s，多轮工具循环总时长可能更长 → 收到事件即续期） ----
        val currentParts = mutableListOf<UIMessagePart>()
        // 本次生成的消息 id(流式期间稳定, 防 UI 消息版本膨胀)
        val assistantMsgId = Uuid.random()
        var deadline = System.currentTimeMillis() + 130_000L
        // [CE] 流式 text O(n²) 缓解(对齐 turbo [TURBO] 优化): 长回答"text 暴涨"阶段
        // 每 token `lastText + delta` 是 O(n) 复制, IO 线程累计 O(n²) → 末段出字变慢+GC 压力;
        // 改用 StringBuilder 累积(每 token O(1) append), ~32ms 降频才 flush 回 currentParts
        var textBuf: StringBuilder? = null
        var textBaseParts: List<UIMessagePart>? = null
        var lastFlush = 0L

        fun flushTextBuf() {
            val buf = textBuf ?: return
            val base = textBaseParts ?: return
            currentParts.clear()
            currentParts.addAll(base)
            currentParts.add(UIMessagePart.Text(buf.toString()))
            textBuf = null
            textBaseParts = null
        }
        while (true) {
            val remaining = deadline - System.currentTimeMillis()
            if (remaining <= 0) {
                logMsg(TAG, "generateText: timeout waiting for engine events")
                channel.trySend(Evt.Finish(false, "engine timeout"))
            }
            val evt = withTimeoutOrNull(remaining) {
                channel.receiveCatching().getOrNull()
            } ?: run {
                logMsg(TAG, "generateText: channel closed or timeout, stopping")
                break
            }
            deadline = System.currentTimeMillis() + 130_000L
            when (evt) {
                is Evt.Delta -> {
                    if (evt.kind == 1) {
                        // reasoning: 先 flush 文本累积器(part 类型切换), 再累积
                        flushTextBuf()
                        val last = currentParts.lastOrNull()
                        if (last is UIMessagePart.Reasoning) {
                            currentParts[currentParts.size - 1] =
                                last.copy(reasoning = last.reasoning + evt.text)
                        } else {
                            currentParts.add(UIMessagePart.Reasoning(evt.text))
                        }
                    } else {
                        val last = currentParts.lastOrNull()
                        if (textBuf != null) {
                            textBuf!!.append(evt.text)
                        } else if (last is UIMessagePart.Text) {
                            textBuf = StringBuilder(last.text).append(evt.text)
                            textBaseParts = currentParts.take(currentParts.size - 1)
                        } else {
                            textBuf = StringBuilder(evt.text)
                            textBaseParts = currentParts
                        }
                        val now = System.currentTimeMillis()
                        if (now - lastFlush >= 32L) {
                            flushTextBuf()
                            lastFlush = now
                        }
                    }
                    emitChunk(messages, currentParts, assistantMsgId).let { emit(it) }
                }
                is Evt.ToolCall -> {
                    flushTextBuf()
                    currentParts.add(
                        UIMessagePart.Tool(
                            toolCallId = "tool-${toolSeq.incrementAndGet()}",
                            toolName = evt.name,
                            input = evt.args,
                            approvalState = ToolApprovalState.Auto,
                        ),
                    )
                    emitChunk(messages, currentParts, assistantMsgId).let { emit(it) }
                }
                is Evt.ToolResult -> {
                    val idx = currentParts.indexOfLast {
                        it is UIMessagePart.Tool && it.toolName == evt.name && !it.isExecuted
                    }
                    if (idx >= 0) {
                        val t = currentParts[idx] as UIMessagePart.Tool
                        currentParts[idx] = t.copy(
                            output = listOf(UIMessagePart.Text(evt.result.take(4000))),
                        )
                    }
                    emitChunk(messages, currentParts, assistantMsgId).let { emit(it) }
                }
                is Evt.Finish -> {
                    flushTextBuf()
                    // [CE] 输出正则替换(assistant.regexes, 对齐 turbo onGenerationFinish)
                    if (assistant.regexes.isNotEmpty()) {
                        val scope = me.rerere.rikkahub.data.model.AssistantAffectScope.ASSISTANT
                        val newParts = currentParts.map { part ->
                            when (part) {
                                is UIMessagePart.Text ->
                                    part.copy(text = part.text.replaceRegexes(assistant, scope, visual = false))
                                is UIMessagePart.Reasoning ->
                                    part.copy(reasoning = part.reasoning.replaceRegexes(assistant, scope, visual = false))
                                else -> part
                            }
                        }
                        currentParts.clear()
                        currentParts.addAll(newParts)
                    }
                    if (!evt.ok && currentParts.isEmpty()) {
                        currentParts.add(UIMessagePart.Text(evt.err ?: "生成失败"))
                        emitChunk(messages, currentParts, assistantMsgId).let { emit(it) }
                    }
                    break
                }
            }
        }
        job.join()
        flushTextBuf()
        // 引擎返回 JSON 里的 usage(Finish 事件后到达) → 重新 emit 带用量统计的最终消息
        nativeUsage?.let { usage ->
            emit(emitChunk(messages, currentParts, assistantMsgId, usage))
        }
        logMsg(TAG, "generateText done: parts=${currentParts.size}")
        }
        processingStatus.value = null
        // 消费循环(事件处理/字符串累积/消息构造)全部在 IO 线程执行;
        // Main 只收 30fps 的 sample 结果做 UI 更新, 生成期间主线程不再被占
    }.flowOn(Dispatchers.IO)

    /** [CE] 翻译：走 C 引擎单轮生成 */
    fun translateText(
        settings: Settings,
        sourceText: String,
        targetLanguage: Locale,
        onStreamUpdate: ((String) -> Unit)? = null,
    ): Flow<String> = flow {
        val model = settings.providers.findModelById(settings.translateModeId)
            ?: error("Translation model not found")
        val provider = model.findProvider(settings.providers)
            ?: error("Translation provider not found")
        val pv = JSONObject()
            .put("base_url", provider.baseUrlOr().trimEnd('/'))
            .put("api_key", provider.apiKeyOr())
            .put("model", model.modelId)
        // 翻译思考预算(对齐 turbo: fromBudgetTokens → reasoningArgs)
        val (trEffort, trThinking) = reasoningArgs(
            provider.baseUrlOr(),
            me.rerere.ai.core.ReasoningLevel.fromBudgetTokens(settings.translateThinkingBudget),
        )
        if (trEffort != null) pv.put("reasoning_effort", trEffort)
        if (trThinking) pv.put("thinking", true)
        val isQwenMt = me.rerere.ai.registry.ModelRegistry.QWEN_MT.match(model.modelId)
        val prompt = if (isQwenMt) {
            sourceText
        } else {
            // 对齐 turbo: 用户可配置翻译提示词模板
            settings.translatePrompt.applyPlaceholders(
                "source_text" to sourceText,
                "target_lang" to targetLanguage.toString(),
            )
        }
        if (isQwenMt) {
            pv.put("temperature", 0.3)
            pv.put("top_p", 0.95)
            pv.put(
                "custom_body",
                "\"translation_options\":{\"source_lang\":\"auto\"," +
                    "\"target_lang\":\"${targetLanguage.getDisplayLanguage(java.util.Locale.ENGLISH)}\"}",
            )
        }
        val history = JSONArray()
            .put(JSONObject().put("role", "user").put("content", prompt))
        val result = withContext(Dispatchers.IO) {
            Engine.nativeChat(
                pv.toString(),
                history.toString(),
                null,
                object : ChatCallback {
                    override fun onDelta(kind: Int, text: String) {
                        onStreamUpdate?.invoke(text)
                    }
                    override fun onToolCall(name: String, args: String) {}
                    override fun onToolResult(name: String, result: String) {}
                    override fun onFinish(ok: Boolean, error: String?) {}
                },
            )
        }
        val parsed = try {
            JSONObject(result)
        } catch (_: Exception) {
            null
        }
        val text = parsed?.optString("text")
        if (text != null) emit(text) else error("translation failed")
    }

    private fun emitChunk(
        base: List<UIMessage>,
        parts: List<UIMessagePart>,
        assistantId: Uuid,
        usage: TokenUsage? = null,
    ): GenerationChunk {
        // 流式期间必须用稳定 id: UIMessage 默认 id=random, 每次 delta 新 id 会触发
        // Conversation.updateCurrentMessages 慢速路径 add → 消息节点无限膨胀(如 28 条)
        return GenerationChunk.Messages(
            base + UIMessage(
                id = assistantId,
                role = MessageRole.ASSISTANT,
                parts = parts.toList(),
                usage = usage,
            ),
        )
    }

    /** 工具白名单(对齐 turbo: assistant.localTools + enableMemory + enabledSkills)。
     * 引擎按白名单条件注册本地工具; web_search/recent_chats 有独立 env 开关。 */
    private fun buildToolWhitelist(assistant: Assistant): JSONArray {
        val wl = JSONArray()
        val opts = assistant.localTools
        if (me.rerere.rikkahub.data.ai.tools.local.LocalToolOption.JavascriptEngine in opts) {
            wl.put("eval_javascript")
        }
        if (me.rerere.rikkahub.data.ai.tools.local.LocalToolOption.TimeInfo in opts) {
            wl.put("get_time_info")
        }
        if (me.rerere.rikkahub.data.ai.tools.local.LocalToolOption.Clipboard in opts) {
            wl.put("clipboard_tool")
        }
        if (me.rerere.rikkahub.data.ai.tools.local.LocalToolOption.Tts in opts) {
            wl.put("text_to_speech")
        }
        if (me.rerere.rikkahub.data.ai.tools.local.LocalToolOption.AskUser in opts) {
            wl.put("ask_user")
        }
        if (me.rerere.rikkahub.data.ai.tools.local.LocalToolOption.ScreenTime in opts) {
            wl.put("get_screen_time")
        }
        if (me.rerere.rikkahub.data.ai.tools.local.LocalToolOption.Calendar in opts) {
            wl.put("calendar_query")
            wl.put("calendar_create")
        }
        if (assistant.enableMemory) wl.put("memory_tool")
        if (assistant.enabledSkills.isNotEmpty()) wl.put("use_skill")
        return wl
    }

    /** 思考模式参数(与 turbo ai 模块的 ChatCompletionsAPI 分派对齐)。
     * 返回 (reasoning_effort, thinking_enabled)。
     * 对齐 turbo: AUTO 一律不写 effort; DeepSeek/Moonshot 的 thinking 在
     * 非 OFF 时 enabled。 */
    private fun reasoningArgs(baseUrl: String, level: ReasoningLevel): Pair<String?, Boolean> {
        val host = runCatching { java.net.URI(baseUrl).host ?: "" }.getOrDefault("")
        return when {
            host.contains("deepseek") -> when (level) {
                ReasoningLevel.OFF -> null to false
                ReasoningLevel.AUTO -> null to true
                ReasoningLevel.XHIGH -> "max" to true
                ReasoningLevel.MEDIUM -> "high" to true
                else -> level.effort to true
            }
            host.contains("moonshot") -> null to (level != ReasoningLevel.OFF)
            host.contains("nvidia") -> when (level) {
                ReasoningLevel.AUTO -> null to false
                ReasoningLevel.OFF -> "none" to false
                ReasoningLevel.XHIGH -> "max" to false
                else -> "high" to false
            }
            host.contains("opencode") -> if (level == ReasoningLevel.AUTO) null to false else level.effort to false
            else -> if (level == ReasoningLevel.AUTO) null to false
            else (if (level.effort == "none") "low" else level.effort) to false
        }
    }
}


