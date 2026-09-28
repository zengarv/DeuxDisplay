package io.github.zengarv.deuxdisplay.stream

import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaCodecList
import android.media.MediaFormat
import android.os.Build
import android.os.Handler
import android.os.HandlerThread
import android.os.Process
import android.util.Log
import android.view.Surface
import io.github.zengarv.deuxdisplay.protocol.Protocol
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.atomic.AtomicInteger

/**
 * Hardware H.264 decoder rendering straight to a Surface, configured for latency: frames are
 * released for display as soon as they're decoded, with no timestamp-based pacing.
 *
 * Input comes from the network thread ([submit]); output is drained on a dedicated thread.
 * After any error the decoder drops frames until the next keyframe and asks for one via
 * [onNeedKeyframe].
 */
class VideoDecoder(
    surface: Surface?, // null: decode to memory without displaying (latency experiments only)
    private val mime: String,
    width: Int,
    height: Int,
    private val onNeedKeyframe: () -> Unit,
    private val onFrameTiming: FrameTimingListener? = null,
    forcedDecoder: String? = null, // debug: use this MediaCodec component instead of auto-picking
    // Degrees clockwise to rotate frames on screen (CONFIG rotation). Applied by the compositor
    // when it shows the surface, so it costs nothing in the decode path.
    private val rotationDegrees: Int = 0,
    private val stats: StreamStats? = null, // debug overlay counters
    // KEY_OPERATING_RATE hint; decoders size their clocks from it. 0 = don't set.
    private val operatingRate: Int = DEFAULT_OPERATING_RATE,
    private val extraKeys: Map<String, Int> = emptyMap(), // debug: more integer format keys
) {
    /** Per-frame timestamps on the client clock (µs, System.nanoTime based); 0 = unknown. */
    fun interface FrameTimingListener {
        fun onFrame(ptsUs: Long, receivedUs: Long, decodedUs: Long, renderedUs: Long)
    }

    private val codec: MediaCodec
    private val render = surface != null
    val codecName: String
    val lowLatency: Boolean

    @Volatile
    private var running = true
    private var awaitingKeyframe = true
    private val renderThread: Thread
    private val callbackThread = HandlerThread("DeuxDisplay-rendered").apply { start() }

    // pts -> [receivedUs, decodedUs]; frames are removed when rendered (or pruned if never shown).
    private val timings = ConcurrentHashMap<Long, LongArray>()
    private val inFlight = AtomicInteger()
    private var inFlightSum = 0L
    private var outputs = 0L

    init {
        val choice = if (forcedDecoder != null) DecoderChoice(forcedDecoder, false) else pickDecoder(mime)
        codecName = choice.name
        codec = MediaCodec.createByCodecName(choice.name)
        lowLatency = configure(surface, width, height, choice)
        codec.setOnFrameRenderedListener({ _, ptsUs, nanoTime ->
            val t = timings.remove(ptsUs) ?: return@setOnFrameRenderedListener
            // nanoTime should be System.nanoTime-based, but some vendor decoders (MediaTek C2 on
            // the Pad Go) use another clock, hours off. Then use when we heard of it: a bit late.
            val nowUs = System.nanoTime() / 1000
            val reportedUs = nanoTime / 1000
            val renderedUs = if (reportedUs in maxOf(t[0], t[1])..nowUs) reportedUs else nowUs
            onFrameTiming?.onFrame(ptsUs, t[0], t[1], renderedUs)
        }, Handler(callbackThread.looper))
        codec.start()
        Log.i(TAG, "decoder $codecName ($mime) ${width}x$height lowLatency=$lowLatency")
        logCapabilities(width, height)
        renderThread = Thread(::renderLoop, "DeuxDisplay-render").apply { start() }
    }

    /** Queues one access unit. Called from the network thread. */
    fun submit(data: ByteArray, length: Int, flags: Int, ptsUs: Long, receivedUs: Long) {
        val isConfig = flags and Protocol.FLAG_CODEC_CONFIG != 0
        val isKey = flags and Protocol.FLAG_KEYFRAME != 0
        if (awaitingKeyframe && !isConfig && !isKey) {
            stats?.onFrameSkipped()
            return // decoding a P-frame without its reference would only show garbage
        }
        try {
            val index = codec.dequeueInputBuffer(INPUT_TIMEOUT_US)
            if (index < 0) {
                Log.w(TAG, "no input buffer; dropping frame and requesting a keyframe")
                recover()
                return
            }
            val buffer = codec.getInputBuffer(index) ?: return
            if (buffer.capacity() < length) {
                Log.w(TAG, "frame of $length bytes exceeds input buffer ${buffer.capacity()}")
                codec.queueInputBuffer(index, 0, 0, ptsUs, 0)
                recover()
                return
            }
            buffer.clear()
            buffer.put(data, 0, length)
            val codecFlags = when {
                isConfig -> MediaCodec.BUFFER_FLAG_CODEC_CONFIG
                isKey -> MediaCodec.BUFFER_FLAG_KEY_FRAME
                else -> 0
            }
            val isRepeat = flags and Protocol.FLAG_REPEAT != 0
            if (!isConfig && !isRepeat && onFrameTiming != null) {
                if (timings.size > MAX_TRACKED_FRAMES) timings.clear()
                timings[ptsUs] = longArrayOf(receivedUs, 0L)
            }
            codec.queueInputBuffer(index, 0, length, ptsUs, codecFlags)
            if (!isConfig) inFlight.incrementAndGet()
            if (isKey) awaitingKeyframe = false
        } catch (e: MediaCodec.CodecException) {
            Log.w(TAG, "decoder input error", e)
            recover()
        } catch (e: IllegalStateException) {
            if (running) Log.w(TAG, "decoder not ready", e)
        }
    }

    fun release() {
        running = false
        renderThread.join(500)
        try {
            codec.stop()
        } catch (e: IllegalStateException) {
            Log.d(TAG, "stop: ${e.message}")
        }
        codec.release()
        callbackThread.quitSafely()
    }

    private fun recover() {
        awaitingKeyframe = true
        onNeedKeyframe()
    }

    private fun renderLoop() {
        Process.setThreadPriority(Process.THREAD_PRIORITY_URGENT_DISPLAY)
        val info = MediaCodec.BufferInfo()
        while (running) {
            try {
                val index = codec.dequeueOutputBuffer(info, OUTPUT_TIMEOUT_US)
                if (index >= 0) {
                    timings[info.presentationTimeUs]?.set(1, System.nanoTime() / 1000)
                    // Frames still inside the decoder when this one came out (diagnostic).
                    val queued = inFlight.getAndDecrement()
                    inFlightSum += queued
                    stats?.onDecoded(queued)
                    if (++outputs % 120 == 0L) {
                        Log.i(TAG, "avg frames in decoder at output: ${"%.2f".format(inFlightSum / 120.0)}")
                        inFlightSum = 0
                    }
                    // Render immediately: this is a live desktop, not a paced video.
                    codec.releaseOutputBuffer(index, render)
                } else if (index == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                    Log.i(TAG, "output format ${codec.outputFormat}")
                }
            } catch (e: MediaCodec.CodecException) {
                Log.w(TAG, "decoder output error", e)
                recover()
            } catch (e: IllegalStateException) {
                if (running) Log.w(TAG, "decoder output not ready", e)
                return
            }
        }
    }

    private fun configure(surface: Surface?, width: Int, height: Int, choice: DecoderChoice): Boolean {
        // Latency keys beyond the baseline, most important first. Vendor codecs vary: when
        // configure() rejects the format, drop the least important key left and retry, so one
        // unsupported key doesn't cost the others.
        val keys = mutableListOf<Pair<String, Int>>()
        // Set even when FEATURE_LowLatency isn't advertised: some vendor decoders (MediaTek C2)
        // honor it anyway.
        var lowLatencyKey: String? = null
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            lowLatencyKey = MediaFormat.KEY_LOW_LATENCY
            keys += lowLatencyKey to 1
        }
        keys += MediaFormat.KEY_PRIORITY to 0 // realtime
        // Decoders scale their clocks to the operating rate; a desktop stream wants frames out as
        // fast as the hardware can, not paced for its average rate.
        if (operatingRate > 0) keys += MediaFormat.KEY_OPERATING_RATE to operatingRate
        if (choice.name.contains("mtk", ignoreCase = true)) {
            keys += "vdec-lowlatency" to 1 // MediaTek (OMX-era) low-latency hint
            // OPPO/OnePlus MediaTek decoders run a video post-processor (motion interpolation,
            // quality tuner) that buffers frames. A desktop stream wants none of it.
            keys += "vendor.mtk.ext.vdec.vpp.disabled.value" to 1
        }
        keys += extraKeys.toList()
        while (true) {
            val format = baseFormat(width, height).apply { keys.forEach { (key, value) -> setInteger(key, value) } }
            try {
                codec.configure(format, surface, null, 0)
                Log.i(TAG, "decoder configured with ${keys.joinToString { "${it.first}=${it.second}" }}")
                logVendorParameters()
                return choice.lowLatency && keys.any { it.first == lowLatencyKey }
            } catch (e: RuntimeException) {
                if (keys.isEmpty()) throw e
                val dropped = keys.removeAt(keys.lastIndex)
                Log.w(TAG, "configure rejected; retrying without ${dropped.first}", e)
                codec.reset()
            }
        }
    }

    /** Diagnostic: the vendor parameters this decoder takes (Android 12+), to find latency knobs. */
    private fun logVendorParameters() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.S) return
        try {
            val names = codec.supportedVendorParameters
            Log.i(TAG, "$codecName vendor parameters (${names.size}): ${names.sorted().joinToString()}")
        } catch (e: IllegalStateException) {
            Log.d(TAG, "vendor parameters unavailable: ${e.message}")
        }
    }

    private fun baseFormat(width: Int, height: Int): MediaFormat =
        MediaFormat.createVideoFormat(mime, width, height).apply {
            // Keyframes of a large desktop can exceed the default input buffer size.
            setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, width * height * 3 / 2)
            if (rotationDegrees != 0) setInteger(MediaFormat.KEY_ROTATION, rotationDegrees)
        }

    private data class DecoderChoice(val name: String, val lowLatency: Boolean)

    /** Diagnostic: what the hardware decoders claim to handle (sizes, frame rates). */
    private fun logCapabilities(width: Int, height: Int) {
        for (mime in listOf(MediaFormat.MIMETYPE_VIDEO_AVC, MediaFormat.MIMETYPE_VIDEO_HEVC)) {
            val info = MediaCodecList(MediaCodecList.REGULAR_CODECS).codecInfos.firstOrNull { i ->
                !i.isEncoder && isHardware(i) && !i.name.endsWith(".secure") &&
                    i.supportedTypes.any { it.equals(mime, ignoreCase = true) }
            } ?: continue
            val v = info.getCapabilitiesForType(mime).videoCapabilities
            val sizes = listOf(width to height, 1920 to 1080, width / 2 to height / 2).joinToString { (w, h) ->
                val fps = if (v.isSizeSupported(w, h)) v.getSupportedFrameRatesFor(w, h).upper.toInt() else 0
                "${w}x$h: ${if (fps > 0) "$fps fps" else "unsupported"}"
            }
            Log.i(TAG, "${info.name}: widths ${v.supportedWidths} heights ${v.supportedHeights}; $sizes")
        }
    }

    companion object {
        private const val TAG = "DeuxDisplay"

        /** True if a hardware (non-secure) decoder exists for [mime]. */
        fun hasHardwareDecoder(mime: String): Boolean =
            MediaCodecList(MediaCodecList.REGULAR_CODECS).codecInfos.any { info ->
                !info.isEncoder && isHardware(info) && !info.name.endsWith(".secure") &&
                    info.supportedTypes.any { it.equals(mime, ignoreCase = true) }
            }

        /** True if the decoder we'd pick for [mime] accepts a [width]x[height] stream. */
        fun supportsSize(mime: String, width: Int, height: Int): Boolean {
            val name = runCatching { pickDecoder(mime).name }.getOrNull() ?: return false
            val info = MediaCodecList(MediaCodecList.REGULAR_CODECS).codecInfos.firstOrNull { it.name == name }
                ?: return false
            return info.getCapabilitiesForType(mime).videoCapabilities?.isSizeSupported(width, height) == true
        }

        /** "As fast as possible": decoders treat rates beyond their limit as their maximum. */
        const val DEFAULT_OPERATING_RATE = Short.MAX_VALUE.toInt()

        private const val INPUT_TIMEOUT_US = 50_000L
        private const val OUTPUT_TIMEOUT_US = 100_000L
        private const val MAX_TRACKED_FRAMES = 256

        /** Prefers a hardware decoder, then one advertising FEATURE_LowLatency; never "secure". */
        private fun pickDecoder(mime: String): DecoderChoice {
            val candidates = MediaCodecList(MediaCodecList.REGULAR_CODECS).codecInfos.filter { info ->
                !info.isEncoder &&
                    !info.name.endsWith(".secure") &&
                    info.supportedTypes.any { it.equals(mime, ignoreCase = true) }
            }
            val best = candidates.maxByOrNull { info ->
                (if (isHardware(info)) 2 else 0) + (if (supportsLowLatency(info, mime)) 1 else 0)
            } ?: throw IllegalStateException("no $mime decoder")
            return DecoderChoice(best.name, supportsLowLatency(best, mime))
        }

        private fun isHardware(info: MediaCodecInfo): Boolean =
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                info.isHardwareAccelerated
            } else {
                !info.name.startsWith("OMX.google.") && !info.name.startsWith("c2.android.")
            }

        private fun supportsLowLatency(info: MediaCodecInfo, mime: String): Boolean =
            Build.VERSION.SDK_INT >= Build.VERSION_CODES.R &&
                info.getCapabilitiesForType(mime)
                    .isFeatureSupported(MediaCodecInfo.CodecCapabilities.FEATURE_LowLatency)
    }
}
