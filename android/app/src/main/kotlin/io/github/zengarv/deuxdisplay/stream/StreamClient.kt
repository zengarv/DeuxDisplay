package io.github.zengarv.deuxdisplay.stream

import android.media.MediaFormat
import android.os.Process
import android.util.Log
import android.view.Surface
import io.github.zengarv.deuxdisplay.protocol.Codec
import io.github.zengarv.deuxdisplay.protocol.Config
import io.github.zengarv.deuxdisplay.protocol.FrameStats
import io.github.zengarv.deuxdisplay.protocol.Header
import io.github.zengarv.deuxdisplay.protocol.Hello
import io.github.zengarv.deuxdisplay.protocol.MessageType
import io.github.zengarv.deuxdisplay.protocol.Protocol
import io.github.zengarv.deuxdisplay.protocol.ProtocolException
import io.github.zengarv.deuxdisplay.protocol.Pong
import io.github.zengarv.deuxdisplay.protocol.parsePing
import io.github.zengarv.deuxdisplay.protocol.serializePing
import java.io.BufferedInputStream
import java.io.DataInputStream
import java.io.IOException
import java.io.OutputStream
import java.net.InetSocketAddress
import java.net.Socket

/**
 * Connects to DeuxDisplayHost through `adb reverse` (127.0.0.1 on the device reaches the PC),
 * sends HELLO, and feeds the video stream into a [VideoDecoder] rendering to [surface].
 * Reconnects automatically until [stop] is called.
 */
class StreamClient(
    private val hello: Hello,
    private val surface: Surface,
    private val listener: Listener,
    private val port: Int = DEFAULT_PORT,
    private val decodeWithoutSurface: Boolean = false, // latency experiment: nothing is displayed
    private val forcedDecoder: String? = null, // debug: MediaCodec component name to use
) {
    fun interface Listener {
        /** A human-readable status, or null once video is streaming. Called on the network thread. */
        fun onStatus(status: String?)
    }

    @Volatile
    private var running = false
    private var thread: Thread? = null

    @Volatile
    private var socket: Socket? = null
    private val sendLock = Any()
    private var output: OutputStream? = null
    private var frameBuffer = ByteArray(1 shl 20)

    fun start() {
        running = true
        thread = Thread(::run, "DeuxDisplay-net").apply { start() }
    }

    fun stop() {
        running = false
        try {
            socket?.close()
        } catch (e: IOException) {
            Log.d(TAG, "close: ${e.message}")
        }
        thread?.join(2000)
        thread = null
    }

    private fun run() {
        Process.setThreadPriority(Process.THREAD_PRIORITY_URGENT_DISPLAY)
        while (running) {
            listener.onStatus("Waiting for DeuxDisplay on your PC…\n(adb reverse tcp:$port tcp:$port)")
            try {
                session()
            } catch (e: IOException) {
                Log.i(TAG, "session ended: ${e.message}")
            } catch (e: ProtocolException) {
                Log.w(TAG, "protocol error: ${e.message}")
            }
            if (running) Thread.sleep(RETRY_DELAY_MS)
        }
    }

    private fun session() {
        Socket().use { s ->
            socket = s
            s.tcpNoDelay = true
            s.receiveBufferSize = 4 shl 20
            s.connect(InetSocketAddress("127.0.0.1", port), CONNECT_TIMEOUT_MS)
            val input = DataInputStream(BufferedInputStream(s.getInputStream(), 1 shl 16))
            output = s.getOutputStream()
            send(MessageType.HELLO, 0, hello.serialize())
            Log.i(TAG, "connected, sent HELLO $hello")

            var decoder: VideoDecoder? = null
            val clock = ClockSync()
            val pinger = startPinger()
            try {
                val headerBytes = ByteArray(Protocol.HEADER_SIZE)
                while (running) {
                    input.readFully(headerBytes)
                    val header = Header.decode(headerBytes)
                    if (header.length > frameBuffer.size) {
                        frameBuffer = ByteArray(Integer.highestOneBit(header.length) shl 1)
                    }
                    input.readFully(frameBuffer, 0, header.length)
                    val receivedUs = nowMicros()

                    when (header.type) {
                        MessageType.VIDEO_FRAME ->
                            decoder?.submit(frameBuffer, header.length, header.flags, header.timestamp, receivedUs)
                        MessageType.CONFIG -> {
                            val config = Config.parse(frameBuffer.copyOf(header.length))
                            Log.i(TAG, "CONFIG $config")
                            decoder?.release()
                            val mime = when (config.codec) {
                                Codec.HEVC -> MediaFormat.MIMETYPE_VIDEO_HEVC
                                Codec.H264 -> MediaFormat.MIMETYPE_VIDEO_AVC
                            }
                            decoder = VideoDecoder(
                                if (decodeWithoutSurface) null else surface,
                                mime,
                                config.widthPx,
                                config.heightPx,
                                onNeedKeyframe = { send(MessageType.REQUEST_KEYFRAME, 0, ByteArray(0)) },
                                onFrameTiming = { pts, received, decoded, rendered ->
                                    if (clock.valid) {
                                        val stats = FrameStats(
                                            pts,
                                            clock.toHost(received),
                                            clock.toHost(decoded),
                                            clock.toHost(rendered),
                                        )
                                        send(MessageType.FRAME_STATS, 0, stats.serialize())
                                    }
                                },
                                forcedDecoder = forcedDecoder,
                            )
                            listener.onStatus(null)
                        }
                        MessageType.PING -> {
                            val id = parsePing(frameBuffer.copyOf(header.length))
                            send(MessageType.PONG, 0, Pong(id, header.timestamp).serialize())
                        }
                        MessageType.PONG -> {
                            val pong = Pong.parse(frameBuffer.copyOf(header.length))
                            clock.onPong(pong.pingTimestamp, receivedUs, header.timestamp)
                        }
                        MessageType.BYE -> {
                            Log.i(TAG, "host said BYE")
                            return
                        }
                        else -> Unit // unknown/unused types are skipped (length already consumed)
                    }
                }
            } finally {
                pinger.interrupt()
                decoder?.release()
                synchronized(sendLock) { output = null }
                socket = null
            }
        }
    }

    /** Pings the host periodically so [ClockSync] can map client timestamps onto the host clock. */
    private fun startPinger(): Thread = Thread({
        var id = 0L
        try {
            while (!Thread.currentThread().isInterrupted) {
                send(MessageType.PING, 0, serializePing(id++))
                Thread.sleep(PING_INTERVAL_MS)
            }
        } catch (e: InterruptedException) {
            Log.d(TAG, "pinger stopped")
        }
    }, "DeuxDisplay-ping").apply { start() }

    private fun send(type: Int, flags: Int, payload: ByteArray) {
        val header = Header(type, flags, payload.size, nowMicros())
        val message = ByteArray(Protocol.HEADER_SIZE + payload.size)
        System.arraycopy(header.encode(), 0, message, 0, Protocol.HEADER_SIZE)
        System.arraycopy(payload, 0, message, Protocol.HEADER_SIZE, payload.size)
        synchronized(sendLock) {
            try {
                output?.write(message) // header + payload in one write (docs/wire-protocol.md)
            } catch (e: IOException) {
                Log.d(TAG, "send failed: ${e.message}")
            }
        }
    }

    private fun nowMicros(): Long = System.nanoTime() / 1000

    companion object {
        private const val TAG = "DeuxDisplay"
        const val DEFAULT_PORT = 27183
        private const val CONNECT_TIMEOUT_MS = 1000
        private const val RETRY_DELAY_MS = 1000L
        private const val PING_INTERVAL_MS = 500L
    }
}
