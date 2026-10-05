package io.github.dmikushin.uti120

import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.graphics.Bitmap
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbDeviceConnection
import android.hardware.usb.UsbManager
import android.net.Uri
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.util.Log
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableDoubleStateOf
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.asImageBitmap
import androidx.core.content.ContextCompat
import androidx.core.content.edit
import java.io.File
import java.util.concurrent.Executors

enum class Phase { NoCamera, AwaitingPermission, ReadingCalibration, Restarting, Starting, Live, Failed }

/**
 * Owns the USB connection and the native pipeline, and exposes their state to
 * the UI.  Camera operations run on one background thread; a render thread
 * turns the newest image into a bitmap 25 times a second and feeds the screen
 * and, while recording, the video encoder.
 */
class CameraController(private val context: Context) {
    var phase by mutableStateOf(Phase.NoCamera)
        private set
    var message by mutableStateOf("")
        private set
    var image by mutableStateOf<ImageBitmap?>(null)
        private set
    var fps by mutableDoubleStateOf(0.0)
        private set
    /** Frame timing of the last second, shown when the fps label is tapped. */
    var diagnostics by mutableStateOf("")
        private set
    var calibrating by mutableStateOf(false)
        private set
    var recording by mutableStateOf(false)
        private set
    /** SystemClock.elapsedRealtime() when the current recording started. */
    var recordingSince by mutableLongStateOf(0L)
        private set
    /** A photo is being taken (8 frames averaged and saved). */
    var photoBusy by mutableStateOf(false)
        private set
    /** A recording is being started. */
    var videoBusy by mutableStateOf(false)
        private set

    /** Debug builds: play back this raw recording instead of using the USB camera. */
    var replay: String? = null
    var lastCapture by mutableStateOf<Uri?>(null)
        private set
    var view by mutableStateOf(Prefs.loadView(context))
        private set
    var settings by mutableStateOf(Prefs.loadSettings(context))
        private set
    var radiometry by mutableStateOf(Prefs.loadRadiometry(context))
        private set
    /** Temperatures of the shown image (spot values averaged over the last 5 frames). */
    var temperatures by mutableStateOf<TemperatureSummary?>(null)
        private set
    /** Short notice shown over the image (saved photo, errors of captures). */
    var notice by mutableStateOf<String?>(null)
        private set

    private val usb = context.getSystemService(Context.USB_SERVICE) as UsbManager
    // Incremented on every connect and disconnect (main thread).  Results of
    // background work carry the session they belong to and are dropped when
    // it is over, e.g. a start that finishes after the camera was unplugged.
    private var session = 0
    private val main = Handler(Looper.getMainLooper())
    private val worker = Executors.newSingleThreadExecutor { Thread(it, "uti120-camera") }

    // Accessed on the worker thread only.
    private var camera: NativeCamera? = null
    private var connection: UsbDeviceConnection? = null
    private var renderer: Thread? = null
    @Volatile private var rendering = false
    // The render thread writes frames into the recorder while the worker
    // creates and finishes it; both hold this lock.
    private val recorderLock = Any()
    private var recorder: VideoRecorder? = null
    private var videoUri: Uri? = null
    // The camera restarted after its calibration was read; the one that comes back must be it.
    private var restartedSensor: String? = null

    private val receiver = object : BroadcastReceiver() {
        override fun onReceive(c: Context, intent: Intent) {
            when (intent.action) {
                ACTION_PERMISSION -> {
                    if (intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)) {
                        setPhase(Phase.NoCamera, "")  // the request is answered
                        connect()
                    } else {
                        fail("USB permission denied")
                    }
                }
                UsbManager.ACTION_USB_DEVICE_DETACHED -> {
                    // While the calibration is read the camera is restarted on purpose.
                    val expected = phase == Phase.ReadingCalibration || phase == Phase.Restarting
                    if (!expected && device(intent)?.let(::isCamera) == true) disconnect(Phase.NoCamera, "")
                }
            }
        }
    }

    fun register() {
        val filter = IntentFilter(ACTION_PERMISSION).apply { addAction(UsbManager.ACTION_USB_DEVICE_DETACHED) }
        ContextCompat.registerReceiver(context, receiver, filter, ContextCompat.RECEIVER_NOT_EXPORTED)
    }

    fun unregister() = context.unregisterReceiver(receiver)

    /** Finds the camera, asks for permission if needed, then opens and calibrates it. */
    fun connect() {
        // Also triggered by USB_DEVICE_ATTACHED while a permission request is open.
        if (phase in BUSY_PHASES || phase == Phase.AwaitingPermission) return
        replay?.let { path ->
            // Debug: temperatures too if a calibration was put into files/calibration/replay.
            val dir = File(context.filesDir, "calibration/replay").takeIf { File(it, "coefficients.json").exists() }
            start { sv, _ -> NativeCamera(path, sv.first, sv.second, dir, radiometry) }
            return
        }
        val device = usb.deviceList.values.firstOrNull(::isCamera)
        if (device == null) {
            setPhase(Phase.NoCamera, "")
            return
        }
        if (!usb.hasPermission(device)) {
            setPhase(Phase.AwaitingPermission, "")
            val intent = Intent(ACTION_PERMISSION).setPackage(context.packageName)
            usb.requestPermission(device, PendingIntent.getBroadcast(context, 0, intent, PendingIntent.FLAG_MUTABLE))
            return
        }
        start { sv, id ->
            val conn = usb.openDevice(device) ?: error("cannot open the USB device")
            connection = conn
            val sensor = NativeCamera.sensorId(conn.fileDescriptor)
            restartedSensor?.let { expected ->
                restartedSensor = null
                check(sensor == expected) {
                    "A different camera ($sensor) came back after restarting $expected."
                }
            }
            val dir = calibrationDir(sensor)
            val cached = File(dir, "coefficients.json").exists()
            val problem = if (cached) NativeCamera.checkCalibration(dir, sensor) else null
            if (problem != null) Log.w(TAG, "discarding the cached calibration: $problem")
            if (cached && problem == null) {
                NativeCamera(conn.fileDescriptor, sv.first, sv.second, dir, radiometry)
            } else {
                readCalibration(conn, dir, sensor, id)
                null
            }
        }
    }

    private fun calibrationDir(sensor: String) =
        File(context.filesDir, "calibration/" + sensor.replace(Regex("[^A-Za-z0-9_.-]"), "_"))

    /**
     * Worker: reads the camera's calibration into [dir] (once per camera), then
     * waits for the camera to come back from the restart the read requires and
     * connects again.
     */
    private fun readCalibration(conn: UsbDeviceConnection, dir: File, sensor: String, id: Int) {
        main.post { if (session == id) setPhase(Phase.ReadingCalibration, "") }
        val partial = File(dir.parentFile, dir.name + ".partial")
        partial.deleteRecursively()
        check(partial.mkdirs()) { "cannot create $partial" }
        val before = usb.deviceList.values.firstOrNull(::isCamera)?.deviceName
        NativeCamera.readCalibration(conn.fileDescriptor, partial)
        dir.deleteRecursively()
        check(partial.renameTo(dir)) { "cannot store the calibration in $dir" }
        connection = null
        conn.close()
        restartedSensor = sensor
        main.post { if (session == id) setPhase(Phase.Restarting, "") }
        val back = waitForCamera(before)
        Log.i(TAG, "camera back after restart: $back")
        main.post {
            if (session != id) return@post
            if (back) {
                setPhase(Phase.NoCamera, "")
                connect()
            } else {
                // It still may: the system then reports it as attached and the app connects.
                fail("The camera has not come back after restarting yet. If it does not, unplug it and plug it in again.")
            }
        }
    }

    /**
     * Worker: true once the camera has enumerated again after its restart,
     * i.e. it is listed under a device name other than [before] (bounded).
     */
    private fun waitForCamera(before: String?): Boolean {
        val deadline = SystemClock.uptimeMillis() + RESTART_TIMEOUT_MS
        while (SystemClock.uptimeMillis() < deadline) {
            val now = usb.deviceList.values.firstOrNull(::isCamera)?.deviceName
            if (now != null && now != before) return true
            Thread.sleep(200)
        }
        return false
    }

    /**
     * Opens the camera with [open] on the worker, calibrates it and starts
     * rendering; [open] returns null when it took the connection elsewhere.
     */
    private fun start(open: (Pair<CameraSettings, ViewSettings>, Int) -> NativeCamera?) {
        setPhase(Phase.Starting, "")
        val id = ++session
        val sv = settings to view
        worker.execute {
            try {
                val cam = open(sv, id) ?: return@execute
                camera = cam
                cam.start()
                startRendering(cam, id)
                main.post { if (session == id) setPhase(Phase.Live, "") }
            } catch (e: Exception) {
                Log.e(TAG, "camera start failed", e)
                closeCamera()
                main.post { if (session == id) fail(e.message ?: e.toString()) }
            }
        }
    }

    fun disconnect(next: Phase = Phase.NoCamera, why: String = "") {
        session++
        worker.execute { closeCamera() }
        setPhase(next, why)
        image = null
        temperatures = null
    }

    fun shutdown() {
        session++
        main.removeCallbacksAndMessages(null)
        worker.execute { closeCamera() }
        worker.shutdown()
    }

    fun updateView(v: ViewSettings) {
        view = v
        Prefs.saveView(context, v)
        worker.execute { camera?.setView(v) }
    }

    /** New camera settings take effect by restarting the pipeline. */
    fun updateSettings(s: CameraSettings) {
        settings = s
        Prefs.saveSettings(context, s)
        if (phase == Phase.Live) {
            // Queued on the worker: the old pipeline is closed before the new one opens.
            disconnect(Phase.NoCamera, "")
            connect()
        }
    }

    /** Measuring conditions; applied from the next frame without restarting the camera. */
    fun updateRadiometry(r: RadiometrySettings) {
        radiometry = r
        Prefs.saveRadiometry(context, r)
        worker.execute { camera?.setRadiometry(r) }
    }

    fun recalibrate() {
        if (phase != Phase.Live || calibrating) return
        calibrating = true
        worker.execute { camera?.recalibrate() }
        // The capture thread calibrates before its next frame: ~1 s of shutter frames.
        main.postDelayed({ calibrating = false }, 1500)
    }

    fun takePhoto() {
        if (phase != Phase.Live || photoBusy) return
        photoBusy = true
        worker.execute {
            val result = runCatching {
                val (shot, temps) = camera!!.snapshot(PHOTO_FRAMES)
                Captures.savePhoto(context.contentResolver, shot, PHOTO_SCALE, temps)
            }
            main.post {
                photoBusy = false
                result.onSuccess { lastCapture = it; show("Photo saved to Pictures/UTi120") }
                    .onFailure { show("Photo failed: ${it.message}") }
            }
        }
    }

    /** Starts recording (press and hold on the capture button). */
    fun startRecording() {
        if (phase != Phase.Live || recording || videoBusy) return
        videoBusy = true
        worker.execute {
            val result = runCatching {
                check(synchronized(recorderLock) { recorder } == null) { "already recording" }
                val uri = Captures.newVideo(context.contentResolver)
                videoUri = uri
                try {
                    val (w, h) = camera!!.imageSize()
                    val fd = context.contentResolver.openFileDescriptor(uri, "rw") ?: error("cannot open $uri")
                    val created = VideoRecorder(fd, w, h)  // closes fd itself if it fails
                    synchronized(recorderLock) { recorder = created }
                } catch (e: Exception) {
                    Captures.discard(context.contentResolver, uri)
                    videoUri = null
                    throw e
                }
            }
            main.post {
                videoBusy = false
                result.onSuccess {
                    recording = true
                    recordingSince = SystemClock.elapsedRealtime()
                }.onFailure { show("Video failed: ${it.message}") }
            }
        }
    }

    /**
     * Stops recording (the capture button is released).  Queued behind a
     * start that is still in progress, so a short hold still ends the video.
     */
    fun stopRecording() {
        worker.execute {
            val active = synchronized(recorderLock) { recorder } ?: return@execute
            val result = runCatching { finishRecording(active) }
            main.post {
                recording = false
                result.onSuccess { uri -> uri?.let { lastCapture = it; show("Video saved to Movies/UTi120") } }
                    .onFailure { show("Video failed: ${it.message}") }
            }
        }
    }

    // --- worker thread ---------------------------------------------------

    private fun finishRecording(active: VideoRecorder): Uri? {
        synchronized(recorderLock) { recorder = null }
        val uri = videoUri!!
        videoUri = null
        return try {
            active.finish()
            check(active.frames > 0) { "no frames were recorded" }
            Captures.publish(context.contentResolver, uri)
            uri
        } catch (e: Exception) {
            Captures.discard(context.contentResolver, uri)
            throw e
        }
    }

    private fun startRendering(cam: NativeCamera, id: Int) {
        rendering = true
        renderer = Thread({ renderLoop(cam, id) }, "uti120-render").apply { start() }
    }

    private fun renderLoop(cam: NativeCamera, id: Int) {
        val period = 1000L / VideoRecorder.FPS
        var next = SystemClock.uptimeMillis()
        var statAt = next
        var stats = cam.stats()
        var renderMs = 0.0
        var renders = 0
        val raw = FloatArray(TemperatureSummary.LEN)
        val smoothing = SpotSmoothing(5)
        while (rendering) {
            try {
                val started = SystemClock.elapsedRealtimeNanos()
                val bitmap = cam.newBitmap()
                if (cam.render(bitmap, raw)) {
                    val temps = TemperatureSummary.of(raw)?.let(smoothing::add)
                    synchronized(recorderLock) { recorder?.write(bitmap, temps) }
                    val shown = bitmap.asImageBitmap()
                    main.post {
                        if (session == id) {
                            image = shown
                            temperatures = temps
                        }
                    }
                }
                renderMs += (SystemClock.elapsedRealtimeNanos() - started) / 1e6
                renders++
            } catch (e: Exception) {
                Log.e(TAG, "rendering failed", e)
                rendering = false
                main.post { if (session == id) disconnect(Phase.Failed, e.message ?: e.toString()) }
                return
            }
            val now = SystemClock.uptimeMillis()
            if (now - statAt >= 1000) {
                val current = cam.stats()
                val interval = current - stats
                val seconds = (now - statAt) / 1000.0
                val text = interval.describe(seconds) + " · render %.1f ms".format(renderMs / renders.coerceAtLeast(1))
                Log.i(TAG, text)
                main.post {
                    if (session == id) {
                        fps = interval.frames / seconds
                        diagnostics = text
                    }
                }
                statAt = now
                stats = current
                renderMs = 0.0
                renders = 0
            }
            next += period
            val delay = next - SystemClock.uptimeMillis()
            if (delay > 0) Thread.sleep(delay) else next = SystemClock.uptimeMillis()
        }
    }

    private fun closeCamera() {
        rendering = false
        renderer?.join()
        renderer = null
        synchronized(recorderLock) { recorder }?.let { active ->
            runCatching { finishRecording(active) }.onSuccess { uri -> main.post { lastCapture = uri } }
        }
        main.post { recording = false }
        camera?.close()
        camera = null
        connection?.close()
        connection = null
    }

    // --- main thread -----------------------------------------------------

    private fun setPhase(p: Phase, why: String) {
        phase = p
        message = why
    }

    private fun fail(why: String) = setPhase(Phase.Failed, why)

    private fun show(text: String) {
        notice = text
        main.postDelayed({ if (notice == text) notice = null }, 2500)
    }

    private fun isCamera(d: UsbDevice) = d.vendorId == VENDOR_ID && d.productId == PRODUCT_ID

    @Suppress("DEPRECATION")
    private fun device(intent: Intent): UsbDevice? =
        if (Build.VERSION.SDK_INT >= 33) intent.getParcelableExtra(UsbManager.EXTRA_DEVICE, UsbDevice::class.java)
        else intent.getParcelableExtra(UsbManager.EXTRA_DEVICE)

    companion object {
        private const val TAG = "uti120"
        private const val ACTION_PERMISSION = "io.github.dmikushin.uti120.USB_PERMISSION"
        const val VENDOR_ID = 0x5656
        const val PRODUCT_ID = 0x1201
        const val PHOTO_FRAMES = 8
        const val PHOTO_SCALE = 4
        private const val RESTART_TIMEOUT_MS = 20_000L
        private val BUSY_PHASES = setOf(Phase.ReadingCalibration, Phase.Restarting, Phase.Starting, Phase.Live)
    }
}

/**
 * Running average of the spot values over the last [n] frames, as the vendor
 * app does (UsbCameraHelper.callBackOneFrameBitmap); positions are the newest.
 * After a jump of the settings the old values age out within [n] frames.
 */
class SpotSmoothing(private val n: Int) {
    private val history = ArrayDeque<TemperatureSummary>()

    fun add(t: TemperatureSummary): TemperatureSummary {
        history.addLast(t)
        if (history.size > n) history.removeFirst()
        return t.copy(
            centre = history.map { it.centre }.average().toFloat(),
            min = history.map { it.min }.average().toFloat(),
            max = history.map { it.max }.average().toFloat(),
        )
    }
}

/** View and camera settings survive restarts. */
object Prefs {
    private fun prefs(c: Context) = c.getSharedPreferences("uti120", Context.MODE_PRIVATE)

    fun loadView(c: Context) = prefs(c).let {
        val d = ViewSettings()
        // Orientation is fixed to the vendor app's portrait view; only the palette is chosen.
        d.copy(palette = it.getString("palette", d.palette)!!.takeIf { p -> p in NativeCamera.paletteNames } ?: d.palette)
    }

    fun saveView(c: Context, v: ViewSettings) = prefs(c).edit {
        putString("palette", v.palette)
        // Orientation saved by 0.1.0-0.1.2, which had orientation controls.
        remove("mirror")
        remove("flip")
        remove("rotation")
    }

    fun loadSettings(c: Context) = prefs(c).let {
        val d = CameraSettings()
        CameraSettings(it.getInt("darkFrames", d.darkFrames), it.getInt("recalibrateSeconds", d.recalibrateSeconds))
    }

    fun saveSettings(c: Context, s: CameraSettings) = prefs(c).edit {
        putInt("darkFrames", s.darkFrames)
        putInt("recalibrateSeconds", s.recalibrateSeconds)
    }

    fun loadRadiometry(c: Context) = prefs(c).let {
        val d = RadiometrySettings()
        RadiometrySettings(
            emissivity = it.getFloat("emissivity", d.emissivity),
            reflected = it.getFloat("reflected", d.reflected),
            distance = it.getFloat("distance", d.distance),
            highRange = it.getBoolean("highRange", d.highRange),
        )
    }

    fun saveRadiometry(c: Context, r: RadiometrySettings) = prefs(c).edit {
        putFloat("emissivity", r.emissivity)
        putFloat("reflected", r.reflected)
        putFloat("distance", r.distance)
        putBoolean("highRange", r.highRange)
    }
}
