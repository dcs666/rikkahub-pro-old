package me.rerere.highlight

import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.runtime.snapshotFlow
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.buildAnnotatedString
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.TextUnit
import androidx.compose.ui.unit.sp
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

private const val MAX_CODE_LENGTH = 4096

// [TURBO] 全局高亮结果 LRU 缓存（引擎无关，跨 composition 生命周期生效）。
// LazyColumn 滚出回收后滚回时 remember 状态丢失，上游 CodeHighlightText 会重新 tokenize；
// 本缓存让滚回命中零重复计算。accessOrder=true 使 get 也刷新顺序（真 LRU），多线程同步保护。
private object HighlightCache {
    private const val MAX_SIZE = 100
    private val cache = object : LinkedHashMap<String, List<HighlightToken>>(64, 0.75f, true) {
        override fun removeEldestEntry(eldest: MutableMap.MutableEntry<String, List<HighlightToken>>?): Boolean =
            size > MAX_SIZE
    }

    private fun key(code: String, language: String): String = "$language\u0000$code"

    fun get(code: String, language: String): List<HighlightToken>? =
        synchronized(this) { cache[key(code, language)] }

    fun put(code: String, language: String, tokens: List<HighlightToken>) {
        synchronized(this) { cache[key(code, language)] = tokens }
    }
}

/**
 * Async code highlighting built on the upstream pure-Kotlin [CodeHighlighter].
 *
 * [TURBO] Unlike upstream's synchronous [CodeHighlightText] (which runs the engine inside
 * `remember` on the main thread during composition), this component runs the engine on
 * [Dispatchers.Default] via a snapshotFlow, so opening a conversation with many code blocks
 * never blocks the main thread. Results are cached in the process-wide [HighlightCache], so
 * scrolling back into a recycled item costs zero re-tokenization.
 */
@Composable
fun HighlightText(
    code: String,
    language: String,
    modifier: Modifier = Modifier,
    colors: HighlightTextColorPalette = HighlightTextColorPalette.Default,
    fontSize: TextUnit = 12.sp,
    fontFamily: FontFamily = FontFamily.Monospace,
    fontStyle: FontStyle = FontStyle.Normal,
    fontWeight: FontWeight = FontWeight.Normal,
    lineHeight: TextUnit = TextUnit.Unspecified,
    overflow: TextOverflow = TextOverflow.Clip,
    softWrap: Boolean = true,
    maxLines: Int = Int.MAX_VALUE,
    minLines: Int = 1,
) {
    val highlighter = LocalCodeHighlighter.current
    var annotatedString by remember { mutableStateOf(AnnotatedString(code)) }

    val updatedCode by rememberUpdatedState(code)
    val updatedLanguage by rememberUpdatedState(language)
    LaunchedEffect(Unit) {
        snapshotFlow { updatedCode to updatedLanguage }.collect { (currentCode, currentLanguage) ->
            val tokens = if (currentCode.length <= MAX_CODE_LENGTH) {
                HighlightCache.get(currentCode, currentLanguage)
                    ?: withContext(Dispatchers.Default) {
                        highlighter.highlight(currentCode, currentLanguage)
                            .also { HighlightCache.put(currentCode, currentLanguage, it) }
                    }
            } else {
                listOf(HighlightToken.Plain(currentCode))
            }
            annotatedString = buildAnnotatedString {
                tokens.forEach { token ->
                    buildHighlightText(token, colors)
                }
            }
        }
    }

    Text(
        modifier = modifier,
        text = annotatedString,
        fontSize = fontSize,
        fontFamily = fontFamily,
        fontStyle = fontStyle,
        fontWeight = fontWeight,
        lineHeight = lineHeight,
        overflow = overflow,
        softWrap = softWrap,
        maxLines = maxLines,
        minLines = minLines,
    )
}
