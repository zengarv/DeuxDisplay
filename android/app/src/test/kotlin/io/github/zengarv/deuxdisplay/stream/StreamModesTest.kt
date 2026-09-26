package io.github.zengarv.deuxdisplay.stream

import io.github.zengarv.deuxdisplay.stream.StreamModes.PanelMode
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class StreamModesTest {

    // OnePlus Pad Go: one portrait-native panel size at 60 and 90 Hz.
    private val padGo = listOf(PanelMode(1720, 2408, 60f), PanelMode(1720, 2408, 90.00001f))

    @Test
    fun nativeSizeIsLandscapeWithAllRates() {
        val options = StreamModes.options(padGo) { _, _ -> true }
        assertEquals(ModeOption(2408, 1720, listOf(90, 60), native = true), options.first())
    }

    @Test
    fun scaledSizesAreEvenDecodableAndKeepNativeRates() {
        val options = StreamModes.options(padGo) { w, _ -> w != 1604 } // decoder rejects the 2/3 size
        assertEquals(
            listOf(2408 to 1720, 1806 to 1290, 1204 to 860),
            options.map { it.width to it.height },
        )
        assertTrue(options.drop(1).all { !it.native && it.refreshRates == listOf(90, 60) })
    }

    @Test
    fun dropsModesOutsideDriverLimits() {
        val options = StreamModes.options(listOf(PanelMode(1280, 800, 60f), PanelMode(1280, 800, 300f))) { _, _ -> true }
        // 300 Hz is out of range; 1/2 of 1280x800 (640x400) is below 640x480.
        assertEquals(
            listOf(
                ModeOption(1280, 800, listOf(60), true),
                ModeOption(960, 600, listOf(60), false),
                ModeOption(852, 532, listOf(60), false),
            ),
            options,
        )
    }

    @Test
    fun noPanelModesMeansNoOptions() {
        assertEquals(emptyList<ModeOption>(), StreamModes.options(emptyList()) { _, _ -> true })
    }
}
