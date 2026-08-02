package dev.rikkahub.ce.ui

import android.content.Context
import android.content.SharedPreferences
import android.net.Uri
import dev.rikkahub.ce.DeviceTools
import dev.rikkahub.ce.Engine
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import dev.rikkahub.ce.ChatCallback
import org.json.JSONArray
import org.json.JSONObject

/** 工具调用消息（卡片展示：参数/结果折叠） */
data class ToolMsg(
    val name: String,
    val args: String,
    val result: String? = null,
    val isError: Boolean = false,
)

data class ChatMsg(
    val role: String,          // user / assistant
    val text: String,
    val reasoning: String = "",  // 推理过程（流式累积，UI 折叠显示）
    val streaming: Boolean = false,
    val isError: Boolean = false,
    val imagePath: String? = null,  // 本地图片路径（消息附带展示）
    val tool: ToolMsg? = null,      // 工具调用卡片
    val time: Long = System.currentTimeMillis(),
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
    var autoTts by mutableStateOf(prefs.getBoolean("auto_tts", false))
        private set
    /** 主题：0=跟随系统 1=浅色 2=深色 */
    var themeMode by mutableStateOf(prefs.getInt("theme_mode", 0))
        private set
    var fontSize by mutableStateOf(prefs.getInt("font_size", 15))
        private set

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

    fun renameSession(id: String, newTitle: String) {
        val s = sessions.firstOrNull { it.id == id } ?: return
        val t = newTitle.trim()
        if (t.isNotBlank()) {
            s.title = t.take(20)
            saveSessionMeta()
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
        val idx = streamingIndex
        if (kind == 1) {
            // 推理内容：累积到当前 assistant 消息的 reasoning 字段
            if (idx >= 0) {
                messages[idx] = messages[idx].copy(reasoning = messages[idx].reasoning + text)
            } else {
                messages.add(ChatMsg("assistant", "", reasoning = text, streaming = true))
            }
            return
        }
        if (idx >= 0) {
            messages[idx] = messages[idx].copy(text = messages[idx].text + text)
        } else {
            messages.add(ChatMsg("assistant", text, streaming = true))
        }
    }

    override fun onToolCall(name: String, args: String) {
        messages.add(ChatMsg("assistant", "", tool = ToolMsg(name, args)))
    }

    override fun onToolResult(name: String, result: String) {
        val idx = messages.indexOfLast { it.tool?.name == name && it.tool.result == null }
        if (idx >= 0) {
            val t = messages[idx].tool!!
            messages[idx] = messages[idx].copy(tool = t.copy(result = result))
        }
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
        if (ok && autoTts) {
            val last = messages.lastOrNull { it.role == "assistant" && !it.isError }
            if (last != null) {
                DeviceTools.ttsSpeak(last.text.take(500))
            }
        }
        if (ok) {
            val s = currentSession
            if (s != null && s.title == "新会话") {
                generateTitle(s)
            }
        }
    }

    /** 会话标题 LLM 生成（首轮回复后异步，失败保留默认名） */
    private fun generateTitle(session: ChatSession) {
        viewModelScope.launch(Dispatchers.IO) {
            try {
                val provider = JSONObject()
                    .put("base_url", providerBaseUrl)
                    .put("api_key", providerApiKey)
                    .put("model", providerModel)
                val content = session.messages
                    .take(6)
                    .joinToString("\n") { "${it.role}: ${it.text.take(200)}" }
                val result = Engine.nativeGenerateTitle(provider.toString(), content)
                val parsed = JSONObject(result)
                if (parsed.optBoolean("ok")) {
                    val t = parsed.optString("title").trim().take(20)
                    if (t.isNotBlank()) {
                        session.title = t
                        saveSessionMeta()
                    }
                }
            } catch (_: Exception) {
                // 标题生成失败不影响主流程
            }
        }
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

    /** 重试：回到最后一条用户消息，清掉其后的 assistant 消息并重新生成 */
    fun retryLast() {
        if (busy) return
        val session = currentSession ?: return
        val lastUser = session.messages.indexOfLast { it.role == "user" && !it.isError }
        if (lastUser < 0) return
        while (session.messages.size > lastUser + 1) {
            session.messages.removeAt(session.messages.size - 1)
        }
        val text = session.messages[lastUser].text
        session.updatedAt = System.currentTimeMillis()
        saveSession()
        sendInternal(text)
    }

    /** 图片直发（多模态）：图片 + 描述作为一条用户消息 */
    fun sendImage(uri: Uri, caption: String) {
        if (busy) return
        busy = true
        viewModelScope.launch(Dispatchers.IO) {
            val session = currentSession ?: run {
                newSession()
                return@launch
            }
            try {
                val path = copyUriToFile(uri)
                val text = caption.ifBlank { "🖼️ 图片" }
                session.messages.add(ChatMsg("user", text, imagePath = path))
                if (session.title == "新会话") {
                    session.title = "图片会话"
                }
                session.updatedAt = System.currentTimeMillis()
                saveSession()
                sendInternal(text, path)
            } catch (e: Exception) {
                session.messages.add(
                    ChatMsg("assistant", "图片发送失败：${e.message}", isError = true),
                )
                busy = false
                saveSession()
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

    private fun sendInternal(text: String, imagePath: String? = null) {
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
                if (m.isError || m.tool != null || m.text.startsWith("⚙️")) continue
                val jo = JSONObject().put("role", m.role).put("content", m.text)
                if (m.role == "user" && m.imagePath != null) {
                    jo.put("image_path", m.imagePath)
                }
                history.put(jo)
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

    fun updateAutoTts(on: Boolean) {
        autoTts = on
        prefs.edit().putBoolean("auto_tts", on).apply()
    }

    fun updateThemeMode(mode: Int) {
        themeMode = mode
        prefs.edit().putInt("theme_mode", mode).apply()
    }

    fun updateFontSize(size: Int) {
        fontSize = size
        prefs.edit().putInt("font_size", size).apply()
    }

    fun clearAllData() {
        prefs.edit().clear().apply()
        sessions.clear()
        newSession()
    }

    fun deleteMessage(index: Int) {
        val session = currentSession ?: return
        if (index in 0 until session.messages.size) {
            session.messages.removeAt(index)
            saveSession()
        }
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
            if (m.tool != null) continue  // 工具消息不持久化（重启后模型上下文亦排除）
            val jo = JSONObject()
                .put("role", m.role)
                .put("text", m.text)
                .put("error", m.isError)
            if (m.imagePath != null) {
                jo.put("image", m.imagePath)
            }
            arr.put(jo)
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
                        imagePath = if (o.has("image")) o.getString("image") else null,
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
