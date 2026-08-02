package dev.rikkahub.ce.ui

import dev.rikkahub.ce.BuildConfig

import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
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
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.FilterChip
import androidx.compose.material3.HorizontalDivider
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.produceState
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import android.content.Context
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

@Composable
fun ChatScreen(vm: ChatViewModel) {
    var showSettings by remember { mutableStateOf(false) }
    var showSessions by remember { mutableStateOf(false) }
    Column(modifier = Modifier.fillMaxSize()) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 12.dp, vertical = 4.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            val cur = vm.sessions.firstOrNull { it.id == vm.currentSessionId }
            TextButton(onClick = { showSessions = true }) {
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
    if (showSettings) {
        SettingsDialog(vm, onDismiss = { showSettings = false })
    }
    if (showSessions) {
        SessionsDialog(vm, onDismiss = { showSessions = false })
    }
}

@Composable
private fun SessionsDialog(vm: ChatViewModel, onDismiss: () -> Unit) {
    var editing by remember { mutableStateOf<ChatSession?>(null) }
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("会话") },
        text = {
            Column {
                TextButton(onClick = {
                    vm.newSession()
                    onDismiss()
                }) { Text("＋ 新建会话") }
                for (s in vm.sessions) {
                    Row(
                        modifier = Modifier
                            .fillMaxWidth()
                            .padding(vertical = 2.dp),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        TextButton(
                            onClick = {
                                vm.switchSession(s.id)
                                onDismiss()
                            },
                            modifier = Modifier.weight(1f),
                        ) {
                            Text(
                                s.title,
                                maxLines = 1,
                                fontWeight = if (s.id == vm.currentSessionId)
                                    FontWeight.Bold else FontWeight.Normal,
                            )
                        }
                        if (vm.sessions.size > 1) {
                            TextButton(onClick = { editing = s }) { Text("改名") }
                            TextButton(onClick = { vm.deleteSession(s.id) }) { Text("删除") }
                        }
                    }
                }
            }
        },
        confirmButton = {
            TextButton(onClick = onDismiss) { Text("关闭") }
        },
    )
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
            )
        }
    }
}

@OptIn(ExperimentalFoundationApi::class)
@Composable
private fun MessageBubble(msg: ChatMsg, onRetry: () -> Unit) {
    val isUser = msg.role == "user"
    val ctx = LocalContext.current
    val onCopy = {
        val cm = ctx.getSystemService(android.content.Context.CLIPBOARD_SERVICE)
                as android.content.ClipboardManager
        cm.setPrimaryClip(android.content.ClipData.newPlainText("rikka", msg.text))
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
                if (msg.text.startsWith("⚙️")) {
                    Text(
                        msg.text,
                        fontFamily = FontFamily.Monospace,
                        fontSize = 13.sp,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                } else {
                    MarkdownText(msg.text)
                }
                if (msg.streaming) {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        CircularProgressIndicator(modifier = Modifier.width(14.dp).height(14.dp), strokeWidth = 2.dp)
                        Spacer(Modifier.width(6.dp))
                        Text("生成中…", fontSize = 12.sp, color = MaterialTheme.colorScheme.outline)
                    }
                }
                if (msg.isError) {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Text("重试", fontSize = 12.sp, color = MaterialTheme.colorScheme.error)
                        TextButton(onClick = onRetry) { Text("🔄") }
                    }
                }
            }
        }
    }
}

@Composable
private fun InputBar(vm: ChatViewModel) {
    var input by remember { mutableStateOf("") }
    val context = LocalContext.current
    val pickImage = rememberLauncherForActivityResult(
        ActivityResultContracts.GetContent()
    ) { uri ->
        if (uri != null) vm.ocrImage(uri)
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
                    if (input.isNotBlank()) {
                        vm.send(input.trim())
                        input = ""
                    }
                },
                enabled = input.isNotBlank(),
            ) { Text("发送") }
        }
    }
}

@Composable
private fun SettingsDialog(vm: ChatViewModel, onDismiss: () -> Unit) {
    var baseUrl by remember { mutableStateOf(vm.providerBaseUrl) }
    var apiKey by remember { mutableStateOf(vm.providerApiKey) }
    var model by remember { mutableStateOf(vm.providerModel) }
    val context = LocalContext.current
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("设置") },
        text = {
            Column(modifier = Modifier.verticalScroll(rememberScrollState())) {
                OutlinedTextField(
                    value = baseUrl,
                    onValueChange = { baseUrl = it },
                    label = { Text("Base URL") },
                    singleLine = true,
                )
                Spacer(Modifier.height(8.dp))
                OutlinedTextField(
                    value = apiKey,
                    onValueChange = { apiKey = it },
                    label = { Text("API Key") },
                    singleLine = true,
                )
                Spacer(Modifier.height(8.dp))
                OutlinedTextField(
                    value = model,
                    onValueChange = { model = it },
                    label = { Text("模型") },
                    singleLine = true,
                )
                Spacer(Modifier.height(12.dp))
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text("朗读回复", modifier = Modifier.weight(1f))
                    Switch(
                        checked = vm.autoTts,
                        onCheckedChange = { vm.updateAutoTts(it) },
                    )
                }
                Spacer(Modifier.height(12.dp))
                Text("主题", style = MaterialTheme.typography.titleSmall)
                Spacer(Modifier.height(4.dp))
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
                HorizontalDivider()
                Spacer(Modifier.height(8.dp))
                Text("权限", style = MaterialTheme.typography.titleSmall)
                Spacer(Modifier.height(4.dp))
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
                Spacer(Modifier.height(8.dp))
                Text(
                    "RikkaHub CE v${BuildConfig.VERSION_NAME}",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.outline,
                )
            }
        },
        confirmButton = {
            TextButton(onClick = {
                vm.providerBaseUrl = baseUrl.trim()
                vm.providerApiKey = apiKey.trim()
                vm.providerModel = model.trim()
                vm.saveProviderSettings()
                onDismiss()
            }) { Text("保存") }
        },
        dismissButton = {
            TextButton(onClick = onDismiss) { Text("取消") }
        },
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
