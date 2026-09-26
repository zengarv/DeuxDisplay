package io.github.zengarv.deuxdisplay.protocol

import java.security.MessageDigest
import java.security.SecureRandom
import javax.crypto.Mac
import javax.crypto.spec.SecretKeySpec

// Pairing-code derivations and the Wi-Fi authentication MACs (docs/wire-protocol.md,
// "Pairing and authentication"). Keep in sync with host/src/protocol/Pairing.{h,cpp}.

/** Everything derived from a pairing code: the host's Wi-Fi network and the auth key. */
class PairingSecrets(val ssid: String, val passphrase: String, val authKey: ByteArray)

object Pairing {
    const val CODE_LENGTH: Int = 20
    private const val BASE32 = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567"
    private val random = SecureRandom()

    /** Upper-cases and drops '-'/spaces; null unless exactly 20 Base32 characters remain. */
    fun normalize(code: String): String? {
        val out = StringBuilder()
        for (ch in code) {
            if (ch == '-' || ch == ' ') continue
            val c = ch.uppercaseChar()
            if (c !in 'A'..'Z' && c !in '2'..'7') return null
            out.append(c)
        }
        return out.toString().takeIf { it.length == CODE_LENGTH }
    }

    /** "XXXXX-XXXXX-XXXXX-XXXXX" for a normalized code. */
    fun format(normalized: String): String = normalized.chunked(5).joinToString("-")

    fun derive(normalized: String): PairingSecrets {
        val key = normalized.toByteArray(Charsets.US_ASCII)
        val ssid = hmac(key, "deuxdisplay ssid".toByteArray())
        val wpa = hmac(key, "deuxdisplay wpa2".toByteArray())
        val auth = hmac(key, "deuxdisplay auth".toByteArray())
        val hex = "%02x%02x".format(ssid[0].toInt() and 0xFF, ssid[1].toInt() and 0xFF)
        return PairingSecrets("DeuxDisplay-$hex", base32(wpa.copyOf(15)), auth)
    }

    fun clientMac(authKey: ByteArray, hostNonce: ByteArray, clientNonce: ByteArray): ByteArray =
        hmac(authKey, "DXDP-C".toByteArray() + hostNonce + clientNonce)

    fun hostMac(authKey: ByteArray, clientNonce: ByteArray, hostNonce: ByteArray): ByteArray =
        hmac(authKey, "DXDP-H".toByteArray() + clientNonce + hostNonce)

    /** Constant-time comparison. */
    fun macEquals(a: ByteArray, b: ByteArray): Boolean = MessageDigest.isEqual(a, b)

    fun nonce(): ByteArray = ByteArray(Protocol.NONCE_SIZE).also { random.nextBytes(it) }

    private fun hmac(key: ByteArray, message: ByteArray): ByteArray =
        Mac.getInstance("HmacSHA256").run {
            init(SecretKeySpec(key, "HmacSHA256"))
            doFinal(message)
        }

    /** RFC 4648 Base32 without padding (input is a multiple of 5 bytes). */
    private fun base32(data: ByteArray): String {
        val out = StringBuilder()
        var buffer = 0
        var bits = 0
        for (b in data) {
            buffer = (buffer shl 8) or (b.toInt() and 0xFF)
            bits += 8
            while (bits >= 5) {
                out.append(BASE32[(buffer shr (bits - 5)) and 31])
                bits -= 5
            }
            buffer = buffer and ((1 shl bits) - 1)
        }
        return out.toString()
    }
}
