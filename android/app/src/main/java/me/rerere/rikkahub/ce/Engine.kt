package me.rerere.rikkahub.ce

/** 引擎回调（增量/工具/完成，调用线程同步触发） */
interface ChatCallback {
    fun onDelta(kind: Int, text: String)   // kind: 0=text, 1=reasoning
    fun onToolCall(name: String, args: String)
    fun onToolResult(name: String, result: String)
    fun onFinish(ok: Boolean, error: String?)
}

/** C 引擎 JNI 入口 */
object Engine {
    init {
        System.loadLibrary("rikka")
    }

    /**
     * 运行一轮对话（含工具循环）。阻塞调用线程，请放在协程 IO 线程。
     * @param providerJson {"base_url","api_key","model"}
     * @param historyJson  [{"role":"user|assistant|system","content":"..."}]
     * @return {"ok":true,"text":"..."} 或 {"ok":false,"error":"..."}
     */
    external fun nativeChat(providerJson: String, historyJson: String, workspaceRoot: String?, callback: ChatCallback): String

    external fun nativeSetCancel(cancel: Boolean)

    /** 图片 OCR：返回 {"ok":true,"text":"..."} 或 {"ok":false,"error":"..."} */
    external fun nativeOcr(providerJson: String, imagePath: String): String
}
