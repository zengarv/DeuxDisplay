package io.github.zengarv.deuxdisplay.stream

/**
 * NTP-style offset between this device's clock and the host's, from PING/PONG round trips.
 * Keeps the sample with the smallest RTT seen recently, since it has the least queuing error.
 */
class ClockSync {
    @Volatile
    var offsetUs: Long = 0 // host = client + offset
        private set

    @Volatile
    var rttUs: Long = Long.MAX_VALUE
        private set

    @Volatile
    var valid = false
        private set

    private var bestRtt = Long.MAX_VALUE
    private var samples = 0

    /** [sentUs]/[receivedUs] on the client clock, [hostUs] = host clock when it answered. */
    @Synchronized
    fun onPong(sentUs: Long, receivedUs: Long, hostUs: Long) {
        val rtt = receivedUs - sentUs
        if (rtt < 0) return
        // Let the best sample age out slowly so drift and route changes are tracked.
        if (++samples % WINDOW == 0) bestRtt = Long.MAX_VALUE
        if (rtt <= bestRtt) {
            bestRtt = rtt
            rttUs = rtt
            offsetUs = hostUs - (sentUs + receivedUs) / 2
            valid = true
        }
    }

    fun toHost(clientUs: Long): Long = if (valid && clientUs != 0L) clientUs + offsetUs else 0L

    private companion object {
        const val WINDOW = 20
    }
}
