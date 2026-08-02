package dev.rikkahub.ce

import android.app.Dialog
import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.graphics.Color
import android.os.Handler
import android.os.Looper
import android.speech.tts.TextToSpeech
import android.view.Gravity
import android.view.WindowManager
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.TextView
import android.widget.Toast
import org.json.JSONArray
import org.json.JSONObject
import java.util.Locale
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicReference

/**
 * 设备工具实现（引擎 JNI 反调）。
 * 所有方法在引擎调用线程（协程 IO 线程）执行；UI 操作切主线程。
 */
object DeviceTools {

    @Volatile
    var appContext: Context? = null

    private val mainHandler = Handler(Looper.getMainLooper())

    @Volatile
    private var tts: TextToSpeech? = null

    fun init(context: Context) {
        appContext = context.applicationContext
    }

    /** 向用户提问（模态对话框，阻塞等待回答） */
    @JvmStatic
    fun askUser(question: String): String? {
        val ctx = appContext ?: return err("no context")
        val latch = CountDownLatch(1)
        val answer = AtomicReference<String?>(null)
        mainHandler.post {
            val dlg = Dialog(ctx)
            val root = LinearLayout(ctx).apply {
                orientation = LinearLayout.VERTICAL
                setPadding(48, 32, 48, 32)
            }
            root.addView(TextView(ctx).apply {
                text = question
                textSize = 16f
            })
            val input = EditText(ctx)
            root.addView(input, LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT))
            val send = Button(ctx).apply { text = "回答" }
            send.setOnClickListener {
                answer.set(input.text.toString())
                dlg.dismiss()
                latch.countDown()
            }
            val cancelBtn = Button(ctx).apply { text = "取消" }
            cancelBtn.setOnClickListener {
                dlg.dismiss()
                latch.countDown()
            }
            root.addView(send)
            root.addView(cancelBtn)
            dlg.setContentView(root)
            dlg.setOnDismissListener { latch.countDown() }
            dlg.window?.setBackgroundDrawableResource(android.R.color.white)
            dlg.show()
        }
        val ok = try {
            latch.await(120, TimeUnit.SECONDS)
        } catch (_: InterruptedException) {
            false
        }
        if (!ok) return err("ask_user timeout")
        val a = answer.get() ?: return err("ask_user cancelled")
        return JSONObject().put("answer", a).toString()
    }

    /** 写入剪贴板 */
    @JvmStatic
    fun clipboardWrite(text: String): Boolean {
        val ctx = appContext ?: return false
        try {
            val cm = ctx.getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
            cm.setPrimaryClip(ClipData.newPlainText("rikka", text))
            return true
        } catch (_: Exception) {
            return false
        }
    }

    /** TTS 朗读 */
    @JvmStatic
    fun ttsSpeak(text: String): Boolean {
        val ctx = appContext ?: return false
        if (tts == null) {
            val latch = CountDownLatch(1)
            tts = TextToSpeech(ctx) { status -> latch.countDown() }
            try { latch.await(5, TimeUnit.SECONDS) } catch (_: InterruptedException) {}
        }
        val t = tts ?: return false
        return try {
            t.language = Locale.getDefault()
            t.speak(text, TextToSpeech.QUEUE_FLUSH, null, "rikka-tts")
            true
        } catch (_: Exception) {
            false
        }
    }

    /** 日历查询（返回 JSON 数组字符串） */
    @JvmStatic
    fun calendarQuery(args: String): String? {
        val ctx = appContext ?: return err("no context")
        return try {
            val req = JSONObject(args)
            val out = JSONArray()
            val resolver = ctx.contentResolver
            val uri = android.provider.CalendarContract.Events.CONTENT_URI
            val cols = arrayOf(
                "title", "description", "eventLocation",
                "dtstart", "dtend", "allDay")
            val cursor = resolver.query(uri, cols, null, null,
                "dtstart ASC LIMIT 50")
            cursor?.use { c ->
                while (c.moveToNext() && out.length() < 50) {
                    out.put(JSONObject()
                        .put("title", c.getString(0) ?: "")
                        .put("description", c.getString(1) ?: "")
                        .put("location", c.getString(2) ?: "")
                        .put("start", c.getLong(3))
                        .put("end", c.getLong(4))
                        .put("all_day", c.getInt(5) != 0))
                }
            }
            out.toString()
        } catch (_: Exception) {
            err("calendar permission denied")
        }
    }

    /** 屏幕时间查询（UsageStats，需特殊权限；无权限返回错误） */
    @JvmStatic
    fun screenTimeQuery(args: String): String? {
        val ctx = appContext ?: return err("no context")
        return try {
            val req = JSONObject(args)
            val um = ctx.getSystemService(Context.USAGE_STATS_SERVICE)
                    as android.app.usage.UsageStatsManager
            val now = System.currentTimeMillis()
            val begin = now - (req.optLong("hours", 24) * 3600_000L)
            val stats = um.queryUsageStats(
                android.app.usage.UsageStatsManager.INTERVAL_DAILY,
                begin, now)
            val out = JSONArray()
            stats?.forEach { s ->
                if (s.totalTimeInForeground > 0) {
                    out.put(JSONObject()
                        .put("package", s.packageName)
                        .put("foreground_ms", s.totalTimeInForeground))
                }
            }
            out.toString()
        } catch (_: Exception) {
            err("screen time permission denied")
        }
    }

    /** JavaScript 求值（WebView 环境，主线程 + latch 桥；10s 超时） */
    @JvmStatic
    fun javascriptEval(code: String): String? {
        val ctx = appContext ?: return err("no context")
        val latch = CountDownLatch(1)
        val result = AtomicReference<String?>(null)
        mainHandler.post {
            try {
                val webView = android.webkit.WebView(ctx)
                webView.settings.javaScriptEnabled = true
                val quoted = org.json.JSONObject.quote(code)
                val js = "(function(){try{" +
                        "return JSON.stringify({ok:true,result:eval($quoted)})" +
                        "}catch(e){return JSON.stringify({ok:false,error:String(e)})}})()"
                webView.evaluateJavascript(js) { r ->
                    try {
                        // r 是 JSON 编码字符串（带引号），解包后即为 {ok,result}
                        val inner = org.json.JSONTokener(r).nextValue().toString()
                        result.set(inner)
                    } catch (_: Exception) {
                        result.set(err("js eval parse failed"))
                    }
                    latch.countDown()
                }
            } catch (e: Exception) {
                result.set(err("js eval failed: ${e.message}"))
                latch.countDown()
            }
        }
        try {
            latch.await(10, TimeUnit.SECONDS)
        } catch (_: InterruptedException) {
        }
        return result.get() ?: err("js eval timeout")
    }

    /** [CE] search_web 引擎反调桥：阻塞执行 SearchService.search（引擎工作线程调用） */
    @JvmStatic
    fun webSearch(query: String): String? {
        val ctx = appContext ?: return err("no context")
        return runCatching {
            val settingsStore = org.koin.java.KoinJavaComponent.getKoin()
                .get<me.rerere.rikkahub.data.datastore.SettingsStore>()
            val settings = settingsStore.settingsFlow.value
            val options = settings.searchServices.getOrElse(
                index = settings.searchServiceSelected,
                defaultValue = { me.rerere.search.SearchServiceOptions.DEFAULT },
            )
            val service = me.rerere.search.SearchService.getService(options)
            val params = JSONObject().put("query", query)
            val jsonObj = kotlinx.serialization.json.Json
                .parseToJsonElement(params.toString()).jsonObject
            val result = kotlinx.coroutines.runBlocking {
                service.search(jsonObj, settings.searchCommonOptions, options)
            }.getOrNull() ?: return err("search failed")
            // 对齐 turbo: items 加 id/index
            val encoded = me.rerere.rikkahub.utils.JsonInstantPretty
                .encodeToJsonElement(result).jsonObject
            val map = encoded.toMutableMap()
            map["items"] = kotlinx.serialization.json.JsonArray(
                encoded["items"]?.jsonArray?.mapIndexed { index, item ->
                    kotlinx.serialization.json.JsonObject(
                        item.jsonObject.toMutableMap().apply {
                            put(
                                "id",
                                kotlinx.serialization.json.JsonPrimitive(
                                    kotlin.uuid.Uuid.random().toString().take(6),
                                ),
                            )
                            put("index", kotlinx.serialization.json.JsonPrimitive(index + 1))
                        },
                    )
                } ?: emptyList(),
            )
            kotlinx.serialization.json.JsonObject(map).toString()
        }.getOrNull()
    }

    /** [CE] calendar_create 引擎反调桥：插入系统日历事件（引擎工作线程调用） */
    @JvmStatic
    fun calendarCreate(args: String): String? {
        val ctx = appContext ?: return err("no context")
        return runCatching {
            val p = JSONObject(args)
            val title = p.optString("title")
            val startRaw = p.optString("start")
            if (title.isBlank() || startRaw.isBlank()) return err("both title and start are required")
            val startMillis = parseCalendarTime(startRaw)
                ?: return err("invalid start time")
            val endMillis = if (p.optString("end").isNotBlank()) {
                parseCalendarTime(p.optString("end")) ?: return err("invalid end time")
            } else {
                startMillis + 3600_000L
            }
            if (endMillis <= startMillis) return err("end must be after start")
            val granted = androidx.core.content.ContextCompat.checkSelfPermission(
                ctx, android.Manifest.permission.WRITE_CALENDAR,
            ) == android.content.pm.PackageManager.PERMISSION_GRANTED
            if (!granted) return err("NO_PERMISSION: calendar write permission not granted")
            val values = android.content.ContentValues().apply {
                put(android.provider.CalendarContract.Events.CALENDAR_ID, 1L)
                put(android.provider.CalendarContract.Events.TITLE, title)
                put(android.provider.CalendarContract.Events.DTSTART, startMillis)
                put(android.provider.CalendarContract.Events.DTEND, endMillis)
                put(
                    android.provider.CalendarContract.Events.EVENT_TIMEZONE,
                    java.util.TimeZone.getDefault().id,
                )
                if (p.has("description")) {
                    put(android.provider.CalendarContract.Events.DESCRIPTION, p.optString("description"))
                }
                if (p.has("location")) {
                    put(android.provider.CalendarContract.Events.EVENT_LOCATION, p.optString("location"))
                }
            }
            val uri = ctx.contentResolver.insert(
                android.provider.CalendarContract.Events.CONTENT_URI, values,
            )
            if (uri == null) {
                err("calendar insert failed")
            } else {
                JSONObject().put("ok", true).put("uri", uri.toString()).toString()
            }
        }.getOrNull()
    }

    /** 解析 ISO-8601 日期/日期时间/epoch 毫秒；失败返回 null */
    private fun parseCalendarTime(raw: String): Long? = runCatching {
        val trimmed = raw.trim()
        trimmed.toLongOrNull()?.let { return it }
        when {
            trimmed.matches(Regex("""\d{4}-\d{2}-\d{2}""")) -> {
                java.time.LocalDate.parse(trimmed)
                    .atStartOfDay(java.time.ZoneId.systemDefault()).toInstant().toEpochMilli()
            }
            trimmed.matches(Regex("""\d{4}-\d{2}-\d{2}T\d{2}:\d{2}(:\d{2})?""")) -> {
                val dt = java.time.LocalDateTime.parse(
                    if (trimmed.length == 16) trimmed + ":00" else trimmed,
                )
                dt.atZone(java.time.ZoneId.systemDefault()).toInstant().toEpochMilli()
            }
            trimmed.endsWith("Z") -> java.time.Instant.parse(trimmed).toEpochMilli()
            else -> {
                java.time.OffsetDateTime.parse(trimmed).toInstant().toEpochMilli()
            }
        }
    }.getOrNull()

    /** [CE] memory_tool 引擎反调桥：落库到 MemoryRepository（引擎工作线程调用） */
    @JvmStatic
    fun memoryAction(action: String, id: Long, content: String, assistantId: String): String? {
        return runCatching {
            val memoryRepo = org.koin.java.KoinJavaComponent.getKoin()
                .get<me.rerere.rikkahub.data.repository.MemoryRepository>()
            kotlinx.coroutines.runBlocking {
                when (action) {
                    "create" -> memoryRepo.addMemory(assistantId, content)
                    "edit" -> memoryRepo.updateContent(id.toInt(), content)
                    "delete" -> memoryRepo.deleteMemory(id.toInt())
                    else -> return null
                }
            }
            JSONObject().put("ok", true).toString()
        }.getOrNull()
    }

    private fun err(msg: String): String {
        return JSONObject().put("error", msg).toString()
    }

    /** 通知类工具反馈（Toast，非阻塞） */
    fun toast(msg: String) {
        mainHandler.post {
            appContext?.let { Toast.makeText(it, msg, Toast.LENGTH_SHORT).show() }
        }
    }
}
