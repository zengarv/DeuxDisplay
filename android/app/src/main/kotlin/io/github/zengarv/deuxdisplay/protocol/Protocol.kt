package io.github.zengarv.deuxdisplay.protocol

import java.nio.BufferUnderflowException
import java.nio.ByteBuffer
import java.nio.ByteOrder

// Implements docs/wire-protocol.md. Keep in sync with host/src/protocol/Protocol.{h,cpp}.
object Protocol {
    const val VERSION: Int = 1
    const val MAGIC: Int = 0x50445844 // "DXDP" little-endian
    const val HEADER_SIZE: Int = 16
    const val MAX_PAYLOAD: Int = 16 * 1024 * 1024

    const val CODEC_MASK_H264: Int = 1 shl 0
    const val CODEC_MASK_HEVC: Int = 1 shl 1

    const val FLAG_KEYFRAME: Int = 1 shl 0
    const val FLAG_CODEC_CONFIG: Int = 1 shl 1

    /** Re-encode of the previous image, sent to push the real frame out of one-behind decoders. */
    const val FLAG_REPEAT: Int = 1 shl 2

    const val NONCE_SIZE: Int = 16
    const val MAC_SIZE: Int = 32

    const val INPUT_KIND_TOUCH: Int = 1
    const val MAX_TOUCH_CONTACTS: Int = 10
}

object MessageType {
    const val HELLO: Int = 0x01
    const val CONFIG: Int = 0x02
    const val BYE: Int = 0x03
    const val AUTH_CHALLENGE: Int = 0x04
    const val AUTH_RESPONSE: Int = 0x05
    const val AUTH_OK: Int = 0x06
    const val PAIRING: Int = 0x07
    const val VIDEO_FRAME: Int = 0x10
    const val REQUEST_KEYFRAME: Int = 0x11
    const val PING: Int = 0x20
    const val PONG: Int = 0x21
    const val FRAME_STATS: Int = 0x22
    const val CURSOR: Int = 0x30
    const val INPUT: Int = 0x40
}

enum class Codec(val wire: Int) {
    H264(0),
    HEVC(1);

    companion object {
        fun fromWire(value: Int): Codec? = entries.firstOrNull { it.wire == value }
    }
}

class ProtocolException(message: String) : Exception(message)

data class Header(val type: Int, val flags: Int, val length: Int, val timestamp: Long) {
    fun encode(): ByteArray = le(Protocol.HEADER_SIZE).also { writeTo(it) }.array()

    fun writeTo(buf: ByteBuffer) {
        buf.put(type.toByte())
        buf.put(flags.toByte())
        buf.putShort(0)
        buf.putInt(length)
        buf.putLong(timestamp)
    }

    companion object {
        /** Throws [ProtocolException] for a malformed header. */
        fun decode(bytes: ByteArray, offset: Int = 0): Header {
            require(bytes.size - offset >= Protocol.HEADER_SIZE) { "need ${Protocol.HEADER_SIZE} bytes" }
            val b = ByteBuffer.wrap(bytes, offset, Protocol.HEADER_SIZE).order(ByteOrder.LITTLE_ENDIAN)
            val type = b.get().toInt() and 0xFF
            val flags = b.get().toInt() and 0xFF
            val reserved = b.short.toInt()
            val length = b.int
            val timestamp = b.long
            if (reserved != 0) throw ProtocolException("reserved header bits set")
            if (length < 0 || length > Protocol.MAX_PAYLOAD) throw ProtocolException("payload length $length")
            return Header(type, flags, length, timestamp)
        }
    }
}

data class Hello(
    val widthPx: Int,
    val heightPx: Int,
    val densityDpi: Int,
    val refreshMilliHz: Int,
    val codecs: Int,
    val deviceName: String,
    /** Physical DPI x1000 (Android xdpi/ydpi); 0 = unknown. Optional on the wire. */
    val xdpiMilli: Int = 0,
    val ydpiMilli: Int = 0,
    /** Stream mode picked by the user; 0 = host decides. Optional on the wire. */
    val modeWidthPx: Int = 0,
    val modeHeightPx: Int = 0,
    val modeRefreshMilliHz: Int = 0,
    val protocolVersion: Int = Protocol.VERSION,
) {
    fun serialize(): ByteArray {
        val name = deviceName.toByteArray(Charsets.UTF_8).let { if (it.size > 0xFFFF) it.copyOf(0xFFFF) else it }
        return le(38 + name.size).apply {
            putInt(Protocol.MAGIC)
            putShort(protocolVersion.toShort())
            putShort(widthPx.toShort())
            putShort(heightPx.toShort())
            putShort(densityDpi.toShort())
            putInt(refreshMilliHz)
            putInt(codecs)
            putShort(name.size.toShort())
            put(name)
            putInt(xdpiMilli)
            putInt(ydpiMilli)
            putShort(modeWidthPx.toShort())
            putShort(modeHeightPx.toShort())
            putInt(modeRefreshMilliHz)
        }.array()
    }

    companion object {
        fun parse(payload: ByteArray): Hello = parsing(payload) {
            if (int != Protocol.MAGIC) throw ProtocolException("bad HELLO magic")
            val version = u16()
            val w = u16()
            val h = u16()
            val dpi = u16()
            val refresh = int
            val codecs = int
            val nameLen = u16()
            val name = ByteArray(nameLen).also { get(it) }
            // Optional trailing fields: older clients don't send them.
            val hasDpi = remaining() >= 8
            val xdpi = if (hasDpi) int else 0
            val ydpi = if (hasDpi) int else 0
            val hasMode = remaining() >= 8
            val modeW = if (hasMode) u16() else 0
            val modeH = if (hasMode) u16() else 0
            val modeRefresh = if (hasMode) int else 0
            Hello(w, h, dpi, refresh, codecs, String(name, Charsets.UTF_8), xdpi, ydpi, modeW, modeH, modeRefresh, version)
        }
    }
}

data class Config(
    val codec: Codec,
    val widthPx: Int,
    val heightPx: Int,
    val fpsMilliHz: Int,
    val bitrateKbps: Int,
    val protocolVersion: Int = Protocol.VERSION,
) {
    fun serialize(): ByteArray = le(16).apply {
        putShort(protocolVersion.toShort())
        put(codec.wire.toByte())
        put(0)
        putShort(widthPx.toShort())
        putShort(heightPx.toShort())
        putInt(fpsMilliHz)
        putInt(bitrateKbps)
    }.array()

    companion object {
        fun parse(payload: ByteArray): Config = parsing(payload) {
            val version = u16()
            val codecWire = get().toInt() and 0xFF
            get()
            val w = u16()
            val h = u16()
            val fps = int
            val bitrate = int
            val codec = Codec.fromWire(codecWire) ?: throw ProtocolException("unknown codec $codecWire")
            Config(codec, w, h, fps, bitrate, version)
        }
    }
}

data class Pong(val pingId: Long, val pingTimestamp: Long) {
    fun serialize(): ByteArray = le(16).putLong(pingId).putLong(pingTimestamp).array()

    companion object {
        fun parse(payload: ByteArray): Pong = parsing(payload) { Pong(long, long) }
    }
}

data class FrameStats(val captureTs: Long, val receivedTs: Long, val decodedTs: Long, val renderedTs: Long) {
    fun serialize(): ByteArray =
        le(32).putLong(captureTs).putLong(receivedTs).putLong(decodedTs).putLong(renderedTs).array()

    companion object {
        fun parse(payload: ByteArray): FrameStats = parsing(payload) { FrameStats(long, long, long, long) }
    }
}

object TouchAction {
    const val DOWN: Int = 0
    const val MOVE: Int = 1
    const val UP: Int = 2
    const val CANCEL: Int = 3
}

/** One contact of an INPUT touch frame. [x]/[y] are 0..65535 across the video frame. */
data class TouchContact(val id: Int, val action: Int, val x: Int, val y: Int, val pressure: Int = 0)

/** INPUT payload listing every contact currently on the screen. */
fun serializeTouchFrame(contacts: List<TouchContact>): ByteArray {
    require(contacts.size in 1..Protocol.MAX_TOUCH_CONTACTS) { "${contacts.size} contacts" }
    return le(4 + 8 * contacts.size).apply {
        put(Protocol.INPUT_KIND_TOUCH.toByte())
        put(contacts.size.toByte())
        putShort(0)
        for (c in contacts) {
            put(c.id.toByte())
            put(c.action.toByte())
            putShort(c.x.toShort())
            putShort(c.y.toShort())
            putShort(c.pressure.toShort())
        }
    }.array()
}

fun parseTouchFrame(payload: ByteArray): List<TouchContact> = parsing(payload) {
    if ((get().toInt() and 0xFF) != Protocol.INPUT_KIND_TOUCH) throw ProtocolException("not a touch frame")
    val count = get().toInt() and 0xFF
    if (count !in 1..Protocol.MAX_TOUCH_CONTACTS) throw ProtocolException("$count contacts")
    getShort() // reserved
    List(count) {
        val id = get().toInt() and 0xFF
        val action = get().toInt() and 0xFF
        if (id >= Protocol.MAX_TOUCH_CONTACTS || action > TouchAction.CANCEL) throw ProtocolException("bad contact")
        TouchContact(id, action, u16(), u16(), u16())
    }
}

/** AUTH_CHALLENGE payload is the host nonce; AUTH_OK payload is the host's MAC. */
fun parseFixed(payload: ByteArray, size: Int): ByteArray {
    if (payload.size < size) throw ProtocolException("truncated payload")
    return payload.copyOf(size)
}

/** AUTH_RESPONSE: the client's nonce and its proof that it knows the pairing code. */
class AuthResponse(val clientNonce: ByteArray, val mac: ByteArray) {
    init {
        require(clientNonce.size == Protocol.NONCE_SIZE && mac.size == Protocol.MAC_SIZE)
    }

    fun serialize(): ByteArray = clientNonce + mac

    companion object {
        fun parse(payload: ByteArray): AuthResponse {
            val bytes = parseFixed(payload, Protocol.NONCE_SIZE + Protocol.MAC_SIZE)
            return AuthResponse(bytes.copyOf(Protocol.NONCE_SIZE), bytes.copyOfRange(Protocol.NONCE_SIZE, bytes.size))
        }
    }
}

/** PAIRING (USB only): the code the client needs for Wi-Fi sessions. */
data class PairingInfo(val code: String, val hostName: String) {
    fun serialize(): ByteArray {
        val c = code.toByteArray(Charsets.UTF_8)
        val h = hostName.toByteArray(Charsets.UTF_8)
        return le(4 + c.size + h.size).putShort(c.size.toShort()).put(c).putShort(h.size.toShort()).put(h).array()
    }

    companion object {
        fun parse(payload: ByteArray): PairingInfo = parsing(payload) { PairingInfo(string(), string()) }
    }
}

fun serializePing(pingId: Long): ByteArray = le(8).putLong(pingId).array()

fun parsePing(payload: ByteArray): Long = parsing(payload) { long }

private fun le(size: Int): ByteBuffer = ByteBuffer.allocate(size).order(ByteOrder.LITTLE_ENDIAN)

private fun ByteBuffer.u16(): Int = short.toInt() and 0xFFFF

private fun ByteBuffer.string(): String = String(ByteArray(u16()).also { get(it) }, Charsets.UTF_8)

private inline fun <T> parsing(payload: ByteArray, block: ByteBuffer.() -> T): T =
    try {
        ByteBuffer.wrap(payload).order(ByteOrder.LITTLE_ENDIAN).block()
    } catch (e: BufferUnderflowException) {
        throw ProtocolException("truncated payload")
    }
