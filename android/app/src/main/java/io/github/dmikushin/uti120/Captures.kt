package io.github.dmikushin.uti120

import android.content.ContentResolver
import android.content.ContentValues
import android.graphics.Bitmap
import android.net.Uri
import android.os.Environment
import android.provider.MediaStore
import androidx.core.graphics.scale
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/** Photos go to Pictures/UTi120, videos to Movies/UTi120, through MediaStore. */
object Captures {
    private const val FOLDER = "UTi120"

    private fun stamp(): String = SimpleDateFormat("yyyyMMdd_HHmmss", Locale.US).format(Date())

    /** Saves [image] upscaled by [scale] with bilinear filtering as PNG. */
    fun savePhoto(resolver: ContentResolver, image: Bitmap, scale: Int): Uri {
        val scaled = image.scale(image.width * scale, image.height * scale, filter = true)
        val values = ContentValues().apply {
            put(MediaStore.Images.Media.DISPLAY_NAME, "uti120_${stamp()}.png")
            put(MediaStore.Images.Media.MIME_TYPE, "image/png")
            put(MediaStore.Images.Media.RELATIVE_PATH, "${Environment.DIRECTORY_PICTURES}/$FOLDER")
            put(MediaStore.Images.Media.IS_PENDING, 1)
        }
        val uri = resolver.insert(MediaStore.Images.Media.EXTERNAL_CONTENT_URI, values)
            ?: error("cannot create a picture in MediaStore")
        try {
            resolver.openOutputStream(uri).use { out ->
                check(out != null && scaled.compress(Bitmap.CompressFormat.PNG, 100, out)) {
                    "cannot write $uri"
                }
            }
            resolver.update(uri, ContentValues().apply { put(MediaStore.Images.Media.IS_PENDING, 0) }, null, null)
        } catch (e: Exception) {
            resolver.delete(uri, null, null)
            throw e
        }
        return uri
    }

    /** A pending MediaStore video entry; [publish] makes it visible, [discard] removes it. */
    fun newVideo(resolver: ContentResolver): Uri {
        val values = ContentValues().apply {
            put(MediaStore.Video.Media.DISPLAY_NAME, "uti120_${stamp()}.mp4")
            put(MediaStore.Video.Media.MIME_TYPE, "video/mp4")
            put(MediaStore.Video.Media.RELATIVE_PATH, "${Environment.DIRECTORY_MOVIES}/$FOLDER")
            put(MediaStore.Video.Media.IS_PENDING, 1)
        }
        return resolver.insert(MediaStore.Video.Media.EXTERNAL_CONTENT_URI, values)
            ?: error("cannot create a video in MediaStore")
    }

    fun publish(resolver: ContentResolver, uri: Uri) {
        resolver.update(uri, ContentValues().apply { put(MediaStore.Video.Media.IS_PENDING, 0) }, null, null)
    }

    fun discard(resolver: ContentResolver, uri: Uri) {
        resolver.delete(uri, null, null)
    }
}
