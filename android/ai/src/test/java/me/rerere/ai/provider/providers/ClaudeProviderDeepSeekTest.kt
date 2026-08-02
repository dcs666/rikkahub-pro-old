package me.rerere.ai.provider.providers

import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import me.rerere.ai.core.ReasoningLevel
import me.rerere.ai.provider.Model
import me.rerere.ai.provider.ModelAbility
import me.rerere.ai.provider.ProviderSetting
import me.rerere.ai.provider.TextGenerationParams
import me.rerere.ai.ui.UIMessage
import okhttp3.OkHttpClient
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Before
import org.junit.Test

/**
 * Unit tests for DeepSeek Anthropic-compatible API (base_url=api.deepseek.com/anthropic) handling.
 *
 * DeepSeek's Anthropic layer supports `thinking` (type enabled/disabled) and
 * `output_config.effort` with values none/low/high/max only
 * (https://api-docs.deepseek.com/zh-cn/guides/thinking_mode/).
 * App's XHIGH("xhigh")/MEDIUM("medium") must be mapped to the official enum,
 * otherwise reasoning strength control silently fails.
 */
class ClaudeProviderDeepSeekTest {

    private lateinit var provider: ClaudeProvider

    @Before
    fun setUp() {
        provider = ClaudeProvider(OkHttpClient())
    }

    private fun invokeBuildMessageRequest(
        baseUrl: String,
        reasoningLevel: ReasoningLevel,
    ): JsonObject {
        val method = ClaudeProvider::class.java.getDeclaredMethod(
            "buildMessageRequest",
            ProviderSetting.Claude::class.java,
            List::class.java,
            TextGenerationParams::class.java,
            Boolean::class.javaPrimitiveType
        )
        method.isAccessible = true
        val setting = ProviderSetting.Claude(
            apiKey = "test-key",
            baseUrl = baseUrl,
        )
        val params = TextGenerationParams(
            model = Model(
                modelId = "deepseek-v4-pro",
                abilities = listOf(ModelAbility.REASONING)
            ),
            reasoningLevel = reasoningLevel,
        )
        return method.invoke(
            provider,
            setting,
            listOf(UIMessage.user("hi")),
            params,
            false
        ) as JsonObject
    }

    @Test
    fun `xhigh maps to output_config effort max on deepseek anthropic api`() {
        val body = invokeBuildMessageRequest("https://api.deepseek.com/anthropic", ReasoningLevel.XHIGH)
        assertEquals("enabled", body["thinking"]?.jsonObject?.get("type")?.jsonPrimitive?.content)
        assertEquals("max", body["output_config"]?.jsonObject?.get("effort")?.jsonPrimitive?.content)
    }

    @Test
    fun `medium maps to output_config effort high on deepseek anthropic api`() {
        val body = invokeBuildMessageRequest("https://api.deepseek.com/anthropic", ReasoningLevel.MEDIUM)
        assertEquals("high", body["output_config"]?.jsonObject?.get("effort")?.jsonPrimitive?.content)
    }

    @Test
    fun `low keeps effort low on deepseek anthropic api`() {
        val body = invokeBuildMessageRequest("https://api.deepseek.com/anthropic", ReasoningLevel.LOW)
        assertEquals("low", body["output_config"]?.jsonObject?.get("effort")?.jsonPrimitive?.content)
    }

    @Test
    fun `off disables thinking without output_config on deepseek anthropic api`() {
        val body = invokeBuildMessageRequest("https://api.deepseek.com/anthropic", ReasoningLevel.OFF)
        assertEquals("disabled", body["thinking"]?.jsonObject?.get("type")?.jsonPrimitive?.content)
        assertNull(body["output_config"])
    }

    @Test
    fun `auto enables thinking without effort on deepseek anthropic api`() {
        val body = invokeBuildMessageRequest("https://api.deepseek.com/anthropic", ReasoningLevel.AUTO)
        assertEquals("enabled", body["thinking"]?.jsonObject?.get("type")?.jsonPrimitive?.content)
        assertNull(body["output_config"])
    }

    @Test
    fun `non deepseek host keeps adaptive thinking behavior`() {
        // 原生 Anthropic 不受影响：adaptive 模式 + effort 透传
        val body = invokeBuildMessageRequest("https://api.anthropic.com/v1", ReasoningLevel.XHIGH)
        assertEquals("adaptive", body["thinking"]?.jsonObject?.get("type")?.jsonPrimitive?.content)
        assertEquals("xhigh", body["output_config"]?.jsonObject?.get("effort")?.jsonPrimitive?.content)
    }
}
