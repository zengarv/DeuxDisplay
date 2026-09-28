package io.github.zengarv.deuxdisplay

import android.annotation.SuppressLint
import android.app.Activity
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.os.Build
import android.os.PowerManager
import android.widget.TextView
import io.github.zengarv.deuxdisplay.protocol.Codec
import io.github.zengarv.deuxdisplay.stream.StreamModes
import io.github.zengarv.deuxdisplay.stream.StreamStats
import java.util.Locale
import kotlin.math.abs
import kotlin.math.roundToInt

/**
 * Live stream stats in a small translucent box, from what the client already measures (no extra
 * messages). Refreshes twice a second while shown; [StreamStats] only records while it is.
 */
@SuppressLint("ViewConstructor")
class StatsOverlay(private val activity: Activity, private val stats: StreamStats) : TextView(activity) {

    private var previous: StreamStats.Snapshot? = null
    private val refresh = object : Runnable {
        override fun run() {
            update()
            postDelayed(this, REFRESH_MS)
        }
    }

    init {
        val density = resources.displayMetrics.density
        setTextColor(Color.WHITE)
        textSize = 11f
        typeface = Typeface.MONOSPACE
        setPadding((10 * density).toInt(), (6 * density).toInt(), (10 * density).toInt(), (6 * density).toInt())
        background = GradientDrawable().apply {
            setColor(Color.argb(0x99, 0x30, 0x30, 0x30))
            cornerRadius = 8 * density
        }
        visibility = GONE
        importantForAccessibility = IMPORTANT_FOR_ACCESSIBILITY_NO
    }

    /** Shows or hides the box; counting starts and stops with it. */
    fun setShown(shown: Boolean) {
        if (shown == (visibility == VISIBLE)) return
        stats.enabled = shown
        removeCallbacks(refresh)
        previous = null
        visibility = if (shown) VISIBLE else GONE
        if (shown) post(refresh)
    }

    override fun onDetachedFromWindow() {
        removeCallbacks(refresh)
        super.onDetachedFromWindow()
    }

    private fun update() {
        val now = stats.snapshot()
        val since = previous
        previous = now
        text = if (since == null) "Measuring…" else format(StreamStats.Interval(since, now), now)
    }

    private fun format(i: StreamStats.Interval, now: StreamStats.Snapshot): String {
        val lines = mutableListOf<String>()
        val warnings = mutableListOf<String>()
        val config = stats.config

        val upSeconds = ((now.timeMs - stats.sessionStartMs) / 1000).coerceAtLeast(0)
        lines += "${stats.transport} · up ${upSeconds / 60}:${String.format(Locale.US, "%02d", upSeconds % 60)} · session ${stats.sessions}"

        val streamHz = config?.let { (it.fpsMilliHz / 1000.0).roundToInt() } ?: 0
        if (config != null) {
            val codec = if (config.codec == Codec.HEVC) "HEVC" else "H.264"
            val rotation = if (config.rotationDegrees != 0) " · rot ${config.rotationDegrees}°" else ""
            lines += "$codec ${config.widthPx}×${config.heightPx} @ $streamHz fps$rotation"
            // The live encoder target: the adaptive controller moves it (ENCODER_STATE).
            val encoder = stats.encoder
            val target = fmt((encoder?.bitrateKbps ?: config.bitrateKbps) / 1000.0, 1)
            lines += if (encoder != null) {
                "Encoder $target Mbps ${if (encoder.adaptive) "(adaptive)" else "(fixed)"} · quality ${encoder.quality}"
            } else {
                "Encoder $target Mbps"
            }
        }

        // The panel's live rate, and the one we asked for: an OEM override shows up as a mismatch.
        val panelHz = StreamModes.currentRefreshHz(activity)
        val askedHz = StreamModes.refreshHzOfMode(activity, activity.window.attributes.preferredDisplayModeId)
        val asked = if (askedHz > 0f) "asked ${hz(askedHz)}" else "system picks"
        lines += "Panel ${hz(panelHz)} Hz ($asked, max ${hz(StreamModes.highestRefreshHz(activity))})"
        if (streamHz > 0 && panelHz > 0f && !isMultiple(panelHz, streamHz)) {
            warnings += "panel ${hz(panelHz)} Hz doesn't fit $streamHz fps: uneven motion"
        }
        if (askedHz > 0f && panelHz > 0f && abs(panelHz - askedHz) >= 0.5f) {
            warnings += "system overrides the requested ${hz(askedHz)} Hz"
        }

        lines += "Recv ${fmt(i.receivedFps, 1)} fps · shown ${fmt(i.shownFps, 1)} · " +
            "${fmt(i.mbps, 1)} Mbps · ${fmt(i.avgFrameKb, 0)} KB/frame"
        if (i.receivedFps >= 10 && i.shownFps < i.receivedFps * 0.9) {
            warnings += "frames arrive but don't all reach the screen"
        }

        lines += if (i.totalMs != null) {
            "Latency ${ms(i.totalMs)} ms (worst ${i.maxTotalMs?.let(::ms) ?: "–"}): " +
                "pc+link ${ms(i.networkMs)} · decode ${ms(i.decodeMs)} · display ${ms(i.displayMs)}"
        } else if (i.decodeMs != null) {
            "Latency: decode ${ms(i.decodeMs)} · display ${ms(i.displayMs)} ms (clock not synced)"
        } else {
            if (i.shownFps > 0) "Latency: – (no usable timestamps)" else "Latency: – (no frames; the PC only sends when the desktop changes)"
        }

        val ping = if (now.lastRttUs > 0) "${fmt(now.lastRttUs / 1000.0, 1)} ms (best ${fmt(now.bestRttUs / 1000.0, 1)})" else "–"
        val queue = i.decoderQueue?.let { fmt(it, 1) } ?: "–"
        lines += "Ping $ping · decoder queue $queue"
        if (stats.transport.startsWith("Wi-Fi") && now.bestRttUs > WIFI_SLOW_RTT_US) warnings += "slow Wi-Fi link"
        if ((i.decoderQueue ?: 0.0) > DECODER_BACKLOG) {
            warnings += "decoder backlog: try a lower frame rate or resolution"
        }

        if (stats.decoderName.isNotEmpty()) {
            lines += "Decoder ${stats.decoderName}${if (stats.decoderLowLatency) " · low-latency" else " · low-latency not advertised"}"
        }
        lines += "Keyframes ${now.keyframes} · requested ${now.keyframeRequests} · skipped ${now.framesSkipped}"
        if (now.keyframeRequests > 0) warnings += "decoder recovered ${now.keyframeRequests}× (errors or overflow)"

        thermalStatus()?.let { (name, severe) ->
            lines += "Thermal $name"
            if (severe) warnings += "device is hot: expect throttling"
        }

        return (lines + warnings.map { "⚠ $it" }).joinToString("\n")
    }

    /** Thermal status name and whether it's throttling territory (Android 10+). */
    private fun thermalStatus(): Pair<String, Boolean>? {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) return null
        val power = activity.getSystemService(PowerManager::class.java) ?: return null
        val status = power.currentThermalStatus
        val name = when (status) {
            PowerManager.THERMAL_STATUS_NONE -> "normal"
            PowerManager.THERMAL_STATUS_LIGHT -> "light"
            PowerManager.THERMAL_STATUS_MODERATE -> "moderate"
            PowerManager.THERMAL_STATUS_SEVERE -> "severe"
            PowerManager.THERMAL_STATUS_CRITICAL -> "critical"
            PowerManager.THERMAL_STATUS_EMERGENCY -> "emergency"
            PowerManager.THERMAL_STATUS_SHUTDOWN -> "shutdown"
            else -> "unknown"
        }
        return name to (status >= PowerManager.THERMAL_STATUS_MODERATE)
    }

    private fun isMultiple(panelHz: Float, streamHz: Int): Boolean {
        val n = (panelHz / streamHz).roundToInt()
        return n >= 1 && abs(panelHz - n * streamHz) < 0.5f
    }

    private fun hz(value: Float): String = value.roundToInt().toString()

    private fun ms(value: Double?): String = value?.let { fmt(it, 1) } ?: "–"

    private fun fmt(value: Double, decimals: Int): String = String.format(Locale.US, "%.${decimals}f", value)

    companion object {
        private const val REFRESH_MS = 500L
        private const val DECODER_BACKLOG = 3.0 // frames inside the decoder on average
        private const val WIFI_SLOW_RTT_US = 10_000L
    }
}
