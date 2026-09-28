package io.github.zengarv.deuxdisplay.stream

import android.app.Activity
import android.media.MediaFormat
import android.os.Build
import android.view.Display
import io.github.zengarv.deuxdisplay.protocol.Protocol
import kotlin.math.abs
import kotlin.math.roundToInt

/** Video codec picked by the user. AUTO lets the host choose (it prefers HEVC when both work). */
enum class StreamCodec { AUTO, H264, HEVC, VP9 }

/** Stream format picked by the user. 0 in a field means the host decides (the default). */
data class StreamMode(
    val width: Int = 0,
    val height: Int = 0,
    val refreshHz: Int = 0,
    val codec: StreamCodec = StreamCodec.AUTO,
) {
    val hasSize: Boolean get() = width > 0 && height > 0

    /**
     * HELLO `codecs` bitmask for this pick. The host only encodes what's advertised, so narrowing
     * the mask is how a codec choice reaches it.
     *
     * [hevcDecoder]: a hardware HEVC decoder exists. [hevcRated]: it's also rated for the stream
     * size. AUTO only offers HEVC within the rating; an explicit HEVC pick is honored beyond it
     * (decoders' ratings are conservative: the Pad Go's H.264 decoder is "unrated" at its own
     * native size yet decodes it fine). Without any HEVC decoder a HEVC pick falls back to H.264.
     */
    fun codecMask(hevcDecoder: Boolean, hevcRated: Boolean): Int = when (codec) {
        StreamCodec.H264 -> Protocol.CODEC_MASK_H264
        StreamCodec.HEVC -> if (hevcDecoder) Protocol.CODEC_MASK_HEVC else Protocol.CODEC_MASK_H264
        StreamCodec.VP9 -> Protocol.CODEC_MASK_VP9 // only offered with a hardware VP9 decoder
        StreamCodec.AUTO ->
            Protocol.CODEC_MASK_H264 or (if (hevcDecoder && hevcRated) Protocol.CODEC_MASK_HEVC else 0)
    }
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

    // Stream-only rates offered in addition to the panel's own (the panel keeps refreshing at its
    // current rate). Lower rates save bandwidth/battery but add latency with slow decoders.
    private val EXTRA_STREAM_RATES = listOf(30)

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
        rates.values.forEach { it.addAll(EXTRA_STREAM_RATES) }
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
     * The panel mode at the current resolution to show a [hz] stream on (see [panelRateFor]), for
     * `WindowManager.LayoutParams.preferredDisplayModeId`; 0 (system default) if there's none.
     */
    fun panelModeIdFor(activity: Activity, hz: Int): Int {
        val modes = modesAtCurrentSize(activity)
        val rate = panelRateFor(modes.map { it.refreshRate }, hz)
        return modes.firstOrNull { it.refreshRate == rate }?.modeId ?: 0
    }

    /**
     * The panel rate (from [rates]) to show a [streamHz] stream on: the stream's own rate, else its
     * smallest whole multiple (every frame then stays up for the same number of refreshes, so
     * motion doesn't judder), else the highest. 0 for no stream rate or no rates.
     */
    fun panelRateFor(rates: List<Float>, streamHz: Int): Float {
        if (streamHz <= 0) return 0f
        rates.firstOrNull { abs(it - streamHz) < 0.5f }?.let { return it }
        val multiples = rates.filter { rate ->
            val n = (rate / streamHz).roundToInt()
            n >= 2 && abs(rate - n * streamHz) < 0.5f
        }
        return multiples.minOrNull() ?: rates.maxOrNull() ?: 0f
    }

    /** The highest rate the panel offers at its current resolution; 0 if unknown. */
    fun highestRefreshHz(activity: Activity): Float = modesAtCurrentSize(activity).maxOfOrNull { it.refreshRate } ?: 0f

    /** The rate the panel is refreshing at right now (it can change at any time). */
    fun currentRefreshHz(activity: Activity): Float = display(activity)?.refreshRate ?: 0f

    /** The refresh rate of panel mode [modeId]; 0 for none (0 = no preference). */
    fun refreshHzOfMode(activity: Activity, modeId: Int): Float =
        if (modeId == 0) 0f else display(activity)?.supportedModes?.firstOrNull { it.modeId == modeId }?.refreshRate ?: 0f

    private fun modesAtCurrentSize(activity: Activity): List<Display.Mode> {
        val display = display(activity) ?: return emptyList()
        val current = display.mode
        return display.supportedModes.filter {
            it.physicalWidth == current.physicalWidth && it.physicalHeight == current.physicalHeight
        }
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
