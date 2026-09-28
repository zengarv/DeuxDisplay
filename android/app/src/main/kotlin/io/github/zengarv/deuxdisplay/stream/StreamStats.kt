package io.github.zengarv.deuxdisplay.stream

import io.github.zengarv.deuxdisplay.protocol.Config
import io.github.zengarv.deuxdisplay.protocol.Protocol
import java.util.concurrent.atomic.AtomicLong

/**
 * Counters for the debug stats overlay, from what the client already sees (no extra messages).
 *
 * Per-frame counters are only touched while [enabled], so with the overlay off the frame path
 * pays one volatile read. Each counter has a single writer thread (network, decoder output or
 * render callback); the UI reads a [snapshot] a couple of times a second and works out rates
 * from the difference between two snapshots.
 */
class StreamStats {
    @Volatile
    var enabled = false

    // Session facts: rare, recorded whether or not the overlay is on.
    @Volatile
    var transport = ""
        private set

    @Volatile
    var sessions = 0
        private set

    @Volatile
    var sessionStartMs = 0L
        private set

    @Volatile
    var config: Config? = null
        private set

    @Volatile
    var decoderName = ""
        private set

    @Volatile
    var decoderLowLatency = false
        private set

    // Network thread.
    @Volatile private var framesReceived = 0L // real frames: not REPEAT, not CODEC_CONFIG
    @Volatile private var bytesReceived = 0L
    @Volatile private var keyframes = 0L
    @Volatile private var lastRttUs = 0L
    @Volatile private var bestRttUs = 0L

    // Network and decoder output threads.
    private val framesSkipped = AtomicLong() // dropped while waiting for a keyframe
    private val keyframeRequests = AtomicLong()

    // Decoder output thread.
    @Volatile private var framesDecoded = 0L
    @Volatile private var queuedAtOutput = 0L // sum of frames still in the decoder at each output

    // Render callback thread. Latency stages are in µs, summed over frames that have them.
    @Volatile private var framesShown = 0L
    @Volatile private var networkSumUs = 0L // capture (host) -> received; needs clock sync
    @Volatile private var networkCount = 0L
    @Volatile private var decodeSumUs = 0L // received -> decoder output
    @Volatile private var displaySumUs = 0L // decoder output -> on the panel
    @Volatile private var localCount = 0L
    private val maxTotalUs = AtomicLong() // worst capture -> shown since the last snapshot

    fun onSession(transport: String) {
        this.transport = transport
        sessions++
        sessionStartMs = System.currentTimeMillis()
        lastRttUs = 0
        bestRttUs = 0
    }

    fun onConfig(config: Config, decoderName: String, lowLatency: Boolean) {
        this.config = config
        this.decoderName = decoderName
        decoderLowLatency = lowLatency
    }

    fun onVideoFrame(length: Int, flags: Int) {
        if (!enabled) return
        bytesReceived += length + Protocol.HEADER_SIZE
        if (flags and (Protocol.FLAG_REPEAT or Protocol.FLAG_CODEC_CONFIG) == 0) framesReceived++
        if (flags and Protocol.FLAG_KEYFRAME != 0) keyframes++
    }

    fun onPong(rttUs: Long) {
        if (rttUs < 0) return
        lastRttUs = rttUs
        if (bestRttUs == 0L || rttUs < bestRttUs) bestRttUs = rttUs
    }

    fun onFrameSkipped() {
        if (enabled) framesSkipped.incrementAndGet()
    }

    fun onKeyframeRequested() {
        keyframeRequests.incrementAndGet()
    }

    fun onDecoded(stillQueued: Int) {
        if (!enabled) return
        framesDecoded++
        queuedAtOutput += stillQueued
    }

    /**
     * A real frame reached the panel. Client times are µs on the client clock; [receivedHostUs]
     * is the receive time on the host clock (0 until the clock is synced).
     */
    fun onFrameShown(captureUs: Long, receivedHostUs: Long, receivedUs: Long, decodedUs: Long, renderedUs: Long) {
        if (!enabled) return
        framesShown++
        val network = if (receivedHostUs != 0L) receivedHostUs - captureUs else -1L
        if (network >= 0) {
            networkSumUs += network
            networkCount++
        }
        if (decodedUs != 0L && decodedUs >= receivedUs && renderedUs >= decodedUs) {
            decodeSumUs += decodedUs - receivedUs
            displaySumUs += renderedUs - decodedUs
            localCount++
            if (network >= 0) {
                val total = network + renderedUs - receivedUs
                if (total > maxTotalUs.get()) maxTotalUs.set(total)
            }
        }
    }

    /** Current counters. Resets the worst-latency tracker, so take one per overlay refresh. */
    fun snapshot(): Snapshot = Snapshot(
        timeMs = System.currentTimeMillis(),
        framesReceived = framesReceived,
        bytesReceived = bytesReceived,
        keyframes = keyframes,
        framesSkipped = framesSkipped.get(),
        keyframeRequests = keyframeRequests.get(),
        framesDecoded = framesDecoded,
        queuedAtOutput = queuedAtOutput,
        framesShown = framesShown,
        networkSumUs = networkSumUs,
        networkCount = networkCount,
        decodeSumUs = decodeSumUs,
        displaySumUs = displaySumUs,
        localCount = localCount,
        maxTotalUs = maxTotalUs.getAndSet(0),
        lastRttUs = lastRttUs,
        bestRttUs = bestRttUs,
    )

    data class Snapshot(
        val timeMs: Long,
        val framesReceived: Long,
        val bytesReceived: Long,
        val keyframes: Long,
        val framesSkipped: Long,
        val keyframeRequests: Long,
        val framesDecoded: Long,
        val queuedAtOutput: Long,
        val framesShown: Long,
        val networkSumUs: Long,
        val networkCount: Long,
        val decodeSumUs: Long,
        val displaySumUs: Long,
        val localCount: Long,
        val maxTotalUs: Long,
        val lastRttUs: Long,
        val bestRttUs: Long,
    )

    /** Rates and averages between two snapshots ([since] is the older one). */
    class Interval(since: Snapshot, now: Snapshot) {
        private val seconds = (now.timeMs - since.timeMs).coerceAtLeast(1) / 1000.0
        val receivedFps = (now.framesReceived - since.framesReceived) / seconds
        val shownFps = (now.framesShown - since.framesShown) / seconds
        val mbps = (now.bytesReceived - since.bytesReceived) * 8 / seconds / 1e6
        val avgFrameKb: Double = (now.framesReceived - since.framesReceived).let { frames ->
            if (frames > 0) (now.bytesReceived - since.bytesReceived) / 1024.0 / frames else 0.0
        }

        /** Average frames inside the decoder (counting itself) when one came out; growing = backlog. */
        val decoderQueue: Double? = (now.framesDecoded - since.framesDecoded).let { decoded ->
            if (decoded > 0) (now.queuedAtOutput - since.queuedAtOutput).toDouble() / decoded else null
        }

        private val networkFrames = now.networkCount - since.networkCount
        private val localFrames = now.localCount - since.localCount

        /** Capture on the PC -> received here: encode + transfer (+ clock-sync error). */
        val networkMs: Double? = if (networkFrames > 0) (now.networkSumUs - since.networkSumUs) / 1000.0 / networkFrames else null
        val decodeMs: Double? = if (localFrames > 0) (now.decodeSumUs - since.decodeSumUs) / 1000.0 / localFrames else null
        val displayMs: Double? = if (localFrames > 0) (now.displaySumUs - since.displaySumUs) / 1000.0 / localFrames else null
        val totalMs: Double? = if (networkMs != null && decodeMs != null && displayMs != null) networkMs + decodeMs + displayMs else null
        val maxTotalMs: Double? = if (now.maxTotalUs > 0) now.maxTotalUs / 1000.0 else null
    }
}
