package com.xcam

import android.Manifest
import android.app.Activity
import android.content.ClipData
import android.content.ClipboardManager
import android.content.Intent
import android.content.pm.PackageManager
import android.graphics.Color
import android.graphics.drawable.GradientDrawable
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.PowerManager
import android.provider.Settings
import android.util.TypedValue
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.Button
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.TextView
import android.widget.Toast

/**
 * The phone screen.
 *
 * Deliberately thin in what it *controls*: this is a capture device, and every
 * camera setting is driven from the desktop over the control channel. What it
 * is not thin in is what it *reports*. Someone glancing at a phone propped on a
 * desk needs to know, from across the room, whether it is running, whether the
 * desktop has found it, and whether it is recording -- and if it is not
 * running, what to type on the other machine.
 *
 * Built in code rather than XML, matching the rest of the project. Colours come
 * from res/values/colors.xml, which is the same palette as the desktop panel.
 */
class MainActivity : Activity() {

    private lateinit var stateDot: View
    private lateinit var stateWord: TextView
    private lateinit var stateDetail: TextView
    private lateinit var numbers: TextView
    private lateinit var permissionRow: TextView
    private lateinit var toggle: Button
    private lateinit var addressText: TextView
    private lateinit var copyButton: Button
    private lateinit var wifiButton: Button
    private lateinit var codeText: TextView
    private lateinit var newCodeButton: Button
    private lateinit var battery: Button

    private var streaming = false
    private var address: String? = null

    private val ticker = Handler(Looper.getMainLooper())
    private val tick = object : Runnable {
        override fun run() {
            refresh()
            ticker.postDelayed(this, 1000)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(buildUi())
        requestPermissionsIfNeeded()

        // Lets the desktop side bring capture up without anyone touching the
        // phone:  adb shell am start -n com.xcam/.MainActivity --ez autostart true
        // The service itself stays unexported, so this is the only way in and it
        // still runs under this app's own uid.
        handleAutostart(intent)
    }

    /**
     * `am start` against an activity that is already up delivers onNewIntent,
     * not onCreate, so without this the autostart extra is silently ignored on
     * every launch after the first.
     */
    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        handleAutostart(intent)
    }

    private fun handleAutostart(intent: Intent?) {
        if (intent?.getBooleanExtra(EXTRA_AUTOSTART, false) == true && !streaming) startCapture()
    }

    override fun onResume() {
        super.onResume()
        // The numbers only matter while someone is looking at them.
        ticker.removeCallbacks(tick)
        ticker.post(tick)
        refreshWifiRow()
    }

    /**
     * Says which of the two states the phone is in, in words rather than in the
     * position of a switch.
     *
     * Switched off is the interesting one to word well: "nothing on the network
     * can see it" is a promise the kernel is keeping -- the listener is bound to
     * loopback -- and somebody deciding whether to leave this running on a cafe
     * Wi-Fi deserves to be told that plainly.
     */
    private fun refreshWifiRow() {
        val on = com.xcam.net.Pairing.wifiAllowed(this)

        wifiButton.text = getString(if (on) R.string.btn_wifi_off else R.string.btn_wifi_on)
        wifiButton.background = pill(colour(if (on) R.color.signal else R.color.text_dim),
                                     outlineOnly = !on)
        wifiButton.setTextColor(colour(if (on) R.color.ink else R.color.text_dim))

        codeText.text = if (on) getString(R.string.hint_wifi_on, com.xcam.net.Pairing.code(this))
                        else getString(R.string.hint_wifi_off)
        codeText.setTextColor(colour(if (on) R.color.text else R.color.text_dim))

        newCodeButton.visibility = if (on) View.VISIBLE else View.GONE

        // The address is only worth showing when something could use it.
        val reachable = on && address != null
        addressText.visibility = if (reachable) View.VISIBLE else View.GONE
        copyButton.visibility = if (reachable) View.VISIBLE else View.GONE
    }

    override fun onPause() {
        super.onPause()
        ticker.removeCallbacks(tick)
    }

    // ---- layout --------------------------------------------------------

    private fun dp(value: Float) = TypedValue.applyDimension(
        TypedValue.COMPLEX_UNIT_DIP, value, resources.displayMetrics).toInt()

    private fun colour(id: Int) = resources.getColor(id, theme)

    private fun buildUi(): ViewGroup {
        val pad = dp(24f)

        // ---- header: the mark and the wordmark -----------------------------

        val mark = ImageView(this).apply {
            setImageResource(R.drawable.ic_mark)
            layoutParams = LinearLayout.LayoutParams(dp(28f), dp(28f))
        }
        val wordmark = TextView(this).apply {
            text = getString(R.string.app_name).uppercase()
            textSize = 15f
            letterSpacing = 0.34f
            setTextColor(colour(R.color.text_dim))
            setPadding(dp(10f), 0, 0, 0)
        }
        val header = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            addView(mark)
            addView(wordmark)
        }

        // ---- the state, which is what anyone reads first -------------------

        stateDot = View(this).apply {
            layoutParams = LinearLayout.LayoutParams(dp(12f), dp(12f)).apply {
                rightMargin = dp(12f)
            }
        }
        stateWord = TextView(this).apply {
            textSize = 30f
            letterSpacing = 0.06f
            setTextColor(colour(R.color.text))
        }
        val stateLine = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            addView(stateDot)
            addView(stateWord)
        }

        stateDetail = TextView(this).apply {
            textSize = 14f
            setTextColor(colour(R.color.text_dim))
            setPadding(dp(24f), dp(6f), 0, 0)
        }

        // Four numbers, and only while there is something to count.
        numbers = TextView(this).apply {
            textSize = 13f
            typeface = android.graphics.Typeface.MONOSPACE
            setTextColor(colour(R.color.text_dim))
            setPadding(dp(24f), dp(14f), 0, 0)
            visibility = View.GONE
        }

        val stateCard = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            background = card(colour(R.color.graphite))
            setPadding(pad, pad, pad, pad)
            addView(stateLine)
            addView(stateDetail)
            addView(numbers)
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT
            ).apply { topMargin = dp(20f) }
        }

        // A missing permission is silent everywhere else: a denied microphone
        // produces working silence rather than an error, which this project has
        // already been caught by twice.
        permissionRow = TextView(this).apply {
            textSize = 13f
            setTextColor(colour(R.color.warn))
            background = card(Color.parseColor("#22FF6B5B"))
            setPadding(dp(16f), dp(12f), dp(16f), dp(12f))
            visibility = View.GONE
            setOnClickListener { requestPermissionsIfNeeded() }
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT
            ).apply { topMargin = dp(12f) }
        }

        // ---- how the desktop reaches this phone ----------------------------

        address = com.xcam.camera.CameraEnumerator
            .describeDevice(this).optString("wifiAddress").ifEmpty { null }

        val usbLine = TextView(this).apply {
            text = getString(R.string.hint_usb)
            textSize = 13f
            setTextColor(colour(R.color.text_dim))
        }

        addressText = TextView(this).apply {
            // The command, at a size that can be read from arm's length. This
            // is the one thing the desktop cannot work out for itself, so it is
            // the most legible thing on the screen after the state.
            text = address?.let { getString(R.string.hint_wifi_cmd, it) }
                ?: getString(R.string.hint_no_wifi)
            textSize = if (address != null) 15f else 13f
            typeface = android.graphics.Typeface.MONOSPACE
            setTextColor(colour(if (address != null) R.color.text else R.color.text_dim))
            setPadding(0, dp(10f), 0, 0)
        }

        copyButton = Button(this).apply {
            text = getString(R.string.btn_copy)
            visibility = if (address != null) View.VISIBLE else View.GONE
            setOnClickListener {
                val command = getString(R.string.hint_wifi_cmd, address)
                (getSystemService(CLIPBOARD_SERVICE) as ClipboardManager)
                    .setPrimaryClip(ClipData.newPlainText("XCam", command))
                Toast.makeText(this@MainActivity, R.string.copied, Toast.LENGTH_SHORT).show()
            }
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT
            ).apply { topMargin = dp(8f) }
        }

        // ---- who else may reach this phone ---------------------------------
        //
        // Off means the listener is bound to loopback, so this is not a filter
        // with a switch on it -- switched off, there is nothing on the network
        // to find. The line underneath says which of the two states it is in,
        // because "can anyone else see my camera" is not a question anybody
        // should have to infer from a button.

        codeText = TextView(this).apply {
            textSize = 13f
            typeface = android.graphics.Typeface.MONOSPACE
            setPadding(0, dp(8f), 0, 0)
        }

        wifiButton = Button(this).apply {
            setOnClickListener {
                val now = !com.xcam.net.Pairing.wifiAllowed(this@MainActivity)
                com.xcam.net.Pairing.setWifiAllowed(this@MainActivity, now)
                refreshWifiRow()
            }
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT
            ).apply { topMargin = dp(12f) }
        }

        newCodeButton = Button(this).apply {
            text = getString(R.string.btn_new_code)
            background = pill(colour(R.color.text_dim), outlineOnly = true)
            setTextColor(colour(R.color.text_dim))
            setOnClickListener {
                com.xcam.net.Pairing.rotate(this@MainActivity)
                refreshWifiRow()
                Toast.makeText(this@MainActivity, R.string.code_rotated, Toast.LENGTH_LONG).show()
            }
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT
            ).apply { topMargin = dp(8f) }
        }

        val connect = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            background = card(colour(R.color.graphite))
            setPadding(pad, pad, pad, pad)
            addView(sectionLabel(getString(R.string.label_connect)))
            addView(usbLine)
            addView(addressText)
            addView(copyButton)
            addView(wifiButton)
            addView(codeText)
            addView(newCodeButton)
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT
            ).apply { topMargin = dp(12f) }
        }

        // ---- the one control this screen has -------------------------------

        toggle = Button(this).apply {
            setOnClickListener { if (streaming) stopCapture() else startCapture() }
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, dp(56f)
            ).apply { topMargin = dp(20f) }
        }

        battery = Button(this).apply {
            text = getString(R.string.btn_battery)
            background = pill(colour(R.color.text_dim), outlineOnly = true)
            setTextColor(colour(R.color.text_dim))
            setOnClickListener { requestIgnoreBatteryOptimizations() }
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT
            ).apply { topMargin = dp(8f) }
        }

        // Everything that reports sits at the top and everything that acts
        // sits at the bottom, where a thumb already is.
        val spacer = View(this).apply {
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f)
        }

        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(pad, pad, pad, pad)
            setBackgroundColor(colour(R.color.ink))
            addView(header)
            addView(stateCard)
            addView(permissionRow)
            addView(connect)
            addView(spacer)
            addView(toggle)
            addView(battery)
        }

        // The window draws under the status bar, so without this the mark ends
        // up behind the clock. Asked for rather than assumed: a status bar is
        // not a fixed height, and on this phone it is not even a fixed shape.
        root.setOnApplyWindowInsetsListener { view, insets ->
            val bars = insets.getInsets(
                android.view.WindowInsets.Type.systemBars() or
                android.view.WindowInsets.Type.displayCutout())
            view.setPadding(pad, pad + bars.top, pad, pad + bars.bottom)
            insets
        }
        return root
    }

    private fun sectionLabel(text: String) = TextView(this).apply {
        this.text = text.uppercase()
        textSize = 10.5f
        letterSpacing = 0.18f
        setTextColor(colour(R.color.text_dim))
        setPadding(0, 0, 0, dp(10f))
    }

    private fun card(fill: Int) = GradientDrawable().apply {
        setColor(fill)
        cornerRadius = dp(14f).toFloat()
    }

    private fun dot(fill: Int) = GradientDrawable().apply {
        shape = GradientDrawable.OVAL
        setColor(fill)
    }

    private fun pill(fill: Int, outlineOnly: Boolean) = GradientDrawable().apply {
        cornerRadius = dp(12f).toFloat()
        if (outlineOnly) {
            setColor(Color.TRANSPARENT)
            setStroke(dp(1.5f), fill)
        } else {
            setColor(fill)
        }
    }

    // ---- state ---------------------------------------------------------

    /**
     * The microphone as a level rather than a word.
     *
     * It used to say "sound" whenever capture was configured, which is exactly
     * what it said both times this project shipped a microphone that produced
     * nothing -- and "silent" already meant "no sound configured", so the one
     * word that would have been the warning was taken. A bar that does not move
     * cannot be misread that way.
     */
    private fun micLabel(live: CaptureService.Live): String {
        if (!live.audio) return getString(R.string.audio_off)
        if (live.audioSilentMs >= 3000) return getString(R.string.audio_dead)

        // Square-rooted: speech at a sane level sits near a tenth of full scale,
        // and a linear bar draws that as nothing at all.
        val filled = (kotlin.math.sqrt(live.audioPeak.coerceIn(0f, 1f)) * BARS).toInt()
        return buildString {
            repeat(BARS) { append(if (it < filled) '█' else '░') }
        }
    }

    /**
     * Redraws from [CaptureService.Live]. Called once a second while visible,
     * which is the rate the service updates at -- polling faster would only
     * show the same numbers again.
     */
    private fun refresh() {
        val live = CaptureService.Live
        streaming = live.running

        val (word, colourId) = when {
            live.recording -> getString(R.string.state_recording) to R.color.record
            // Above STREAMING, and in the same red as recording, because it
            // carries the same kind of fact: something is happening that the
            // person in front of the camera would want to know about before
            // they do anything else.
            live.tally -> getString(R.string.state_on_air) to R.color.record
            live.clientConnected -> getString(R.string.state_streaming) to R.color.good
            live.running -> getString(R.string.state_waiting) to R.color.signal
            !hasCameraPermission() -> getString(R.string.state_no_camera) to R.color.warn
            else -> getString(R.string.state_idle) to R.color.text_dim
        }

        stateWord.text = word
        stateWord.setTextColor(colour(if (colourId == R.color.text_dim) R.color.text_dim
                                      else R.color.text))
        stateDot.background = dot(colour(colourId))

        stateDetail.text = when {
            // Said even while recording, when the word above is RECORDING and
            // this is the only place it can be said.
            live.tally -> getString(R.string.detail_on_air, live.width, live.height)
            live.clientConnected -> getString(R.string.detail_streaming,
                                              live.width, live.height)
            live.running -> getString(R.string.detail_waiting)
            else -> getString(R.string.detail_idle)
        }

        if (live.clientConnected) {
            numbers.visibility = View.VISIBLE
            numbers.text = getString(
                R.string.live_numbers,
                live.fps, live.mbps,
                micLabel(live),
                live.battery, live.thermal)
        } else {
            numbers.visibility = View.GONE
        }

        // Say which permission is missing, not merely that one is.
        val missing = buildList {
            if (!hasCameraPermission()) add(getString(R.string.perm_camera))
            if (!hasAudioPermission()) add(getString(R.string.perm_microphone))
        }
        if (missing.isEmpty()) {
            permissionRow.visibility = View.GONE
        } else {
            permissionRow.visibility = View.VISIBLE
            permissionRow.text = getString(R.string.perm_missing, missing.joinToString(", "))
        }

        toggle.text = getString(if (streaming) R.string.btn_stop else R.string.btn_start)
        toggle.background = pill(colour(R.color.signal), outlineOnly = streaming)
        toggle.setTextColor(colour(if (streaming) R.color.signal else R.color.ink))

        val pm = getSystemService(PowerManager::class.java)
        battery.visibility =
            if (pm.isIgnoringBatteryOptimizations(packageName)) View.GONE else View.VISIBLE
    }

    private fun startCapture() {
        if (!hasCameraPermission()) {
            requestPermissionsIfNeeded()
            return
        }
        startForegroundService(Intent(this, CaptureService::class.java))
        streaming = true
        refresh()
    }

    private fun stopCapture() {
        startService(
            Intent(this, CaptureService::class.java).apply { action = CaptureService.ACTION_STOP }
        )
        streaming = false
        refresh()
    }

    // ---- permissions ---------------------------------------------------

    private fun hasCameraPermission() =
        checkSelfPermission(Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED

    /**
     * The microphone is asked for but never required. A refused microphone
     * leaves a perfectly good silent webcam; refusing to start over it would
     * turn an optional feature into a blocker.
     */
    private fun hasAudioPermission() =
        checkSelfPermission(Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED

    private fun requestPermissionsIfNeeded() {
        val wanted = mutableListOf<String>()
        if (!hasCameraPermission()) wanted += Manifest.permission.CAMERA
        if (!hasAudioPermission()) wanted += Manifest.permission.RECORD_AUDIO
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU &&
            checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) !=
            PackageManager.PERMISSION_GRANTED
        ) {
            wanted += Manifest.permission.POST_NOTIFICATIONS
        }
        if (wanted.isNotEmpty()) requestPermissions(wanted.toTypedArray(), REQ_PERMS)
    }

    override fun onRequestPermissionsResult(
        requestCode: Int,
        permissions: Array<out String>,
        grantResults: IntArray,
    ) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        refresh()
    }

    /**
     * HyperOS is aggressive about background services; without this exemption the
     * capture service is killed within minutes of the screen turning off.
     */
    @Suppress("BatteryLife")
    private fun requestIgnoreBatteryOptimizations() {
        val pm = getSystemService(PowerManager::class.java)
        if (pm.isIgnoringBatteryOptimizations(packageName)) return
        try {
            startActivity(
                Intent(Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS)
                    .setData(Uri.parse("package:$packageName"))
            )
        } catch (_: Throwable) {
            startActivity(Intent(Settings.ACTION_IGNORE_BATTERY_OPTIMIZATION_SETTINGS))
        }
    }

    companion object {
        /** Blocks in the level bar. Eight reads as a meter and still fits
         *  the line the other numbers share. */
        private const val BARS = 8

        private const val REQ_PERMS = 1001

        /** Boolean intent extra: start capturing as soon as the activity opens. */
        const val EXTRA_AUTOSTART = "autostart"
    }
}
