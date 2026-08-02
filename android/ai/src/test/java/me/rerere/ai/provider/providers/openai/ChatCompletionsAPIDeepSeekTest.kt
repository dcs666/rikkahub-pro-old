package me.rerere.ai.provider.providers.openai

import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import me.rerere.ai.core.MessageRole
import me.rerere.ai.core.ReasoningLevel
import me.rerere.ai.provider.Model
import me.rerere.ai.provider.ModelAbility
import me.rerere.ai.provider.ProviderSetting
import me.rerere.ai.provider.TextGenerationParams
import me.rerere.ai.ui.UIMessage
import me.rerere.ai.ui.UIMessagePart
import me.rerere.ai.util.KeyRoulette
import okhttp3.OkHttpClient
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Before
import org.junit.Test

/**
 * Unit tests for DeepSeek official API (api.deepseek.com) thinking mode handling:
 * - thinking.type = enabled/disabled (OpenAI format)
 * - reasoning_effort: low/high/max — App's XHIGH is mapped to "max", MEDIUM to "high"
 *   (https://api-docs.deepseek.com/zh-cn/guides/thinking_mode/)
 * - 工具调用轮次的 reasoning_content 强制回传（关闭 includeHistoryReasoning 也不受影响）
 */
class ChatCompletionsAPIDeepSeekTest {

    private lateinit var api: ChatCompletionsAPI

    @Before
    fun setUp() {
        api = ChatCompletionsAPI(OkHttpClient(), KeyRoulette.default())
    }

    // Helper to invoke private buildChatCompletionRequest via reflection
    private fun buildRequest(
        modelId: String,
        reasoningLevel: ReasoningLevel,
    ): JsonObject {
        val method = ChatCompletionsAPI::class.java.getDeclaredMethod(
            "buildChatCompletionRequest",
            List::class.java,
            TextGenerationParams::class.java,
            ProviderSetting.OpenAI::class.java,
            Boolean::class.javaPrimitiveType
        )
        method.isAccessible = true
        val model = Model(
            modelId = modelId,
            abilities = listOf(ModelAbility.REASONING)
        )
        val params = TextGenerationParams(
            model = model,
            reasoningLevel = reasoningLevel,
        )
        val providerSetting = ProviderSetting.OpenAI(baseUrl = "https://api.deepseek.com/v1")
        return method.invoke(
            api,
            listOf(UIMessage.user("hi")),
            params,
            providerSetting,
            true
        ) as JsonObject
    }

    @Test
    fun `xhigh maps to reasoning_effort max on deepseek official`() {
        val body = buildRequest("deepseek-v4-pro", ReasoningLevel.XHIGH)
        val thinking = body["thinking"]?.jsonObject
        assertEquals("enabled", thinking?.get("type")?.jsonPrimitive?.content)
        assertEquals("max", body["reasoning_effort"]?.jsonPrimitive?.content)
    }

    @Test
    fun `high keeps reasoning_effort high on deepseek official`() {
        val body = buildRequest("deepseek-v4-pro", ReasoningLevel.HIGH)
        assertEquals("enabled", body["thinking"]?.jsonObject?.get("type")?.jsonPrimitive?.content)
        assertEquals("high", body["reasoning_effort"]?.jsonPrimitive?.content)
    }

    @Test
    fun `low keeps reasoning_effort low on deepseek official`() {
        val body = buildRequest("deepseek-v4-pro", ReasoningLevel.LOW)
        assertEquals("enabled", body["thinking"]?.jsonObject?.get("type")?.jsonPrimitive?.content)
        assertEquals("low", body["reasoning_effort"]?.jsonPrimitive?.content)
    }

    @Test
    fun `off sends thinking disabled and no reasoning_effort`() {
        val body = buildRequest("deepseek-v4-pro", ReasoningLevel.OFF)
        assertEquals("disabled", body["thinking"]?.jsonObject?.get("type")?.jsonPrimitive?.content)
        assertNull(body["reasoning_effort"])
    }

    @Test
    fun `auto sends thinking enabled and no reasoning_effort`() {
        // 文档: 思考模式默认打开, effort 默认为 high —— 不显式传 reasoning_effort 即可
        val body = buildRequest("deepseek-v4-pro", ReasoningLevel.AUTO)
        assertEquals("enabled", body["thinking"]?.jsonObject?.get("type")?.jsonPrimitive?.content)
        assertNull(body["reasoning_effort"])
    }

    @Test
    fun `xhigh mapping applies to v4-flash too`() {
        val body = buildRequest("deepseek-v4-flash", ReasoningLevel.XHIGH)
        assertEquals("max", body["reasoning_effort"]?.jsonPrimitive?.content)
    }

    @Test
    fun `medium maps to effort high on deepseek official`() {
        // 官方 reasoning_effort 枚举只有 low/high/max，medium 由服务端映射为 high；
        // 为与 Responses / Anthropic 两条路径一致并给将来服务端行为变化兜底，此处显式映射 high
        val body = buildRequest("deepseek-v4-pro", ReasoningLevel.MEDIUM)
        assertEquals("high", body["reasoning_effort"]?.jsonPrimitive?.content)
    }

    @Test
    fun `tool history reasoning resent even when includeHistoryReasoning off`() {
        // DeepSeek 思考模式文档：携带 tools 的请求在后续所有请求中必须完整回传 reasoning_content，
        // 否则 API 返回 400 —— 即使关闭 includeHistoryReasoning，工具调用轮次的思维链也必须回传
        val method = ChatCompletionsAPI::class.java.getDeclaredMethod(
            "buildChatCompletionRequest",
            List::class.java,
            TextGenerationParams::class.java,
            ProviderSetting.OpenAI::class.java,
            Boolean::class.javaPrimitiveType
        )
        method.isAccessible = true
        val assistant = UIMessage(
            role = MessageRole.ASSISTANT,
            parts = listOf(
                UIMessagePart.Reasoning(reasoning = "thinking trace"),
                UIMessagePart.Tool(
                    toolCallId = "call_1",
                    toolName = "get_weather",
                    input = """{"location":"Hangzhou"}""",
                    output = listOf(UIMessagePart.Text("Cloudy"))
                )
            )
        )
        val body = method.invoke(
            api,
            listOf(assistant, UIMessage.user("hi")),
            TextGenerationParams(
                model = Model(
                    modelId = "deepseek-v4-pro",
                    abilities = listOf(ModelAbility.REASONING)
                ),
                reasoningLevel = ReasoningLevel.HIGH,
            ),
            ProviderSetting.OpenAI(
                baseUrl = "https://api.deepseek.com/v1",
                includeHistoryReasoning = false
            ),
            true
        ) as JsonObject

        val assistantMsg = body["messages"]?.jsonArray
            ?.first { it.jsonObject["role"]?.jsonPrimitive?.content == "assistant" }
        assertEquals("thinking trace", assistantMsg?.jsonObject?.get("reasoning_content")?.jsonPrimitive?.content)
        assertEquals(
            "call_1",
            assistantMsg?.jsonObject?.get("tool_calls")?.jsonArray?.first()?.jsonObject?.get("id")?.jsonPrimitive?.content
        )
    }
}
