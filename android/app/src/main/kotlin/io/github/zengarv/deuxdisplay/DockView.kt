package io.github.zengarv.deuxdisplay

import android.animation.Animator
import android.animation.AnimatorListenerAdapter
import android.animation.TimeInterpolator
import android.animation.ValueAnimator
import android.annotation.SuppressLint
import android.content.Context
import android.content.SharedPreferences
import android.content.res.ColorStateList
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.RectF
import android.graphics.drawable.RippleDrawable
import android.view.MotionEvent
import android.view.View
import android.view.ViewConfiguration
import android.view.animation.AccelerateInterpolator
import android.view.animation.OvershootInterpolator
import android.widget.ImageView
import android.widget.LinearLayout
import io.github.zengarv.deuxdisplay.protocol.DockAction
import io.github.zengarv.deuxdisplay.protocol.MediaState
import io.github.zengarv.deuxdisplay.protocol.Playback
import kotlin.math.PI
import kotlin.math.cos
import kotlin.math.exp
import kotlin.math.hypot
import kotlin.math.max
import kotlin.math.min

/**
 * The shortcut dock: a translucent circle that springs open into a row of white outline icons.
 * Drag the circle to move the dock, tap it to expand or collapse. The position is remembered per
 * orientation. It's an ordinary view above the video surface and only redraws while touched or
 * animating, so the frame path doesn't see it.
 *
 * The window shows the video surface everywhere except where views were at the last layout pass
 * (the "transparent region"), so anything drawn elsewhere is cut off. Moving the dock therefore
 * requests a layout, and the open/close animation stays inside the dock's laid-out bounds.
 */
@SuppressLint("ViewConstructor")
class DockView(
    context: Context,
    private val prefs: SharedPreferences,
    private val onAction: (Int) -> Unit,
) : LinearLayout(context) {

    private val buttonSize = dp(44)
    private val pad = dp(4)
    private val slack = dp(14) // room past the pill's end for the spring to overshoot into
    private val margin = dp(8).toFloat()
    private val touchSlop = ViewConfiguration.get(context).scaledTouchSlop

    private val handle = button(R.drawable.dock_handle, R.string.dock_toggle) { toggle() }
    private val playPause = button(R.drawable.dock_play, R.string.dock_play_pause) { onPlayPause() }
    private val shortcuts = listOf(
        button(R.drawable.dock_undo, R.string.dock_undo) { onAction(DockAction.UNDO) },
        button(R.drawable.dock_redo, R.string.dock_redo) { onAction(DockAction.REDO) },
        button(R.drawable.dock_task_view, R.string.dock_task_view) { onAction(DockAction.TASK_VIEW) },
        playPause,
    )

    private var expanded = false
    private var handleLast = false // buttons before the handle (dock opens left/up)

    // 0 = just the circle around the handle, 1 = the full pill. Overshoots a little while opening.
    private var reveal = 0f
    private var revealAnimator: ValueAnimator? = null
    private val pillPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = Color.argb(0x99, 0x30, 0x30, 0x30) }
    private val pill = RectF()

    // Where the handle sits in the parent; the rest of the dock is laid out around it.
    private var anchorX = 0f
    private var anchorY = 0f
    private var placedFor: String? = null // orientation the anchor was restored for

    private var playback = Playback.NONE
    private var showingPlaying = false
    private val showRealPlayback = Runnable { showPlaying(playback == Playback.PLAYING) }

    init {
        setWillNotDraw(false) // draws its own pill
        setPadding(pad, pad, pad, pad)
        addView(handle)
        handle.setOnTouchListener(::onHandleTouch)
        addOnLayoutChangeListener { _, _, _, _, _, _, _, _, _ -> place() }
    }

    override fun onAttachedToWindow() {
        super.onAttachedToWindow()
        (parent as? View)?.addOnLayoutChangeListener(parentLayoutListener)
    }

    override fun onDetachedFromWindow() {
        (parent as? View)?.removeOnLayoutChangeListener(parentLayoutListener)
        revealAnimator?.cancel()
        super.onDetachedFromWindow()
    }

    private val parentLayoutListener = View.OnLayoutChangeListener { _, _, _, _, _, _, _, _, _ -> place() }

    /** The host's playback state; replaces any guess made when play/pause was tapped. */
    fun setMediaState(state: MediaState) {
        playback = state.playback
        removeCallbacks(showRealPlayback)
        showPlaying(playback == Playback.PLAYING)
    }

    private fun onPlayPause() {
        onAction(DockAction.PLAY_PAUSE)
        if (playback == Playback.NONE) return // nothing known to toggle
        // Flip at once; the host's MEDIA_STATE confirms it. Without one, fall back to the last state.
        showPlaying(!showingPlaying)
        removeCallbacks(showRealPlayback)
        postDelayed(showRealPlayback, PLAYBACK_CONFIRM_MS)
    }

    private fun showPlaying(playing: Boolean) {
        showingPlaying = playing
        playPause.setImageResource(if (playing) R.drawable.dock_pause else R.drawable.dock_play)
    }

    // --- Opening and closing ---------------------------------------------------------------

    private fun toggle() {
        revealAnimator?.cancel()
        shortcuts.forEach { it.animate().cancel() }
        expanded = !expanded
        handle.setImageResource(if (expanded) R.drawable.dock_close else R.drawable.dock_handle)
        handle.rotation = if (expanded) -90f else 90f
        handle.animate().rotation(0f).setDuration(OPEN_MS).setInterpolator(OvershootInterpolator()).start()
        if (expanded) open() else close()
    }

    private fun open() {
        arrange()
        // Buttons pop in one after another, starting next to the handle.
        val ordered = if (handleLast) shortcuts.reversed() else shortcuts
        ordered.forEachIndexed { i, button ->
            button.alpha = 0f
            button.scaleX = 0.3f
            button.scaleY = 0.3f
            button.animate().alpha(1f).scaleX(1f).scaleY(1f)
                .setStartDelay(i * STAGGER_MS)
                .setDuration(BUTTON_MS)
                .setInterpolator(OvershootInterpolator(2.5f))
                .start()
        }
        animateReveal(reveal, 1f, OPEN_MS, SpringInterpolator)
    }

    private fun close() {
        shortcuts.forEach {
            it.animate().alpha(0f).scaleX(0.5f).scaleY(0.5f)
                .setStartDelay(0)
                .setDuration(CLOSE_MS)
                .setInterpolator(AccelerateInterpolator())
                .start()
        }
        animateReveal(reveal, 0f, CLOSE_MS, AccelerateInterpolator()) {
            if (!expanded) arrange() // shrink the layout to the circle once the pill is gone
        }
    }

    private fun animateReveal(from: Float, to: Float, duration: Long, interpolator: TimeInterpolator, onEnd: () -> Unit = {}) {
        revealAnimator = ValueAnimator.ofFloat(from, to).apply {
            this.duration = duration
            this.interpolator = interpolator
            addUpdateListener {
                reveal = it.animatedValue as Float
                invalidate()
            }
            addListener(object : AnimatorListenerAdapter() {
                private var canceled = false

                override fun onAnimationCancel(animation: Animator) {
                    canceled = true
                }

                override fun onAnimationEnd(animation: Animator) {
                    if (!canceled) onEnd()
                }
            })
            start()
        }
    }

    // Expanded, the buttons extend from the handle towards the middle of the screen, in a row if
    // it fits and in a column otherwise, so the handle stays under the finger. The layout takes
    // its full size before the animation starts, so the animation is never cut off.
    private fun arrange() {
        val parentView = parent as? View ?: return
        val rowLength = (shortcuts.size + 1) * buttonSize + 2 * pad + slack
        orientation = if (rowLength <= parentView.width) HORIZONTAL else VERTICAL
        handleLast = if (orientation == HORIZONTAL) {
            anchorX + buttonSize / 2 > parentView.width / 2
        } else {
            anchorY + buttonSize / 2 > parentView.height / 2
        }
        removeAllViews()
        if (!(expanded && handleLast)) addView(handle)
        if (expanded) shortcuts.forEach(::addView)
        if (expanded && handleLast) addView(handle)
        val extra = if (expanded) slack else 0
        when {
            orientation == HORIZONTAL && handleLast -> setPadding(pad + extra, pad, pad, pad)
            orientation == HORIZONTAL -> setPadding(pad, pad, pad + extra, pad)
            handleLast -> setPadding(pad, pad + extra, pad, pad)
            else -> setPadding(pad, pad, pad, pad + extra)
        }
    }

    override fun onDraw(canvas: Canvas) {
        // The pill grows from the circle around the handle to the full row (minus the slack).
        val left = handle.left - pad.toFloat()
        val top = handle.top - pad.toFloat()
        val right = handle.right + pad.toFloat()
        val bottom = handle.bottom + pad.toFloat()
        if (childCount == 1) {
            pill.set(left, top, right, bottom)
        } else {
            val w = width.toFloat()
            val h = height.toFloat()
            val s = slack.toFloat()
            val horizontal = orientation == HORIZONTAL
            val fullLeft = if (horizontal && handleLast) s else 0f
            val fullTop = if (!horizontal && handleLast) s else 0f
            val fullRight = if (horizontal && !handleLast) w - s else w
            val fullBottom = if (!horizontal && !handleLast) h - s else h
            pill.set(
                max(0f, lerp(left, fullLeft, reveal)),
                max(0f, lerp(top, fullTop, reveal)),
                min(w, lerp(right, fullRight, reveal)),
                min(h, lerp(bottom, fullBottom, reveal)),
            )
        }
        val radius = min(pill.width(), pill.height()) / 2
        canvas.drawRoundRect(pill, radius, radius, pillPaint)
    }

    private fun lerp(a: Float, b: Float, t: Float) = a + (b - a) * t

    // --- Dragging and placement ------------------------------------------------------------

    private var downX = 0f
    private var downY = 0f
    private var startX = 0f
    private var startY = 0f
    private var dragging = false

    private fun onHandleTouch(view: View, event: MotionEvent): Boolean {
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                downX = event.rawX
                downY = event.rawY
                startX = anchorX
                startY = anchorY
                dragging = false
                view.isPressed = true
            }
            MotionEvent.ACTION_MOVE -> {
                val dx = event.rawX - downX
                val dy = event.rawY - downY
                if (!dragging && hypot(dx, dy) > touchSlop) {
                    dragging = true
                    view.isPressed = false
                }
                if (dragging) {
                    anchorX = startX + dx
                    anchorY = startY + dy
                    if (applyAnchor()) requestLayout() // re-mark where the window shows the dock
                }
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                view.isPressed = false
                if (dragging) {
                    saveAnchor()
                    if (expanded) arrange() // expand towards the middle from the new spot
                } else if (event.actionMasked == MotionEvent.ACTION_UP) {
                    view.performClick()
                }
            }
        }
        return true
    }

    /** Restores the saved spot after a rotation, then keeps the dock inside the screen. */
    private fun place() {
        val parentView = parent as? View ?: return
        if (parentView.width == 0 || parentView.height == 0) return
        val key = orientationKey(parentView)
        if (key != placedFor) {
            placedFor = key
            anchorX = margin + prefs.getFloat("${key}_x", DEFAULT_X) * (parentView.width - buttonSize - 2 * margin)
            anchorY = margin + prefs.getFloat("${key}_y", DEFAULT_Y) * (parentView.height - buttonSize - 2 * margin)
            if (expanded) arrange()
        }
        // Called during layout: the window's visible region is gathered right after, so no new
        // layout is needed here.
        applyAnchor()
    }

    /** Moves the dock so the handle is at the anchor. Returns whether it moved. */
    private fun applyAnchor(): Boolean {
        val parentView = parent as? View ?: return false
        anchorX = anchorX.coerceIn(margin, max(margin, parentView.width - buttonSize - margin))
        anchorY = anchorY.coerceIn(margin, max(margin, parentView.height - buttonSize - margin))
        val x = (anchorX - handle.left).coerceIn(0f, max(0f, (parentView.width - width).toFloat()))
        val y = (anchorY - handle.top).coerceIn(0f, max(0f, (parentView.height - height).toFloat()))
        if (x == translationX && y == translationY) return false
        translationX = x
        translationY = y
        return true
    }

    private fun saveAnchor() {
        val parentView = parent as? View ?: return
        val key = orientationKey(parentView)
        val spanX = parentView.width - buttonSize - 2 * margin
        val spanY = parentView.height - buttonSize - 2 * margin
        prefs.edit()
            .putFloat("${key}_x", if (spanX > 0) ((anchorX - margin) / spanX).coerceIn(0f, 1f) else DEFAULT_X)
            .putFloat("${key}_y", if (spanY > 0) ((anchorY - margin) / spanY).coerceIn(0f, 1f) else DEFAULT_Y)
            .apply()
    }

    private fun orientationKey(parentView: View) = if (parentView.width >= parentView.height) "land" else "port"

    private fun button(icon: Int, description: Int, onClick: () -> Unit): ImageView =
        ImageView(context).apply {
            layoutParams = LayoutParams(buttonSize, buttonSize)
            scaleType = ImageView.ScaleType.CENTER
            setImageResource(icon)
            contentDescription = context.getString(description)
            background = RippleDrawable(ColorStateList.valueOf(Color.argb(0x40, 0xFF, 0xFF, 0xFF)), null, null)
            setOnClickListener { onClick() }
        }

    private fun dp(value: Int): Int = (value * resources.displayMetrics.density).toInt()

    /** A damped spring: overshoots by about 4 % once, then settles exactly on 1. */
    private object SpringInterpolator : TimeInterpolator {
        private const val DAMPING = 7.0
        private const val FREQUENCY = 2 * PI * 1.1
        private val end = raw(1f)

        private fun raw(t: Float): Double = 1 - exp(-DAMPING * t) * cos(FREQUENCY * t)

        override fun getInterpolation(input: Float): Float = (raw(input) + input * (1 - end)).toFloat()
    }

    companion object {
        // Default spot: right edge, lower part of the screen (fractions of the free space).
        private const val DEFAULT_X = 1f
        private const val DEFAULT_Y = 0.75f
        private const val PLAYBACK_CONFIRM_MS = 1500L
        private const val OPEN_MS = 450L
        private const val CLOSE_MS = 160L
        private const val BUTTON_MS = 320L
        private const val STAGGER_MS = 22L
    }
}
