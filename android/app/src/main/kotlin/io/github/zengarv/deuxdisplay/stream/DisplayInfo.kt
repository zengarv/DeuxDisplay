package io.github.zengarv.deuxdisplay.stream

import android.app.Activity
import android.media.MediaFormat
import android.os.Build
import android.util.DisplayMetrics
import io.github.zengarv.deuxdisplay.protocol.Hello
import kotlin.math.roundToInt

/** Describes this device's panel for the HELLO message (landscape, as the app is locked to it). */
object DisplayInfo {

    /**
     * [mode] is the user's stream format pick, sent so the host streams exactly that. [options]
     * are all the formats this device can show; the virtual monitor offers each of them.
     */
    fun hello(activity: Activity, mode: StreamMode = StreamMode(), options: List<ModeOption> = emptyList()): Hello {
        val metrics = activity.resources.displayMetrics
        var width: Int
        var height: Int
        var refreshHz: Float
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            val bounds = activity.windowManager.maximumWindowMetrics.bounds
            width = bounds.width()
            height = bounds.height()
            refreshHz = activity.display?.refreshRate ?: 60f
        } else {
            @Suppress("DEPRECATION")
            val display = activity.windowManager.defaultDisplay
            val real = DisplayMetrics()
            @Suppress("DEPRECATION")
            display.getRealMetrics(real)
            width = real.widthPixels
            height = real.heightPixels
            @Suppress("DEPRECATION")
            refreshHz = display.refreshRate
        }

        // Report the panel's best rate, not the current one: adaptive-refresh panels idle at a
        // lower rate, and whatever we report is the fastest mode the virtual monitor offers.
        refreshHz = maxOf(refreshHz, StreamModes.highestRefreshHz(activity))

        var xdpi = metrics.xdpi
        var ydpi = metrics.ydpi
        if (width < height) {
            width = height.also { height = width }
            xdpi = ydpi.also { ydpi = xdpi }
        }

        // Auto means the panel's size, unless the decoder can't take it (options leaves it out):
        // then the largest size it can.
        val nativeListed = options.any { it.native && it.width == width and 1.inv() && it.height == height and 1.inv() }
        val picked = if (mode.hasSize || nativeListed || options.isEmpty()) {
            mode
        } else {
            mode.copy(width = options.first().width, height = options.first().height)
        }

        // Whether HEVC is rated for the picked size (native if none picked); see StreamMode.codecMask.
        val hevcDecoder = VideoDecoder.hasHardwareDecoder(MediaFormat.MIMETYPE_VIDEO_HEVC)
        val hevcRated = hevcDecoder &&
            (!picked.hasSize || VideoDecoder.supportsSize(MediaFormat.MIMETYPE_VIDEO_HEVC, picked.width, picked.height))

        return Hello(
            widthPx = width,
            heightPx = height,
            densityDpi = metrics.densityDpi,
            refreshMilliHz = (refreshHz * 1000).roundToInt(),
            codecs = picked.codecMask(hevcDecoder, hevcRated),
            deviceName = "${Build.MANUFACTURER} ${Build.MODEL}",
            xdpiMilli = (xdpi * 1000).roundToInt(),
            ydpiMilli = (ydpi * 1000).roundToInt(),
            modeWidthPx = if (picked.hasSize) picked.width else 0,
            modeHeightPx = if (picked.hasSize) picked.height else 0,
            modeRefreshMilliHz = picked.refreshHz * 1000,
            sizes = options.map { it.width to it.height },
            rates = options.flatMap { it.refreshRates }.distinct().sortedDescending(),
        )
    }
}
