package io.github.dmikushin.uti120

import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.RectF
import kotlin.math.max

/**
 * Draws the centre crosshair and the hottest / coldest spot markers with their
 * temperatures.  One painter for the live screen, photos and video frames, so
 * all three look the same.  Coordinates of the summary are pixels of the
 * rendered image ([imageWidth] x [imageHeight]); the canvas area is
 * [width] x [height] and shows that image scaled to fill it.
 */
object TemperatureOverlay {
    private const val HOT = 0xFFFF3B30.toInt()
    private const val COLD = 0xFF2F80FF.toInt()

    fun draw(
        canvas: Canvas, width: Float, height: Float,
        imageWidth: Int, imageHeight: Int, t: TemperatureSummary,
    ) {
        val sx = width / imageWidth
        val sy = height / imageHeight
        // Sizes relative to the drawn image, so a 480 px video frame and a phone
        // screen get the same proportions.
        val unit = max(width, height) / 120f
        val stroke = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            style = Paint.Style.STROKE
            strokeWidth = unit * 0.9f
            color = Color.BLACK
        }
        val line = Paint(stroke).apply { strokeWidth = unit * 0.45f; color = Color.WHITE }
        val text = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            textSize = unit * 4.2f
            color = Color.WHITE
            isFakeBoldText = true
        }
        val pill = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = 0xA0000000.toInt() }

        // Centre crosshair.
        val cx = width / 2f
        val cy = height / 2f
        val arm = unit * 3.5f
        for (p in listOf(stroke, line)) {
            canvas.drawLine(cx - arm, cy, cx - arm / 3, cy, p)
            canvas.drawLine(cx + arm / 3, cy, cx + arm, cy, p)
            canvas.drawLine(cx, cy - arm, cx, cy - arm / 3, p)
            canvas.drawLine(cx, cy + arm / 3, cx, cy + arm, p)
        }
        label(canvas, format(t.centre), cx + arm * 0.7f, cy - arm * 0.7f, width, height, text, pill, unit)

        marker(canvas, (t.maxX + 0.5f) * sx, (t.maxY + 0.5f) * sy, HOT, unit)
        label(canvas, format(t.max), (t.maxX + 0.5f) * sx + unit * 2.5f, (t.maxY + 0.5f) * sy - unit * 2.5f,
            width, height, text.withColor(HOT), pill, unit)
        marker(canvas, (t.minX + 0.5f) * sx, (t.minY + 0.5f) * sy, COLD, unit)
        label(canvas, format(t.min), (t.minX + 0.5f) * sx + unit * 2.5f, (t.minY + 0.5f) * sy - unit * 2.5f,
            width, height, text.withColor(COLD), pill, unit)
    }

    fun format(c: Float) = "%.1f°C".format(c)

    private fun Paint.withColor(c: Int) = Paint(this).apply {
        // Readable on a dark pill: lighten the marker colour.
        color = blend(c, Color.WHITE, 0.35f)
    }

    private fun blend(a: Int, b: Int, f: Float): Int {
        fun ch(shift: Int) = (((a shr shift) and 0xFF) * (1 - f) + ((b shr shift) and 0xFF) * f).toInt()
        return Color.argb(255, ch(16), ch(8), ch(0))
    }

    private fun marker(canvas: Canvas, x: Float, y: Float, color: Int, unit: Float) {
        val r = unit * 1.6f
        canvas.drawCircle(x, y, r, Paint(Paint.ANTI_ALIAS_FLAG).apply {
            style = Paint.Style.STROKE; strokeWidth = unit * 1.1f; this.color = Color.BLACK
        })
        canvas.drawCircle(x, y, r, Paint(Paint.ANTI_ALIAS_FLAG).apply {
            style = Paint.Style.STROKE; strokeWidth = unit * 0.6f; this.color = color
        })
    }

    /** A value on a dark pill, kept inside the image. */
    private fun label(
        canvas: Canvas, s: String, x: Float, y: Float, width: Float, height: Float,
        text: Paint, pill: Paint, unit: Float,
    ) {
        val pad = unit * 0.8f
        val w = text.measureText(s) + 2 * pad
        val h = text.textSize + 2 * pad
        val left = x.coerceIn(0f, width - w)
        val top = (y - h).coerceIn(0f, height - h)
        canvas.drawRoundRect(RectF(left, top, left + w, top + h), h / 2, h / 2, pill)
        canvas.drawText(s, left + pad, top + pad - text.ascent() * 0.92f, text)
    }
}
