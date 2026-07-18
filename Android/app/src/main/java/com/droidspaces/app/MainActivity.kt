package com.droidspaces.app

import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.provider.Settings
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.material3.*
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.core.content.ContextCompat
import androidx.core.splashscreen.SplashScreen.Companion.installSplashScreen
import androidx.core.view.WindowCompat
import com.droidspaces.app.ui.navigation.DroidspacesNavigation
import com.droidspaces.app.ui.theme.DroidspacesTheme
import com.droidspaces.app.ui.theme.rememberThemeState

class MainActivity : AppCompatActivity() {

    companion object {
        const val ACTION_SHORTCUT_CONTAINERS = "com.droidspaces.app.action.SHORTCUT_CONTAINERS"
        const val ACTION_SHORTCUT_PANEL = "com.droidspaces.app.action.SHORTCUT_PANEL"
        const val ACTION_SHORTCUT_SETTINGS = "com.droidspaces.app.action.SHORTCUT_SETTINGS"
    }

    private var isLoading by mutableStateOf(false)

    // Deep-link target from a launcher long-press shortcut ("containers" / "panel"
    // / "settings"), consumed once by DroidspacesNavigation then reset to null.
    private var pendingShortcut by mutableStateOf<String?>(null)

    // ── POST_NOTIFICATIONS permission (Android 13+ / API 33+) ─────────────────
    // Samsung's SecFgsManagerController suppresses any FGS notification from apps
    // that don't hold this permission, which immediately strips the ONGOING flag
    // and causes TerminalSessionService to be treated as non-foreground.
    // The service then gets killed and the binder cycles null → non-null, causing
    // the crash loop.  Requesting this up-front breaks that cycle.
    // Approach ported from ReTerminal's MainActivity (retries up to 3 times).

    private var showNotificationRationale by mutableStateOf(false)

    private val requestNotificationPermission =
        registerForActivityResult(ActivityResultContracts.RequestPermission()) { isGranted ->
            if (!isGranted) {
                showNotificationRationale = true
            }
        }

    fun requestNotificationPermission() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            when {
                ContextCompat.checkSelfPermission(
                    this,
                    android.Manifest.permission.POST_NOTIFICATIONS
                ) == PackageManager.PERMISSION_GRANTED -> {
                    // Already granted
                }
                shouldShowRequestPermissionRationale(android.Manifest.permission.POST_NOTIFICATIONS) -> {
                    showNotificationRationale = true
                }
                else -> {
                    requestNotificationPermission.launch(android.Manifest.permission.POST_NOTIFICATIONS)
                }
            }
        }
    }

    @Composable
    private fun NotificationRationaleDialog() {
        if (showNotificationRationale) {
            androidx.compose.material3.AlertDialog(
                onDismissRequest = { showNotificationRationale = false },
                title = { Text(getString(R.string.notification_permission_title)) },
                text = { Text(getString(R.string.notification_permission_rationale)) },
                confirmButton = {
                    TextButton(onClick = {
                        showNotificationRationale = false
                        if (shouldShowRequestPermissionRationale(android.Manifest.permission.POST_NOTIFICATIONS)) {
                            requestNotificationPermission.launch(android.Manifest.permission.POST_NOTIFICATIONS)
                        } else {
                            val intent = Intent(Settings.ACTION_APP_NOTIFICATION_SETTINGS).apply {
                                putExtra(Settings.EXTRA_APP_PACKAGE, packageName)
                            }
                            startActivity(intent)
                        }
                    }) {
                        Text(getString(R.string.grant_permission))
                    }
                },
                dismissButton = {
                    TextButton(onClick = { showNotificationRationale = false }) {
                        Text(getString(R.string.i_understand))
                    }
                }
            )
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        // Install splash screen before super.onCreate for faster display
        val splashScreen = installSplashScreen()
        WindowCompat.setDecorFitsSystemWindows(window, false)
        super.onCreate(savedInstanceState)

        // Set condition immediately - UI will hide splash when ready
        // Start with false to show UI immediately (content is ready)
        splashScreen.setKeepOnScreenCondition { isLoading }

        // Request POST_NOTIFICATIONS early so TerminalSessionService is never
        // suppressed by Samsung's notification manager before it starts.
        requestNotificationPermission()

        // Skip shortcut handling when the activity is relaunched from Recents,
        // otherwise the stale shortcut intent would re-fire the deep link.
        if ((intent.flags and Intent.FLAG_ACTIVITY_LAUNCHED_FROM_HISTORY) == 0) {
            handleIntent(intent)
        }

        // Render UI immediately - no blocking operations
        setContent {
            ThemeWrapper {
                NotificationRationaleDialog()
                Surface(
                    modifier = Modifier.fillMaxSize(),
                    color = MaterialTheme.colorScheme.background
                ) {
                    DroidspacesNavigation(
                        onContentReady = { isLoading = false },
                        pendingShortcut = pendingShortcut,
                        onShortcutHandled = { pendingShortcut = null }
                    )
                }
            }
        }
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        handleIntent(intent)
    }

    private fun handleIntent(intent: Intent) {
        pendingShortcut = when (intent.action) {
            ACTION_SHORTCUT_CONTAINERS -> "containers"
            ACTION_SHORTCUT_PANEL -> "panel"
            ACTION_SHORTCUT_SETTINGS -> "settings"
            else -> pendingShortcut
        }
    }
}

@Composable
private fun ThemeWrapper(content: @Composable () -> Unit) {
    // Use reactive theme state that updates instantly when preferences change
    val themeState = rememberThemeState()

    DroidspacesTheme(
        darkTheme = themeState.darkTheme,
        dynamicColor = themeState.useDynamicColor,
        amoledMode = themeState.amoledMode,
        themePalette = themeState.themePalette
    ) {
        content()
    }
}
