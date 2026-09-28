package io.github.zengarv.deuxdisplay

import android.app.Activity
import android.content.res.Configuration
import android.graphics.Color
import android.media.MediaFormat
import android.text.InputType
import android.os.Build
import android.os.Bundle
import android.util.Log
import android.view.Gravity
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.Surface
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.view.WindowInsets
import android.view.WindowInsetsController
import android.view.WindowManager
import android.widget.AdapterView
import android.widget.ArrayAdapter
import android.widget.Button
import android.widget.EditText
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.Spinner
import android.widget.Switch
import android.widget.TextView
import io.github.zengarv.deuxdisplay.protocol.Config
import io.github.zengarv.deuxdisplay.protocol.DockAction
import io.github.zengarv.deuxdisplay.protocol.MediaState
import io.github.zengarv.deuxdisplay.protocol.Pairing
import io.github.zengarv.deuxdisplay.protocol.PairingInfo
import io.github.zengarv.deuxdisplay.stream.DisplayInfo
import io.github.zengarv.deuxdisplay.stream.ModeOption
import io.github.zengarv.deuxdisplay.stream.PairingStore
import io.github.zengarv.deuxdisplay.stream.StreamClient
import io.github.zengarv.deuxdisplay.stream.StreamCodec
import io.github.zengarv.deuxdisplay.stream.StreamMode
import io.github.zengarv.deuxdisplay.stream.StreamModes
import io.github.zengarv.deuxdisplay.stream.StreamStats
import io.github.zengarv.deuxdisplay.stream.TouchInput
import io.github.zengarv.deuxdisplay.stream.VideoDecoder
import io.github.zengarv.deuxdisplay.stream.WifiLink
import kotlin.math.roundToInt

class MainActivity : Activity(), SurfaceHolder.Callback {

    private lateinit var status: TextView
    private lateinit var settings: LinearLayout
    private lateinit var resolutionSpinner: Spinner
    private lateinit var refreshSpinner: Spinner
    private lateinit var codecSpinner: Spinner
    private var codecChoices: List<StreamCodec> = emptyList()
    private val hevcDecoder by lazy { VideoDecoder.hasHardwareDecoder(MediaFormat.MIMETYPE_VIDEO_HEVC) }
    private lateinit var connectionSpinner: Spinner
    private lateinit var pairingLabel: TextView
    private lateinit var codeField: EditText
    private lateinit var pairing: PairingStore
    private var client: StreamClient? = null
    private var surfaceHolder: SurfaceHolder? = null

    // Stream format choices: resolution index 0 and refresh index 0 are "Auto".
    private lateinit var modeOptions: List<ModeOption>
    private var refreshChoices: List<Int> = emptyList()
    private var mode = StreamMode()
    private var useWifi = false

    private var streaming = false
    private var settingsOpened = false // opened over a running stream with Back

    // Shortcut dock and volume keys. Both need a host that takes ACTION, which it signals with
    // MEDIA_STATE; until then the volume keys control the tablet as usual.
    private lateinit var dock: DockView
    private lateinit var volumeIndicator: VolumeIndicator
    private var dockEnabled = true
    private var mediaState: MediaState? = null // this session's last, null = host has no controls

    // Frame rate of the running stream (CONFIG), which the panel follows when the rate is on Auto.
    private var streamHz = 0

    // Debug stats overlay; counters are only kept while it's shown.
    private val stats = StreamStats()
    private lateinit var statsOverlay: StatsOverlay
    private var statsEnabled = false

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)

        pairing = PairingStore(this)
        modeOptions = StreamModes.options(this)
        mode = loadMode()
        useWifi = wifiSupported && getSharedPreferences(PREFS, MODE_PRIVATE).getBoolean(KEY_WIFI, false)
        Log.i(TAG, "stream modes $modeOptions, selected $mode")
        applyPanelRefresh()

        val surfaceView = SurfaceView(this)
        surfaceView.holder.addCallback(this)
        surfaceView.setOnTouchListener(::onSurfaceTouch)

        status = TextView(this).apply {
            setTextColor(Color.LTGRAY)
            textSize = 18f
            gravity = Gravity.CENTER
        }
        dockEnabled = getSharedPreferences(PREFS, MODE_PRIVATE).getBoolean(KEY_DOCK, true)
        dock = DockView(this, getSharedPreferences(DOCK_PREFS, MODE_PRIVATE)) { client?.sendAction(it) }
        volumeIndicator = VolumeIndicator(this)
        statsEnabled = getSharedPreferences(PREFS, MODE_PRIVATE).getBoolean(KEY_STATS, false)
        statsOverlay = StatsOverlay(this, stats)
        settings = buildSettings()

        setContentView(
            FrameLayout(this).apply {
                setBackgroundColor(Color.BLACK)
                addView(surfaceView)
                addView(dock, FrameLayout.LayoutParams(
                    FrameLayout.LayoutParams.WRAP_CONTENT,
                    FrameLayout.LayoutParams.WRAP_CONTENT,
                    Gravity.TOP or Gravity.START,
                ))
                addView(volumeIndicator, FrameLayout.LayoutParams(
                    FrameLayout.LayoutParams.WRAP_CONTENT,
                    FrameLayout.LayoutParams.WRAP_CONTENT,
                    Gravity.TOP or Gravity.CENTER_HORIZONTAL,
                ).apply { topMargin = dp(24) })
                addView(statsOverlay, FrameLayout.LayoutParams(
                    FrameLayout.LayoutParams.WRAP_CONTENT,
                    FrameLayout.LayoutParams.WRAP_CONTENT,
                    Gravity.TOP or Gravity.END,
                ).apply { setMargins(dp(12), dp(12), dp(12), dp(12)) })
                addView(status, FrameLayout.LayoutParams(
                    FrameLayout.LayoutParams.MATCH_PARENT,
                    FrameLayout.LayoutParams.MATCH_PARENT,
                ))
                addView(settings, FrameLayout.LayoutParams(
                    FrameLayout.LayoutParams.WRAP_CONTENT,
                    FrameLayout.LayoutParams.WRAP_CONTENT,
                    Gravity.BOTTOM or Gravity.CENTER_HORIZONTAL,
                ).apply { bottomMargin = dp(48) })
            },
        )
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus) hideSystemBars()
    }

    private fun hideSystemBars() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            window.insetsController?.let {
                it.hide(WindowInsets.Type.systemBars())
                it.systemBarsBehavior = WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
            }
        } else {
            @Suppress("DEPRECATION")
            window.decorView.systemUiVisibility = (
                View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                    or View.SYSTEM_UI_FLAG_FULLSCREEN
                    or View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                    or View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                    or View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                    or View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                )
        }
    }

    // Touches on the stream go to the PC. The video fills the view, so view coordinates map
    // straight onto the video frame.
    private fun onSurfaceTouch(view: View, event: MotionEvent): Boolean {
        if (event.actionMasked == MotionEvent.ACTION_DOWN) {
            view.requestUnbufferedDispatch(event) // moves as they happen, not batched per vsync
        }
        if (streaming && !settingsOpened) {
            client?.sendTouch(TouchInput.contacts(event, view.width, view.height))
        }
        if (event.actionMasked == MotionEvent.ACTION_UP) view.performClick()
        return true
    }

    // Back toggles the stream settings while streaming (touches go to the PC). The volume keys
    // change the Windows volume instead of the tablet's, which stays as it was.
    override fun onKeyDown(keyCode: Int, event: KeyEvent?): Boolean {
        if (keyCode == KeyEvent.KEYCODE_BACK && streaming) {
            settingsOpened = !settingsOpened
            updateSettingsVisibility()
            return true
        }
        if (isVolumeKey(keyCode) && hostVolumeKeys()) {
            client?.sendAction(if (keyCode == KeyEvent.KEYCODE_VOLUME_UP) DockAction.VOLUME_UP else DockAction.VOLUME_DOWN)
            mediaState?.let(volumeIndicator::show) // updated when the host reports the new level
            return true
        }
        return super.onKeyDown(keyCode, event)
    }

    override fun onKeyUp(keyCode: Int, event: KeyEvent?): Boolean {
        if (isVolumeKey(keyCode) && hostVolumeKeys()) return true
        return super.onKeyUp(keyCode, event)
    }

    private fun isVolumeKey(keyCode: Int) =
        keyCode == KeyEvent.KEYCODE_VOLUME_UP || keyCode == KeyEvent.KEYCODE_VOLUME_DOWN

    private fun hostVolumeKeys() = streaming && mediaState != null

    /** The host's volume and playback state (network thread). */
    private fun onMediaState(state: MediaState) {
        runOnUiThread {
            val previous = mediaState
            mediaState = state
            dock.setMediaState(state)
            if (previous != null && (previous.volumePercent != state.volumePercent || previous.muted != state.muted)) {
                volumeIndicator.show(state)
            }
            updateDockVisibility()
        }
    }

    // The stream lives exactly as long as the surface: when the tablet sleeps or the app goes to
    // the background, the host unplugs the virtual monitor and Windows moves the windows back.
    override fun surfaceCreated(holder: SurfaceHolder) {
        Log.i(TAG, "surface created")
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
        Log.i(TAG, "surface ${width}x$height")
        surfaceHolder = holder
        if (client == null) startClient(holder)
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        Log.i(TAG, "surface destroyed")
        surfaceHolder = null
        client?.stop()
        client = null
    }

    private fun startClient(holder: SurfaceHolder) {
        val hello = DisplayInfo.hello(this, mode)
        val secrets = pairing.load()?.let { Pairing.derive(it.code) }
        if (useWifi && secrets == null) {
            showStatus(getString(R.string.status_not_paired))
            return
        }
        val wifi = if (useWifi && secrets != null && wifiSupported) WifiLink(this, secrets) else null
        // adb shell am start -n io.github.zengarv.deuxdisplay/.MainActivity --ez debug_no_surface true
        // ... --es debug_decoder c2.android.avc.decoder   (force a specific decoder)
        val noSurface = intent.getBooleanExtra("debug_no_surface", false)
        val decoder = intent.getStringExtra("debug_decoder")
        client = StreamClient(
            hello,
            holder.surface,
            ::showStatus,
            wifi = wifi,
            authKey = secrets?.authKey,
            onPaired = ::onPaired,
            onMediaState = ::onMediaState,
            onConfig = ::onStreamConfig,
            stats = stats,
            decodeWithoutSurface = noSurface,
            forcedDecoder = decoder,
        ).also {
            it.setOrientation(orientationDegrees(resources.configuration))
            it.start()
        }
    }

    /** A session's stream parameters (network thread): keep the panel in step with its rate. */
    private fun onStreamConfig(config: Config) {
        val hz = (config.fpsMilliHz / 1000.0).roundToInt()
        runOnUiThread {
            if (hz == streamHz) return@runOnUiThread
            streamHz = hz
            applyPanelRefresh()
        }
    }

    // Rotating the tablet rotates the Windows display to match, like a pivoting monitor. The
    // stream keeps running: the host switches orientation and sends CONFIG with the new rotation.
    override fun onConfigurationChanged(newConfig: Configuration) {
        super.onConfigurationChanged(newConfig)
        client?.setOrientation(orientationDegrees(newConfig))
    }

    private fun orientationDegrees(config: Configuration): Int =
        if (config.orientation == Configuration.ORIENTATION_PORTRAIT) 90 else 0

    /** A USB session handed us the PC's pairing code: remember it for Wi-Fi. */
    private fun onPaired(info: PairingInfo) {
        val known = pairing.load()
        if (known?.code == Pairing.normalize(info.code) && known?.hostName == info.hostName) return
        if (pairing.save(info.code, info.hostName)) {
            Log.i(TAG, "paired with ${info.hostName}")
            runOnUiThread { updatePairingLabel() }
        }
    }

    private fun updatePairingLabel() {
        val paired = pairing.load()
        pairingLabel.text = when {
            paired == null -> getString(R.string.pairing_none)
            paired.hostName.isEmpty() -> getString(R.string.pairing_code_entered)
            else -> getString(R.string.pairing_host, paired.hostName)
        }
    }

    private fun showStatus(text: String?) {
        runOnUiThread {
            status.text = text.orEmpty()
            status.visibility = if (text == null) View.GONE else View.VISIBLE
            streaming = text == null
            if (!streaming) {
                settingsOpened = false
                mediaState = null // the next session tells us again
            }
            updateSettingsVisibility()
        }
    }

    // --- Stream settings -------------------------------------------------------------------

    private fun buildSettings(): LinearLayout {
        refreshSpinner = Spinner(this)
        updateRefreshChoices(resolutionIndexOf(mode), mode.refreshHz)

        val resolutionLabels = listOf(getString(R.string.setting_auto)) + modeOptions.map {
            getString(if (it.native) R.string.setting_native_size else R.string.setting_size, it.width, it.height)
        }
        resolutionSpinner = Spinner(this).apply {
            adapter = spinnerAdapter(resolutionLabels)
            setSelection(resolutionIndexOf(mode))
            onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
                override fun onItemSelected(parent: AdapterView<*>?, view: View?, position: Int, id: Long) {
                    // Refresh rates depend on the size; keep the picked rate if it's still offered.
                    updateRefreshChoices(position, selectedRefreshHz())
                }

                override fun onNothingSelected(parent: AdapterView<*>?) = Unit
            }
        }

        codecChoices = listOf(StreamCodec.AUTO, StreamCodec.H264) +
            if (hevcDecoder) listOf(StreamCodec.HEVC) else emptyList()
        codecSpinner = Spinner(this).apply {
            adapter = spinnerAdapter(codecChoices.map { getString(codecLabel(it)) })
            setSelection(codecChoices.indexOf(mode.codec).coerceAtLeast(0))
        }

        val connectionLabels = listOf(getString(R.string.connection_usb)) +
            if (wifiSupported) listOf(getString(R.string.connection_wifi)) else emptyList()
        connectionSpinner = Spinner(this).apply {
            adapter = spinnerAdapter(connectionLabels)
            setSelection(if (useWifi) 1 else 0)
        }
        pairingLabel = TextView(this).apply {
            setTextColor(Color.GRAY)
        }
        updatePairingLabel()
        codeField = EditText(this).apply {
            setHint(R.string.pairing_code_hint)
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_CAP_CHARACTERS or
                InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS
            setSingleLine()
            setTextColor(Color.WHITE)
            setHintTextColor(Color.GRAY)
            minWidth = dp(260)
        }

        val dockSwitch = Switch(this).apply {
            isChecked = dockEnabled
            setOnCheckedChangeListener { _, checked ->
                dockEnabled = checked
                getSharedPreferences(PREFS, MODE_PRIVATE).edit().putBoolean(KEY_DOCK, checked).apply()
                updateDockVisibility()
            }
        }

        val statsSwitch = Switch(this).apply {
            isChecked = statsEnabled
            setOnCheckedChangeListener { _, checked ->
                statsEnabled = checked
                getSharedPreferences(PREFS, MODE_PRIVATE).edit().putBoolean(KEY_STATS, checked).apply()
                updateStatsVisibility()
            }
        }

        val apply = Button(this).apply {
            setText(R.string.setting_apply)
            setOnClickListener { applySettings() }
        }
        val hint = TextView(this).apply {
            setText(R.string.setting_hint)
            setTextColor(Color.GRAY)
        }

        return LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(Color.argb(0xE0, 0x20, 0x20, 0x20))
            setPadding(dp(24), dp(16), dp(24), dp(16))
            addView(settingRow(R.string.setting_resolution, resolutionSpinner))
            addView(settingRow(R.string.setting_refresh, refreshSpinner))
            addView(settingRow(R.string.setting_codec, codecSpinner))
            addView(settingRow(R.string.setting_connection, connectionSpinner))
            addView(settingRow(R.string.setting_dock, dockSwitch))
            addView(settingRow(R.string.setting_stats, statsSwitch))
            if (wifiSupported) {
                addView(pairingLabel)
                addView(codeField)
            }
            addView(apply)
            addView(hint)
        }
    }

    private fun settingRow(label: Int, spinner: View): LinearLayout =
        LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            addView(TextView(this@MainActivity).apply {
                setText(label)
                setTextColor(Color.LTGRAY)
                textSize = 16f
                minWidth = dp(120)
            })
            addView(spinner)
        }

    private fun spinnerAdapter(labels: List<String>): ArrayAdapter<String> =
        ArrayAdapter(this, android.R.layout.simple_spinner_item, labels).apply {
            setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item)
        }

    /** Rates offered for the resolution at [resolutionIndex] ("Auto" = the native size's). */
    private fun ratesFor(resolutionIndex: Int): List<Int> =
        (modeOptions.getOrNull(resolutionIndex - 1) ?: modeOptions.firstOrNull { it.native })?.refreshRates.orEmpty()

    private fun updateRefreshChoices(resolutionIndex: Int, keepHz: Int) {
        refreshChoices = ratesFor(resolutionIndex)
        val labels = listOf(getString(R.string.setting_auto)) + refreshChoices.map { getString(R.string.setting_hz, it) }
        refreshSpinner.adapter = spinnerAdapter(labels)
        refreshSpinner.setSelection(refreshChoices.indexOf(keepHz) + 1) // not offered -> 0 (Auto)
    }

    private fun selectedRefreshHz(): Int = refreshChoices.getOrNull(refreshSpinner.selectedItemPosition - 1) ?: 0

    private fun codecLabel(codec: StreamCodec): Int = when (codec) {
        StreamCodec.AUTO -> R.string.setting_auto
        StreamCodec.H264 -> R.string.codec_h264
        StreamCodec.HEVC -> R.string.codec_hevc
    }

    private fun resolutionIndexOf(m: StreamMode): Int =
        modeOptions.indexOfFirst { it.width == m.width && it.height == m.height } + 1 // -1 -> 0 (Auto)

    private fun applySettings() {
        val typed = codeField.text.toString()
        var codeChanged = false
        if (typed.isNotBlank()) {
            if (Pairing.normalize(typed) == null) {
                codeField.error = getString(R.string.pairing_code_invalid)
                return
            }
            codeChanged = pairing.save(typed, "")
            codeField.text.clear()
            updatePairingLabel()
        }

        val option = modeOptions.getOrNull(resolutionSpinner.selectedItemPosition - 1)
        val picked = StreamMode(
            option?.width ?: 0,
            option?.height ?: 0,
            selectedRefreshHz(),
            codecChoices.getOrElse(codecSpinner.selectedItemPosition) { StreamCodec.AUTO },
        )
        val pickedWifi = connectionSpinner.selectedItemPosition == 1
        settingsOpened = false
        updateSettingsVisibility()
        if (picked == mode && pickedWifi == useWifi && !codeChanged) return

        Log.i(TAG, "stream mode $mode -> $picked, wifi $useWifi -> $pickedWifi")
        mode = picked
        useWifi = pickedWifi
        saveMode()
        applyPanelRefresh()
        // Reconnect with a new HELLO: the host unplugs the monitor and plugs one in the new mode.
        val holder = surfaceHolder ?: return
        client?.stop()
        startClient(holder)
    }

    private fun updateSettingsVisibility() {
        settings.visibility = if (!streaming || settingsOpened) View.VISIBLE else View.GONE
        updateDockVisibility()
        updateStatsVisibility()
    }

    private fun updateStatsVisibility() {
        statsOverlay.setShown(statsEnabled && streaming)
    }

    private fun updateDockVisibility() {
        val show = dockEnabled && streaming && !settingsOpened && mediaState != null
        dock.visibility = if (show) View.VISIBLE else View.GONE
    }

    /**
     * Asks for the panel mode at the stream's rate (the picked one, else the host's from CONFIG),
     * so frames aren't shown on a mismatched refresh. Only a request: some OEM policies still
     * decide themselves (ColorOS on the Pad Go stays at 60 Hz unless the screen is touched).
     */
    private fun applyPanelRefresh() {
        val hz = if (mode.refreshHz != 0) mode.refreshHz else streamHz
        val params = window.attributes
        val modeId = StreamModes.panelModeIdFor(this, hz)
        if (params.preferredDisplayModeId != modeId) {
            params.preferredDisplayModeId = modeId
            window.attributes = params
        }
        // Also tell the compositor the video's own rate (Android 11+); frame rate votes from
        // decoder surfaces otherwise don't count, as the panel only sees the app as idle.
        val surface = surfaceHolder?.surface
        if (hz > 0 && surface != null && surface.isValid && Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            try {
                surface.setFrameRate(hz.toFloat(), Surface.FRAME_RATE_COMPATIBILITY_FIXED_SOURCE)
            } catch (e: IllegalArgumentException) {
                Log.w(TAG, "setFrameRate($hz) rejected", e)
            }
        }
        Log.i(TAG, "panel: stream $hz Hz -> mode $modeId (${StreamModes.refreshHzOfMode(this, modeId)} Hz)")
    }

    private fun loadMode(): StreamMode {
        val prefs = getSharedPreferences(PREFS, MODE_PRIVATE)
        val codec = StreamCodec.entries.firstOrNull { it.name == prefs.getString(KEY_CODEC, null) } ?: StreamCodec.AUTO
        var saved = StreamMode(
            prefs.getInt(KEY_WIDTH, 0),
            prefs.getInt(KEY_HEIGHT, 0),
            prefs.getInt(KEY_REFRESH, 0),
            codec,
        )
        // Drop anything this device no longer offers (e.g. restored from another device).
        val index = resolutionIndexOf(saved)
        if (index == 0) saved = saved.copy(width = 0, height = 0)
        if (saved.refreshHz !in ratesFor(index)) saved = saved.copy(refreshHz = 0)
        if (saved.codec == StreamCodec.HEVC && !hevcDecoder) saved = saved.copy(codec = StreamCodec.AUTO)
        return saved
    }

    private fun saveMode() {
        getSharedPreferences(PREFS, MODE_PRIVATE).edit()
            .putInt(KEY_WIDTH, mode.width)
            .putInt(KEY_HEIGHT, mode.height)
            .putInt(KEY_REFRESH, mode.refreshHz)
            .putString(KEY_CODEC, mode.codec.name)
            .putBoolean(KEY_WIFI, useWifi)
            .apply()
    }

    private fun dp(value: Int): Int = (value * resources.displayMetrics.density).toInt()

    companion object {
        const val TAG = "DeuxDisplay"
        private const val PREFS = "stream"
        private const val KEY_WIDTH = "width"
        private const val KEY_HEIGHT = "height"
        private const val KEY_REFRESH = "refresh_hz"
        private const val KEY_WIFI = "wifi"
        private const val KEY_CODEC = "codec"
        private const val KEY_DOCK = "dock"
        private const val KEY_STATS = "debug_stats"
        private const val DOCK_PREFS = "dock"

        /** Joining the PC's network needs WifiNetworkSpecifier (Android 10). */
        private val wifiSupported = Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q
    }
}
