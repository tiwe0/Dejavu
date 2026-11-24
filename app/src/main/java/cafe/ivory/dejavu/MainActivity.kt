package cafe.ivory.dejavu

import android.content.Context
import android.graphics.Color as AndroidColor
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.SystemBarStyle
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.graphics.toArgb
import cafe.ivory.dejavu.ui.ManagerApp
import cafe.ivory.dejavu.ui.theme.DejavuTheme
import cafe.ivory.dejavu.ui.theme.isDark
import cafe.ivory.dejavu.ui.theme.managerDisplayColorScheme

class MainActivity : ComponentActivity() {
    override fun attachBaseContext(newBase: Context) {
        super.attachBaseContext(ManagerLanguage.localizedContext(newBase))
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        setContent {
            var appearance by remember { mutableStateOf(ManagerAppearanceStore.load(this)) }
            val dark = appearance.mode.isDark()
            // Resolve the target navigation scrim once per theme change instead of every animation frame.
            val targetSurface = managerDisplayColorScheme(appearance.palette, dark).surface.toArgb()
            LaunchedEffect(dark, targetSurface) {
                enableEdgeToEdge(
                    statusBarStyle = SystemBarStyle.auto(AndroidColor.TRANSPARENT, AndroidColor.TRANSPARENT) { dark },
                    navigationBarStyle = SystemBarStyle.auto(targetSurface, targetSurface) { dark },
                )
            }
            DejavuTheme(appearance) {
                ManagerApp(
                    appearance = appearance,
                    onAppearanceChange = {
                        ManagerAppearanceStore.save(this, it)
                        appearance = it
                    },
                    onLanguageChange = { tag ->
                        if (ManagerLanguage.set(this, tag)) recreate()
                    },
                )
            }
        }
    }
}
