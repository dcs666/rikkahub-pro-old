package me.rerere.rikkahub.data.ai

import android.content.Context
import android.util.Log
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.flow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.put
import me.rerere.ai.core.MessageRole
import me.rerere.ai.core.Tool
import me.rerere.ai.provider.Model
import me.rerere.ai.provider.ProviderManager
import me.rerere.ai.ui.ToolApprovalState
import me.rerere.ai.ui.UIMessage
import me.rerere.ai.ui.UIMessagePart
import me.rerere.rikkahub.data.ai.transformers.InputMessageTransformer
import me.rerere.rikkahub.data.ai.transformers.OutputMessageTransformer
import me.rerere.rikkahub.data.datastore.Settings
import me.rerere.rikkahub.data.memory.MemoryRepository
import me.rerere.rikkahub.data.model.Assistant
import me.rerere.rikkahub.data.model.AssistantMemory
import dev.rikkahub.ce.Engine
import dev.rikkahub.ce.ChatCallback
import org.json.JSONArray
import org.json.JSONObject
import java.util.concurrent.atomic.AtomicInteger
import kotlin.uuid.Uuid

private const val TAG = "GenerationHandler(CE)"

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
        Log.i(TAG, "generateText: model=${model.id} messages=${messages.size}")
        processingStatus.value = "生成中…"

        // ---- 组装 C 引擎输入 ----
        val providerJson = JSONObject()
            .put("base_url", provider.baseUrl.trimEnd('/'))
            .put("api_key", provider.apiKey)
            .put("model", model.id)

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

        // ---- 回调事件 ----
        sealed interface Evt
        class DeltaEvt(val kind: Int, val text: String) : Evt
        class ToolCallEvt(val name: String, val args: String) : Evt
        class ToolResultEvt(val name: String, val result: String) : Evt
        class FinishEvt(val ok: Boolean, val err: String?) : Evt

        val channel = Channel<Evt>(Channel.UNLIMITED)
        val toolSeq = AtomicInteger(0)
        val callback = object : ChatCallback {
            override fun onDelta(kind: Int, text: String) {
                channel.trySend(DeltaEvt(kind, text))
            }
            override fun onToolCall(name: String, args: String) {
                channel.trySend(ToolCallEvt(name, args))
            }
            override fun onToolResult(name: String, result: String) {
                channel.trySend(ToolResultEvt(name, result))
            }
            override fun onFinish(ok: Boolean, error: String?) {
                channel.trySend(FinishEvt(ok, error))
            }
        }

        // ---- C 引擎生成（IO 线程阻塞） ----
        val job = launch(Dispatchers.IO) {
            try {
                Engine.nativeChat(
                    providerJson.toString(),
                    history.toString(),
                    workspaceCwd,
                    callback,
                )
            } catch (e: Throwable) {
                Log.e(TAG, "nativeChat failed", e)
                channel.trySend(FinishEvt(false, e.message ?: "engine error"))
            }
        }

        // ---- 消费事件 → 组装 UI 消息 ----
        val currentParts = mutableListOf<UIMessagePart>()
        while (true) {
            val evt = channel.receiveCatching().getOrNull() ?: break
            when (evt) {
                is DeltaEvt -> {
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
                is ToolCallEvt -> {
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
                is ToolResultEvt -> {
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
                is FinishEvt -> {
                    if (!evt.ok && currentParts.isEmpty()) {
                        currentParts.add(UIMessagePart.Text(evt.err ?: "生成失败"))
                        emitChunk(messages, currentParts).let { emit(it) }
                    }
                    break
                }
            }
        }
        job.join()
        processingStatus.value = null
        Log.i(TAG, "generateText done: parts=${currentParts.size}")
    }

    private fun emitChunk(base: List<UIMessage>, parts: List<UIMessagePart>): GenerationChunk {
        return GenerationChunk.Messages(base + UIMessage(role = MessageRole.ASSISTANT, parts = parts.toList()))
    }

    fun translateText(
        settings: Settings,
        model: Model,
        text: String,
        targetLanguage: String,
    ): Flow<String> = flow {
        val provider = model.findProvider(settings.providers) ?: error("Provider not found")
        val providerJson = JSONObject()
            .put("base_url", provider.baseUrl.trimEnd('/'))
            .put("api_key", provider.apiKey)
            .put("model", model.id)
        val prompt = "Translate the following text to $targetLanguage. Output only the translation:\n\n$text"
        val history = JSONArray()
            .put(JSONObject().put("role", "user").put("content", prompt))
        val result = withContext(Dispatchers.IO) {
            Engine.nativeChat(
                providerJson.toString(),
                history.toString(),
                null,
                object : ChatCallback {
                    override fun onDelta(kind: Int, text: String) {}
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
        val text2 = parsed?.optString("text")
        if (text2 != null) emit(text2) else error("translation failed")
    }
}
