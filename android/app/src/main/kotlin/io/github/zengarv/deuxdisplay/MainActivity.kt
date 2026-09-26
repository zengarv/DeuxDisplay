package io.github.zengarv.deuxdisplay

import android.app.Activity
import android.graphics.Color
import android.os.Build
import android.os.Bundle
import android.util.Log
import android.view.Gravity
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.view.WindowInsets
import android.view.WindowInsetsController
import android.view.WindowManager
import android.widget.FrameLayout
import android.widget.TextView
import io.github.zengarv.deuxdisplay.stream.DisplayInfo
import io.github.zengarv.deuxdisplay.stream.StreamClient

class MainActivity : Activity(), SurfaceHolder.Callback {

    private lateinit var status: TextView
    private var client: StreamClient? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)

        val surfaceView = SurfaceView(this)
        surfaceView.holder.addCallback(this)

        status = TextView(this).apply {
            setTextColor(Color.LTGRAY)
            textSize = 18f
            gravity = Gravity.CENTER
        }

        setContentView(
            FrameLayout(this).apply {
                setBackgroundColor(Color.BLACK)
                addView(surfaceView)
                addView(status, FrameLayout.LayoutParams(
                    FrameLayout.LayoutParams.MATCH_PARENT,
                    FrameLayout.LayoutParams.MATCH_PARENT,
                ))
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

    // The stream lives exactly as long as the surface: when the tablet sleeps or the app goes to
    // the background, the host unplugs the virtual monitor and Windows moves the windows back.
    override fun surfaceCreated(holder: SurfaceHolder) {
        Log.i(TAG, "surface created")
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
        Log.i(TAG, "surface ${width}x$height")
        if (client == null) {
            val hello = DisplayInfo.hello(this)
            // adb shell am start -n io.github.zengarv.deuxdisplay/.MainActivity --ez debug_no_surface true
            // ... --es debug_decoder c2.android.avc.decoder   (force a specific decoder)
            val noSurface = intent.getBooleanExtra("debug_no_surface", false)
            val decoder = intent.getStringExtra("debug_decoder")
            client = StreamClient(
                hello,
                holder.surface,
                ::showStatus,
                decodeWithoutSurface = noSurface,
                forcedDecoder = decoder,
            ).also { it.start() }
        }
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        Log.i(TAG, "surface destroyed")
        client?.stop()
        client = null
    }

    private fun showStatus(text: String?) {
        runOnUiThread {
            status.text = text.orEmpty()
            status.visibility = if (text == null) View.GONE else View.VISIBLE
        }
    }

    companion object {
        const val TAG = "DeuxDisplay"
    }
}
