package io.github.zengarv.deuxdisplay.stream

import android.view.MotionEvent
import io.github.zengarv.deuxdisplay.protocol.Protocol
import io.github.zengarv.deuxdisplay.protocol.TouchAction
import io.github.zengarv.deuxdisplay.protocol.TouchContact
import kotlin.math.roundToInt

/** Converts Android touch events into INPUT touch frames (docs/wire-protocol.md). */
object TouchInput {

    /**
     * Every contact in [event], normalized to a [width]x[height] view that shows the whole video
     * frame. Empty for events that aren't touches (the host lifts nothing it didn't see go down).
     */
    fun contacts(event: MotionEvent, width: Int, height: Int): List<TouchContact> {
        val masked = event.actionMasked
        val contacts = ArrayList<TouchContact>(event.pointerCount)
        for (i in 0 until event.pointerCount) {
            val id = event.getPointerId(i)
            if (id >= Protocol.MAX_TOUCH_CONTACTS) continue // more fingers than the host tracks
            val action = actionFor(masked, i == event.actionIndex) ?: return emptyList()
            contacts += TouchContact(
                id,
                action,
                normalize(event.getX(i), width),
                normalize(event.getY(i), height),
                pressure(event.getPressure(i)),
            )
        }
        return contacts
    }

    /** Wire action for one pointer of an event; null for events that aren't touch contact. */
    fun actionFor(actionMasked: Int, isActionPointer: Boolean): Int? = when (actionMasked) {
        MotionEvent.ACTION_DOWN -> TouchAction.DOWN
        MotionEvent.ACTION_POINTER_DOWN -> if (isActionPointer) TouchAction.DOWN else TouchAction.MOVE
        MotionEvent.ACTION_MOVE -> TouchAction.MOVE
        MotionEvent.ACTION_UP -> TouchAction.UP
        MotionEvent.ACTION_POINTER_UP -> if (isActionPointer) TouchAction.UP else TouchAction.MOVE
        MotionEvent.ACTION_CANCEL -> TouchAction.CANCEL
        else -> null
    }

    /** View coordinate -> 0..65535 across [size] pixels. */
    fun normalize(value: Float, size: Int): Int {
        if (size <= 1) return 0
        return (value / (size - 1) * 65535f).roundToInt().coerceIn(0, 65535)
    }

    /** Android pressure (nominally 0..1) -> 1..1024; the wire reserves 0 for "unknown". */
    fun pressure(value: Float): Int = (value * 1024f).roundToInt().coerceIn(1, 1024)
}
