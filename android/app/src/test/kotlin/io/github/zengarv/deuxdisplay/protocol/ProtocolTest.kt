package io.github.zengarv.deuxdisplay.protocol

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Test

class ProtocolTest {

    @Test
    fun headerLayoutMatchesSpec() {
        val h = Header(
            MessageType.VIDEO_FRAME,
            Protocol.FLAG_KEYFRAME or Protocol.FLAG_CODEC_CONFIG,
            123456,
            0x0102030405060708L,
        )
        val bytes = h.encode()
        // Same vector as host/tests/ProtocolTests.cpp so both sides agree on the layout.
        val expected = byteArrayOf(
            0x10, 0x03, 0x00, 0x00,
            0x40, 0xE2.toByte(), 0x01, 0x00,
            0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
        )
        assertArrayEquals(expected, bytes)
        assertEquals(h, Header.decode(bytes))
    }

    @Test(expected = ProtocolException::class)
    fun headerRejectsOversizedPayload() {
        Header.decode(Header(MessageType.PING, 0, Protocol.MAX_PAYLOAD + 1, 0).encode())
    }

    @Test(expected = ProtocolException::class)
    fun headerRejectsReservedBits() {
        val bytes = Header(MessageType.PING, 0, 8, 0).encode()
        bytes[2] = 1
        Header.decode(bytes)
    }

    @Test
    fun helloRoundTrip() {
        val hello = Hello(2408, 1720, 360, 90_000, Protocol.CODEC_MASK_H264, "OnePlus OPD2305")
        val bytes = hello.serialize()
        assertEquals(22 + hello.deviceName.length, bytes.size)
        assertEquals('D'.code.toByte(), bytes[0])
        assertEquals(hello, Hello.parse(bytes))
        assertEquals(hello, Hello.parse(bytes + byteArrayOf(0x55))) // trailing future fields ignored
    }

    @Test(expected = ProtocolException::class)
    fun helloRejectsTruncated() {
        val bytes = Hello(1, 1, 1, 1, 1, "abc").serialize()
        Hello.parse(bytes.copyOf(bytes.size - 2))
    }

    @Test
    fun configRoundTrip() {
        val config = Config(Codec.H264, 2408, 1720, 60_000, 20_000)
        val bytes = config.serialize()
        assertEquals(16, bytes.size)
        assertEquals(config, Config.parse(bytes))
    }

    @Test
    fun pingPongStatsRoundTrip() {
        assertEquals(42L, parsePing(serializePing(42L)))
        assertEquals(Pong(7, 99), Pong.parse(Pong(7, 99).serialize()))
        val stats = FrameStats(1, 2, 3, 4)
        assertEquals(stats, FrameStats.parse(stats.serialize()))
    }
}
