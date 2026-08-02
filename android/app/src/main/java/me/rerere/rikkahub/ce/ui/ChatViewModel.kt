package me.rerere.rikkahub.ce.ui

import android.content.Context
import android.content.SharedPreferences
import android.net.Uri
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

data class ChatSession(
    val id: String,
    var title: String,
    val messages: MutableList<ChatMsg> = mutableListOf(),
    var updatedAt: Long = System.currentTimeMillis(),
)

class ChatViewModel(private val appContext: Context) : ViewModel(), ChatCallback {

    private val prefs: SharedPreferences =
        appContext.getSharedPreferences("rikka_ce", Context.MODE_PRIVATE)

    val sessions = mutableStateListOf<ChatSession>()
    var currentSessionId by mutableStateOf("")

    var providerBaseUrl by mutableStateOf(
        prefs.getString("base_url", "https://api.openai.com/v1") ?: "https://api.openai.com/v1")
    var providerApiKey by mutableStateOf(prefs.getString("api_key", "") ?: "")
    var providerModel by mutableStateOf(
        prefs.getString("model", "gpt-4o-mini") ?: "gpt-4o-mini")

    var busy by mutableStateOf(false)
        private set

    val messages: MutableList<ChatMsg>
        get() = currentSession?.messages ?: mutableListOf()

    private val currentSession: ChatSession?
        get() = sessions.firstOrNull { it.id == currentSessionId }

    init {
        restoreSessions()
    }

    fun newSession() {
        if (busy) return
        val s = ChatSession(
            id = System.currentTimeMillis().toString(),
            title = "新会话",
        )
        sessions.add(0, s)
        currentSessionId = s.id
        saveSessionMeta()
    }

    fun switchSession(id: String) {
        if (busy) return
        if (sessions.any { it.id == id }) {
            currentSessionId = id
        }
    }

    fun deleteSession(id: String) {
        if (busy) return
        val idx = sessions.indexOfFirst { it.id == id }
        if (idx < 0) return
        sessions.removeAt(idx)
        prefs.edit().remove("session_$id").apply()
        if (currentSessionId == id) {
            currentSessionId = if (sessions.isNotEmpty()) sessions[0].id else ""
            if (sessions.isEmpty()) newSession()
        }
    }

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
        val session = currentSession ?: run {
            newSession()
            return
        }
        if (busy) return
        session.messages.add(ChatMsg("user", text))
        if (session.title == "新会话") {
            session.title = text.take(20)
        }
        session.updatedAt = System.currentTimeMillis()
        sendInternal(text)
    }

    fun cancel() {
        Engine.nativeSetCancel(true)
    }

    /** 图片 OCR：识别文本作为用户消息自动发送 */
    fun ocrImage(uri: Uri) {
        if (busy) return
        messages.add(ChatMsg("user", "🖼️ 正在识别图片…", streaming = true))
        busy = true
        viewModelScope.launch(Dispatchers.IO) {
            val provider = JSONObject()
                .put("base_url", providerBaseUrl)
                .put("api_key", providerApiKey)
                .put("model", providerModel)
            try {
                val path = copyUriToFile(uri)
                val result = Engine.nativeOcr(provider.toString(), path)
                val parsed = JSONObject(result)
                val text = if (parsed.optBoolean("ok")) {
                    parsed.optString("text")
                } else {
                    "OCR 失败：${parsed.optString("error")}"
                }
                val idx = messages.indexOfLast { it.role == "user" && it.streaming }
                if (idx >= 0) {
                    messages[idx] = messages[idx].copy(
                        text = "🖼️ 图片内容：\n$text",
                        streaming = false,
                        isError = !parsed.optBoolean("ok"),
                    )
                }
                if (parsed.optBoolean("ok") && text.isNotBlank()) {
                    // 识别成功：作为用户消息发送
                    messages.add(ChatMsg("user", text))
                    val session = currentSession ?: return@launch
                    session.updatedAt = System.currentTimeMillis()
                    saveSession()
                    sendInternal(text)
                }
            } catch (e: Exception) {
                val idx = messages.indexOfLast { it.role == "user" && it.streaming }
                if (idx >= 0) {
                    messages[idx] = messages[idx].copy(
                        text = "🖼️ OCR 出错：${e.message}",
                        streaming = false,
                        isError = true,
                    )
                }
                busy = false
            }
        }
    }

    private fun copyUriToFile(uri: Uri): String {
        val file = java.io.File(appContext.cacheDir, "ocr_input.png")
        appContext.contentResolver.openInputStream(uri)?.use { input ->
            file.outputStream().use { output -> input.copyTo(output) }
        }
        return file.absolutePath
    }

    private fun sendInternal(text: String) {
        val session = currentSession ?: return
        busy = true
        saveSession()
        viewModelScope.launch(Dispatchers.IO) {
            val provider = JSONObject()
                .put("base_url", providerBaseUrl)
                .put("api_key", providerApiKey)
                .put("model", providerModel)
            val history = JSONArray()
            for (m in session.messages) {
                if (m.role != "user" && m.role != "assistant") continue
                if (m.isError || m.text.startsWith("⚙️")) continue
                history.put(JSONObject().put("role", m.role).put("content", m.text))
            }
            Engine.nativeChat(
                provider.toString(),
                history.toString(),
                appContext.filesDir.absolutePath,
                this@ChatViewModel,
            )
        }
    }

    fun saveProviderSettings() {
        prefs.edit()
            .putString("base_url", providerBaseUrl)
            .putString("api_key", providerApiKey)
            .putString("model", providerModel)
            .apply()
    }

    fun clearSession() {
        val s = currentSession ?: return
        s.messages.clear()
        s.title = "新会话"
        prefs.edit().remove("session_${s.id}").apply()
    }

    /* ---------- 持久化 ---------- */

    private fun saveSession() {
        val s = currentSession ?: return
        val arr = JSONArray()
        for (m in s.messages) {
            arr.put(JSONObject()
                .put("role", m.role)
                .put("text", m.text)
                .put("error", m.isError))
        }
        prefs.edit().putString("session_${s.id}", arr.toString()).apply()
        saveSessionMeta()
    }

    private fun saveSessionMeta() {
        val arr = JSONArray()
        for (s in sessions) {
            arr.put(JSONObject()
                .put("id", s.id)
                .put("title", s.title)
                .put("updated", s.updatedAt))
        }
        prefs.edit().putString("sessions", arr.toString()).apply()
    }

    private fun restoreSessions() {
        val raw = prefs.getString("sessions", null)
        if (raw != null) {
            try {
                val arr = JSONArray(raw)
                for (i in 0 until arr.length()) {
                    val o = arr.getJSONObject(i)
                    val s = ChatSession(
                        id = o.getString("id"),
                        title = o.getString("title"),
                        updatedAt = o.optLong("updated", 0L),
                    )
                    sessions.add(s)
                }
            } catch (_: Exception) {
                sessions.clear()
            }
        }
        // 恢复各会话消息
        for (s in sessions) {
            val rawMsg = prefs.getString("session_${s.id}", null) ?: continue
            try {
                val arr = JSONArray(rawMsg)
                for (i in 0 until arr.length()) {
                    val o = arr.getJSONObject(i)
                    s.messages.add(ChatMsg(
                        role = o.getString("role"),
                        text = o.getString("text"),
                        isError = o.optBoolean("error", false),
                    ))
                }
            } catch (_: Exception) {
                prefs.edit().remove("session_${s.id}").apply()
            }
        }
        if (sessions.isEmpty()) {
            newSession()
        } else {
            currentSessionId = sessions[0].id
        }
    }
}
