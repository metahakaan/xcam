package com.xcam

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.media.MediaMetadataRetriever
import android.os.BatteryManager
import android.os.Build
import android.os.Handler
import android.os.HandlerThread
import android.os.IBinder
import android.os.PowerManager
import android.util.Log
import com.xcam.camera.CameraEnumerator
import com.xcam.camera.CameraSession
import com.xcam.codec.AudioEncoder
import com.xcam.codec.Recorder
import com.xcam.codec.VideoEncoder
import com.xcam.net.Beacon
import com.xcam.net.FrameServer
import com.xcam.net.Pairing
import java.io.File
import org.json.JSONArray
import org.json.JSONObject

/**
 * Owns the whole capture pipeline for as long as streaming is active. Runs in the
 * foreground so HyperOS does not reclaim it when the screen goes off.
 *
 * Everything that touches the camera or encoder is funnelled onto [controlHandler]
 * so socket callbacks and UI actions cannot reconfigure concurrently.
 */
class CaptureService : Service() {

    private lateinit var server: FrameServer

    /**
     * Announces this phone while it is available. Runs for as long as the
     * service does, not just while a client is attached -- the whole point is
     * to be found before anyone has connected.
     */
    private var beacon: Beacon? = null
    private var camera: CameraSession? = null
    private var encoder: VideoEncoder? = null
    private var recorder: Recorder? = null
    private var audio: AudioEncoder? = null

    /**
     * The audio track's format, kept at the service rather than the recorder.
     *
     * Sound belongs to the client session, not to a pipeline run: it does not
     * depend on the camera's resolution and restarting it on every format change
     * would put an audible gap in the middle of a call. So it outlives the
     * recorder, and each new recorder is handed the format on the way up.
     */
    @Volatile private var audioFormat: android.media.MediaFormat? = null
    @Volatile private var audioEnabled = true

    /**
     * Which system clock the camera stamps frames with, decided by measurement
     * rather than by asking.
     *
     * Audio has to be timed against the same clock as the video or the two do
     * not line up, and the candidates -- System.nanoTime and
     * SystemClock.elapsedRealtimeNanos -- differ by however long the device has
     * been suspended. On this phone that was five days: the first recording with
     * sound had its audio track starting 429496 seconds after its video.
     *
     * SENSOR_INFO_TIMESTAMP_SOURCE is supposed to say which, and got it wrong
     * here. Comparing a real camera timestamp against both clocks cannot: they
     * are days apart, so the nearer one is the right one by an enormous margin.
     */
    @Volatile private var cameraClockIsBootTime = false
    @Volatile private var cameraClockKnown = false
    private var wakeLock: PowerManager.WakeLock? = null

    private val controlThread = HandlerThread("xcam-control").apply { start() }
    private val controlHandler = Handler(controlThread.looper)

    @Volatile private var settings = Settings()
    @Volatile private var frameSeq = 0

    /**
     * Timestamp origin for the whole client session, not for one pipeline run.
     *
     * The protocol promises ptsUs is monotonic, and restarting it on every
     * reconfiguration breaks that: the receiver sees the clock jump backwards by
     * however long the session has been up, which makes any latency measured
     * against it read as the session's entire runtime. Camera timestamps are
     * already monotonic across a restart, so anchoring once per client keeps the
     * promise for free.
     */
    @Volatile private var basePtsUs = -1L

    // Rolling counters for the STATS packet.
    @Volatile private var framesSinceTick = 0
    @Volatile private var bytesSinceTick = 0L
    @Volatile private var lastTickNanos = 0L

    data class Settings(
        val cameraId: String? = null,
        val width: Int = 1920,
        val height: Int = 1080,
        val fps: Int = 30,
        val bitrate: Int = 40_000_000,
        val codec: String = "h264",
    )

    /**
     * The local recording format, which is deliberately not the streaming one.
     * Defaults to the best mode the chosen camera offers -- the file is the
     * point of the exercise, so it should not inherit a size picked to keep a
     * USB link comfortable.
     */
    data class RecordSettings(
        // Whether a recording encoder exists at all. Having one ready costs
        // about five milliseconds of stream latency even while it sits idle --
        // it is a second output the camera has to configure -- so it can be
        // turned off entirely by anyone who only wants a webcam.
        val enabled: Boolean = true,

        /**
         * Where the file is written. The desktop is the default: it is where
         * the person using the recording already is, and the alternative leaves
         * a file to be collected later. The phone remains the answer when the
         * link cannot carry a second stream -- over Wi-Fi, or on a cable that
         * will not do 180 Mbit/s.
         */
        val toPc: Boolean = true,
        val width: Int = 0,          // 0 means "the camera's best"
        val height: Int = 0,
        val fps: Int = 0,            // 0 means "as fast as the stream"
        val bitrate: Int = 120_000_000,
        val codec: String = "hevc",

        /**
         * Seconds of footage kept ready so a take can begin before the button
         * was pressed. Zero is off, and off is the default.
         *
         * Arming this means the encoder runs whenever the camera does, which is
         * battery and heat -- exactly the cost the recorder was built to avoid
         * paying while idle. It buys the one thing that cannot be bought later:
         * a moment nobody knew to record.
         */
        val preRollSeconds: Int = 0,
    )

    @Volatile private var recordSettings = RecordSettings()

    /**
     * The rate the pipeline is actually running at, which is not always the one
     * in [settings]: a recording format the sensor cannot drive that fast pulls
     * it down. Every answer to the client quotes this rather than the request.
     */
    @Volatile private var activeStreamFps = 0

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        startForegroundNotification(getString(R.string.status_waiting))
        acquireWakeLock()

        server = FrameServer(
            handshakeJson = { resumed ->
                CameraEnumerator.describeDevice(this).apply {
                    // Says "everything you configured is still running". A
                    // client that answered this with a format command would
                    // restart the pipeline and undo the hand-over it just made.
                    put("resumed", resumed)
                }.toString()
            },
            onControl = { json -> controlHandler.post { handleControl(json) } },
            wifiAllowed = { Pairing.wifiAllowed(this) },
            checkPairing = { code -> Pairing.check(this, code) },
            onClientChange = { connected, resumed ->
                controlHandler.post {
                    if (connected && resumed) {
                        // The same session on the other transport. Everything
                        // that was running is still running, and restarting it
                        // is precisely what a hand-over must not do -- a new
                        // encoder session would cost several seconds of black,
                        // a fresh timestamp base, and every manual camera
                        // setting re-applied from the template.
                        //
                        // What the new socket does need is the two things the
                        // encoders only say once, and a frame it can decode
                        // from: the mid-GOP frames it would otherwise receive
                        // reference pictures that went out over the dead one.
                        updateNotification(getString(R.string.status_streaming))
                        encoder?.resendConfig()
                        audio?.resendConfig()
                        encoder?.requestKeyFrame()
                        Log.i(TAG, "session resumed on a new connection")
                    } else if (connected) {
                        updateNotification(getString(R.string.status_streaming))
                        basePtsUs = -1L
                        // Audio waits for the first camera frame: until one
                        // arrives there is nothing to measure the clock
                        // against, and guessing is what broke it.
                        startPipeline()
                    } else {
                        updateNotification(getString(R.string.status_waiting))
                        stopPipeline()
                        stopAudio()
                        Live.clear()
                    }
                }
            },
        )
        server.start()
        // Only when the phone is willing to be reached over the network. A
        // beacon is an advertisement, and advertising a port that is bound to
        // loopback tells the room something about this phone in exchange for
        // nothing at all.
        if (Pairing.wifiAllowed(this)) beacon = Beacon(this).apply { start() }
        Live.running = true
        controlHandler.postDelayed(statsTick, STATS_INTERVAL_MS)
        Log.i(TAG, "service created")
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == ACTION_STOP) {
            stopSelf()
            return START_NOT_STICKY
        }
        return START_STICKY
    }

    override fun onDestroy() {
        Live.running = false
        Live.clear()
        controlHandler.removeCallbacksAndMessages(null)
        stopPipeline()
        stopAudio()
        beacon?.stop()
        beacon = null
        server.stop()
        controlThread.quitSafely()
        wakeLock?.let { if (it.isHeld) it.release() }
        Log.i(TAG, "service destroyed")
        super.onDestroy()
    }

    // ---- pipeline ------------------------------------------------------

    private fun startPipeline() {
        stopPipeline()

        val s = settings
        val cameraId = s.cameraId ?: defaultCameraId() ?: run {
            Log.e(TAG, "no camera available")
            return
        }

        frameSeq = 0

        // The recording format is resolved first, because it can hold the whole
        // session back. One sensor feeds both encoders, so it runs at a single
        // rate, and a recording size the sensor can only deliver at 30 caps
        // everything at 30 -- the stream included. Deciding that here and
        // reporting it is the only honest arrangement; the alternative is a
        // frame rate that silently collapses the moment recording starts.
        val plan = planRecording(cameraId, s.fps)
        val recordConfig = plan.record
        val streamFps = plan.streamFps
        activeStreamFps = streamFps
        if (streamFps != s.fps) {
            Log.i(TAG, "stream capped to ${streamFps}fps by the ${recordConfig.height}p " +
                    "recording format")
        }

        val enc = VideoEncoder(
            VideoEncoder.Config(s.width, s.height, streamFps, s.bitrate, s.codec),
            object : VideoEncoder.Sink {
                override fun onConfig(csd: ByteArray) = server.sendConfig(csd)

                override fun onFrame(data: ByteArray, isKeyFrame: Boolean, ptsUs: Long) {
                    if (basePtsUs < 0) {
                        basePtsUs = ptsUs
                        // The clock is a property of the device and only needs
                        // working out once; audio is started per client session,
                        // so the post happens every time.
                        if (!cameraClockKnown) identifyCameraClock(ptsUs)
                        controlHandler.post { startAudio() }
                    }
                    server.sendFrame(data, isKeyFrame, ptsUs - basePtsUs, frameSeq++)
                    framesSinceTick++
                    bytesSinceTick += data.size
                }

                override fun onError(t: Throwable) {
                    Log.e(TAG, "encoder error", t)
                    controlHandler.post { restartPipeline() }
                }
            },
        )

        try {
            enc.start()
        } catch (t: Throwable) {
            Log.e(TAG, "encoder start failed", t)
            return
        }
        encoder = enc

        // The recorder is built before the camera, not on demand, because its
        // surface has to be part of the capture session from the start: adding
        // one later means rebuilding the session, and the stream would black out
        // at the moment someone pressed record. The cost is an idle encoder
        // instance; the gain is that recording begins on the next frame.
        val target = if (recordSettings.toPc) Recorder.Target.Pc else Recorder.Target.Phone
        val rec = if (!recordSettings.enabled) null else try {
            Recorder(recordingsDir(), object : Recorder.Sink {
                override fun onRecordFrame(data: ByteArray, ptsUs: Long,
                                           isConfig: Boolean, isKey: Boolean) {
                    // Anchored to the same origin as the live stream and the
                    // sound, so the desktop can mux all three without knowing
                    // how any of them was captured.
                    val base = basePtsUs
                    val stamped = if (isConfig || base < 0) 0L else ptsUs - base
                    server.sendRecord(data, stamped, isConfig, isKey)
                }
            }).apply { prepare(recordConfig, target) }
        } catch (t: Throwable) {
            // A device that will not give a second encoder at this size still
            // works as a webcam, so this is a lost capability rather than a
            // failed start.
            Log.w(TAG, "recorder unavailable", t)
            null
        }
        recorder = rec
        rec?.expectAudio(audio != null)
        audioFormat?.let { rec?.setAudioFormat(it) }

        val cam = CameraSession(this, cameraId, streamFps, enc.inputSurface, rec?.surface) { t ->
            Log.e(TAG, "camera error", t)
            controlHandler.post { restartPipeline() }
        }
        try {
            cam.open()
            camera = cam
        } catch (t: Throwable) {
            Log.e(TAG, "camera open failed", t)
            enc.stop()
            encoder = null
            rec?.release()
            recorder = null
        }
    }

    // ---- audio ---------------------------------------------------------

    /**
     * Brings the microphone up for the client session.
     *
     * Failure here is not failure of the session. A phone whose microphone is
     * refused, missing or held by another app is still a perfectly good camera,
     * and the one thing that must not happen is losing the picture over the
     * sound.
     */
    private fun startAudio() {
        if (!audioEnabled || audio != null) return
        if (!cameraClockKnown) {
            // Nothing to time the samples against yet. The first camera frame
            // calls back here once there is.
            Log.i(TAG, "audio deferred until the camera clock is known")
            return
        }
        if (checkSelfPermission(android.Manifest.permission.RECORD_AUDIO) !=
            android.content.pm.PackageManager.PERMISSION_GRANTED) {
            Log.i(TAG, "no microphone permission; streaming without sound")
            return
        }

        // Re-declare the foreground service types before opening the microphone.
        //
        // The types are fixed at the startForeground call, and the service
        // starts before anyone has answered the permission dialog -- so a
        // session that registered as camera-only keeps the microphone app-op
        // denied even after the permission is granted. The denial is not an
        // error: Android hands over a stream of digital silence, so the symptom
        // is a microphone that connects, runs at the right rate and records
        // nothing, indistinguishable from a hardware fault.
        startForegroundNotification(getString(R.string.status_streaming))

        val enc = AudioEncoder(
            AudioEncoder.Config(bootTimeClock = cameraClockIsBootTime),
            object : AudioEncoder.Sink {
                override fun onAudioConfig(csd: ByteArray) {
                    server.sendAudio(csd, 0, isConfig = true)
                }

                override fun onAudioFrame(data: ByteArray, ptsUs: Long) {
                    // Anchored to the same origin as the video, so a receiver
                    // can line the two up without knowing anything about how
                    // either was captured.
                    val base = basePtsUs
                    if (base >= 0) server.sendAudio(data, ptsUs - base, isConfig = false)
                    recorder?.writeAudio(data, ptsUs)
                }

                override fun onAudioFormat(format: android.media.MediaFormat) {
                    audioFormat = format
                    controlHandler.post { recorder?.setAudioFormat(format) }
                }

                override fun onAudioError(t: Throwable) {
                    Log.w(TAG, "audio failed; carrying on without it", t)
                    controlHandler.post {
                        stopAudio()
                        recorder?.expectAudio(false)
                    }
                }
            },
        )

        try {
            enc.start()
            audio = enc
            recorder?.expectAudio(true)
        } catch (t: Throwable) {
            Log.w(TAG, "could not start audio; streaming without sound", t)
            audio = null
            recorder?.expectAudio(false)
        }
    }

    private fun stopAudio() {
        audio?.stop()
        audio = null
        audioFormat = null
    }

    /**
     * Works out which clock the camera used, from a timestamp it actually
     * produced.
     *
     * Called with the presentation time of the first encoded frame. That is a
     * capture time seen after encoding, so it lags the clock by a few tens of
     * milliseconds -- irrelevant here, since the two candidate clocks are days
     * apart on any phone that has been asleep.
     */
    private fun identifyCameraClock(cameraPtsUs: Long) {
        val monotonicUs = System.nanoTime() / 1000
        val bootTimeUs = android.os.SystemClock.elapsedRealtimeNanos() / 1000

        val toMonotonic = Math.abs(monotonicUs - cameraPtsUs)
        val toBootTime = Math.abs(bootTimeUs - cameraPtsUs)

        cameraClockIsBootTime = toBootTime < toMonotonic
        cameraClockKnown = true
        Log.i(TAG, "camera clock is ${if (cameraClockIsBootTime) "boot time" else "monotonic"} " +
                "(off by ${minOf(toMonotonic, toBootTime) / 1000}ms; the other is " +
                "${maxOf(toMonotonic, toBootTime) / 1_000_000}s away)")
    }

    /**
     * Where recordings land: the app's own external files directory, which
     * `adb pull` can read without root and which Android clears with the app.
     */
    // 0, 90, 180 or 270. Read from the display rather than from a sensor
    // listener: what matters is the orientation the window manager has settled
    // on, not what the accelerometer felt a moment ago.
    @Suppress("DEPRECATION")
    private fun surfaceRotationDegrees(): Int {
        val rotation = try {
            if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.R) {
                display?.rotation ?: android.view.Surface.ROTATION_0
            } else {
                val wm = getSystemService(WINDOW_SERVICE) as android.view.WindowManager
                wm.defaultDisplay.rotation
            }
        } catch (_: Throwable) {
            android.view.Surface.ROTATION_0
        }
        return when (rotation) {
            android.view.Surface.ROTATION_90 -> 90
            android.view.Surface.ROTATION_180 -> 180
            android.view.Surface.ROTATION_270 -> 270
            else -> 0
        }
    }

    private fun recordingsDir(): File =
        File(getExternalFilesDir(null) ?: filesDir, "recordings")

    private data class Plan(val record: Recorder.Config, val streamFps: Int)

    /**
     * Works out what the camera can actually do, given a streaming rate and the
     * requested recording format.
     *
     * Two separate constraints, and conflating them cost a frame rate:
     *
     *  - **The recording size can hold the whole session back.** The sensor
     *    produces one stream of frames, and a request carrying a 4K target
     *    cannot complete in 16ms however the other targets are arranged. So a
     *    4K recording takes the stream down to 30 with it. There is no way
     *    around that, and asking anyway wedges the HAL rather than producing a
     *    slower stream.
     *
     *  - **A recording rate below the streaming rate does not.** Wanting a
     *    smaller file at a size the sensor can drive at full speed is a
     *    deliberate choice, and it is served by leaving the record target out
     *    of some requests -- the repeating burst -- while the stream keeps
     *    every frame.
     *
     * Taking every n-th frame is the only mechanism available, so the recording
     * rate has to divide the streaming one; a request that does not divide
     * evenly is snapped to the nearest one that does.
     */
    private fun planRecording(cameraId: String, requestedStreamFps: Int): Plan {
        val r = recordSettings
        var width = r.width
        var height = r.height

        if (width <= 0 || height <= 0) {
            val best = bestRecordSize(cameraId, requestedStreamFps)
            width = best.first
            height = best.second
        }

        // Only the size constrains the stream.
        val sizeCap = sustainableFps(cameraId, width, height)
        val streamFps = minOf(requestedStreamFps, sizeCap).coerceAtLeast(1)

        val wanted = (if (r.fps > 0) r.fps else streamFps).coerceIn(1, streamFps)
        val divisor = Math.round(streamFps.toFloat() / wanted).coerceAtLeast(1)
        val recordFps = (streamFps / divisor).coerceAtLeast(1)

        val codec = if (r.codec == "hevc" && !hevcEncoderAvailable()) "h264" else r.codec
        return Plan(Recorder.Config(width, height, recordFps, r.bitrate, codec), streamFps)
    }

    /** The frame rate this camera can sustain at a given size. */
    private fun sustainableFps(cameraId: String, width: Int, height: Int): Int {
        val json = CameraEnumerator.describeDevice(this)
        val cameras = json.optJSONArray("cameras") ?: return 30
        for (i in 0 until cameras.length()) {
            val c = cameras.getJSONObject(i)
            if (c.optString("id") != cameraId) continue
            val modes = c.optJSONArray("modes") ?: continue
            for (m in 0 until modes.length()) {
                val mode = modes.getJSONObject(m)
                val size = mode.optJSONArray("size") ?: continue
                if (size.optInt(0) == width && size.optInt(1) == height) {
                    return mode.optInt("maxFps", 30).coerceAtLeast(1)
                }
            }
        }
        return 30
    }

    /**
     * The default recording size: the largest standard 16:9 format this camera
     * can still deliver at [streamFps].
     *
     * Deliberately not the sensor's largest. The biggest mode here is
     * 4096x2304 at 30, and defaulting to it would quietly halve the webcam's
     * frame rate for everyone who never asked to record -- a bad trade for a
     * device whose first job is being a camera. Staying inside the streaming
     * rate gives a file far better than the stream at no cost to it, and 4K
     * remains one press away for anyone who wants it.
     *
     * The ladder is the standard heights rather than whatever the sensor
     * reports: 4096-wide footage is a nuisance in every editor.
     */
    private fun bestRecordSize(cameraId: String, streamFps: Int): Pair<Int, Int> {
        val ladder = intArrayOf(2160, 1440, 1080, 720)
        val json = CameraEnumerator.describeDevice(this)
        val cameras = json.optJSONArray("cameras") ?: return 1920 to 1080
        for (i in 0 until cameras.length()) {
            val c = cameras.getJSONObject(i)
            if (c.optString("id") != cameraId) continue
            val modes = c.optJSONArray("modes") ?: continue

            for (wanted in ladder) {
                for (m in 0 until modes.length()) {
                    val mode = modes.getJSONObject(m)
                    val size = mode.optJSONArray("size") ?: continue
                    val w = size.optInt(0)
                    val h = size.optInt(1)
                    if (h != wanted || w * 9 != h * 16) continue
                    if (mode.optInt("maxFps", 0) >= streamFps) return w to h
                }
            }
        }
        return 1920 to 1080
    }

    private fun hevcEncoderAvailable(): Boolean = try {
        android.media.MediaCodecList(android.media.MediaCodecList.REGULAR_CODECS)
            .codecInfos.any { it.isEncoder &&
                it.supportedTypes.any { t -> t.equals("video/hevc", ignoreCase = true) } }
    } catch (t: Throwable) {
        false
    }

    private fun stopPipeline() {
        // Close a running take before the camera goes: the recorder is fed by a
        // camera target, and signalling end-of-stream while frames are still
        // arriving costs the tail of the file.
        if (recorder?.isRecording == true) {
            camera?.setRecording(false, 1)
            recorder?.stop()
        }
        armedPreRoll = 0
        camera?.close()
        camera = null
        recorder?.release()
        recorder = null
        encoder?.stop()
        encoder = null
    }

    private fun restartPipeline() {
        if (!server.isConnected) return
        stopPipeline()
        controlHandler.postDelayed({ if (server.isConnected) startPipeline() }, 500)
    }

    private fun defaultCameraId(): String? {
        val json = CameraEnumerator.describeDevice(this)
        val cameras = json.optJSONArray("cameras") ?: return null
        for (i in 0 until cameras.length()) {
            val c = cameras.getJSONObject(i)
            if (c.optString("facing") == "back") return c.optString("id")
        }
        return if (cameras.length() > 0) cameras.getJSONObject(0).optString("id") else null
    }

    // ---- control -------------------------------------------------------

    private fun handleControl(raw: String) {
        val json = try {
            JSONObject(raw)
        } catch (t: Throwable) {
            Log.w(TAG, "bad control payload: $raw")
            return
        }

        when (json.optString("cmd")) {
            "set" -> {
                val s = settings
                settings = Settings(
                    cameraId = json.optString("camera", s.cameraId ?: "").ifEmpty { s.cameraId },
                    width = json.optInt("width", s.width),
                    height = json.optInt("height", s.height),
                    fps = json.optInt("fps", s.fps),
                    bitrate = json.optInt("bitrate", s.bitrate),
                    codec = json.optString("codec", s.codec),
                )
                startPipeline()
                ack(true, "set", JSONObject().apply {
                    put("width", settings.width)
                    put("height", settings.height)
                    put("fps", activeStreamFps)
                    put("bitrate", settings.bitrate)
                    put("codec", settings.codec)
                    put("camera", settings.cameraId)
                })
            }

            "idr" -> {
                encoder?.requestKeyFrame()
                ack(true, "idr", null)
            }

            "bitrate" -> {
                val bps = json.optInt("value", settings.bitrate)
                settings = settings.copy(bitrate = bps)
                encoder?.setBitrate(bps)
                ack(true, "bitrate", JSONObject().put("value", bps))
            }

            "zoom" -> {
                camera?.setZoom(json.optDouble("ratio", 1.0).toFloat())
                ack(true, "zoom", null)
            }

            "torch" -> {
                camera?.setTorch(json.optBoolean("on", false))
                ack(true, "torch", null)
            }

            "ev" -> {
                camera?.setExposureCompensation(json.optDouble("value", 0.0).toFloat())
                ack(true, "ev", null)
            }

            "focus" -> {
                val cam = camera
                // v1 clients sent no mode at all and meant "go back to auto".
                when (json.optString("mode", "continuous")) {
                    "tap", "lock" -> {
                        cam?.focusAt(
                            json.optDouble("x", 0.5).toFloat(),
                            json.optDouble("y", 0.5).toFloat(),
                        )
                        ack(true, "focus", JSONObject().put("mode", "tap"))
                    }
                    "manual" -> {
                        val requested = json.optDouble("distance", 0.0).toFloat()
                        val applied = cam?.setManualFocus(requested) ?: -1f
                        if (applied < 0f) {
                            ackError("focus", "this lens is fixed focus")
                        } else {
                            ack(true, "focus", JSONObject().apply {
                                put("mode", "manual")
                                put("distance", applied)
                            })
                        }
                    }
                    else -> {
                        cam?.setAutoFocus()
                        ack(true, "focus", JSONObject().put("mode", "continuous"))
                    }
                }
            }

            "exposure" -> {
                val cam = camera
                if (json.optString("mode", "auto") == "manual") {
                    if (!manualSensorSupported()) {
                        ackError("exposure", "manual sensor control unavailable")
                    } else {
                        val iso = json.optInt("iso", 400)
                        val shutterNs = json.optLong("shutterNs", 16_666_666L)
                        val applied = cam?.setManualExposure(iso, shutterNs) ?: shutterNs
                        ack(true, "exposure", JSONObject().apply {
                            put("mode", "manual")
                            put("iso", iso)
                            put("shutterNs", applied)
                        })
                    }
                } else {
                    cam?.setAutoExposure()
                    ack(true, "exposure", JSONObject().put("mode", "auto"))
                }
            }

            "profile" -> {
                val log = json.optString("mode", "standard") == "log"
                if (camera?.setPictureProfile(log) == true) {
                    ack(true, "profile", JSONObject().put("mode", if (log) "log" else "standard"))
                } else {
                    ackError("profile", "this camera cannot produce a log profile")
                }
            }

            "wb" -> {
                val cam = camera
                val mode = json.optString("mode", "auto")
                if (mode == "manual") {
                    val applied = cam?.setManualWhiteBalance(json.optInt("temperature", 5600)) ?: 0
                    ack(true, "wb", JSONObject().apply {
                        put("mode", "manual")
                        put("temperature", applied)
                    })
                } else if (cam?.setWhiteBalancePreset(mode) == true) {
                    ack(true, "wb", JSONObject().put("mode", mode))
                } else {
                    ackError("wb", "unsupported white balance mode: $mode")
                }
            }

            "audio" -> {
                audioEnabled = json.optBoolean("enabled", true)
                if (audioEnabled) startAudio() else {
                    stopAudio()
                    recorder?.expectAudio(false)
                }
                ack(true, "audio", JSONObject().apply {
                    put("enabled", audioEnabled && audio != null)
                    audio?.let {
                        put("sampleRate", it.sampleRate)
                        put("channels", it.channels)
                    }
                })
            }

            "record" -> handleRecord(json)
            "framing" -> {
                val cam = camera
                if (cam == null) {
                    ackError("framing", "no camera")
                } else if (json.optBoolean("off", false)) {
                    cam.clearFraming()
                    ack(true, "framing", JSONObject().apply { put("off", true) })
                } else {
                    // Clamped here as well as on the desktop. These four numbers
                    // become a rectangle handed to the camera HAL, and a HAL
                    // given a rectangle outside its sensor does not always say
                    // so politely.
                    val x = json.optDouble("x", 0.0).toFloat().coerceIn(0f, 1f)
                    val y = json.optDouble("y", 0.0).toFloat().coerceIn(0f, 1f)
                    val w = json.optDouble("w", 1.0).toFloat().coerceIn(0.05f, 1f)
                    val h = json.optDouble("h", 1.0).toFloat().coerceIn(0.05f, 1f)
                    cam.setFraming(x.coerceAtMost(1f - w), y.coerceAtMost(1f - h), w, h)
                    ack(true, "framing", JSONObject().apply {
                        put("x", x); put("y", y); put("w", w); put("h", h)
                    })
                }
            }
            "takes" -> handleTakes(json)
            "ramp" -> {
                val what = json.optString("what", "")
                val target = json.optDouble("to", Double.NaN).toFloat()
                val ms = json.optLong("ms", 1000L).coerceIn(0L, 60_000L)
                val cam = camera
                if (cam == null || target.isNaN()) {
                    ackError("ramp", "nothing to move")
                } else if (json.optBoolean("cancel", false)) {
                    cam.cancelRamp(what.ifEmpty { null })
                    ack(true, "ramp", JSONObject().apply { put("cancelled", true) })
                } else if (!cam.startRamp(what, target, ms)) {
                    ackError("ramp", "this camera cannot move its $what")
                } else {
                    ack(true, "ramp", JSONObject().apply {
                        put("what", what)
                        put("to", target)
                        put("ms", ms)
                    })
                }
            }
            "tally" -> {
                // Not an acknowledgement worth sending: the desktop already
                // knows what it asked for, and this changes every time an
                // application opens or closes the camera.
                Live.tally = json.optBoolean("live", false)
            }

            "stop" -> {
                stopPipeline()
                ack(true, "stop", null)
            }

            else -> {
                // Answered, not just logged.
                //
                // A client newer than this phone asks for things it cannot do,
                // and until now that was a warning in a log nobody was reading
                // while the desktop went on sending the same command five times
                // a second forever. Saying so lets it stop and explain.
                Log.w(TAG, "unknown command: $raw")
                ackError(json.optString("cmd", "?"), "this phone does not know that command")
            }
        }
    }

    /**
     * Local recording. `start` and `stop` are immediate and never touch the
     * stream; `config` changes the file's format, which fixes the size of a
     * surface the capture session was built around and so has to restart the
     * pipeline exactly as `set` does.
     */
    // ---- takes ---------------------------------------------------------

    /**
     * The file a fetch is currently sending, and the thread sending it. One at
     * a time: two transfers would halve each other's rate for no gain, and the
     * pacing below only reasons about one.
     */
    @Volatile private var fetchThread: Thread? = null
    @Volatile private var fetchCancel = false

    /**
     * Resolves a client-supplied name inside the recordings directory.
     *
     * Returns null for anything that escapes it. The name arrives over a socket
     * and is used to open a file, so it is exactly the kind of string that must
     * not be trusted to be what it looks like -- "../../databases/x" is a
     * perfectly ordinary file name to a JSON parser.
     */
    private fun takeFile(name: String): File? {
        if (name.isEmpty() || name.contains('/') || name.contains('\\')) return null
        val dir = recordingsDir()
        val file = File(dir, name)
        return if (file.parentFile?.canonicalPath == dir.canonicalPath && file.isFile) file
               else null
    }

    private fun handleTakes(json: JSONObject) {
        when (json.optString("action", "list")) {
            "list" -> {
                val dir = recordingsDir()
                val files = dir.listFiles { f: File -> f.isFile && f.name.endsWith(".mp4") }
                    ?.sortedByDescending { it.lastModified() }
                    ?: emptyList()

                val takes = JSONArray()
                for (file in files.take(MAX_TAKES_LISTED)) {
                    takes.put(JSONObject().apply {
                        put("name", file.name)
                        put("bytes", file.length())
                        put("modified", file.lastModified() / 1000)
                        put("durationMs", durationOf(file))
                    })
                }
                ack(true, "takes", JSONObject().apply {
                    put("dir", dir.absolutePath)
                    put("takes", takes)
                })
            }

            "delete" -> {
                val file = takeFile(json.optString("name", ""))
                if (file == null) {
                    ackError("takes", "no such take")
                    return
                }
                if (recorder?.currentFile == file.absolutePath &&
                    recorder?.isRecording == true) {
                    ackError("takes", "that take is still being written")
                    return
                }
                val name = file.name
                if (!file.delete()) {
                    ackError("takes", "could not delete $name")
                    return
                }
                Log.i(TAG, "deleted $name")
                ack(true, "takes", JSONObject().apply {
                    put("deleted", name)
                })
            }

            "fetch" -> {
                val file = takeFile(json.optString("name", ""))
                if (file == null) {
                    ackError("takes", "no such take")
                    return
                }
                if (fetchThread?.isAlive == true) {
                    ackError("takes", "already sending one")
                    return
                }
                startFetch(file)
                ack(true, "takes", JSONObject().apply {
                    put("fetching", file.name)
                    put("bytes", file.length())
                })
            }

            "cancel" -> {
                fetchCancel = true
                ack(true, "takes", JSONObject().apply { put("cancelled", true) })
            }
        }
    }

    /** Length in milliseconds, or zero when the file will not say. */
    private fun durationOf(file: File): Long {
        val retriever = MediaMetadataRetriever()
        return try {
            retriever.setDataSource(file.absolutePath)
            retriever.extractMetadata(MediaMetadataRetriever.METADATA_KEY_DURATION)
                ?.toLongOrNull() ?: 0L
        } catch (t: Throwable) {
            // A take still being written has no duration yet, and a truncated
            // one never will. Neither is a reason to fail the listing.
            0L
        } finally {
            try { retriever.release() } catch (_: Throwable) {}
        }
    }

    /**
     * Sends one file to the client, a chunk at a time, while the link is idle.
     *
     * The pacing is the whole design. The send queue sheds DELTA packets when
     * it fills, so a transfer that enqueued chunks as fast as it could read them
     * would pay for the file in dropped frames -- the live picture would stutter
     * for as long as the copy took, which is the one thing this project does not
     * do. Offering a chunk only when the queue is empty means a fetch takes
     * whatever bandwidth is left over and never a frame that was not.
     *
     * Over USB with a 1080p stream that is most of the link; over Wi-Fi at the
     * ceiling it can be almost nothing, and the transfer simply takes as long as
     * it takes. Slow is a fair price; a stuttering call is not.
     */
    private fun startFetch(file: File) {
        fetchCancel = false
        fetchThread = Thread({
            var sent = 0L
            try {
                file.inputStream().use { input ->
                    val chunk = ByteArray(FETCH_CHUNK)
                    while (!fetchCancel && server.isConnected) {
                        // Wait for the link to be idle rather than sleeping a
                        // fixed amount: the right interval is however long the
                        // stream needs, and it changes with the picture.
                        if (server.queueDepth() > 0) {
                            Thread.sleep(4)
                            continue
                        }
                        val read = input.read(chunk)
                        if (read <= 0) break
                        val payload = if (read == chunk.size) chunk.copyOf()
                                      else chunk.copyOf(read)
                        sent += read
                        server.sendFile(payload, sent - read, isLast = false)
                    }
                }
                if (!fetchCancel && server.isConnected) {
                    // An empty last chunk rather than a flag on the final data
                    // chunk: the reader learns the file is complete without
                    // having to know its length in advance.
                    server.sendFile(ByteArray(0), sent, isLast = true)
                    Log.i(TAG, "sent ${file.name}, ${sent / 1_000_000}MB")
                } else {
                    Log.i(TAG, "fetch of ${file.name} stopped after ${sent / 1_000_000}MB")
                }
            } catch (t: Throwable) {
                Log.w(TAG, "fetch failed", t)
            }
        }, "xcam-fetch").apply {
            // Below the encoder and the sender. This work is never urgent.
            priority = Thread.MIN_PRIORITY
            start()
        }
    }

    /** Seconds of pre-roll actually granted, after clamping. */
    @Volatile private var armedPreRoll = 0

    private var micSilentMs = 0L

    private fun preRollArmed(): Boolean = armedPreRoll > 0

    /**
     * The frame interval the ring fills at.
     *
     * The same divisor a take would use. A ring that filled at full rate and
     * then handed its frames to a take running at a quarter of it would produce
     * a file whose first seconds run four times too fast.
     */
    private fun preRollDivisor(): Int {
        val cfg = recorder?.config ?: return 1
        return (activeStreamFps / cfg.fps.coerceAtLeast(1)).coerceAtLeast(1)
    }

    /**
     * Arms or disarms the ring to match the current settings, and puts the
     * record target in the repeating request for as long as it is armed.
     *
     * Returns the seconds granted, which the client is told about: asking for
     * twenty and getting six is not a failure, but it is not a detail either.
     */
    private fun applyPreRoll(): Int {
        val rec = recorder
        val cfg = rec?.config
        if (rec == null || cfg == null || !recordSettings.enabled) {
            armedPreRoll = 0
            return 0
        }

        // Local takes only, and never over the top of one already running.
        val wanted = if (recordSettings.toPc) 0 else recordSettings.preRollSeconds
        val granted = if (rec.isRecording) armedPreRoll
                      else rec.armPreRoll(wanted, cfg.bitrate)
        armedPreRoll = granted

        if (!rec.isRecording) camera?.setRecording(granted > 0, preRollDivisor())
        if (granted > 0) {
            Log.i(TAG, "pre-roll armed for ${granted}s at ${preRollDivisor()}:1")
        }
        return granted
    }

    private fun handleRecord(json: JSONObject) {
        val rec = recorder
        when (json.optString("action", "start")) {
            "config" -> {
                val r = recordSettings
                val before = r
                recordSettings = RecordSettings(
                    enabled = json.optBoolean("enabled", r.enabled),
                    toPc = when (json.optString("target", "")) {
                        "pc" -> true
                        "phone" -> false
                        else -> r.toPc
                    },
                    // Zero is a value, not an absence: it means "the camera at
                    // its best", and the client sends it whenever nobody has
                    // chosen a size. Treating a present zero as "keep what you
                    // have" is what made a size chosen once outlive every
                    // attempt to take it back.
                    width = json.optInt("width", r.width),
                    height = json.optInt("height", r.height),
                    fps = json.optInt("fps", r.fps),
                    bitrate = json.optInt("bitrate", r.bitrate),
                    codec = json.optString("codec", r.codec),
                    preRollSeconds = json.optInt("preroll", r.preRollSeconds),
                )
                // Only when something actually changed. A client that
                // re-states the recording configuration on every connection --
                // which it must, or the phone's memory of it outlives the
                // client's -- would otherwise rebuild the camera session for a
                // message that asked for exactly what was already running.
                if (recordSettings != before) startPipeline()
                val preRoll = applyPreRoll()
                val applied = recorder?.config
                if (!recordSettings.enabled) {
                    ack(true, "record", JSONObject().apply {
                        put("state", "off")
                        put("enabled", false)
                        put("streamFps", activeStreamFps)
                    })
                } else if (applied == null) {
                    ackError("record", "this device cannot run a second encoder")
                } else {
                    ack(true, "record", JSONObject().apply {
                        put("state", "idle")
                        put("width", applied.width)
                        put("height", applied.height)
                        put("fps", applied.fps)
                        put("bitrate", applied.bitrate)
                        put("codec", applied.codec)
                        put("enabled", true)
                        put("target", if (recordSettings.toPc) "pc" else "phone")
                        // Granted rather than obeyed: the ring lives in the heap
                        // at a bitrate chosen for a file, so what was asked for
                        // is not always what fits.
                        put("preroll", preRoll)
                        // A recording size the sensor cannot drive at the
                        // streaming rate pulls the stream down with it. The
                        // client has to see that here, not discover it as a
                        // frame rate that no longer matches its own display.
                        put("streamFps", activeStreamFps)
                    })
                }
            }

            "start" -> {
                val cfg = rec?.config
                if (!recordSettings.enabled) {
                    ackError("record", "recording is switched off")
                    return
                }
                if (rec == null || cfg == null) {
                    ackError("record", "this device cannot run a second encoder")
                    return
                }
                if (rec.isRecording) {
                    ackError("record", "already recording")
                    return
                }
                try {
                    // Start the encoder before the camera is told to feed it, or
                    // the first frames arrive at a codec that is not listening
                    // and the take opens on a stall.
                    val file = rec.start()
                    val divisor = (activeStreamFps / cfg.fps.coerceAtLeast(1)).coerceAtLeast(1)
                    camera?.setRecording(true, divisor)
                    ack(true, "record", JSONObject().apply {
                        put("state", "recording")
                        put("file", file)
                        put("width", cfg.width)
                        put("height", cfg.height)
                        put("fps", cfg.fps)
                        put("bitrate", cfg.bitrate)
                        put("codec", cfg.codec)
                        put("target", if (recordSettings.toPc) "pc" else "phone")
                    })
                } catch (t: Throwable) {
                    Log.e(TAG, "record start failed", t)
                    ackError("record", t.message ?: "record start failed")
                }
            }

            "stop" -> {
                if (rec == null || !rec.isRecording) {
                    ackError("record", "not recording")
                    return
                }
                camera?.setRecording(preRollArmed(), preRollDivisor())
                val durationMs = rec.elapsedMs()
                val bytes = rec.bytes()
                val file = rec.stop()
                ack(true, "record", JSONObject().apply {
                    put("state", "idle")
                    put("file", file)
                    put("durationMs", durationMs)
                    put("bytes", bytes)
                })
            }

            else -> ackError("record", "unknown record action")
        }
    }

    /**
     * Whether the camera in use reports MANUAL_SENSOR. Checked before accepting
     * manual exposure: without it the HAL takes the request and ignores it, so
     * refusing outright is the only way the client learns the truth.
     */
    private fun manualSensorSupported(): Boolean {
        val id = settings.cameraId ?: defaultCameraId() ?: return false
        return try {
            val manager = getSystemService(Context.CAMERA_SERVICE) as android.hardware.camera2.CameraManager
            manager.getCameraCharacteristics(id)
                .get(android.hardware.camera2.CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES)
                ?.contains(android.hardware.camera2.CameraCharacteristics
                    .REQUEST_AVAILABLE_CAPABILITIES_MANUAL_SENSOR) == true
        } catch (t: Throwable) {
            Log.w(TAG, "could not read camera capabilities", t)
            false
        }
    }

    private fun ackError(cmd: String, message: String) {
        server.sendAck(JSONObject().apply {
            put("ok", false)
            put("cmd", cmd)
            put("error", message)
        }.toString(), 0)
    }

    private fun ack(ok: Boolean, cmd: String, applied: JSONObject?) {
        val payload = JSONObject().apply {
            put("ok", ok)
            put("cmd", cmd)
            if (applied != null) put("applied", applied)
        }
        server.sendAck(payload.toString(), 0)
    }

    // ---- stats ---------------------------------------------------------

    private val statsTick = object : Runnable {
        override fun run() {
            if (server.isConnected) emitStats()
            controlHandler.postDelayed(this, STATS_INTERVAL_MS)
        }
    }

    private fun emitStats() {
        val now = System.nanoTime()
        val elapsed = if (lastTickNanos == 0L) STATS_INTERVAL_MS / 1000.0
                      else (now - lastTickNanos) / 1_000_000_000.0
        lastTickNanos = now

        val frames = framesSinceTick
        val bytes = bytesSinceTick
        framesSinceTick = 0
        bytesSinceTick = 0

        val battery = (getSystemService(Context.BATTERY_SERVICE) as BatteryManager)
            .getIntProperty(BatteryManager.BATTERY_PROPERTY_CAPACITY)

        val thermal = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            when ((getSystemService(Context.POWER_SERVICE) as PowerManager).currentThermalStatus) {
                PowerManager.THERMAL_STATUS_NONE -> "none"
                PowerManager.THERMAL_STATUS_LIGHT -> "light"
                PowerManager.THERMAL_STATUS_MODERATE -> "moderate"
                PowerManager.THERMAL_STATUS_SEVERE -> "severe"
                PowerManager.THERMAL_STATUS_CRITICAL -> "critical"
                else -> "emergency"
            }
        } else "none"

        // Read once: taking the peak resets it, so a second reader would see
        // half a meter. A number that is measured and then thrown away is how
        // this project twice shipped a microphone that was silently dead -- once
        // because the permission was revoked behind its back, once because the
        // foreground service type was missing from startForeground. Neither
        // raised an error; both would have been obvious in the first second as
        // a meter that never moved.
        val audioPeak = audio?.takePeak() ?: 0.0f
        if (audio != null && audioPeak <= 0.0f) micSilentMs += STATS_INTERVAL_MS
        else micSilentMs = 0L

        val actualFps = if (elapsed > 0) frames / elapsed else 0.0
        val actualBitrate = if (elapsed > 0) (bytes * 8 / elapsed) else 0.0

        // A ring that is armed keeps the encoder running whenever the camera
        // does, and that heat lands on the same silicon the sensor is on. Left
        // alone it ends as a thermally throttled stream -- the failure the
        // person on the call sees, rather than the one they chose. So the ring
        // gives way first, and says that it did.
        if (armedPreRoll > 0 && (thermal == "severe" || thermal == "critical" ||
                                 thermal == "emergency")) {
            Log.w(TAG, "pre-roll disarmed: thermal status $thermal")
            recorder?.armPreRoll(0, 0)
            armedPreRoll = 0
            if (recorder?.isRecording != true) camera?.setRecording(false, 1)
            // No separate message for it. This same tick reports both the
            // thermal status and a ring length of zero, and an unsolicited ACK
            // would be indistinguishable from a reply to something.
        }

        // The same numbers the phone screen shows. Computed once, read twice.
        Live.clientConnected = server.isConnected
        Live.recording = recorder?.isRecording == true
        Live.fps = actualFps
        Live.mbps = actualBitrate / 1_000_000.0
        Live.battery = battery
        Live.thermal = thermal
        Live.width = settings.width
        Live.height = settings.height
        Live.audio = audio != null
        Live.audioPeak = audioPeak
        Live.audioSilentMs = micSilentMs

        val stats = JSONObject().apply {
            put("audioPeak", audioPeak)
            put("prerollArmedMs", recorder?.preRollArmedMs() ?: 0L)
            put("prerollFillMs", recorder?.preRollFillMs() ?: 0L)
            put("actualFps", actualFps)
            put("actualBitrate", actualBitrate.toLong())
            put("droppedFrames", server.takeDroppedCount())
            put("battery", battery)
            put("thermal", thermal)

            // Which way up the phone is being held.
            //
            // The sensor does not turn with the body -- a phone held upright
            // still hands over a landscape buffer with the scene lying on its
            // side. The desktop can stand that up and use every pixel, but only
            // if it knows to, and this is the only way it can find out. It has
            // been in the protocol document since v5 and sent by nobody.
            put("surfaceRotation", surfaceRotationDegrees())

            val rec = recorder
            put("recording", rec?.isRecording == true)
            if (rec?.isRecording == true) {
                put("recordMs", rec.elapsedMs())
                put("recordBytes", rec.bytes())
            }
            // A recording stops where the storage runs out, so the space left
            // belongs in the same tick as the bytes written.
            put("storageFreeMb", recordingsDir().let {
                try { it.mkdirs(); it.usableSpace / (1024 * 1024) } catch (_: Throwable) { -1L }
            })
        }
        server.sendStats(stats.toString())
    }

    // ---- foreground plumbing -------------------------------------------

    private fun acquireWakeLock() {
        val pm = getSystemService(Context.POWER_SERVICE) as PowerManager
        wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "xcam:capture").apply {
            setReferenceCounted(false)
            acquire(WAKELOCK_TIMEOUT_MS)
        }
    }

    private fun buildNotification(text: String): Notification {
        val open = PendingIntent.getActivity(
            this, 0,
            Intent(this, MainActivity::class.java),
            PendingIntent.FLAG_IMMUTABLE,
        )
        return Notification.Builder(this, CHANNEL_ID)
            .setContentTitle(getString(R.string.app_name))
            .setContentText(text)
            .setSmallIcon(android.R.drawable.presence_video_online)
            .setContentIntent(open)
            .setOngoing(true)
            .build()
    }

    private fun startForegroundNotification(text: String) {
        val nm = getSystemService(NotificationManager::class.java)
        nm.createNotificationChannel(
            NotificationChannel(CHANNEL_ID, "XCam capture", NotificationManager.IMPORTANCE_LOW)
        )

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
            // Both types, not just the camera.
            //
            // The microphone app-op is granted "while in use", and what counts
            // as in use for a background service is precisely whether it
            // declared the microphone foreground-service type. Declaring only
            // the camera does not fail: Android hands the app a perfectly
            // well-formed stream of digital silence, which arrives at the PC as
            // a working microphone that records nothing.
            var types = ServiceInfo.FOREGROUND_SERVICE_TYPE_CAMERA
            if (checkSelfPermission(android.Manifest.permission.RECORD_AUDIO) ==
                android.content.pm.PackageManager.PERMISSION_GRANTED) {
                types = types or ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE
            }
            startForeground(NOTIFICATION_ID, buildNotification(text), types)
        } else {
            startForeground(NOTIFICATION_ID, buildNotification(text))
        }
    }

    private fun updateNotification(text: String) {
        getSystemService(NotificationManager::class.java)
            .notify(NOTIFICATION_ID, buildNotification(text))
    }

    /**
     * What the screen shows, published by the service.
     *
     * The activity does not bind: four numbers and a couple of flags are not
     * worth a service connection, a lifecycle, and the reconnection logic that
     * comes with one. They are written here where they are already being
     * computed for STATS and read once a second by whatever is on screen.
     */
    object Live {
        @Volatile @JvmField var running = false
        @Volatile @JvmField var clientConnected = false
        @Volatile @JvmField var recording = false
        @Volatile @JvmField var fps = 0.0
        @Volatile @JvmField var mbps = 0.0
        @Volatile @JvmField var battery = -1
        @Volatile @JvmField var thermal = ""
        @Volatile @JvmField var width = 0
        @Volatile @JvmField var height = 0

        /**
         * True while an application on the desktop actually has the camera
         * open, which is not the same as the desktop being connected. Someone
         * in front of the camera is asking whether anyone is looking, and until
         * now the phone could not tell the difference.
         */
        @Volatile @JvmField var tally = false

        /** True while sound is being captured, as against merely permitted. */
        @Volatile @JvmField var audio = false

        /** The loudest sample in the last tick, 0..1, and how long it has been
         *  at nothing. A microphone that is on and flat is broken, not quiet. */
        @Volatile @JvmField var audioPeak = 0.0f
        @Volatile @JvmField var audioSilentMs = 0L

        fun clear() {
            clientConnected = false
            tally = false
            recording = false
            fps = 0.0
            mbps = 0.0
            audio = false
            audioPeak = 0.0f
            audioSilentMs = 0L
        }
    }

    companion object {
        private const val TAG = "XCam/Service"

        /**
         * Enough to fill a browser twice over. A phone holding more than this
         * has a housekeeping problem the panel cannot solve.
         */
        private const val MAX_TAKES_LISTED = 200

        /**
         * One chunk of a fetch. Large enough that the per-packet header is
         * noise, small enough that a chunk in flight cannot stall a frame.
         */
        private const val FETCH_CHUNK = 256 * 1024
        private const val CHANNEL_ID = "xcam.capture"
        private const val NOTIFICATION_ID = 1
        private const val STATS_INTERVAL_MS = 1000L
        private const val WAKELOCK_TIMEOUT_MS = 12L * 60 * 60 * 1000

        const val ACTION_STOP = "com.xcam.STOP"
    }
}
