package dev.rikkahub.ce

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
import dev.rikkahub.ce.ui.ChatScreen
import dev.rikkahub.ce.ui.ChatViewModel

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        DeviceTools.init(applicationContext)
        ChatStore.init(applicationContext)
        setContent {
            val vm: ChatViewModel = viewModel {
                ChatViewModel(applicationContext)
            }
            RikkaTheme(
                darkTheme = when (vm.themeMode) {
                    1 -> false
                    2 -> true
                    else -> isSystemInDarkTheme()
                },
            ) {
                Surface(modifier = Modifier.fillMaxSize()) {
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
        onPrimary = Color(0xFF0A1A2F),
        primaryContainer = Color(0xFF1E3A5F),
        onPrimaryContainer = Color(0xFFD3E3FF),
        secondary = Color(0xFFB39DFF),
        onSecondary = Color(0xFF221447),
        secondaryContainer = Color(0xFF3A2E6B),
        onSecondaryContainer = Color(0xFFE5DEFF),
        tertiary = Color(0xFF5EE0B6),
        onTertiary = Color(0xFF0B2E22),
        tertiaryContainer = Color(0xFF1B4A3A),
        onTertiaryContainer = Color(0xFFC9F5E5),
        error = Color(0xFFF28B82),
        onError = Color(0xFF3B0A07),
        errorContainer = Color(0xFF5C1F1B),
        onErrorContainer = Color(0xFFFFDAD6),
        background = Color(0xFF101418),
        onBackground = Color(0xFFE3E6EA),
        surface = Color(0xFF161B20),
        onSurface = Color(0xFFE3E6EA),
        surfaceVariant = Color(0xFF232A31),
        onSurfaceVariant = Color(0xFFB8C1CA),
        outline = Color(0xFF5A6470),
        outlineVariant = Color(0xFF3A434C),
    ) else lightColorScheme(
        primary = Color(0xFF1A73E8),
        onPrimary = Color(0xFFFFFFFF),
        primaryContainer = Color(0xFFD8E7FF),
        onPrimaryContainer = Color(0xFF0A3D91),
        secondary = Color(0xFF5B4BB5),
        onSecondary = Color(0xFFFFFFFF),
        secondaryContainer = Color(0xFFE8E2FF),
        onSecondaryContainer = Color(0xFF32207E),
        tertiary = Color(0xFF00875E),
        onTertiary = Color(0xFFFFFFFF),
        tertiaryContainer = Color(0xFFC0F2E0),
        onTertiaryContainer = Color(0xFF005238),
        error = Color(0xFFD93025),
        onError = Color(0xFFFFFFFF),
        errorContainer = Color(0xFFFCE8E6),
        onErrorContainer = Color(0xFF8F1D12),
        background = Color(0xFFF7F9FB),
        onBackground = Color(0xFF1B1F24),
        surface = Color(0xFFFFFFFF),
        onSurface = Color(0xFF1B1F24),
        surfaceVariant = Color(0xFFEFF2F5),
        onSurfaceVariant = Color(0xFF5A6470),
        outline = Color(0xFF8A949E),
        outlineVariant = Color(0xFFD9DEE3),
    )
    MaterialTheme(colorScheme = scheme) {
        content()
    }
}
