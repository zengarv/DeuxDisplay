package io.github.zengarv.deuxdisplay.stream

import android.app.Activity
import android.media.MediaFormat
import android.os.Build
import android.view.Display
import kotlin.math.abs
import kotlin.math.roundToInt

/** Stream format picked by the user. 0 in a field means the host decides (the default). */
data class StreamMode(val width: Int = 0, val height: Int = 0, val refreshHz: Int = 0) {
    val hasSize: Boolean get() = width > 0 && height > 0
}

/** A landscape stream size the device can show, with its refresh rates (Hz, highest first). */
data class ModeOption(val width: Int, val height: Int, val refreshRates: List<Int>, val native: Boolean)

/** Works out which stream formats this device supports (panel modes + decoder limits). */
object StreamModes {
    /** One entry of `Display.getSupportedModes()`, in the panel's natural orientation. */
    data class PanelMode(val width: Int, val height: Int, val refreshHz: Float)

    // Scaled-down panel sizes offered for weaker decoders or less bandwidth.
    private val SCALES = listOf(3 to 4, 2 to 3, 1 to 2)

    // Driver limits (driver/DeuxDisplayIdd/Public.h, IsValidPlugRequest).
    private const val MIN_WIDTH = 640
    private const val MIN_HEIGHT = 480
    private const val MAX_WIDTH = 7680
    private const val MAX_HEIGHT = 4320
    private val RATE_RANGE = 24..240

    fun options(activity: Activity): List<ModeOption> =
        options(panelModes(activity)) { w, h -> VideoDecoder.supportsSize(MediaFormat.MIMETYPE_VIDEO_AVC, w, h) }

    /**
     * Every panel size (the device shows these already), plus scaled-down copies of the largest
     * one that [decodes] accepts. Scaled sizes keep the largest size's refresh rates. Sizes are
     * landscape and even; the list is ordered largest first.
     */
    fun options(panel: List<PanelMode>, decodes: (width: Int, height: Int) -> Boolean): List<ModeOption> {
        val rates = LinkedHashMap<Pair<Int, Int>, MutableSet<Int>>()
        for (mode in panel) {
            val size = landscapeEven(mode.width, mode.height)
            val hz = mode.refreshHz.roundToInt()
            if (fits(size) && hz in RATE_RANGE) rates.getOrPut(size) { mutableSetOf() }.add(hz)
        }
        val native = rates.map { (size, hz) -> ModeOption(size.first, size.second, hz.sortedDescending(), true) }
        val largest = native.maxByOrNull { it.width * it.height } ?: return emptyList()
        val scaled = SCALES
            .map { (num, den) -> landscapeEven(largest.width * num / den, largest.height * num / den) }
            .distinct()
            .filter { size ->
                fits(size) && native.none { it.width == size.first && it.height == size.second } &&
                    decodes(size.first, size.second)
            }
            .map { (w, h) -> ModeOption(w, h, largest.refreshRates, native = false) }
        return (native + scaled).sortedByDescending { it.width * it.height }
    }

    /** Panel modes as reported by the display. */
    fun panelModes(activity: Activity): List<PanelMode> =
        display(activity)?.supportedModes?.map { PanelMode(it.physicalWidth, it.physicalHeight, it.refreshRate) }
            .orEmpty()

    /**
     * The panel mode at the current resolution refreshing at [hz], for
     * `WindowManager.LayoutParams.preferredDisplayModeId`; 0 (system default) if there's none.
     */
    fun panelModeIdFor(activity: Activity, hz: Int): Int {
        if (hz == 0) return 0
        val display = display(activity) ?: return 0
        val current = display.mode
        return display.supportedModes.firstOrNull {
            it.physicalWidth == current.physicalWidth && it.physicalHeight == current.physicalHeight &&
                abs(it.refreshRate - hz) < 0.5f
        }?.modeId ?: 0
    }

    private fun landscapeEven(width: Int, height: Int): Pair<Int, Int> =
        (maxOf(width, height) and 1.inv()) to (minOf(width, height) and 1.inv())

    private fun fits(size: Pair<Int, Int>): Boolean =
        size.first in MIN_WIDTH..MAX_WIDTH && size.second in MIN_HEIGHT..MAX_HEIGHT

    private fun display(activity: Activity): Display? {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) return activity.display
        @Suppress("DEPRECATION")
        val display = activity.windowManager.defaultDisplay
        return display
    }
}
