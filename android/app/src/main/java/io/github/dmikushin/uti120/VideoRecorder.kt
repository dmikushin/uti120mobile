package io.github.dmikushin.uti120

import android.graphics.Bitmap
import android.graphics.Paint
import android.graphics.Rect
import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.media.MediaMuxer
import android.os.ParcelFileDescriptor
import android.view.Surface

/**
 * H.264 in MP4 through the platform encoder (hardware on phones).  Frames are
 * drawn, upscaled with filtering, onto the encoder's input surface; their
 * time stamps are the moments they are drawn, so the video plays at the pace
 * it was recorded.
 *
 * The output size is the sensor image scaled by 16/3 (120x90 -> 640x480),
 * which keeps both sides multiples of 16 as some hardware encoders require.
 */
class VideoRecorder(output: ParcelFileDescriptor, imageWidth: Int, imageHeight: Int) {
    val width = imageWidth * 16 / 3
    val height = imageHeight * 16 / 3

    private val codec: MediaCodec
    private val surface: Surface
    private val muxer = MediaMuxer(output.fileDescriptor, MediaMuxer.OutputFormat.MUXER_OUTPUT_MPEG_4)
    private val fd = output
    private val paint = Paint(Paint.FILTER_BITMAP_FLAG)
    private val drainer: Thread
    @Volatile private var failure: Throwable? = null
    var frames = 0
        private set

    init {
        val format = MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, width, height).apply {
            setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface)
            setInteger(MediaFormat.KEY_BIT_RATE, 4_000_000)
            setInteger(MediaFormat.KEY_FRAME_RATE, FPS)
            setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 1)
        }
        codec = MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_VIDEO_AVC)
        codec.configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
        surface = codec.createInputSurface()
        codec.start()
        drainer = Thread({ drain() }, "uti120-video").apply { start() }
    }

    fun write(image: Bitmap) {
        failure?.let { throw IllegalStateException("video encoder failed", it) }
        val canvas = surface.lockHardwareCanvas()
        try {
            canvas.drawBitmap(image, null, Rect(0, 0, width, height), paint)
        } finally {
            surface.unlockCanvasAndPost(canvas)
        }
        frames++
    }

    /** Flushes the encoder and finalises the MP4. */
    fun finish() {
        try {
            codec.signalEndOfInputStream()
            drainer.join()
        } finally {
            codec.release()
            surface.release()
            fd.close()
        }
        failure?.let { throw IllegalStateException("video encoder failed", it) }
    }

    private fun drain() {
        val info = MediaCodec.BufferInfo()
        var track = -1
        try {
            while (true) {
                val index = codec.dequeueOutputBuffer(info, 10_000)
                when {
                    index == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED -> {
                        track = muxer.addTrack(codec.outputFormat)
                        muxer.start()
                    }
                    index >= 0 -> {
                        val buffer = codec.getOutputBuffer(index)!!
                        val config = info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG != 0
                        if (!config && info.size > 0 && track >= 0) muxer.writeSampleData(track, buffer, info)
                        codec.releaseOutputBuffer(index, false)
                        if (info.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM != 0) break
                    }
                }
            }
            if (track >= 0) muxer.stop()
        } catch (t: Throwable) {
            failure = t
        } finally {
            muxer.release()
        }
    }

    companion object {
        const val FPS = 25
    }
}
