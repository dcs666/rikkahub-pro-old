package dev.rikkahub.ce.ui

import android.content.Context
import android.net.Uri
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import androidx.compose.material3.DrawerValue
import androidx.compose.material3.FilterChip
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalDrawerSheet
import androidx.compose.material3.ModalNavigationDrawer
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Slider
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.rememberDrawerState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.produceState
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import dev.rikkahub.ce.BuildConfig
import kotlinx.coroutines.launch




@Composable
fun ChatScreen(vm: ChatViewModel) {
    var showSettings by remember { mutableStateOf(false) }
    val drawerState = rememberDrawerState(DrawerValue.Closed)
    val scope = rememberCoroutineScope()
    ModalNavigationDrawer(
        drawerState = drawerState,
        drawerContent = {
            SessionDrawer(
                vm,
                onClose = { scope.launch { drawerState.close() } },
            )
        },
    ) {
        Column(modifier = Modifier.fillMaxSize()) {
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(horizontal = 12.dp, vertical = 4.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                val cur = vm.sessions.firstOrNull { it.id == vm.currentSessionId }
                TextButton(onClick = { scope.launch { drawerState.open() } }) {
                    Text(
                        cur?.title ?: "会话",
                        maxLines = 1,
                        style = MaterialTheme.typography.titleMedium,
                    )
                }
                Spacer(Modifier.weight(1f))
                TextButton(onClick = { vm.clearSession() }) { Text("清空") }
                TextButton(onClick = { showSettings = true }) { Text("设置") }
            }
            MessageList(vm, Modifier.weight(1f))
            InputBar(vm)
        }
    }
    if (showSettings) {
        SettingsDialog(vm, onDismiss = { showSettings = false })
    }
}

@Composable
private fun SessionDrawer(vm: ChatViewModel, onClose: () -> Unit) {
    var editing by remember { mutableStateOf<ChatSession?>(null) }
    ModalDrawerSheet(modifier = Modifier.width(300.dp)) {
        Text(
            "会话",
            style = MaterialTheme.typography.titleLarge,
            modifier = Modifier.padding(start = 16.dp, top = 16.dp, bottom = 4.dp),
        )
        TextButton(
            onClick = {
                vm.newSession()
                onClose()
            },
            modifier = Modifier.padding(start = 8.dp),
        ) { Text("＋ 新建会话") }
        HorizontalDivider(modifier = Modifier.padding(vertical = 4.dp))
        LazyColumn(modifier = Modifier.weight(1f)) {
            items(vm.sessions, key = { it.id }) { s ->
                Row(
                    modifier = Modifier
                        .fillMaxWidth()
                        .clickable {
                            vm.switchSession(s.id)
                            onClose()
                        }
                        .padding(start = 16.dp, end = 4.dp, top = 6.dp, bottom = 6.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Column(modifier = Modifier.weight(1f)) {
                        Text(
                            s.title,
                            maxLines = 1,
                            fontSize = 14.sp,
                            fontWeight = if (s.id == vm.currentSessionId)
                                FontWeight.Bold else FontWeight.Normal,
                        )
                        Text(
                            formatDate(s.updatedAt),
                            fontSize = 11.sp,
                            color = MaterialTheme.colorScheme.outline,
                        )
                    }
                    if (vm.sessions.size > 1) {
                        IconButton(onClick = { editing = s }) {
                            Text("✏️", fontSize = 12.sp)
                        }
                        IconButton(onClick = { vm.deleteSession(s.id) }) {
                            Text("🗑", fontSize = 12.sp)
                        }
                    }
                }
            }
        }
    }
    editing?.let { s ->
        var title by remember(s.id) { mutableStateOf(s.title) }
        AlertDialog(
            onDismissRequest = { editing = null },
            title = { Text("重命名会话") },
            text = {
                OutlinedTextField(
                    value = title,
                    onValueChange = { title = it },
                    singleLine = true,
                )
            },
            confirmButton = {
                TextButton(onClick = {
                    vm.renameSession(s.id, title)
                    editing = null
                }) { Text("保存") }
            },
            dismissButton = {
                TextButton(onClick = { editing = null }) { Text("取消") }
            },
        )
    }
}

private fun formatDate(ts: Long): String {
    val now = System.currentTimeMillis()
    val fmt = if (now - ts < 24 * 3600_000L) {
        java.text.SimpleDateFormat("HH:mm", java.util.Locale.getDefault())
    } else {
        java.text.SimpleDateFormat("MM-dd", java.util.Locale.getDefault())
    }
    return fmt.format(java.util.Date(ts))
}

@Composable
private fun MessageList(vm: ChatViewModel, modifier: Modifier = Modifier) {
    val listState = rememberLazyListState()
    LaunchedEffect(vm.messages.size) {
        if (vm.messages.isNotEmpty()) {
            listState.animateScrollToItem(vm.messages.size - 1)
        }
    }
    LazyColumn(
        state = listState,
        modifier = modifier.fillMaxWidth(),
        contentPadding = androidx.compose.foundation.layout.PaddingValues(12.dp),
        verticalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        items(vm.messages) { msg ->
            MessageBubble(
                msg,
                onRetry = { vm.retryLast() },
                fontSize = vm.fontSize,
            )
        }
    }
}

@OptIn(ExperimentalFoundationApi::class)
@Composable
private fun MessageBubble(msg: ChatMsg, onRetry: () -> Unit, fontSize: Int = 15) {
    val isUser = msg.role == "user"
    val ctx = LocalContext.current
    val onCopy = {
        val cm = ctx.getSystemService(android.content.Context.CLIPBOARD_SERVICE)
                as android.content.ClipboardManager
        cm.setPrimaryClip(android.content.ClipData.newPlainText("rikka", msg.text))
    }
    if (msg.tool != null) {
        // 工具调用卡片（参数/结果折叠）
        ToolCard(msg.tool, modifier = Modifier.fillMaxWidth())
        return
    }
    Row(
        modifier = Modifier.fillMaxWidth(),
        horizontalArrangement = if (isUser) Arrangement.End else Arrangement.Start,
    ) {
        Card(
            colors = CardDefaults.cardColors(
                containerColor = when {
                    msg.isError -> MaterialTheme.colorScheme.errorContainer
                    isUser -> MaterialTheme.colorScheme.primaryContainer
                    else -> MaterialTheme.colorScheme.surfaceVariant
                },
            ),
            shape = RoundedCornerShape(12.dp),
            modifier = Modifier.combinedClickable(
                onClick = {},
                onLongClick = onCopy,
            ),
        ) {
            Column(modifier = Modifier.padding(horizontal = 12.dp, vertical = 8.dp)) {
                msg.imagePath?.let { path ->
                    val bitmap by produceState<android.graphics.Bitmap?>(initialValue = null, path) {
                        value = try {
                            android.graphics.BitmapFactory.decodeFile(path)
                        } catch (_: Exception) {
                            null
                        }
                    }
                    bitmap?.let { bmp ->
                        Image(
                            bitmap = bmp.asImageBitmap(),
                            contentDescription = "图片",
                            modifier = Modifier
                                .fillMaxWidth()
                                .height(160.dp)
                                .clip(RoundedCornerShape(8.dp)),
                            contentScale = ContentScale.Fit,
                        )
                        Spacer(Modifier.height(4.dp))
                    }
                }
                if (msg.reasoning.isNotBlank()) {
                    var showReason by remember { mutableStateOf(false) }
                    TextButton(onClick = { showReason = !showReason }) {
                        Text(
                            if (showReason) "💭 收起推理" else "💭 推理过程",
                            fontSize = 12.sp,
                            color = MaterialTheme.colorScheme.outline,
                        )
                    }
                    if (showReason) {
                        Text(
                            msg.reasoning,
                            fontSize = 12.sp,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                            modifier = Modifier
                                .fillMaxWidth()
                                .background(
                                    MaterialTheme.colorScheme.surface,
                                    RoundedCornerShape(6.dp),
                                )
                                .padding(8.dp),
                        )
                    }
                }
                if (msg.text.isNotBlank()) {
                    MarkdownText(msg.text, fontSize = fontSize.sp)
                }
                if (msg.streaming) {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        CircularProgressIndicator(
                            modifier = Modifier.width(14.dp).height(14.dp),
                            strokeWidth = 2.dp,
                        )
                        Spacer(Modifier.width(6.dp))
                        Text("生成中…", fontSize = 12.sp, color = MaterialTheme.colorScheme.outline)
                    }
                }
                if (msg.isError) {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Text("生成失败", fontSize = 12.sp, color = MaterialTheme.colorScheme.error)
                        TextButton(onClick = onRetry) { Text("🔄 重试", fontSize = 12.sp) }
                    }
                } else if (!msg.streaming) {
                    // 时间戳（非流式、非工具消息）
                    Text(
                        formatTime(msg.time),
                        fontSize = 10.sp,
                        color = MaterialTheme.colorScheme.outline,
                        modifier = Modifier.padding(top = 2.dp),
                    )
                }
            }
        }
    }
}

private fun formatTime(ts: Long): String {
    val fmt = java.text.SimpleDateFormat("HH:mm", java.util.Locale.getDefault())
    return fmt.format(java.util.Date(ts))
}

/** 工具调用卡片：⚙️ 名称 + 参数/结果折叠，失败高亮 */
@Composable
private fun ToolCard(tool: ToolMsg, modifier: Modifier = Modifier) {
    val scheme = MaterialTheme.colorScheme
    var showArgs by remember { mutableStateOf(false) }
    var showResult by remember { mutableStateOf(false) }
    Column(
        modifier = modifier
            .padding(vertical = 2.dp)
            .fillMaxWidth()
            .background(
                if (tool.isError) scheme.errorContainer else scheme.surfaceVariant,
                RoundedCornerShape(8.dp),
            )
            .padding(10.dp),
    ) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(
                if (tool.isError) "❌" else "⚙️",
                fontSize = 13.sp,
            )
            Spacer(Modifier.width(6.dp))
            Text(
                tool.name,
                fontSize = 13.sp,
                fontWeight = FontWeight.Bold,
                fontFamily = FontFamily.Monospace,
                color = if (tool.isError) scheme.error else scheme.primary,
                modifier = Modifier.weight(1f),
            )
            if (tool.result != null) {
                Text("✓", fontSize = 13.sp, color = scheme.tertiary)
            }
        }
        Spacer(Modifier.height(4.dp))
        TextButton(
            onClick = { showArgs = !showArgs },
            modifier = Modifier.padding(0.dp),
        ) {
            Text(
                if (showArgs) "▾ 收起参数" else "▸ 参数",
                fontSize = 11.sp,
                color = scheme.onSurfaceVariant,
            )
        }
        if (showArgs) {
            Text(
                tool.args.take(2000),
                fontSize = 11.sp,
                fontFamily = FontFamily.Monospace,
                color = scheme.onSurfaceVariant,
                modifier = Modifier
                    .fillMaxWidth()
                    .background(scheme.surface, RoundedCornerShape(4.dp))
                    .padding(6.dp),
            )
        }
        if (tool.result != null) {
            TextButton(
                onClick = { showResult = !showResult },
                modifier = Modifier.padding(0.dp),
            ) {
                Text(
                    if (showResult) "▾ 收起结果" else "▸ 结果",
                    fontSize = 11.sp,
                    color = scheme.onSurfaceVariant,
                )
            }
            if (showResult) {
                Text(
                    tool.result.take(3000),
                    fontSize = 11.sp,
                    fontFamily = FontFamily.Monospace,
                    color = scheme.onSurfaceVariant,
                    modifier = Modifier
                        .fillMaxWidth()
                        .background(scheme.surface, RoundedCornerShape(4.dp))
                        .padding(6.dp),
                )
            }
        }
    }
}

@Composable
private fun InputBar(vm: ChatViewModel) {
    var input by remember { mutableStateOf("") }
    var pendingImage by remember { mutableStateOf<Uri?>(null) }
    val context = LocalContext.current
    val pickImage = rememberLauncherForActivityResult(
        ActivityResultContracts.GetContent()
    ) { uri ->
        if (uri != null) pendingImage = uri
    }
    Column(modifier = Modifier.fillMaxWidth()) {
        // 附件预览条
        pendingImage?.let { uri ->
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(horizontal = 12.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                val bitmap by produceState<android.graphics.Bitmap?>(
                    initialValue = null, uri,
                ) {
                    value = try {
                        context.contentResolver.openInputStream(uri)?.use {
                            android.graphics.BitmapFactory.decodeStream(it)
                        }
                    } catch (_: Exception) {
                        null
                    }
                }
                bitmap?.let { bmp ->
                    Image(
                        bitmap = bmp.asImageBitmap(),
                        contentDescription = "待发送图片",
                        modifier = Modifier
                            .width(64.dp)
                            .height(64.dp)
                            .clip(RoundedCornerShape(8.dp)),
                        contentScale = ContentScale.Crop,
                    )
                } ?: Text("📷 图片", fontSize = 12.sp)
                Spacer(Modifier.width(8.dp))
                Text(
                    "将识别图片内容并发送",
                    fontSize = 12.sp,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.weight(1f),
                )
                IconButton(onClick = { pendingImage = null }) { Text("✕", fontSize = 12.sp) }
            }
        }
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(8.dp),
            verticalAlignment = Alignment.Bottom,
        ) {
            OutlinedTextField(
                value = input,
                onValueChange = { input = it },
                modifier = Modifier.weight(1f),
                placeholder = { Text("输入消息…") },
                maxLines = 5,
                keyboardOptions = KeyboardOptions(imeAction = ImeAction.Send),
                keyboardActions = KeyboardActions(onSend = {
                    if (input.isNotBlank() || pendingImage != null) {
                        val text = input.trim()
                        val img = pendingImage
                        input = ""
                        pendingImage = null
                        if (img != null) vm.ocrImage(img)
                        if (text.isNotBlank()) vm.send(text)
                    }
                }),
            )
            Spacer(Modifier.width(4.dp))
            if (!vm.busy) {
                TextButton(onClick = {
                    pickImage.launch("image/*")
                }) { Text("📷") }
            }
            Spacer(Modifier.width(4.dp))
            if (vm.busy) {
                IconButton(onClick = { vm.cancel() }) {
                    Box(contentAlignment = Alignment.Center) {
                        CircularProgressIndicator(modifier = Modifier.width(24.dp).height(24.dp), strokeWidth = 2.dp)
                    }
                }
            } else {
                Button(
                    onClick = {
                        if (input.isNotBlank() || pendingImage != null) {
                            val text = input.trim()
                            val img = pendingImage
                            input = ""
                            pendingImage = null
                            if (img != null) vm.ocrImage(img)
                            if (text.isNotBlank()) vm.send(text)
                        }
                    },
                    enabled = input.isNotBlank() || pendingImage != null,
                ) { Text("发送") }
            }
        }
    }
}

@Composable
private fun SettingsDialog(vm: ChatViewModel, onDismiss: () -> Unit) {
    var baseUrl by remember { mutableStateOf(vm.providerBaseUrl) }
    var apiKey by remember { mutableStateOf(vm.providerApiKey) }
    var model by remember { mutableStateOf(vm.providerModel) }
    var confirmClear by remember { mutableStateOf(false) }
    val context = LocalContext.current
    Dialog(
        onDismissRequest = onDismiss,
        properties = DialogProperties(usePlatformDefaultWidth = false),
    ) {
        Surface(
            modifier = Modifier.fillMaxSize(),
            color = MaterialTheme.colorScheme.background,
        ) {
            Column(modifier = Modifier.fillMaxSize()) {
                // 顶栏
                Row(
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(horizontal = 8.dp, vertical = 4.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    IconButton(onClick = onDismiss) { Text("←", fontSize = 18.sp) }
                    Text("设置", style = MaterialTheme.typography.titleLarge)
                    Spacer(Modifier.weight(1f))
                    TextButton(onClick = {
                        vm.providerBaseUrl = baseUrl.trim()
                        vm.providerApiKey = apiKey.trim()
                        vm.providerModel = model.trim()
                        vm.saveProviderSettings()
                        onDismiss()
                    }) { Text("保存") }
                }
                HorizontalDivider()
                Column(
                    modifier = Modifier
                        .fillMaxSize()
                        .verticalScroll(rememberScrollState())
                        .padding(horizontal = 16.dp),
                ) {
                    // ---- Provider ----
                    SectionTitle("Provider")
                    OutlinedTextField(
                        value = baseUrl,
                        onValueChange = { baseUrl = it },
                        label = { Text("Base URL") },
                        singleLine = true,
                        modifier = Modifier.fillMaxWidth(),
                    )
                    Spacer(Modifier.height(8.dp))
                    OutlinedTextField(
                        value = apiKey,
                        onValueChange = { apiKey = it },
                        label = { Text("API Key") },
                        singleLine = true,
                        modifier = Modifier.fillMaxWidth(),
                    )
                    Spacer(Modifier.height(8.dp))
                    OutlinedTextField(
                        value = model,
                        onValueChange = { model = it },
                        label = { Text("模型") },
                        singleLine = true,
                        modifier = Modifier.fillMaxWidth(),
                    )
                    // ---- 通用 ----
                    SectionTitle("通用")
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Text("朗读回复", modifier = Modifier.weight(1f))
                        Switch(
                            checked = vm.autoTts,
                            onCheckedChange = { vm.updateAutoTts(it) },
                        )
                    }
                    Spacer(Modifier.height(8.dp))
                    Text("主题", style = MaterialTheme.typography.bodyMedium)
                    Row {
                        listOf(0 to "跟随系统", 1 to "浅色", 2 to "深色").forEach { (mode, label) ->
                            FilterChip(
                                selected = vm.themeMode == mode,
                                onClick = { vm.updateThemeMode(mode) },
                                label = { Text(label) },
                                modifier = Modifier.padding(end = 8.dp),
                            )
                        }
                    }
                    Spacer(Modifier.height(8.dp))
                    Text("字体大小", style = MaterialTheme.typography.bodyMedium)
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Text("小", fontSize = 11.sp, color = MaterialTheme.colorScheme.outline)
                        Slider(
                            value = vm.fontSize.toFloat(),
                            onValueChange = { vm.updateFontSize(it.toInt()) },
                            valueRange = 12f..20f,
                            modifier = Modifier.weight(1f).padding(horizontal = 8.dp),
                        )
                        Text("大", fontSize = 16.sp, color = MaterialTheme.colorScheme.outline)
                    }
                    // ---- 权限 ----
                    SectionTitle("权限")
                    PermissionRow(
                        label = "日历读取",
                        granted = hasCalendarPermission(context),
                        onRequest = { openAppSettings(context) },
                    )
                    PermissionRow(
                        label = "屏幕时间",
                        granted = hasUsageAccess(context),
                        onRequest = {
                            context.startActivity(
                                android.content.Intent(
                                    android.provider.Settings.ACTION_USAGE_ACCESS_SETTINGS,
                                ),
                            )
                        },
                    )
                    // ---- 关于 ----
                    SectionTitle("关于")
                    Text(
                        "RikkaHub CE v${BuildConfig.VERSION_NAME}",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.outline,
                    )
                    Spacer(Modifier.height(8.dp))
                    OutlinedButton(
                        onClick = { confirmClear = true },
                        modifier = Modifier.fillMaxWidth(),
                    ) {
                        Text("清空所有数据", color = MaterialTheme.colorScheme.error)
                    }
                    Spacer(Modifier.height(24.dp))
                }
            }
        }
    }
    if (confirmClear) {
        AlertDialog(
            onDismissRequest = { confirmClear = false },
            title = { Text("清空所有数据？") },
            text = { Text("将删除全部会话与设置，此操作不可恢复。") },
            confirmButton = {
                TextButton(onClick = {
                    vm.clearAllData()
                    confirmClear = false
                    onDismiss()
                }) { Text("清空", color = MaterialTheme.colorScheme.error) }
            },
            dismissButton = {
                TextButton(onClick = { confirmClear = false }) { Text("取消") }
            },
        )
    }
}

@Composable
private fun SectionTitle(title: String) {
    Text(
        title,
        style = MaterialTheme.typography.titleSmall,
        color = MaterialTheme.colorScheme.primary,
        modifier = Modifier.padding(top = 16.dp, bottom = 8.dp),
    )
}

@Composable
private fun PermissionRow(label: String, granted: Boolean, onRequest: () -> Unit) {
    Row(
        modifier = Modifier.fillMaxWidth(),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(label, modifier = Modifier.weight(1f))
        Text(
            if (granted) "已授予" else "未授予",
            style = MaterialTheme.typography.bodySmall,
            color = if (granted) MaterialTheme.colorScheme.primary
            else MaterialTheme.colorScheme.error,
        )
        if (!granted) {
            TextButton(onClick = onRequest) { Text("去授权") }
        }
    }
}

private fun hasCalendarPermission(context: Context): Boolean {
    return androidx.core.content.ContextCompat.checkSelfPermission(
        context,
        android.Manifest.permission.READ_CALENDAR,
    ) == android.content.pm.PackageManager.PERMISSION_GRANTED
}

private fun hasUsageAccess(context: Context): Boolean {
    return try {
        val um = context.getSystemService(Context.USAGE_STATS_SERVICE)
                as android.app.usage.UsageStatsManager
        val now = System.currentTimeMillis()
        val stats = um.queryUsageStats(
            android.app.usage.UsageStatsManager.INTERVAL_DAILY,
            now - 3600_000L, now,
        )
        !stats.isNullOrEmpty()
    } catch (_: Exception) {
        false
    }
}

private fun openAppSettings(context: Context) {
    context.startActivity(
        android.content.Intent(
            android.provider.Settings.ACTION_APPLICATION_DETAILS_SETTINGS,
            android.net.Uri.fromParts("package", context.packageName, null),
        ),
    )
}