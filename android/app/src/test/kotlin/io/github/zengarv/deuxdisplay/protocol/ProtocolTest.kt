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
        val hello = Hello(
            2408, 1720, 360, 90_000, Protocol.CODEC_MASK_H264, "OnePlus OPD2305", 260_047, 260_268,
            modeWidthPx = 1920, modeHeightPx = 1370, modeRefreshMilliHz = 60_000,
            bitrateKbps = 45_000, encoderQuality = 30,
        )
        val bytes = hello.serialize()
        assertEquals(44 + hello.deviceName.length, bytes.size)
        assertEquals('D'.code.toByte(), bytes[0])
        assertEquals(hello, Hello.parse(bytes))
        assertEquals(hello, Hello.parse(bytes + byteArrayOf(0x55))) // trailing future fields ignored
    }

    @Test
    fun helloWithoutRequestedMode() {
        val hello = Hello(2408, 1720, 360, 90_000, Protocol.CODEC_MASK_H264, "old client", 1, 2, 1920, 1370, 60_000)
        val bytes = hello.serialize()
        val parsed = Hello.parse(bytes.copyOf(bytes.size - 6 - 8))
        assertEquals(1, parsed.xdpiMilli)
        assertEquals(0, parsed.modeWidthPx)
        assertEquals(0, parsed.modeRefreshMilliHz)
    }

    @Test
    fun helloWithoutOptionalDpi() {
        val hello = Hello(2408, 1720, 360, 90_000, Protocol.CODEC_MASK_H264, "old client", 1, 2)
        val bytes = hello.serialize()
        val parsed = Hello.parse(bytes.copyOf(bytes.size - 6 - 16))
        assertEquals(0, parsed.xdpiMilli)
        assertEquals("old client", parsed.deviceName)
    }

    @Test(expected = ProtocolException::class)
    fun helloRejectsTruncated() {
        val bytes = Hello(1, 1, 1, 1, 1, "abc").serialize()
        Hello.parse(bytes.copyOf(bytes.size - 24)) // cuts into the name
    }

    @Test
    fun configRoundTrip() {
        val config = Config(Codec.H264, 2408, 1720, 60_000, 20_000, rotationDegrees = 90)
        val bytes = config.serialize()
        assertEquals(18, bytes.size)
        assertEquals(config, Config.parse(bytes))
        // Older host: no rotation field.
        assertEquals(0, Config.parse(bytes.copyOf(16)).rotationDegrees)
    }

    @Test
    fun orientationRoundTrip() {
        assertEquals(90, parseOrientation(serializeOrientation(90)))
        assertEquals(0, parseOrientation(serializeOrientation(0)))
        // Same bytes as the host: little-endian u16.
        assertArrayEquals(byteArrayOf(90, 0), serializeOrientation(90))
    }

    @Test(expected = ProtocolException::class)
    fun orientationRejectsOddAngles() {
        parseOrientation(serializeOrientation(45))
    }

    @Test
    fun pingPongStatsRoundTrip() {
        assertEquals(42L, parsePing(serializePing(42L)))
        assertEquals(Pong(7, 99), Pong.parse(Pong(7, 99).serialize()))
        val stats = FrameStats(1, 2, 3, 4)
        assertEquals(stats, FrameStats.parse(stats.serialize()))
    }

    @Test
    fun touchFrameLayoutMatchesSpec() {
        val contacts = listOf(TouchContact(1, TouchAction.DOWN, 0x1234, 0xABCD, 512))
        val bytes = serializeTouchFrame(contacts)
        // Same vector as host/tests/ProtocolTests.cpp.
        val expected = byteArrayOf(
            0x01, 0x01, 0x00, 0x00,
            0x01, 0x00, 0x34, 0x12, 0xCD.toByte(), 0xAB.toByte(), 0x00, 0x02,
        )
        assertArrayEquals(expected, bytes)
        assertEquals(contacts, parseTouchFrame(bytes))
    }

    @Test(expected = ProtocolException::class)
    fun touchFrameRejectsBadSlot() {
        val bytes = serializeTouchFrame(listOf(TouchContact(1, TouchAction.MOVE, 0, 0)))
        bytes[4] = 10
        parseTouchFrame(bytes)
    }

    @Test
    fun mediaStateMatchesHostLayout() {
        // Same bytes as host/tests/ProtocolTests.cpp.
        val state = MediaState(Playback.PLAYING, muted = true, volumePercent = 45)
        assertArrayEquals(byteArrayOf(2, 1, 45, 0), state.serialize())
        assertEquals(state, MediaState.parse(state.serialize()))
        assertArrayEquals(byteArrayOf(4), serializeAction(DockAction.UNDO))
    }

    @Test
    fun encoderMessagesMatchHostLayout() {
        // Same bytes as host/tests/ProtocolTests.cpp.
        val settings = EncoderSettings(45_000, 30)
        assertArrayEquals(byteArrayOf(0xC8.toByte(), 0xAF.toByte(), 0, 0, 30, 0), settings.serialize())
        assertEquals(settings, EncoderSettings.parse(settings.serialize()))

        val state = EncoderState(62_000, adaptive = true, quality = 0)
        assertArrayEquals(byteArrayOf(0x30, 0xF2.toByte(), 0, 0, 1, 0, 0, 0), state.serialize())
        assertEquals(state, EncoderState.parse(state.serialize()))
    }

    @Test
    fun helloWithoutEncoderSettings() {
        val hello = Hello(2408, 1720, 360, 90_000, Protocol.CODEC_MASK_H264, "c", 1, 2, 1920, 1370, 60_000, 45_000, 30)
        val bytes = hello.serialize()
        val parsed = Hello.parse(bytes.copyOf(bytes.size - 6))
        assertEquals(1920, parsed.modeWidthPx)
        assertEquals(0, parsed.bitrateKbps)
        assertEquals(Protocol.QUALITY_HOST_DECIDES, parsed.encoderQuality)
    }

    @Test(expected = ProtocolException::class)
    fun mediaStateRejectsVolumeOver100() {
        MediaState.parse(byteArrayOf(1, 0, 101, 0))
    }

    @Test(expected = ProtocolException::class)
    fun mediaStateRejectsTruncated() {
        MediaState.parse(byteArrayOf(1, 0, 50))
    }
}
