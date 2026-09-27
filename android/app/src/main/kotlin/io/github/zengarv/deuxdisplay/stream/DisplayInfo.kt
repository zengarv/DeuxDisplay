package io.github.zengarv.deuxdisplay.stream

import android.app.Activity
import android.media.MediaFormat
import android.os.Build
import android.util.DisplayMetrics
import io.github.zengarv.deuxdisplay.protocol.Hello
import kotlin.math.roundToInt

/** Describes this device's panel for the HELLO message (landscape, as the app is locked to it). */
object DisplayInfo {

    /** [mode] is the user's stream format pick, sent so the host streams exactly that. */
    fun hello(activity: Activity, mode: StreamMode = StreamMode()): Hello {
        val metrics = activity.resources.displayMetrics
        var width: Int
        var height: Int
        val refreshHz: Float
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

        var xdpi = metrics.xdpi
        var ydpi = metrics.ydpi
        if (width < height) {
            width = height.also { height = width }
            xdpi = ydpi.also { ydpi = xdpi }
        }

        // Whether HEVC is rated for the picked size (native if none picked); see StreamMode.codecMask.
        val hevcDecoder = VideoDecoder.hasHardwareDecoder(MediaFormat.MIMETYPE_VIDEO_HEVC)
        val hevcRated = hevcDecoder &&
            (!mode.hasSize || VideoDecoder.supportsSize(MediaFormat.MIMETYPE_VIDEO_HEVC, mode.width, mode.height))

        return Hello(
            widthPx = width,
            heightPx = height,
            densityDpi = metrics.densityDpi,
            refreshMilliHz = (refreshHz * 1000).roundToInt(),
            codecs = mode.codecMask(hevcDecoder, hevcRated),
            deviceName = "${Build.MANUFACTURER} ${Build.MODEL}",
            xdpiMilli = (xdpi * 1000).roundToInt(),
            ydpiMilli = (ydpi * 1000).roundToInt(),
            modeWidthPx = if (mode.hasSize) mode.width else 0,
            modeHeightPx = if (mode.hasSize) mode.height else 0,
            modeRefreshMilliHz = mode.refreshHz * 1000,
        )
    }
}
