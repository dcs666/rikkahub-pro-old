package me.rerere.rikkahub.ce.ui

import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
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
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
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
            MessageBubble(msg)
        }
    }
}

@Composable
private fun MessageBubble(msg: ChatMsg) {
    val isUser = msg.role == "user"
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
        ) {
            Column(modifier = Modifier.padding(horizontal = 12.dp, vertical = 8.dp)) {
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
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("Provider 设置") },
        text = {
            Column {
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
                        onCheckedChange = { vm.setAutoTts(it) },
                    )
                }
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
