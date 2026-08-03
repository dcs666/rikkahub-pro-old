package dev.rikkahub.ce

import android.content.Context
import org.json.JSONArray
import org.json.JSONObject
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * 会话存储查询（引擎 recent_chats / conversation_search 工具的反调实现）。
 * 与 ChatViewModel 共享 "rikka_ce" SharedPreferences。
 */
object ChatStore {

    @Volatile
    var appContext: Context? = null

    fun init(context: Context) {
        appContext = context.applicationContext
    }

    private fun prefs(): android.content.SharedPreferences? {
        val ctx = appContext ?: return null
        return ctx.getSharedPreferences("rikka_ce", Context.MODE_PRIVATE)
    }

    /** 最近会话列表（pinned 优先 + 时间降序） */
    @JvmStatic
    fun recentChats(limit: Int): String? {
        val p = prefs() ?: return null
        val sessions = loadSessions(p) ?: return "[]"
        val out = JSONArray()
        val fmt = SimpleDateFormat("yyyy-MM-dd", Locale.US)
        var n = 0
        for (i in 0 until sessions.length()) {
            if (n >= limit) break
            val s = sessions.getJSONObject(i)
            val updated = s.optLong("updated", 0L)
            out.put(JSONObject()
                .put("id", s.getString("id"))
                .put("title", s.optString("title").ifBlank { "Untitled" })
                .put("last_chat", fmt.format(Date(updated))))
            n++
        }
        return out.toString()
    }

    /** 全文搜索（大小写不敏感子串；snippet 含 [kw] 高亮） */
    @JvmStatic
    fun conversationSearch(query: String): String? {
        val p = prefs() ?: return null
        val sessions = loadSessions(p) ?: return "[]"
        val q = query.lowercase(Locale.US)
        val out = JSONArray()
        val fmt = SimpleDateFormat("yyyy-MM-dd", Locale.US)
        for (si in 0 until sessions.length()) {
            val s = sessions.getJSONObject(si)
            val id = s.getString("id")
            val title = s.optString("title").ifBlank { "Untitled" }
            val raw = p.getString("session_$id", null) ?: continue
            val msgs = JSONArray(raw)
            for (i in 0 until msgs.length()) {
                val text = msgs.getJSONObject(i).optString("text")
                if (text.lowercase(Locale.US).contains(q)) {
                    val snippet = makeSnippet(text, query)
                    out.put(JSONObject()
                        .put("id", id)
                        .put("title", title)
                        .put("snippet", snippet)
                        .put("date", fmt.format(Date(s.optLong("updated", 0L)))))
                    break
                }
            }
        }
        return out.toString()
    }

    /** [CE] 会话索引写入(ChatService.saveConversation 调用; 引擎 recent_chats 数据源) */
    @JvmStatic
    fun indexConversation(id: String, title: String, updated: Long) {
        val p = prefs() ?: return
        val sessions = loadSessions(p) ?: JSONArray()
        val out = JSONArray()
        for (i in 0 until sessions.length()) {
            val s = sessions.optJSONObject(i) ?: continue
            if (s.optString("id") != id) out.put(s)
        }
        out.put(JSONObject().put("id", id).put("title", title).put("updated", updated))
        // 会话索引上限 200(防 prefs 无限增长)
        while (out.length() > 200) out.remove(0)
        p.edit().putString("sessions", out.toString()).apply()
    }

    /** [CE] 会话消息索引写入(引擎 conversation_search 数据源; 每条文本消息追加) */
    @JvmStatic
    fun indexMessage(id: String, text: String) {
        if (text.isBlank()) return
        val p = prefs() ?: return
        val key = "session_$id"
        val arr = try {
            JSONArray(p.getString(key, null) ?: "[]")
        } catch (_: Exception) {
            JSONArray()
        }
        arr.put(JSONObject().put("text", text.take(4000)))
        // 单会话消息索引上限 500 条(防 prefs 无限增长)
        while (arr.length() > 500) arr.remove(0)
        p.edit().putString(key, arr.toString()).apply()
    }

    private fun loadSessions(p: android.content.SharedPreferences): JSONArray? {
        val raw = p.getString("sessions", null) ?: return null
        return try {
            JSONArray(raw)
        } catch (_: Exception) {
            null
        }
    }

    private fun makeSnippet(text: String, query: String): String {
        val idx = text.lowercase(Locale.US).indexOf(query.lowercase(Locale.US))
        if (idx < 0) return text.take(100)
        val start = (idx - 40).coerceAtLeast(0)
        val end = (idx + query.length + 60).coerceAtMost(text.length)
        val prefix = if (start > 0) "..." else ""
        val suffix = if (end < text.length) "..." else ""
        val match = text.substring(idx, (idx + query.length).coerceAtMost(text.length))
        return prefix + text.substring(start, idx) + "[" + match + "]" +
                text.substring(idx + query.length, end) + suffix
    }
}
