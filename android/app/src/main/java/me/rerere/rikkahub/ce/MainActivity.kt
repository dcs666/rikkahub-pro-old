package me.rerere.rikkahub.ce

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.lifecycle.viewmodel.compose.viewModel
import me.rerere.rikkahub.ce.ui.ChatScreen
import me.rerere.rikkahub.ce.ui.ChatViewModel

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        DeviceTools.init(applicationContext)
        ChatStore.init(applicationContext)
        setContent {
            RikkaTheme {
                Surface(modifier = Modifier.fillMaxSize()) {
                    val vm: ChatViewModel = viewModel {
                        ChatViewModel(applicationContext)
                    }
                    ChatScreen(vm)
                }
            }
        }
    }
}

@Composable
fun RikkaTheme(
    darkTheme: Boolean = isSystemInDarkTheme(),
    content: @Composable () -> Unit,
) {
    val scheme = if (darkTheme) darkColorScheme(
        primary = Color(0xFF8AB4F8),
        background = Color(0xFF101418),
        surface = Color(0xFF161B20),
        surfaceVariant = Color(0xFF232A31),
        onSurface = Color(0xFFE3E6EA),
        onSurfaceVariant = Color(0xFFB8C1CA),
    ) else lightColorScheme(
        primary = Color(0xFF1A73E8),
        background = Color(0xFFF7F9FB),
        surface = Color(0xFFFFFFFF),
        surfaceVariant = Color(0xFFEFF2F5),
        onSurface = Color(0xFF1B1F24),
        onSurfaceVariant = Color(0xFF5A6470),
    )
    MaterialTheme(colorScheme = scheme) {
        content()
    }
}
