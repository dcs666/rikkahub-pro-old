package me.rerere.rikkahub.ce.ui

import android.content.Context
import android.content.SharedPreferences
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

class ChatViewModel(private val appContext: Context) : ViewModel(), ChatCallback {

    private val prefs: SharedPreferences =
        appContext.getSharedPreferences("rikka_ce", Context.MODE_PRIVATE)

    val messages = mutableStateListOf<ChatMsg>()

    var providerBaseUrl by mutableStateOf(
        prefs.getString("base_url", "https://api.openai.com/v1") ?: "https://api.openai.com/v1")
    var providerApiKey by mutableStateOf(prefs.getString("api_key", "") ?: "")
    var providerModel by mutableStateOf(
        prefs.getString("model", "gpt-4o-mini") ?: "gpt-4o-mini")

    init {
        restoreSession()
    }

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
        saveSession()
    }

    fun send(text: String) {
        if (busy) return
        messages.add(ChatMsg("user", text))
        busy = true
        saveSession()
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

    fun saveProviderSettings() {
        prefs.edit()
            .putString("base_url", providerBaseUrl)
            .putString("api_key", providerApiKey)
            .putString("model", providerModel)
            .apply()
    }

    fun clearSession() {
        messages.clear()
        prefs.edit().remove("session").apply()
    }

    private fun saveSession() {
        val arr = JSONArray()
        for (m in messages) {
            arr.put(JSONObject()
                .put("role", m.role)
                .put("text", m.text)
                .put("error", m.isError))
        }
        prefs.edit().putString("session", arr.toString()).apply()
    }

    private fun restoreSession() {
        val raw = prefs.getString("session", null) ?: return
        try {
            val arr = JSONArray(raw)
            for (i in 0 until arr.length()) {
                val o = arr.getJSONObject(i)
                messages.add(ChatMsg(
                    role = o.getString("role"),
                    text = o.getString("text"),
                    isError = o.optBoolean("error", false),
                ))
            }
        } catch (_: Exception) {
            prefs.edit().remove("session").apply()
        }
    }
}
