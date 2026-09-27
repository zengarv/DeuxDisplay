package io.github.zengarv.deuxdisplay

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.RectF
import android.graphics.drawable.Drawable
import android.view.View
import io.github.zengarv.deuxdisplay.protocol.MediaState

/** A small pill showing the Windows volume for a moment after it changes. */
class VolumeIndicator(context: Context) : View(context) {

    private val density = resources.displayMetrics.density
    private val pill = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = Color.argb(0x99, 0x30, 0x30, 0x30) }
    private val track = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = Color.argb(0x4D, 0xFF, 0xFF, 0xFF) }
    private val fill = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = Color.WHITE }
    private val speaker: Drawable = context.getDrawable(R.drawable.dock_volume)!!
    private val speakerMuted: Drawable = context.getDrawable(R.drawable.dock_volume_muted)!!
    private val rect = RectF()

    private var level = 0f
    private var muted = false
    private val hide = Runnable { visibility = GONE }

    init {
        visibility = GONE
    }

    fun show(state: MediaState) {
        level = state.volumePercent / 100f
        muted = state.muted
        contentDescription = context.getString(R.string.volume_level, state.volumePercent)
        visibility = VISIBLE
        invalidate()
        removeCallbacks(hide)
        postDelayed(hide, SHOW_MS)
    }

    override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
        setMeasuredDimension((WIDTH_DP * density).toInt(), (HEIGHT_DP * density).toInt())
    }

    override fun onDraw(canvas: Canvas) {
        val h = height.toFloat()
        rect.set(0f, 0f, width.toFloat(), h)
        canvas.drawRoundRect(rect, h / 2, h / 2, pill)

        val icon = if (muted) speakerMuted else speaker
        val iconSize = (24 * density).toInt()
        val iconLeft = (12 * density).toInt()
        val iconTop = (height - iconSize) / 2
        icon.setBounds(iconLeft, iconTop, iconLeft + iconSize, iconTop + iconSize)
        icon.draw(canvas)

        val barLeft = iconLeft + iconSize + 10 * density
        val barRight = width - 18 * density
        val barHalf = 2 * density
        rect.set(barLeft, h / 2 - barHalf, barRight, h / 2 + barHalf)
        canvas.drawRoundRect(rect, barHalf, barHalf, track)
        if (!muted && level > 0f) {
            rect.right = barLeft + (barRight - barLeft) * level
            canvas.drawRoundRect(rect, barHalf, barHalf, fill)
        }
    }

    companion object {
        private const val WIDTH_DP = 200
        private const val HEIGHT_DP = 44
        private const val SHOW_MS = 1500L
    }
}
