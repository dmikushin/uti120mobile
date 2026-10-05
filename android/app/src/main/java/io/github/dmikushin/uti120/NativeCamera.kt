package io.github.dmikushin.uti120

import android.graphics.Bitmap
import androidx.core.graphics.createBitmap

/** How the image is shown; mirrors uti120::View in the native backend. */
data class ViewSettings(
    val palette: String = "ironbow",
    val mirror: Boolean = false,
    val flip: Boolean = false,
    /**
     * Clockwise, degrees.  270 is the vendor app's portrait orientation (camera
     * in the phone's bottom port): it calls its native core with rotation type 3,
     * which turns the sensor's right edge to the top, measured by running
     * CInfraredCore::Rotation from its libguide_sdk_unitrend.so on a test image.
     */
    val rotation: Int = 270,
)

/** uti120::GrabStats: where the time of camera frames goes (sums since opening). */
data class GrabStats(
    val frames: Double, val ignored: Double, val dropped: Double, val reads: Double,
    val drainMs: Double, val firstMs: Double, val transferMs: Double, val totalMs: Double,
) {
    constructor(v: DoubleArray) : this(v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7])

    operator fun minus(o: GrabStats) = GrabStats(
        frames - o.frames, ignored - o.ignored, dropped - o.dropped, reads - o.reads,
        drainMs - o.drainMs, firstMs - o.firstMs, transferMs - o.transferMs, totalMs - o.totalMs,
    )

    /** Per-frame averages of an interval (a difference of two samples). */
    fun describe(seconds: Double): String {
        val n = frames.coerceAtLeast(1.0)
        return "%.1f fps · per frame: drain %.1f, first read %.1f, transfer %.1f, total %.1f ms · %.1f reads · %d ignored, %d dropped".format(
            frames / seconds, drainMs / n, firstMs / n, transferMs / n, totalMs / n, reads / n,
            ignored.toInt(), dropped.toInt(),
        )
    }
}

/** uti120::RadiometrySettings: the measuring conditions. */
data class RadiometrySettings(
    val emissivity: Float = 0.95f,
    /** Reflected (ambient) temperature, degrees C; the vendor core's default. */
    val reflected: Float = 23.0f,
    /** Distance to the target in metres. */
    val distance: Float = 0.6f,
    /** High measuring range (above ~120 C). */
    val highRange: Boolean = false,
)

/**
 * Temperatures of one image in display coordinates (pixels of the rendered
 * image): the centre, and the coldest and hottest spots with their positions.
 * Spot values are 3x3 means, like the vendor app's.
 */
data class TemperatureSummary(
    val centre: Float, val min: Float, val minX: Float, val minY: Float,
    val max: Float, val maxX: Float, val maxY: Float,
) {
    companion object {
        const val LEN = 8

        /** From the native layout {valid, centre, min, min_x, min_y, max, max_x, max_y}. */
        fun of(v: FloatArray): TemperatureSummary? =
            if (v[0] == 0f) null else TemperatureSummary(v[1], v[2], v[3], v[4], v[5], v[6], v[7])
    }
}

data class CameraSettings(
    val darkFrames: Int = 16,
    /** Periodic shutter recalibration in seconds, 0 = never. */
    val recalibrateSeconds: Int = 0,
)

/**
 * One opened camera, backed by uti120::Pipeline.  [start] blocks for a few
 * seconds (on-camera NUC and shutter calibration) and must not run on the UI
 * thread.  Native failures arrive as RuntimeException with the backend's message.
 */
class NativeCamera private constructor(
    fd: Int, replay: String?, settings: CameraSettings, view: ViewSettings,
    calibration: java.io.File?, radiometry: RadiometrySettings,
) : AutoCloseable {
    /**
     * The camera behind a USB file descriptor (UsbDeviceConnection.getFileDescriptor());
     * with its [calibration] directory, images carry temperatures.
     */
    constructor(fd: Int, settings: CameraSettings, view: ViewSettings,
                calibration: java.io.File?, radiometry: RadiometrySettings) :
        this(fd, null, settings, view, calibration, radiometry)

    /** Raw frames recorded from a camera, played back at 25 fps (debug builds, testing). */
    constructor(replay: String, settings: CameraSettings, view: ViewSettings,
                calibration: java.io.File?, radiometry: RadiometrySettings) :
        this(-1, replay, settings, view, calibration, radiometry)

    @Volatile private var handle: Long = nativeOpen(
        fd, replay, settings.darkFrames, settings.recalibrateSeconds.toDouble(),
        view.palette, view.mirror, view.flip, view.rotation, calibration?.path,
        radiometry.emissivity, radiometry.reflected, radiometry.distance, radiometry.highRange,
    )

    /** The native handle; using a closed camera is a programming error, not a crash. */
    private fun h(): Long = handle.also { check(it != 0L) { "camera is closed" } }

    fun start() = nativeStart(h())

    /** Width and height of rendered images with the current view. */
    fun imageSize(): Pair<Int, Int> = nativeImageSize(h()).let { it[0] to it[1] }

    fun newBitmap(): Bitmap = imageSize().let { (w, h) -> createBitmap(w, h) }

    /** Renders the newest image and its temperatures into [summary]; false if none is available yet. */
    fun render(into: Bitmap, summary: FloatArray): Boolean = nativeRender(h(), into, summary)

    /** The average of the next [frames] images, and of their temperatures. */
    fun snapshot(frames: Int): Pair<Bitmap, TemperatureSummary?> {
        val summary = FloatArray(TemperatureSummary.LEN)
        val bitmap = newBitmap().also { nativeSnapshot(h(), frames, it, summary) }
        return bitmap to TemperatureSummary.of(summary)
    }

    /** Takes effect from the next frame, without restarting the camera. */
    fun setRadiometry(r: RadiometrySettings) =
        nativeSetRadiometry(h(), r.emissivity, r.reflected, r.distance, r.highRange)

    fun setView(view: ViewSettings) =
        nativeSetView(h(), view.palette, view.mirror, view.flip, view.rotation)

    fun recalibrate() = nativeRecalibrate(h())

    /** Cumulative frame timing, see [GrabStats]. */
    fun stats(): GrabStats = GrabStats(nativeStats(h()))

    /** Stops capturing and returns the camera to idle. */
    override fun close() {
        if (handle != 0L) {
            nativeClose(handle)
            handle = 0L
        }
    }

    companion object {
        init {
            System.loadLibrary("uti120_jni")
        }

        val paletteNames: List<String> by lazy { nativePaletteNames().toList() }

        fun paletteColors(name: String): IntArray = nativePaletteColors(name)

        /** Null if the calibration in [dir] belongs to the camera [sensor], otherwise why not. */
        fun checkCalibration(dir: java.io.File, sensor: String): String? = nativeCheckCalibration(dir.path, sensor)

        /** The sensor id of the camera behind [fd]; names its calibration cache. */
        fun sensorId(fd: Int): String = nativeSensorId(fd)

        /**
         * Reads the camera's calibration into [dir] and restarts the camera: the
         * firmware sends no frames after a flash read until it reboots.  The camera
         * leaves the bus and comes back as a new device; close [fd] afterwards.
         */
        fun readCalibration(fd: Int, dir: java.io.File) = nativeReadCalibration(fd, dir.path)

        @JvmStatic private external fun nativeOpen(
            fd: Int, replay: String?, darkFrames: Int, recalibrateSeconds: Double,
            palette: String, mirror: Boolean, flip: Boolean, rotation: Int, calibrationDir: String?,
            emissivity: Float, reflected: Float, distance: Float, highRange: Boolean,
        ): Long
        @JvmStatic private external fun nativeStart(handle: Long)
        @JvmStatic private external fun nativeClose(handle: Long)
        @JvmStatic private external fun nativeImageSize(handle: Long): IntArray
        @JvmStatic private external fun nativeRender(handle: Long, bitmap: Bitmap, summary: FloatArray): Boolean
        @JvmStatic private external fun nativeSnapshot(handle: Long, frames: Int, bitmap: Bitmap, summary: FloatArray)
        @JvmStatic private external fun nativeSetRadiometry(
            handle: Long, emissivity: Float, reflected: Float, distance: Float, highRange: Boolean,
        )
        @JvmStatic private external fun nativeSensorId(fd: Int): String
        @JvmStatic private external fun nativeCheckCalibration(dir: String, sensor: String): String?
        @JvmStatic private external fun nativeReadCalibration(fd: Int, dir: String)
        @JvmStatic private external fun nativeSetView(
            handle: Long, palette: String, mirror: Boolean, flip: Boolean, rotation: Int,
        )
        @JvmStatic private external fun nativeRecalibrate(handle: Long)
        @JvmStatic private external fun nativeStats(handle: Long): DoubleArray
        @JvmStatic private external fun nativePaletteNames(): Array<String>
        @JvmStatic private external fun nativePaletteColors(name: String): IntArray
    }
}
