package io.github.dmikushin.uti120

import android.content.ActivityNotFoundException
import android.content.Intent
import android.provider.MediaStore
import android.os.SystemClock
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.ui.hapticfeedback.HapticFeedbackType
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalHapticFeedback
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import kotlinx.coroutines.delay
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.systemBarsPadding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.FiberManualRecord
import androidx.compose.material.icons.filled.Palette
import androidx.compose.material.icons.filled.PhotoCamera
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material.icons.filled.Stop
import androidx.compose.material.icons.outlined.PhotoLibrary
import androidx.compose.material.icons.outlined.Usb
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.FilterChip
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.FilterQuality
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

// Colours of the vendor app (res/values/colors.xml and TouchBoardPanel).
private val BarColor = Color(0xFF0E0E0E)      // gray_deep: header and bottom bar
private val ContentColor = Color(0xFF050316)  // black: behind the image
private val PanelColor = Color(0xFF202020)    // panel under the image
private val Muted = Color(0xFF8E898C)         // gray
private val RecColor = Color(0xFFFF3B30)

private enum class Tab { Palettes, Settings }

@Composable
fun CameraScreen(c: CameraController) {
    MaterialTheme(colorScheme = darkColorScheme()) {
        var tab by rememberSaveable { mutableStateOf<Tab?>(null) }
        var diagnostics by rememberSaveable { mutableStateOf(false) }
        Column(Modifier.fillMaxSize().background(BarColor).systemBarsPadding()) {
            Header(c) { diagnostics = !diagnostics }
            BoxWithConstraints(Modifier.weight(1f).fillMaxWidth().background(ContentColor)) {
                // Image area as in the vendor's MainActivity: full width at 3:4,
                // or the height left after 190 dp for the panel.
                var w = maxWidth
                var h = w / 0.75f
                if (h > maxHeight - 190.dp) {
                    h = maxHeight - 190.dp
                    w = h * 0.75f
                }
                Column(Modifier.fillMaxSize(), horizontalAlignment = Alignment.CenterHorizontally) {
                    Box(Modifier.width(w).height(h)) {
                        ImageArea(c, diagnostics)
                        when (tab) {
                            Tab.Palettes -> PaletteStrip(c, Modifier.align(Alignment.BottomCenter))
                            else -> {}
                        }
                    }
                    CapturePanel(c, Modifier.weight(1f).fillMaxWidth())
                }
            }
            BottomBar(tab) { tab = if (tab == it) null else it }
        }
        if (tab == Tab.Settings) SettingsDialog(c) { tab = null }
    }
}

@Composable
private fun Header(c: CameraController, toggleDiagnostics: () -> Unit) {
    Row(
        Modifier.fillMaxWidth().height(45.dp).padding(horizontal = 16.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text("UTi120Mobile", color = Color.White, fontWeight = FontWeight.Medium, fontSize = 17.sp)
        Spacer(Modifier.weight(1f))
        val status = when {
            c.calibrating -> "Calibrating…"
            c.phase == Phase.Live -> "%.0f fps".format(c.fps)
            c.phase == Phase.Starting -> "Calibrating…"
            else -> ""
        }
        // Tapping the frame rate shows where the time of each camera frame goes.
        Text(
            status, color = Muted, fontSize = 14.sp,
            modifier = Modifier.clickable(enabled = c.phase == Phase.Live, onClick = toggleDiagnostics).padding(8.dp),
        )
    }
}

@Composable
private fun ImageArea(c: CameraController, diagnostics: Boolean) {
    Box(
        Modifier.fillMaxSize().clickable(enabled = c.phase == Phase.Live) { c.recalibrate() },
        contentAlignment = Alignment.Center,
    ) {
        val image = c.image
        if (image != null && c.phase == Phase.Live) {
            Image(
                image, contentDescription = "Thermal image (tap to recalibrate)",
                modifier = Modifier.fillMaxSize(), contentScale = ContentScale.Fit,
                filterQuality = FilterQuality.Medium,
            )
        } else {
            Placeholder(c)
        }
        if (diagnostics && c.phase == Phase.Live && c.diagnostics.isNotEmpty()) {
            Text(
                c.diagnostics, color = Color.White, fontSize = 12.sp, lineHeight = 15.sp,
                modifier = Modifier.align(Alignment.BottomStart).fillMaxWidth()
                    .background(Color(0xB0000000)).padding(8.dp),
            )
        }
        c.notice?.let {
            Text(
                it, color = Color.White, fontSize = 14.sp,
                modifier = Modifier.align(Alignment.TopCenter).padding(12.dp)
                    .background(Color(0xB0000000), RoundedCornerShape(6.dp)).padding(8.dp, 4.dp),
            )
        }
    }
}

@Composable
private fun Placeholder(c: CameraController) {
    Column(horizontalAlignment = Alignment.CenterHorizontally, modifier = Modifier.padding(24.dp)) {
        when (c.phase) {
            Phase.Starting, Phase.Live -> CircularProgressIndicator(color = Color.White)
            else -> Icon(Icons.Outlined.Usb, null, tint = Muted, modifier = Modifier.size(48.dp))
        }
        Spacer(Modifier.height(16.dp))
        val text = when (c.phase) {
            Phase.NoCamera -> "Connect the UNI-T UTi120Mobile camera"
            Phase.AwaitingPermission -> "Allow access to the camera"
            Phase.Starting, Phase.Live -> "Calibrating the camera…"
            Phase.Failed -> c.message
        }
        Text(text, color = Color.White, textAlign = TextAlign.Center)
        if (c.phase == Phase.Failed || c.phase == Phase.NoCamera) {
            TextButton(onClick = { c.connect() }) { Text("Retry") }
        }
    }
}

/** Horizontal strip of palette swatches (vendor: ColorStylePanel, 150 dp, 120 dp items). */
@Composable
private fun PaletteStrip(c: CameraController, modifier: Modifier) {
    val swatches = remember {
        NativeCamera.paletteNames.associateWith { name ->
            NativeCamera.paletteColors(name).let { argb -> (0 until 256 step 8).map { Color(argb[it]) } + Color(argb[255]) }
        }
    }
    LazyRow(
        modifier.fillMaxWidth().height(150.dp).background(Color(0xE00E0E0E)),
        contentPadding = PaddingValues(12.dp), horizontalArrangement = Arrangement.spacedBy(12.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        items(NativeCamera.paletteNames) { name ->
            val selected = c.view.palette == name
            Column(
                Modifier.width(120.dp).clickable { c.updateView(c.view.copy(palette = name)) },
                horizontalAlignment = Alignment.CenterHorizontally,
            ) {
                Box(
                    Modifier.fillMaxWidth().height(72.dp).clip(RoundedCornerShape(6.dp))
                        .background(Brush.verticalGradient(swatches.getValue(name).reversed()))
                        .border(if (selected) 3.dp else 1.dp, if (selected) Color.White else Muted, RoundedCornerShape(6.dp)),
                )
                Spacer(Modifier.height(6.dp))
                Text(name.replaceFirstChar { it.uppercase() }, color = if (selected) Color.White else Muted, fontSize = 13.sp)
            }
        }
    }
}

/** Panel under the image (vendor: TouchBoardPanel): gallery and the capture button. */
@Composable
private fun CapturePanel(c: CameraController, modifier: Modifier) {
    val context = LocalContext.current
    Box(modifier.background(PanelColor)) {
        Box(
            Modifier.align(Alignment.CenterStart).padding(start = 15.dp).size(60.dp).clip(CircleShape)
                .clickable {
                    val intent = c.lastCapture?.let { uri ->
                        Intent(Intent.ACTION_VIEW, uri).addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
                    } ?: Intent(Intent.ACTION_VIEW, MediaStore.Images.Media.EXTERNAL_CONTENT_URI)
                    try {
                        context.startActivity(intent)
                    } catch (_: ActivityNotFoundException) {
                    }
                },
            contentAlignment = Alignment.Center,
        ) {
            Icon(Icons.Outlined.PhotoLibrary, "Gallery", tint = Color.White, modifier = Modifier.size(30.dp))
        }
        CaptureButton(c, Modifier.align(Alignment.Center))
    }
}

/**
 * The capture button: a tap takes a photo; pressing and holding records video
 * until the finger is lifted (the ring turns red and a timer runs above it).
 */
@Composable
private fun CaptureButton(c: CameraController, modifier: Modifier) {
    val live = c.phase == Phase.Live
    val haptics = LocalHapticFeedback.current
    val active = c.recording || c.videoBusy
    val ring = if (active) RecColor else Color.White
    Column(modifier, horizontalAlignment = Alignment.CenterHorizontally) {
        Box(Modifier.height(28.dp)) { if (c.recording) RecordingTimer(c.recordingSince) }
        Spacer(Modifier.height(6.dp))
        Box(
            Modifier.size(84.dp).clip(CircleShape).border(5.dp, if (live) ring else Color(0xFF808080), CircleShape)
                .semantics { contentDescription = "Capture: tap for a photo, press and hold for video" }
                .pointerInput(live) {
                    if (!live) return@pointerInput
                    var holding = false
                    detectTapGestures(
                        onTap = { c.takePhoto() },
                        onLongPress = {
                            holding = true
                            haptics.performHapticFeedback(HapticFeedbackType.LongPress)
                            c.startRecording()
                        },
                        onPress = {
                            tryAwaitRelease()  // released or cancelled
                            if (holding) {
                                holding = false
                                c.stopRecording()
                            }
                        },
                    )
                }
                .padding(9.dp).clip(CircleShape)
                .background(if (!live) Color(0xFF808080) else if (active) RecColor else Color.White),
            contentAlignment = Alignment.Center,
        ) {
            when {
                c.photoBusy -> CircularProgressIndicator(Modifier.size(30.dp), color = Color.Black, strokeWidth = 3.dp)
                active -> Box(Modifier.size(22.dp).clip(RoundedCornerShape(5.dp)).background(Color.White))
            }
        }
        Text(
            if (active) "Release to stop" else "Tap: photo · Hold: video",
            color = Muted, fontSize = 12.sp, modifier = Modifier.padding(top = 8.dp),
        )
    }
}

@Composable
private fun RecordingTimer(since: Long) {
    var now by remember { mutableLongStateOf(SystemClock.elapsedRealtime()) }
    LaunchedEffect(since) {
        while (true) {
            now = SystemClock.elapsedRealtime()
            delay(250)
        }
    }
    val seconds = ((now - since) / 1000).coerceAtLeast(0)
    Row(
        Modifier.background(Color(0xCC000000), RoundedCornerShape(12.dp)).padding(horizontal = 10.dp, vertical = 3.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Box(Modifier.size(8.dp).clip(CircleShape).background(RecColor))
        Spacer(Modifier.width(6.dp))
        Text("%02d:%02d".format(seconds / 60, seconds % 60), color = Color.White, fontSize = 13.sp, fontWeight = FontWeight.Medium)
    }
}

/** 45 dp tab bar (vendor: BottomBar_home), equal-width tabs. */
@Composable
private fun BottomBar(selected: Tab?, onSelect: (Tab) -> Unit) {
    Row(Modifier.fillMaxWidth().height(45.dp).background(BarColor), verticalAlignment = Alignment.CenterVertically) {
        listOf(
            Tab.Palettes to Icons.Filled.Palette,
            Tab.Settings to Icons.Filled.Settings,
        ).forEach { (tab, icon) ->
            Box(Modifier.weight(1f).fillMaxSize().clickable { onSelect(tab) }, contentAlignment = Alignment.Center) {
                Icon(icon, tab.name, tint = if (tab == selected) Color(0xFFFFA000) else Color.White)
            }
        }
    }
}

@Composable
private fun SettingsDialog(c: CameraController, onDismiss: () -> Unit) {
    val s = c.settings
    AlertDialog(
        onDismissRequest = onDismiss,
        confirmButton = { TextButton(onClick = onDismiss) { Text("Close") } },
        title = { Text("Settings") },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text("Shutter frames averaged for calibration")
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    listOf(8, 16, 32).forEach { n ->
                        FilterChip(s.darkFrames == n, { c.updateSettings(s.copy(darkFrames = n)) }, { Text("$n") })
                    }
                }
                Text("Automatic recalibration")
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    listOf(0 to "Off", 30 to "30 s", 60 to "1 min", 300 to "5 min").forEach { (sec, label) ->
                        FilterChip(s.recalibrateSeconds == sec, { c.updateSettings(s.copy(recalibrateSeconds = sec)) }, { Text(label) })
                    }
                }
                Text("Tap the image to recalibrate at any time.", color = Muted, fontSize = 13.sp)
                Spacer(Modifier.height(8.dp))
                Text(
                    "uti120 ${BuildConfig.VERSION_NAME} · open source, MIT\n" +
                        "Uses libusb (LGPL-2.1). Not affiliated with UNI-T.",
                    color = Muted, fontSize = 12.sp,
                )
            }
        },
    )
}
