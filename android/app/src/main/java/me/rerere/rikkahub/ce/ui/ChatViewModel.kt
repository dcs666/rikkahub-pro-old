package me.rerere.rikkahub.ce.ui

import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import me.rerere.rikkahub.ce.ChatCallback
import me.rerere.rikkahub.ce.Engine
import org.json.JSONArray
import org.json.JSONObject

data class ChatMsg(
    val role: String,          // user / assistant
    val text: String,
    val streaming: Boolean = false,
    val isError: Boolean = false,
)

class ChatViewModel : ViewModel(), ChatCallback {
    val messages = mutableStateListOf<ChatMsg>()

    var providerBaseUrl by mutableStateOf("https://api.openai.com/v1")
    var providerApiKey by mutableStateOf("")
    var providerModel by mutableStateOf("gpt-4o-mini")

    var busy by mutableStateOf(false)
        private set

    private val streamingIndex: Int
        get() = messages.indexOfLast { it.role == "assistant" && it.streaming }

    override fun onDelta(kind: Int, text: String) {
        if (kind != 0) return
        val idx = streamingIndex
        if (idx >= 0) {
            messages[idx] = messages[idx].copy(text = messages[idx].text + text)
        } else {
            messages.add(ChatMsg("assistant", text, streaming = true))
        }
    }

    override fun onToolCall(name: String, args: String) {
        messages.add(ChatMsg("assistant", "⚙️ 调用工具: $name", streaming = false))
    }

    override fun onToolResult(name: String, result: String) {
        messages.add(ChatMsg("assistant", "⚙️ 工具 $name 返回", streaming = false))
    }

    override fun onFinish(ok: Boolean, error: String?) {
        val idx = streamingIndex
        if (idx >= 0) {
            messages[idx] = messages[idx].copy(streaming = false, isError = !ok)
        }
        if (!ok) {
            messages.add(ChatMsg("assistant", error ?: "生成失败", streaming = false, isError = true))
        }
        busy = false
    }

    fun send(text: String) {
        if (busy) return
        messages.add(ChatMsg("user", text))
        busy = true
        viewModelScope.launch(Dispatchers.IO) {
            val provider = JSONObject()
                .put("base_url", providerBaseUrl)
                .put("api_key", providerApiKey)
                .put("model", providerModel)
            val history = JSONArray()
            for (m in messages) {
                if (m.role != "user" && m.role != "assistant") continue
                if (m.isError || m.text.startsWith("⚙️")) continue
                history.put(JSONObject().put("role", m.role).put("content", m.text))
            }
            Engine.nativeChat(provider.toString(), history.toString(), this@ChatViewModel)
        }
    }

    fun cancel() {
        Engine.nativeSetCancel(true)
    }
}
