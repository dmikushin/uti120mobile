package io.github.dmikushin.uti120

import android.graphics.Bitmap
import androidx.core.graphics.createBitmap

/** How the image is shown; mirrors uti120::View in the native backend. */
data class ViewSettings(
    val palette: String = "ironbow",
    val mirror: Boolean = false,
    val flip: Boolean = false,
    /** Clockwise, degrees. The vendor app shows the camera as a 3:4 portrait image. */
    val rotation: Int = 90,
)

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
class NativeCamera(fd: Int, settings: CameraSettings, view: ViewSettings) : AutoCloseable {
    private var handle: Long = nativeOpen(
        fd, settings.darkFrames, settings.recalibrateSeconds.toDouble(),
        view.palette, view.mirror, view.flip, view.rotation,
    )

    fun start() = nativeStart(handle)

    /** Width and height of rendered images with the current view. */
    fun imageSize(): Pair<Int, Int> = nativeImageSize(handle).let { it[0] to it[1] }

    fun newBitmap(): Bitmap = imageSize().let { (w, h) -> createBitmap(w, h) }

    /** Renders the newest image; false if none is available yet. */
    fun render(into: Bitmap): Boolean = nativeRender(handle, into)

    /** The average of the next [frames] images. */
    fun snapshot(frames: Int): Bitmap = newBitmap().also { nativeSnapshot(handle, frames, it) }

    fun setView(view: ViewSettings) =
        nativeSetView(handle, view.palette, view.mirror, view.flip, view.rotation)

    fun recalibrate() = nativeRecalibrate(handle)

    fun framesCaptured(): Long = nativeFramesCaptured(handle)

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

        @JvmStatic private external fun nativeOpen(
            fd: Int, darkFrames: Int, recalibrateSeconds: Double,
            palette: String, mirror: Boolean, flip: Boolean, rotation: Int,
        ): Long
        @JvmStatic private external fun nativeStart(handle: Long)
        @JvmStatic private external fun nativeClose(handle: Long)
        @JvmStatic private external fun nativeImageSize(handle: Long): IntArray
        @JvmStatic private external fun nativeRender(handle: Long, bitmap: Bitmap): Boolean
        @JvmStatic private external fun nativeSnapshot(handle: Long, frames: Int, bitmap: Bitmap)
        @JvmStatic private external fun nativeSetView(
            handle: Long, palette: String, mirror: Boolean, flip: Boolean, rotation: Int,
        )
        @JvmStatic private external fun nativeRecalibrate(handle: Long)
        @JvmStatic private external fun nativeFramesCaptured(handle: Long): Long
        @JvmStatic private external fun nativePaletteNames(): Array<String>
        @JvmStatic private external fun nativePaletteColors(name: String): IntArray
    }
}
