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

    /** JavaScript 求值（当前返回错误：WebView 环境打磨期接入） */
    @JvmStatic
    fun javascriptEval(code: String): String? {
        return err("javascript eval not available")
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
