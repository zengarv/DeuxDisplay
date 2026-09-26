package io.github.zengarv.deuxdisplay.stream

import android.content.Context
import io.github.zengarv.deuxdisplay.protocol.Pairing

/** The pairing code for Wi-Fi sessions, from a USB session's PAIRING message or typed by the user. */
class PairingStore(context: Context) {
    data class Paired(val code: String, val hostName: String)

    private val prefs = context.getSharedPreferences("pairing", Context.MODE_PRIVATE)

    fun load(): Paired? {
        val code = prefs.getString(KEY_CODE, null)?.let(Pairing::normalize) ?: return null
        return Paired(code, prefs.getString(KEY_HOST, null).orEmpty())
    }

    /** Returns false (and stores nothing) if [code] isn't a valid pairing code. */
    fun save(code: String, hostName: String): Boolean {
        val normalized = Pairing.normalize(code) ?: return false
        prefs.edit().putString(KEY_CODE, normalized).putString(KEY_HOST, hostName).apply()
        return true
    }

    private companion object {
        const val KEY_CODE = "code"
        const val KEY_HOST = "host_name"
    }
}
