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
import me.rerere.ai.core.Tool
import me.rerere.ai.provider.Model
import me.rerere.ai.provider.ProviderManager
import me.rerere.ai.provider.ProviderSetting
import me.rerere.ai.registry.ModelRegistry
import me.rerere.ai.ui.ToolApprovalState
import me.rerere.ai.ui.UIMessage
import me.rerere.ai.ui.UIMessagePart
import me.rerere.rikkahub.data.ai.transformers.InputMessageTransformer
import me.rerere.rikkahub.data.ai.transformers.OutputMessageTransformer
import me.rerere.rikkahub.data.datastore.Settings
import me.rerere.rikkahub.data.datastore.findModelById
import me.rerere.rikkahub.data.datastore.findProvider
import me.rerere.rikkahub.data.repository.MemoryRepository
import me.rerere.rikkahub.utils.applyPlaceholders
import me.rerere.rikkahub.data.model.Assistant
import me.rerere.rikkahub.data.model.AssistantMemory
import dev.rikkahub.ce.Engine
import dev.rikkahub.ce.ChatCallback
import org.json.JSONArray
import org.json.JSONObject
import java.util.Locale
import java.util.concurrent.atomic.AtomicInteger
import kotlin.uuid.Uuid

private const val TAG = "GenerationHandler(CE)"

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
) {
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
        Log.i(TAG, "generateText: model=${model.modelId} messages=${messages.size} " +
            "baseUrl=$baseUrl apiKey=${if (apiKey.isBlank()) "BLANK" else apiKey.take(4) + "***"} " +
            "systemPrompt=${conversationSystemPrompt?.take(50)}")
        if (baseUrl.isBlank()) error("Provider base URL is empty: ${provider.name}")
        if (apiKey.isBlank()) error("Provider API key is empty: ${provider.name}")
        processingStatus.value = "生成中…"

        // ---- 组装 C 引擎输入 ----
        val providerJson = JSONObject()
            .put("base_url", baseUrl)
            .put("api_key", apiKey)
            .put("model", modelId)

        val history = JSONArray()
        if (!conversationSystemPrompt.isNullOrBlank()) {
            history.put(JSONObject().put("role", "system").put("content", conversationSystemPrompt))
        }
        for (m in messages) {
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
                        if (part.url.startsWith("http")) {
                            history.put(
                                JSONObject()
                                    .put("role", "user")
                                    .put("content", "[图片]")
                                    .put("image_url", part.url),
                            )
                        }
                    }
                    else -> { /* 工具等中间态由 C 引擎自管理，不进入历史 */ }
                }
            }
        }

        // ---- 回调事件（文件级 Evt） ----
        val channel = Channel<Evt>(Channel.UNLIMITED)
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
                Log.i(TAG, "nativeChat returned: ${nativeResult.take(200)}")
                val parsed = try {
                    JSONObject(nativeResult)
                } catch (_: Exception) {
                    null
                }
                if (parsed != null && !parsed.optBoolean("ok", true)) {
                    channel.trySend(Evt.Finish(false, parsed.optString("error", "engine error")))
                }
            } catch (e: Throwable) {
                Log.e(TAG, "nativeChat failed", e)
                channel.trySend(Evt.Finish(false, e.message ?: "engine error"))
            }
        }

        // ---- 消费事件 → 组装 UI 消息（90s 无事件超时保护） ----
        val currentParts = mutableListOf<UIMessagePart>()
        val deadline = System.currentTimeMillis() + 90_000L
        while (true) {
            val remaining = deadline - System.currentTimeMillis()
            if (remaining <= 0) {
                Log.w(TAG, "generateText: timeout waiting for engine events")
                channel.trySend(Evt.Finish(false, "engine timeout"))
            }
            val evt = withTimeoutOrNull(remaining) {
                channel.receiveCatching().getOrNull()
            } ?: run {
                Log.w(TAG, "generateText: channel closed or timeout, stopping")
                break
            }
            when (evt) {
                is Evt.Delta -> {
                    if (evt.kind == 1) {
                        // reasoning
                        val last = currentParts.lastOrNull()
                        if (last is UIMessagePart.Reasoning) {
                            currentParts[currentParts.size - 1] =
                                last.copy(reasoning = last.reasoning + evt.text)
                        } else {
                            currentParts.add(UIMessagePart.Reasoning(evt.text))
                        }
                    } else {
                        val last = currentParts.lastOrNull()
                        if (last is UIMessagePart.Text) {
                            currentParts[currentParts.size - 1] =
                                last.copy(text = last.text + evt.text)
                        } else {
                            currentParts.add(UIMessagePart.Text(evt.text))
                        }
                    }
                    emitChunk(messages, currentParts).let { emit(it) }
                }
                is Evt.ToolCall -> {
                    currentParts.add(
                        UIMessagePart.Tool(
                            toolCallId = "tool-${toolSeq.incrementAndGet()}",
                            toolName = evt.name,
                            input = evt.args,
                            approvalState = ToolApprovalState.Auto,
                        ),
                    )
                    emitChunk(messages, currentParts).let { emit(it) }
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
                    emitChunk(messages, currentParts).let { emit(it) }
                }
                is Evt.Finish -> {
                    if (!evt.ok && currentParts.isEmpty()) {
                        currentParts.add(UIMessagePart.Text(evt.err ?: "生成失败"))
                        emitChunk(messages, currentParts).let { emit(it) }
                    }
                    break
                }
            }
        }
        job.join()
        Log.i(TAG, "generateText done: parts=${currentParts.size}")
        }
        processingStatus.value = null
    }

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
        val prompt = "Translate the following text to ${targetLanguage.displayName}. " +
            "Output only the translation, no explanation:\n\n$sourceText"
        val history = JSONArray()
            .put(JSONObject().put("role", "user").put("content", prompt))
        val result = withContext(Dispatchers.IO) {
            Engine.nativeChat(
                JSONObject()
                    .put("base_url", provider.baseUrlOr().trimEnd('/'))
                    .put("api_key", provider.apiKeyOr())
                    .put("model", model.modelId)
                    .toString(),
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

    private fun emitChunk(base: List<UIMessage>, parts: List<UIMessagePart>): GenerationChunk {
        return GenerationChunk.Messages(base + UIMessage(role = MessageRole.ASSISTANT, parts = parts.toList()))
    }
}


