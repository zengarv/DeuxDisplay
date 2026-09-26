package io.github.zengarv.deuxdisplay.stream

import android.annotation.TargetApi
import android.content.Context
import android.net.ConnectivityManager
import android.net.LinkProperties
import android.net.Network
import android.net.NetworkCapabilities
import android.net.NetworkRequest
import android.net.wifi.WifiManager
import android.net.wifi.WifiNetworkSpecifier
import android.os.Build
import android.util.Log
import io.github.zengarv.deuxdisplay.protocol.PairingSecrets
import java.net.InetAddress
import java.net.Socket
import java.util.concurrent.TimeUnit
import java.util.concurrent.locks.ReentrantLock
import kotlin.concurrent.withLock

/**
 * Joins the Wi-Fi network the PC runs (docs/architecture.md, "Wi-Fi") for as long as this
 * object is open, and keeps the radio out of power save meanwhile. The system asks the user
 * once to approve joining; Android then routes only this app's sockets over it.
 */
@TargetApi(Build.VERSION_CODES.Q) // callers check: the Wi-Fi option only exists on Android 10+
class WifiLink(context: Context, private val secrets: PairingSecrets) {
    private val connectivity = context.getSystemService(ConnectivityManager::class.java)
    private val wifi = context.applicationContext.getSystemService(WifiManager::class.java)
    private val lock = ReentrantLock()
    private val changed = lock.newCondition()

    private var network: Network? = null
    private var unavailable = false
    private var closed = false
    private var callback: ConnectivityManager.NetworkCallback? = null

    // Without it, power save holds packets for the PC in the access point until the next
    // beacon (~100 ms). Only effective while the app is in the foreground with the screen on.
    private val wifiLock = wifi.createWifiLock(WifiManager.WIFI_MODE_FULL_LOW_LATENCY, "DeuxDisplay:stream").apply {
        setReferenceCounted(false)
    }

    val ssid: String get() = secrets.ssid

    /**
     * Waits until the tablet is on the PC's network (requesting it if needed) and returns a
     * connected socket to the host, or null if the network or the host isn't reachable.
     */
    fun connect(port: Int, timeoutMs: Long, connectTimeoutMs: Int, configure: (Socket) -> Unit): Socket? {
        val net = awaitNetwork(timeoutMs) ?: return null
        val host = hostAddress(net) ?: return null
        val socket = net.socketFactory.createSocket()
        return try {
            configure(socket)
            socket.trafficClass = TRAFFIC_CLASS_VIDEO
            socket.connect(java.net.InetSocketAddress(host, port), connectTimeoutMs)
            socket
        } catch (e: java.io.IOException) {
            socket.close()
            throw e
        }
    }

    /** Leaves the network (the tablet returns to its usual Wi-Fi) and wakes a pending [connect]. */
    fun close() {
        lock.withLock {
            closed = true
            callback?.let { connectivity.unregisterNetworkCallback(it) }
            callback = null
            network = null
            changed.signalAll()
        }
        wifiLock.release()
    }

    private fun awaitNetwork(timeoutMs: Long): Network? = lock.withLock {
        if (closed) return null
        if (callback == null || unavailable) request()
        var remaining = TimeUnit.MILLISECONDS.toNanos(timeoutMs)
        while (network == null && !unavailable && remaining > 0) {
            remaining = changed.awaitNanos(remaining)
        }
        network
    }

    private fun request() {
        callback?.let { connectivity.unregisterNetworkCallback(it) }
        unavailable = false
        val specifier = WifiNetworkSpecifier.Builder()
            .setSsid(secrets.ssid)
            .setWpa2Passphrase(secrets.passphrase)
            .build()
        val request = NetworkRequest.Builder()
            .addTransportType(NetworkCapabilities.TRANSPORT_WIFI)
            .removeCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET) // the PC doesn't route out
            .setNetworkSpecifier(specifier)
            .build()
        val cb = object : ConnectivityManager.NetworkCallback() {
            override fun onAvailable(net: Network) {
                Log.i(TAG, "joined ${secrets.ssid}")
                lock.withLock {
                    if (closed) return
                    wifiLock.acquire()
                    network = net
                    changed.signalAll()
                }
            }

            override fun onLost(net: Network) {
                Log.i(TAG, "left ${secrets.ssid}")
                lock.withLock {
                    if (network == net) {
                        network = null
                        unavailable = true // ask again on the next attempt
                    }
                }
            }

            override fun onUnavailable() {
                Log.i(TAG, "${secrets.ssid} unavailable (not found, or the user declined)")
                lock.withLock {
                    unavailable = true
                    changed.signalAll()
                }
            }
        }
        callback = cb
        connectivity.requestNetwork(request, cb, REQUEST_TIMEOUT_MS)
    }

    /** The PC is the network's router (its DHCP server hands itself out as the gateway). */
    private fun hostAddress(net: Network): InetAddress? {
        val props: LinkProperties = connectivity.getLinkProperties(net) ?: return null
        props.routes.firstOrNull { it.isDefaultRoute && it.gateway != null }?.gateway?.let { return it }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) props.dhcpServerAddress?.let { return it }
        return InetAddress.getByName(DEFAULT_HOST)
    }

    private companion object {
        const val TAG = "DeuxDisplay"
        const val REQUEST_TIMEOUT_MS = 30_000
        const val DEFAULT_HOST = "192.168.137.1" // Windows' address on its own Wi-Fi Direct networks
        const val TRAFFIC_CLASS_VIDEO = 0xA0 // DSCP CS5: Wi-Fi WMM video access category
    }
}
