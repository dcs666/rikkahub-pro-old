package dev.rikkahub.ce.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.SpanStyle
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.buildAnnotatedString
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextDecoration
import androidx.compose.ui.text.withStyle
import androidx.compose.ui.unit.TextUnit
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

/**
 * 轻量 Markdown 渲染（引擎 md 渲染器的 UI 端补充）：
 * 代码块（语言标签+复制）、标题 #/##/###、引用 >、行内代码、粗体、斜体、链接、列表。
 */
@Composable
fun MarkdownText(text: String, modifier: Modifier = Modifier, fontSize: TextUnit = 15.sp) {
    Column(modifier = modifier.fillMaxWidth()) {
        for (block in splitBlocks(text)) {
            when (block) {
                is Block.Code -> CodeBlock(block)
                is Block.Heading -> Text(
                    inlineMarkdown(block.content, MaterialTheme.colorScheme.surfaceVariant, MaterialTheme.colorScheme.primary),
                    fontSize = when (block.level) {
                        1 -> 20.sp
                        2 -> 18.sp
                        else -> 16.sp
                    },
                    lineHeight = when (block.level) {
                        1 -> 27.sp
                        2 -> 25.sp
                        else -> 23.sp
                    },
                    fontWeight = FontWeight.Bold,
                    modifier = Modifier.padding(top = 6.dp, bottom = 2.dp),
                )
                is Block.Quote -> QuoteBlock(block.content, fontSize)
                is Block.Para -> Text(
                    inlineMarkdown(block.content, MaterialTheme.colorScheme.surfaceVariant, MaterialTheme.colorScheme.primary),
                    fontSize = fontSize,
                    lineHeight = (fontSize.value * 1.4).sp,
                )
            }
        }
    }
}

/* ---------- 块模型与解析 ---------- */

private sealed class Block {
    data class Code(val lang: String, val code: String) : Block()
    data class Heading(val level: Int, val content: String) : Block()
    data class Quote(val content: String) : Block()
    data class Para(val content: String) : Block()
}

private fun splitBlocks(text: String): List<Block> {
    val blocks = mutableListOf<Block>()
    val lines = text.split('\n')
    var i = 0
    while (i < lines.size) {
        val line = lines[i]
        when {
            line.trimStart().startsWith("```") -> {
                val lang = line.trimStart().removePrefix("```").trim()
                val sb = StringBuilder()
                i++
                while (i < lines.size && !lines[i].trimStart().startsWith("```")) {
                    sb.append(lines[i]).append('\n')
                    i++
                }
                i++ // 跳过结束 ```
                blocks.add(Block.Code(lang, sb.toString().trim('\n')))
            }
            line.startsWith("### ") || line == "###" -> {
                blocks.add(Block.Heading(3, line.removePrefix("###").trim()))
                i++
            }
            line.startsWith("## ") || line == "##" -> {
                blocks.add(Block.Heading(2, line.removePrefix("##").trim()))
                i++
            }
            line.startsWith("# ") || line == "#" -> {
                blocks.add(Block.Heading(1, line.removePrefix("#").trim()))
                i++
            }
            line.startsWith(">") -> {
                val sb = StringBuilder()
                while (i < lines.size && lines[i].startsWith(">")) {
                    sb.append(lines[i].removePrefix(">").trim()).append('\n')
                    i++
                }
                blocks.add(Block.Quote(sb.toString().trim('\n')))
            }
            line.isBlank() -> i++
            else -> {
                val sb = StringBuilder()
                while (i < lines.size && lines[i].isNotBlank() &&
                    !lines[i].trimStart().startsWith("```") &&
                    !lines[i].startsWith("#") && !lines[i].startsWith(">")
                ) {
                    sb.append(lines[i]).append('\n')
                    i++
                }
                blocks.add(Block.Para(sb.toString().trim('\n')))
            }
        }
    }
    return blocks
}

/* ---------- 块渲染 ---------- */

@Composable
private fun CodeBlock(block: Block.Code) {
    val scheme = MaterialTheme.colorScheme
    val context = LocalContext.current
    Column(
        modifier = Modifier
            .fillMaxWidth()
            .background(scheme.surfaceVariant, RoundedCornerShape(8.dp))
            .padding(top = 2.dp),
    ) {
        Row(
            modifier = Modifier.fillMaxWidth().padding(start = 10.dp, end = 2.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text(
                block.lang.ifBlank { "code" },
                color = scheme.onSurfaceVariant,
                fontSize = 11.sp,
                modifier = Modifier.weight(1f),
            )
            IconButton(
                onClick = {
                    val cm = context.getSystemService(android.content.Context.CLIPBOARD_SERVICE)
                            as android.content.ClipboardManager
                    cm.setPrimaryClip(
                        android.content.ClipData.newPlainText("rikka-code", block.code),
                    )
                },
                modifier = Modifier.size(28.dp),
            ) {
                Text("📋", fontSize = 12.sp)
            }
        }
        Text(
            block.code,
            fontFamily = FontFamily.Monospace,
            fontSize = 13.sp,
            lineHeight = 18.sp,
            color = scheme.onSurfaceVariant,
            modifier = Modifier
                .fillMaxWidth()
                .padding(start = 10.dp, end = 10.dp, bottom = 8.dp),
        )
    }
    Spacer(Modifier.size(4.dp))
}

@Composable
private fun QuoteBlock(content: String, fontSize: TextUnit) {
    val scheme = MaterialTheme.colorScheme
    Row(modifier = Modifier.fillMaxWidth().padding(vertical = 2.dp)) {
        Box(
            modifier = Modifier
                .width(3.dp)
                .background(scheme.primary, RoundedCornerShape(2.dp)),
        )
        Spacer(Modifier.width(8.dp))
        Text(
            inlineMarkdown(content, MaterialTheme.colorScheme.surfaceVariant, MaterialTheme.colorScheme.primary),
            fontSize = fontSize,
            lineHeight = (fontSize.value * 1.4).sp,
            color = scheme.onSurfaceVariant,
            modifier = Modifier.weight(1f),
        )
    }
}

/* ---------- 行内解析 ---------- */

/** 行内 Markdown：**粗体**、*斜体*、`行内代码`、[链接](url)、- 列表 */
private fun inlineMarkdown(text: String, codeBg: androidx.compose.ui.graphics.Color, linkColor: androidx.compose.ui.graphics.Color): AnnotatedString = buildAnnotatedString {
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
        // 链接 [text](url)
        if (c == '[') {
            val close = text.indexOf(']', i + 1)
            if (close > 0 && close + 1 < n && text[close + 1] == '(') {
                val urlEnd = text.indexOf(')', close + 2)
                if (urlEnd > close + 1) {
                    val label = text.substring(i + 1, close)
                    val url = text.substring(close + 2, urlEnd)
                    withStyle(
                        SpanStyle(
                            color = linkColor,
                            textDecoration = TextDecoration.Underline,
                        ),
                    ) {
                        append(label)
                    }
                    pushStringAnnotation("URL", url)
                    i = urlEnd + 1
                    continue
                }
            }
        }
        // 行内代码
        if (c == '`') {
            val end = text.indexOf('`', i + 1)
            if (end > 0) {
                withStyle(
                    SpanStyle(
                        fontFamily = FontFamily.Monospace,
                        fontSize = 13.sp,
                        background = codeBg,
                    ),
                ) {
                    append(text.substring(i + 1, end))
                }
                i = end + 1
                continue
            }
        }
        // 粗体 **
        if (c == '*' && i + 1 < n && text[i + 1] == '*') {
            val end = text.indexOf("**", i + 2)
            if (end > 0) {
                withStyle(SpanStyle(fontWeight = FontWeight.Bold)) {
                    append(text.substring(i + 2, end))
                }
                i = end + 2
                continue
            }
        }
        // 斜体 *（仅当后一个字符不是空格/星号）
        if (c == '*' && i + 1 < n && text[i + 1] != ' ' && text[i + 1] != '*') {
            val end = text.indexOf('*', i + 1)
            if (end > 0) {
                withStyle(SpanStyle(fontStyle = androidx.compose.ui.text.font.FontStyle.Italic)) {
                    append(text.substring(i + 1, end))
                }
                i = end + 1
                continue
            }
        }
        append(c)
        i++
    }
}
