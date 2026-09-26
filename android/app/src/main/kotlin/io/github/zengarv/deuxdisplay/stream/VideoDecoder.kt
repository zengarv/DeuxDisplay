package io.github.zengarv.deuxdisplay.stream

import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaCodecList
import android.media.MediaFormat
import android.os.Build
import android.os.Process
import android.util.Log
import android.view.Surface
import io.github.zengarv.deuxdisplay.protocol.Protocol

/**
 * Hardware H.264 decoder rendering straight to a Surface, configured for latency: frames are
 * released for display as soon as they're decoded, with no timestamp-based pacing.
 *
 * Input comes from the network thread ([submit]); output is drained on a dedicated thread.
 * After any error the decoder drops frames until the next keyframe and asks for one via
 * [onNeedKeyframe].
 */
class VideoDecoder(
    surface: Surface,
    width: Int,
    height: Int,
    private val onNeedKeyframe: () -> Unit,
) {
    private val codec: MediaCodec
    val codecName: String
    val lowLatency: Boolean

    @Volatile
    private var running = true
    private var awaitingKeyframe = true
    private val renderThread: Thread

    init {
        val choice = pickDecoder(MIME)
        codecName = choice.name
        codec = MediaCodec.createByCodecName(choice.name)
        lowLatency = configure(surface, width, height, choice)
        codec.start()
        Log.i(TAG, "decoder $codecName ${width}x$height lowLatency=$lowLatency")
        renderThread = Thread(::renderLoop, "DeuxDisplay-render").apply { start() }
    }

    /** Queues one access unit. Called from the network thread. */
    fun submit(data: ByteArray, length: Int, flags: Int, ptsUs: Long) {
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
            codec.queueInputBuffer(index, 0, length, ptsUs, codecFlags)
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
                    // Render immediately: this is a live desktop, not a paced video.
                    codec.releaseOutputBuffer(index, true)
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

    private fun configure(surface: Surface, width: Int, height: Int, choice: DecoderChoice): Boolean {
        val format = baseFormat(width, height)
        if (choice.lowLatency && Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            format.setInteger(MediaFormat.KEY_LOW_LATENCY, 1)
        }
        format.setInteger(MediaFormat.KEY_PRIORITY, 0) // realtime
        if (choice.name.contains("mtk", ignoreCase = true)) {
            format.setInteger("vdec-lowlatency", 1) // MediaTek vendor low-latency hint
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
        MediaFormat.createVideoFormat(MIME, width, height).apply {
            // Keyframes of a large desktop can exceed the default input buffer size.
            setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, width * height * 3 / 2)
        }

    private data class DecoderChoice(val name: String, val lowLatency: Boolean)

    companion object {
        private const val TAG = "DeuxDisplay"
        const val MIME = MediaFormat.MIMETYPE_VIDEO_AVC
        private const val INPUT_TIMEOUT_US = 50_000L
        private const val OUTPUT_TIMEOUT_US = 100_000L

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
