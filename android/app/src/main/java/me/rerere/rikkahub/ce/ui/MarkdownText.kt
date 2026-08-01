package me.rerere.rikkahub.ce.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.SpanStyle
import androidx.compose.ui.text.buildAnnotatedString
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.withStyle
import androidx.compose.ui.text.withStyle
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

/**
 * 轻量 Markdown 渲染（引擎 md 渲染器的 UI 端补充）：
 * 代码块 (```)、行内代码 (`)、粗体 (**)、列表项 (- / * / 数字)。
 */
@Composable
fun MarkdownText(text: String, modifier: Modifier = Modifier) {
    val codeColor = MaterialTheme.colorScheme.surfaceVariant
    val onCode = MaterialTheme.colorScheme.onSurfaceVariant

    val blocks = text.split("```")
    if (blocks.size < 3) {
        Text(
            inlineMarkdown(text),
            modifier = modifier,
            fontSize = 15.sp,
            lineHeight = 21.sp,
        )
        return
    }
    Column(modifier = modifier.fillMaxWidth()) {
        for ((i, block) in blocks.withIndex()) {
            if (i % 2 == 1) {
                // 代码块
                val code = block.removePrefix("kotlin").removePrefix("c")
                    .removePrefix("java").removePrefix("python").removePrefix("bash")
                    .removePrefix("json").removePrefix("xml").removePrefix("text")
                    .removePrefix("plain").trimStart('\n')
                Text(
                    code.trim('\n'),
                    fontFamily = FontFamily.Monospace,
                    fontSize = 13.sp,
                    lineHeight = 18.sp,
                    modifier = Modifier
                        .fillMaxWidth()
                        .background(codeColor, RoundedCornerShape(8.dp))
                        .padding(10.dp),
                    color = onCode,
                )
            } else if (block.isNotBlank()) {
                Text(
                    inlineMarkdown(block),
                    fontSize = 15.sp,
                    lineHeight = 21.sp,
                )
            }
        }
    }
}

/** 行内 Markdown：**粗体**、`行内代码`、- 列表 */
private fun inlineMarkdown(text: String): AnnotatedString = buildAnnotatedString {
    var i = 0
    val n = text.length
    var listPrefix = true
    while (i < n) {
        val c = text[i]
        // 列表项
        if (listPrefix && (c == '-' || c == '•' || c == '*') && i + 1 < n && text[i + 1] == ' ') {
            append("• ")
            i += 2
            listPrefix = false
            continue
        }
        if (c == '\n') {
            append(c)
            i++
            listPrefix = true
            continue
        }
        listPrefix = false
        // 行内代码
        if (c == '`') {
            val end = text.indexOf('`', i + 1)
            if (end > 0) {
                withStyle(SpanStyle(fontFamily = FontFamily.Monospace, fontSize = 13.sp)) {
                    append(text.substring(i + 1, end))
                }
                i = end + 1
                continue
            }
        }
        // 粗体
        if (c == '*' && i + 2 < n && text[i + 1] == '*' && i + 2 < n) {
            val end = text.indexOf("**", i + 2)
            if (end > 0) {
                withStyle(SpanStyle(fontWeight = FontWeight.Bold)) {
                    append(text.substring(i + 2, end))
                }
                i = end + 2
                continue
            }
        }
        append(c)
        i++
    }
}
