package io.github.zengarv.deuxdisplay.stream

import android.view.MotionEvent
import io.github.zengarv.deuxdisplay.protocol.TouchAction
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class TouchInputTest {

    @Test
    fun actionPointerGetsDownOrUpOthersMove() {
        assertEquals(TouchAction.DOWN, TouchInput.actionFor(MotionEvent.ACTION_DOWN, true))
        assertEquals(TouchAction.DOWN, TouchInput.actionFor(MotionEvent.ACTION_POINTER_DOWN, true))
        assertEquals(TouchAction.MOVE, TouchInput.actionFor(MotionEvent.ACTION_POINTER_DOWN, false))
        assertEquals(TouchAction.UP, TouchInput.actionFor(MotionEvent.ACTION_POINTER_UP, true))
        assertEquals(TouchAction.MOVE, TouchInput.actionFor(MotionEvent.ACTION_POINTER_UP, false))
        assertEquals(TouchAction.UP, TouchInput.actionFor(MotionEvent.ACTION_UP, true))
        assertEquals(TouchAction.CANCEL, TouchInput.actionFor(MotionEvent.ACTION_CANCEL, false))
        assertNull(TouchInput.actionFor(MotionEvent.ACTION_OUTSIDE, true))
    }

    @Test
    fun normalizesEdgesToFullRange() {
        assertEquals(0, TouchInput.normalize(0f, 2408))
        assertEquals(65535, TouchInput.normalize(2407f, 2408))
        assertEquals(32768, TouchInput.normalize(1203.5f, 2408))
        assertEquals(65535, TouchInput.normalize(5000f, 2408)) // clamped
        assertEquals(0, TouchInput.normalize(-3f, 2408))
    }

    @Test
    fun pressureNeverReportsUnknown() {
        assertEquals(1, TouchInput.pressure(0f))
        assertEquals(1024, TouchInput.pressure(1f))
        assertEquals(1024, TouchInput.pressure(1.7f))
    }
}
