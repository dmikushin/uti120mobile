package io.github.dmikushin.uti120

import android.content.ActivityNotFoundException
import android.content.Intent
import android.provider.MediaStore
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectHorizontalDragGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
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
import androidx.compose.material.icons.automirrored.filled.RotateRight
import androidx.compose.material.icons.filled.FiberManualRecord
import androidx.compose.material.icons.filled.Flip
import androidx.compose.material.icons.filled.Palette
import androidx.compose.material.icons.filled.PhotoCamera
import androidx.compose.material.icons.filled.ScreenRotation
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
import androidx.compose.ui.draw.rotate
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.FilterQuality
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.input.pointer.pointerInput
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

private enum class Tab { Orientation, Palettes, Settings }
private enum class Mode { Photo, Video }

@Composable
fun CameraScreen(c: CameraController) {
    MaterialTheme(colorScheme = darkColorScheme()) {
        var tab by rememberSaveable { mutableStateOf<Tab?>(null) }
        var mode by rememberSaveable { mutableStateOf(Mode.Photo) }
        Column(Modifier.fillMaxSize().background(BarColor).systemBarsPadding()) {
            Header(c)
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
                        ImageArea(c)
                        when (tab) {
                            Tab.Orientation -> OrientationBar(c, Modifier.align(Alignment.BottomCenter))
                            Tab.Palettes -> PaletteStrip(c, Modifier.align(Alignment.BottomCenter))
                            else -> {}
                        }
                    }
                    CapturePanel(c, mode, { mode = it }, Modifier.weight(1f).fillMaxWidth())
                }
            }
            BottomBar(tab) { tab = if (tab == it) null else it }
        }
        if (tab == Tab.Settings) SettingsDialog(c) { tab = null }
    }
}

@Composable
private fun Header(c: CameraController) {
    Row(
        Modifier.fillMaxWidth().height(45.dp).padding(horizontal = 16.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text("UTi120Mobile", color = Color.White, fontWeight = FontWeight.Medium, fontSize = 17.sp)
        Spacer(Modifier.weight(1f))
        val status = when {
            c.recording -> "● REC"
            c.calibrating -> "Calibrating…"
            c.phase == Phase.Live -> "%.0f fps".format(c.fps)
            c.phase == Phase.Starting -> "Calibrating…"
            else -> ""
        }
        Text(status, color = if (c.recording) Color(0xFFFF4040) else Muted, fontSize = 14.sp)
    }
}

@Composable
private fun ImageArea(c: CameraController) {
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

/** Mirror / flip / rotate, shown over the bottom of the image like the vendor's tool bars. */
@Composable
private fun OrientationBar(c: CameraController, modifier: Modifier) {
    val v = c.view
    Row(
        modifier.fillMaxWidth().height(60.dp).background(Color(0xC00E0E0E)),
        horizontalArrangement = Arrangement.SpaceEvenly, verticalAlignment = Alignment.CenterVertically,
    ) {
        ToolButton(Icons.Filled.Flip, "Mirror", v.mirror) { c.updateView(v.copy(mirror = !v.mirror)) }
        ToolButton(Icons.Filled.Flip, "Flip", v.flip, iconRotation = 90f) { c.updateView(v.copy(flip = !v.flip)) }
        // Rotation changes the image size, which a running recording cannot follow.
        ToolButton(Icons.AutoMirrored.Filled.RotateRight, "Rotate ${v.rotation}°", false, enabled = !c.recording) {
            c.updateView(v.copy(rotation = (v.rotation + 90) % 360))
        }
    }
}

@Composable
private fun ToolButton(
    icon: ImageVector, label: String, active: Boolean, iconRotation: Float = 0f,
    enabled: Boolean = true, onClick: () -> Unit,
) {
    val tint = when {
        !enabled -> Color(0xFF555555)
        active -> Color(0xFFFFA000)
        else -> Color.White
    }
    Column(
        Modifier.clickable(enabled = enabled, onClick = onClick).padding(8.dp, 4.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        Icon(icon, label, tint = tint, modifier = Modifier.size(26.dp).rotate(iconRotation))
        Text(label, color = tint, fontSize = 11.sp)
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

/** Panel under the image (vendor: TouchBoardPanel): gallery, capture, photo/video by swipe. */
@Composable
private fun CapturePanel(c: CameraController, mode: Mode, setMode: (Mode) -> Unit, modifier: Modifier) {
    val context = LocalContext.current
    Box(
        modifier.background(PanelColor).pointerInput(c.recording) {
            var drag = 0f
            detectHorizontalDragGestures(
                onDragStart = { drag = 0f },
                onDragEnd = {
                    if (!c.recording) {
                        if (drag < -60f) setMode(Mode.Video) else if (drag > 60f) setMode(Mode.Photo)
                    }
                },
            ) { _, delta -> drag += delta }
        },
    ) {
        Column(Modifier.align(Alignment.TopCenter).padding(top = 10.dp), horizontalAlignment = Alignment.CenterHorizontally) {
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Mode.entries.forEach {
                    Box(Modifier.size(8.dp).clip(CircleShape).background(if (it == mode) Color.White else Color(0xFF606060)))
                }
            }
            Text(if (mode == Mode.Photo) "PHOTO" else "VIDEO", color = Muted, fontSize = 11.sp, modifier = Modifier.padding(top = 4.dp))
        }
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
        CaptureButton(c, mode, Modifier.align(Alignment.Center))
    }
}

@Composable
private fun CaptureButton(c: CameraController, mode: Mode, modifier: Modifier) {
    val enabled = c.phase == Phase.Live && !c.busy
    Box(
        modifier.size(68.dp).clip(CircleShape).border(3.dp, Color.White, CircleShape)
            .clickable(enabled = enabled) { if (mode == Mode.Photo) c.takePhoto() else c.toggleRecording() },
        contentAlignment = Alignment.Center,
    ) {
        Box(
            Modifier.size(56.dp).clip(CircleShape).background(if (enabled) Color.White else Color(0xFF808080)),
            contentAlignment = Alignment.Center,
        ) {
            when {
                c.busy -> CircularProgressIndicator(Modifier.size(28.dp), color = Color.Black, strokeWidth = 3.dp)
                mode == Mode.Photo -> Icon(Icons.Filled.PhotoCamera, "Take photo", tint = Color.Black)
                c.recording -> Icon(Icons.Filled.Stop, "Stop recording", tint = Color(0xFFE02020), modifier = Modifier.size(34.dp))
                else -> Icon(Icons.Filled.FiberManualRecord, "Start recording", tint = Color(0xFFE02020), modifier = Modifier.size(34.dp))
            }
        }
    }
}

/** 45 dp tab bar (vendor: BottomBar_home), equal-width tabs. */
@Composable
private fun BottomBar(selected: Tab?, onSelect: (Tab) -> Unit) {
    Row(Modifier.fillMaxWidth().height(45.dp).background(BarColor), verticalAlignment = Alignment.CenterVertically) {
        listOf(
            Tab.Orientation to Icons.Filled.ScreenRotation,
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
