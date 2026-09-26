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
            val t = timings.remove(ptsUs)
            if (t != null) onFrameTiming?.onFrame(ptsUs, t[0], t[1], nanoTime / 1000)
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
                    inFlightSum += inFlight.getAndDecrement()
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
        val format = baseFormat(width, height)
        // Set even when FEATURE_LowLatency isn't advertised: some vendor decoders (MediaTek C2)
        // honor it anyway, and configure() falls back to a plain format if it's rejected.
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            format.setInteger(MediaFormat.KEY_LOW_LATENCY, 1)
        }
        format.setInteger(MediaFormat.KEY_PRIORITY, 0) // realtime
        if (choice.name.contains("mtk", ignoreCase = true)) {
            format.setInteger("vdec-lowlatency", 1) // MediaTek (OMX-era) low-latency hint
            // OPPO/OnePlus MediaTek decoders run a video post-processor (motion interpolation,
            // quality tuner) that buffers frames. A desktop stream wants none of it.
            format.setInteger("vendor.mtk.ext.vdec.vpp.disabled.value", 1)
        }
        return try {
            codec.configure(format, surface, null, 0)
            choice.lowLatency
        } catch (e: RuntimeException) {
            // Some vendor codecs reject low-latency/priority keys; retry with the plain format.
            Log.w(TAG, "low-latency configure failed, falling back", e)
            codec.reset()
            codec.configure(baseFormat(width, height), surface, null, 0)
            false
        }
    }

    private fun baseFormat(width: Int, height: Int): MediaFormat =
        MediaFormat.createVideoFormat(mime, width, height).apply {
            // Keyframes of a large desktop can exceed the default input buffer size.
            setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, width * height * 3 / 2)
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
