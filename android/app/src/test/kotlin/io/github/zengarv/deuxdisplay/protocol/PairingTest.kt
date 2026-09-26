package io.github.zengarv.deuxdisplay.protocol

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class PairingTest {

    private fun hex(s: String): ByteArray = ByteArray(s.length / 2) { s.substring(2 * it, 2 * it + 2).toInt(16).toByte() }

    @Test
    fun normalizesCodes() {
        assertEquals("ABCDEFGHIJKLMNOPQRST", Pairing.normalize("abcde-fghij klmno-pqrst"))
        assertNull(Pairing.normalize("ABCDE-FGHIJ-KLMNO-PQRS"))
        assertNull(Pairing.normalize("ABCDE-FGHIJ-KLMNO-PQRS1"))
        assertEquals("ABCDE-FGHIJ-KLMNO-PQRST", Pairing.format("ABCDEFGHIJKLMNOPQRST"))
    }

    @Test
    fun derivationsMatchHost() {
        // Same vectors as docs/wire-protocol.md and host/tests/ProtocolTests.cpp.
        val secrets = Pairing.derive("ABCDEFGHIJKLMNOPQRST")
        assertEquals("DeuxDisplay-9968", secrets.ssid)
        assertEquals("3252SPVCLUVNTF5LBJDW4VOV", secrets.passphrase)
        assertArrayEquals(hex("29a36685e1e1d7687dbc70c81ffd8a402317f3a748ca2e00b9cbcae5b996a64e"), secrets.authKey)

        val hostNonce = ByteArray(16) { it.toByte() }
        val clientNonce = ByteArray(16) { (16 + it).toByte() }
        val client = Pairing.clientMac(secrets.authKey, hostNonce, clientNonce)
        val host = Pairing.hostMac(secrets.authKey, clientNonce, hostNonce)
        assertArrayEquals(hex("ee895399e449a1562e0db0c6656f9a09e9a0f9655a0a09e6c7717a36935dd2cb"), client)
        assertArrayEquals(hex("cfa68d242928b528cccf900c86d75c21e6434ff4595955445482ac9ef4ec50c8"), host)
        assertTrue(Pairing.macEquals(client, client.copyOf()))
        assertFalse(Pairing.macEquals(client, host))
    }

    @Test
    fun authMessagesRoundTrip() {
        val response = AuthResponse(ByteArray(16) { it.toByte() }, ByteArray(32).also { it[31] = 0xEE.toByte() })
        val bytes = response.serialize()
        assertEquals(48, bytes.size)
        val parsed = AuthResponse.parse(bytes)
        assertArrayEquals(response.clientNonce, parsed.clientNonce)
        assertArrayEquals(response.mac, parsed.mac)

        val pairing = PairingInfo("ABCDE-FGHIJ-KLMNO-PQRST", "DESKTOP")
        assertEquals(pairing, PairingInfo.parse(pairing.serialize()))
    }

    @Test(expected = ProtocolException::class)
    fun authResponseRejectsTruncated() {
        AuthResponse.parse(ByteArray(47))
    }
}
