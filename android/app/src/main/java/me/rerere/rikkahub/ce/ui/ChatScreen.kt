package me.rerere.rikkahub.ce.ui

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
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

@Composable
fun ChatScreen(vm: ChatViewModel) {
    var showSettings by remember { mutableStateOf(false) }
    Column(modifier = Modifier.fillMaxSize()) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 12.dp, vertical = 4.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text("RikkaHub CE", style = MaterialTheme.typography.titleMedium)
            Spacer(Modifier.weight(1f))
            TextButton(onClick = { showSettings = true }) { Text("设置") }
        }
        MessageList(vm, Modifier.weight(1f))
        InputBar(vm)
    }
    if (showSettings) {
        SettingsDialog(vm, onDismiss = { showSettings = false })
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
        modifier = modifier
            .fillMaxWidth()
            .weight(1f),
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
                Text(
                    msg.text,
                    fontFamily = if (msg.text.startsWith("⚙️")) FontFamily.Monospace else FontFamily.Default,
                    fontSize = 15.sp,
                )
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
        Spacer(Modifier.width(8.dp))
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
            }
        },
        confirmButton = {
            TextButton(onClick = {
                vm.providerBaseUrl = baseUrl.trim()
                vm.providerApiKey = apiKey.trim()
                vm.providerModel = model.trim()
                onDismiss()
            }) { Text("保存") }
        },
        dismissButton = {
            TextButton(onClick = onDismiss) { Text("取消") }
        },
    )
}
