package io.github.zengarv.deuxdisplay.stream

import android.media.MediaFormat
import android.os.Handler
import android.os.HandlerThread
import android.os.Process
import android.util.Log
import android.view.Surface
import io.github.zengarv.deuxdisplay.protocol.AuthResponse
import io.github.zengarv.deuxdisplay.protocol.Codec
import io.github.zengarv.deuxdisplay.protocol.Config
import io.github.zengarv.deuxdisplay.protocol.FrameStats
import io.github.zengarv.deuxdisplay.protocol.Header
import io.github.zengarv.deuxdisplay.protocol.Hello
import io.github.zengarv.deuxdisplay.protocol.MessageType
import io.github.zengarv.deuxdisplay.protocol.Pairing
import io.github.zengarv.deuxdisplay.protocol.PairingInfo
import io.github.zengarv.deuxdisplay.protocol.Protocol
import io.github.zengarv.deuxdisplay.protocol.ProtocolException
import io.github.zengarv.deuxdisplay.protocol.Pong
import io.github.zengarv.deuxdisplay.protocol.TouchContact
import io.github.zengarv.deuxdisplay.protocol.parseFixed
import io.github.zengarv.deuxdisplay.protocol.parsePing
import io.github.zengarv.deuxdisplay.protocol.serializePing
import io.github.zengarv.deuxdisplay.protocol.serializeTouchFrame
import java.io.BufferedInputStream
import java.io.DataInputStream
import java.io.IOException
import java.io.OutputStream
import java.net.InetSocketAddress
import java.net.Socket

/**
 * Connects to DeuxDisplayHost, sends HELLO, and feeds the video stream into a [VideoDecoder]
 * rendering to [surface]. Reconnects automatically until [stop] is called.
 *
 * Over USB it connects through `adb reverse` (127.0.0.1 on the device reaches the PC). With a
 * [wifi] link it joins the PC's own Wi-Fi network instead and authenticates with [authKey]
 * (docs/wire-protocol.md). The client owns [wifi] and closes it in [stop].
 */
class StreamClient(
    private val hello: Hello,
    private val surface: Surface,
    private val listener: Listener,
    private val port: Int = DEFAULT_PORT,
    private val wifi: WifiLink? = null,
    private val authKey: ByteArray? = null,
    private val onPaired: ((PairingInfo) -> Unit)? = null, // USB: the host sent its pairing code
    private val decodeWithoutSurface: Boolean = false, // latency experiment: nothing is displayed
    private val forcedDecoder: String? = null, // debug: MediaCodec component name to use
) {
    /** The host closed a Wi-Fi session during authentication: the pairing code is wrong. */
    private class RejectedException : Exception()

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

    // Touch frames come from the UI thread, which mustn't touch the socket.
    private var inputThread: HandlerThread? = null

    @Volatile
    private var inputHandler: Handler? = null

    fun start() {
        running = true
        inputThread = HandlerThread("DeuxDisplay-input", Process.THREAD_PRIORITY_URGENT_DISPLAY).also {
            it.start()
            inputHandler = Handler(it.looper)
        }
        thread = Thread(::run, "DeuxDisplay-net").apply { start() }
    }

    /** Forwards one touch frame to the host; dropped while not connected. Any thread. */
    fun sendTouch(contacts: List<TouchContact>) {
        if (contacts.isEmpty()) return
        val payload = serializeTouchFrame(contacts)
        inputHandler?.post { send(MessageType.INPUT, 0, payload) }
    }

    fun stop() {
        running = false
        inputHandler = null
        inputThread?.quitSafely()
        inputThread = null
        wifi?.close()
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
            listener.onStatus(
                if (wifi == null) {
                    "Waiting for DeuxDisplay on your PC over USB…\n(adb reverse tcp:$port tcp:$port)"
                } else {
                    "Connecting over Wi-Fi (${wifi.ssid})…\nIs DeuxDisplayHost running on your PC?"
                },
            )
            var retryDelayMs = if (wifi == null) USB_RETRY_DELAY_MS else RETRY_DELAY_MS
            try {
                session()
            } catch (e: IOException) {
                Log.i(TAG, "session ended: ${e.message}")
            } catch (e: ProtocolException) {
                Log.w(TAG, "protocol error: ${e.message}")
            } catch (e: RejectedException) {
                Log.w(TAG, "host rejected the pairing code")
                listener.onStatus("Your PC rejected this tablet's pairing code.\nConnect once over USB to pair again.")
                retryDelayMs = REJECTED_RETRY_DELAY_MS
            }
            if (running) {
                try {
                    Thread.sleep(retryDelayMs)
                } catch (e: InterruptedException) {
                    return
                }
            }
        }
    }

    private fun connect(): Socket? {
        val configure = { s: Socket ->
            s.tcpNoDelay = true
            s.receiveBufferSize = 4 shl 20 // before connecting, so the window scales
        }
        if (wifi != null) return wifi.connect(port, NETWORK_TIMEOUT_MS, CONNECT_TIMEOUT_MS, configure)
        return Socket().also {
            configure(it)
            it.connect(InetSocketAddress("127.0.0.1", port), CONNECT_TIMEOUT_MS)
        }
    }

    private fun session() {
        val connected = connect() ?: return
        connected.use { s ->
            socket = s
            val input = DataInputStream(BufferedInputStream(s.getInputStream(), 1 shl 16))
            output = s.getOutputStream()
            if (wifi != null) authenticate(input, authKey ?: throw RejectedException())
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
                        MessageType.PAIRING -> if (wifi == null) {
                            val pairing = PairingInfo.parse(frameBuffer.copyOf(header.length))
                            if (Pairing.normalize(pairing.code) != null) onPaired?.invoke(pairing)
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

    /** Wi-Fi handshake: prove we know the pairing code, then check that the host does too. */
    private fun authenticate(input: DataInputStream, key: ByteArray) {
        val challenge = readMessage(input)
        if (challenge.type != MessageType.AUTH_CHALLENGE) throw ProtocolException("expected AUTH_CHALLENGE")
        val hostNonce = parseFixed(frameBuffer.copyOf(challenge.length), Protocol.NONCE_SIZE)
        val clientNonce = Pairing.nonce()
        val mac = Pairing.clientMac(key, hostNonce, clientNonce)
        send(MessageType.AUTH_RESPONSE, 0, AuthResponse(clientNonce, mac).serialize())

        val reply = try {
            readMessage(input)
        } catch (e: IOException) {
            throw RejectedException() // the host closes right after BYE
        }
        if (reply.type == MessageType.BYE) throw RejectedException()
        val proof = parseFixed(frameBuffer.copyOf(reply.length), Protocol.MAC_SIZE)
        if (reply.type != MessageType.AUTH_OK || !Pairing.macEquals(proof, Pairing.hostMac(key, clientNonce, hostNonce))) {
            throw ProtocolException("host failed authentication")
        }
        Log.i(TAG, "authenticated over Wi-Fi")
    }

    /** Reads one message into [frameBuffer]. */
    private fun readMessage(input: DataInputStream): Header {
        val headerBytes = ByteArray(Protocol.HEADER_SIZE)
        input.readFully(headerBytes)
        val header = Header.decode(headerBytes)
        if (header.length > frameBuffer.size) frameBuffer = ByteArray(Integer.highestOneBit(header.length) shl 1)
        input.readFully(frameBuffer, 0, header.length)
        return header
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

        // Over USB a failed connect is an instant local refusal, so poll quickly: the display
        // appears within a quarter second of the host setting up the adb tunnel.
        private const val USB_RETRY_DELAY_MS = 250L
        private const val REJECTED_RETRY_DELAY_MS = 5000L
        private const val NETWORK_TIMEOUT_MS = 35_000L
        private const val PING_INTERVAL_MS = 500L
    }
}
