package com.xcam.camera

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.Rect
import android.hardware.camera2.CameraCaptureSession
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraDevice
import android.hardware.camera2.CameraManager
import android.hardware.camera2.CaptureRequest
import android.hardware.camera2.params.MeteringRectangle
import android.hardware.camera2.params.OutputConfiguration
import android.hardware.camera2.params.SessionConfiguration
import android.os.Build
import android.os.Handler
import android.os.HandlerThread
import android.os.SystemClock
import android.util.Log
import android.util.Range
import android.view.Surface
import java.util.concurrent.CountDownLatch
import java.util.concurrent.Executor
import java.util.concurrent.TimeUnit

/**
 * Drives Camera2 with encoder input Surfaces as the only targets, so frames go
 * camera -> encoder entirely on the GPU and never round-trip through the CPU.
 *
 * [recordTarget], when present, is the local recorder's input surface. It is
 * part of the capture session for the session's whole life but only joins the
 * repeating request while a take is running, which is what lets recording start
 * and stop without disturbing the stream.
 */
@SuppressLint("MissingPermission")
class CameraSession(
    context: Context,
    private val cameraId: String,
    private val fps: Int,
    private val target: Surface,
    private val recordTarget: Surface? = null,
    private val onError: (Throwable) -> Unit,
) {

    private val manager = context.getSystemService(Context.CAMERA_SERVICE) as CameraManager
    private var device: CameraDevice? = null
    private var session: CameraCaptureSession? = null
    private var requestBuilder: CaptureRequest.Builder? = null

    /**
     * openCamera's callbacks keep firing after close(), and [target] belongs to an
     * encoder that close() is the signal to tear down. Configuring a session
     * against that released Surface fails with "Broken pipe", so a session that
     * has been closed must decline every callback it still receives.
     */
    @Volatile private var closed = false

    /**
     * Counts down when the camera device has really shut down.
     *
     * CameraDevice.close() returns before the device is released, and the next
     * openCamera then fails with ERROR_MAX_CAMERAS_IN_USE -- which is how a
     * camera switch or a resolution change can leave the app with no camera at
     * all. Waiting for onClosed is the only way to sequence one session after
     * another.
     */
    private val deviceClosed = CountDownLatch(1)
    @Volatile private var everOpened = false

    /**
     * What the capture template asked for before anything touched it. Restoring
     * these is the only honest definition of "standard": picking a value like
     * TONEMAP_MODE_FAST instead is a guess, and on this HAL a wrong one.
     */
    private var capturedDefaults = false
    private var defaultTonemapMode: Int? = null
    private var defaultColorCorrectionMode: Int? = null
    private var defaultEdgeMode: Int? = null
    private var defaultNoiseReductionMode: Int? = null

    private val thread = HandlerThread("xcam-camera").apply { start() }
    private val handler = Handler(thread.looper)
    private val executor = Executor { handler.post(it) }

    private val characteristics: CameraCharacteristics by lazy {
        manager.getCameraCharacteristics(cameraId)
    }

    fun open() {
        manager.openCamera(cameraId, executor, object : CameraDevice.StateCallback() {
            override fun onOpened(camera: CameraDevice) {
                everOpened = true
                if (closed) {
                    camera.close()
                    return
                }
                device = camera
                createSession(camera)
            }

            override fun onDisconnected(camera: CameraDevice) {
                Log.w(TAG, "camera disconnected")
                camera.close()
                device = null
            }

            override fun onClosed(camera: CameraDevice) {
                deviceClosed.countDown()
            }

            override fun onError(camera: CameraDevice, error: Int) {
                camera.close()
                device = null
                deviceClosed.countDown()
                this@CameraSession.onError(RuntimeException("camera error $error"))
            }
        })
    }

    private fun createSession(camera: CameraDevice) {
        val callback = object : CameraCaptureSession.StateCallback() {
            override fun onConfigured(s: CameraCaptureSession) {
                if (closed) {
                    s.close()
                    return
                }
                session = s
                startRepeating(camera, s)
            }

            override fun onConfigureFailed(s: CameraCaptureSession) {
                onError(RuntimeException("capture session configuration failed"))
            }
        }

        val useCases = characteristics.get(
            CameraCharacteristics.SCALER_AVAILABLE_STREAM_USE_CASES)?.toSet() ?: emptySet()

        // Tell the HAL what each stream is for.
        //
        // Two outputs of the same size and format are indistinguishable to a
        // camera HAL without this, and this one does not cope: it hands back
        // buffers the framework never gave it ("Camera3-Stream: returnBuffer:
        // Returning an unknown buffer") and then stops producing frames on
        // every stream, taking the live picture down with the recording.
        //
        // The live stream is a preview -- it wants latency above all -- and the
        // file is a recording. Saying so is what lets the HAL configure them
        // differently.
        val outputs = mutableListOf(
            OutputConfiguration(target).apply { applyUseCase(useCases, preview = true) })
        recordTarget?.let {
            outputs += OutputConfiguration(it).apply { applyUseCase(useCases, preview = false) }
        }

        val cfg = SessionConfiguration(
            SessionConfiguration.SESSION_REGULAR,
            outputs,
            executor,
            callback,
        )
        // This runs on the camera's own callback thread, where an escaping
        // exception takes the whole process down rather than surfacing as an
        // error we can restart from.
        try {
            camera.createCaptureSession(cfg)
        } catch (t: Throwable) {
            onError(t)
        }
    }

    /**
     * Marks an output as preview or as recording, when the device supports
     * stream use cases at all. Silently does nothing on devices that do not,
     * where DEFAULT is the only option and the HAL sorts itself out.
     */
    private fun OutputConfiguration.applyUseCase(available: Set<Long>, preview: Boolean) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU) return
        val wanted = if (preview) {
            android.hardware.camera2.CameraMetadata.SCALER_AVAILABLE_STREAM_USE_CASES_PREVIEW
        } else {
            android.hardware.camera2.CameraMetadata.SCALER_AVAILABLE_STREAM_USE_CASES_VIDEO_RECORD
        }.toLong()
        if (!available.contains(wanted)) return
        try {
            streamUseCase = wanted
        } catch (t: Throwable) {
            Log.w(TAG, "stream use case rejected", t)
        }
    }

    private fun startRepeating(camera: CameraDevice, s: CameraCaptureSession) {
        val b = camera.createCaptureRequest(CameraDevice.TEMPLATE_RECORD).apply {
            addTarget(target)
            set(CaptureRequest.CONTROL_MODE, CaptureRequest.CONTROL_MODE_AUTO)
            set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_VIDEO)
            set(CaptureRequest.CONTROL_AE_MODE, CaptureRequest.CONTROL_AE_MODE_ON)
            set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, pickFpsRange())
            // Stabilisation costs latency and crops the frame; leave it off and let
            // the desktop UI turn it on deliberately.
            set(
                CaptureRequest.CONTROL_VIDEO_STABILIZATION_MODE,
                CaptureRequest.CONTROL_VIDEO_STABILIZATION_MODE_OFF,
            )

            // The picture pipeline is deliberately left as the record template
            // set it. Those defaults are what the camera normally looks like,
            // and they are captured below so the standard profile can restore
            // exactly them rather than guessing at a value like FAST -- which
            // on this HAL is a harder curve than the default and made the
            // standard picture noticeably more contrasty.
        }
        requestBuilder = b

        // Snapshot the template's own picture settings once, while nothing has
        // overwritten them yet.
        if (!capturedDefaults) {
            defaultTonemapMode = b.get(CaptureRequest.TONEMAP_MODE)
            defaultColorCorrectionMode = b.get(CaptureRequest.COLOR_CORRECTION_MODE)
            defaultEdgeMode = b.get(CaptureRequest.EDGE_MODE)
            defaultNoiseReductionMode = b.get(CaptureRequest.NOISE_REDUCTION_MODE)
            capturedDefaults = true
            Log.i(TAG, "template picture defaults: tonemap=$defaultTonemapMode " +
                    "colour=$defaultColorCorrectionMode edge=$defaultEdgeMode " +
                    "noise=$defaultNoiseReductionMode")
        }

        // Anything asked for while the session was coming up, applied now in
        // the order it was asked for.
        val queued = synchronized(pending) {
            val copy = pending.toList()
            pending.clear()
            copy
        }
        for (block in queued) {
            try {
                block(b)
            } catch (t: Throwable) {
                Log.w(TAG, "queued control failed", t)
            }
        }
        if (queued.isNotEmpty()) {
            Log.i(TAG, "applied ${queued.size} controls held while the camera opened")
        }

        try {
            submit(s, b)
        } catch (t: Throwable) {
            onError(t)
            return
        }
        Log.i(TAG, "camera $cameraId streaming at ${pickFpsRange()}")
    }

    /**
     * Picks the tightest AE range that reaches [fps]. A fixed range such as (60,60)
     * stops the sensor from halving its rate in dim light, which would otherwise
     * reach the PC as stutter.
     */
    private fun pickFpsRange(): Range<Int> {
        val ranges = characteristics.get(
            CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES
        ) ?: return Range(fps, fps)

        return ranges.filter { it.upper >= fps }.minByOrNull { it.upper - it.lower }
            ?: ranges.maxByOrNull { it.upper }
            ?: Range(fps, fps)
    }

    // Where the two rampable controls were last put. A ramp has to start from
    // where the picture actually is, and the capture request is write-only as
    // far as this class is concerned.
    @Volatile private var lastFocusDioptres = 0f
    @Volatile private var lastZoom = 1f

    // ---- live controls -------------------------------------------------

    /**
     * Zoom, as a person asked for it. Cancels a ramp on the same control --
     * reaching for a control during a move is how someone says they want it back.
     */
    fun setZoom(ratio: Float) {
        cancelRamp("zoom")
        applyZoom(ratio)
    }

    /** The same without the cancel, for the ramp to drive. */
    private fun applyZoom(ratio: Float) {
        lastZoom = ratio
        update { b ->
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                val range = characteristics.get(CameraCharacteristics.CONTROL_ZOOM_RATIO_RANGE)
                val clamped =
                    if (range != null) ratio.coerceIn(range.lower, range.upper) else ratio
                b.set(CaptureRequest.CONTROL_ZOOM_RATIO, clamped)
            } else {
                b.set(CaptureRequest.SCALER_CROP_REGION, cropRegionFor(ratio))
            }
        }
    }

    fun setTorch(on: Boolean) = update { b ->
        b.set(
            CaptureRequest.FLASH_MODE,
            if (on) CaptureRequest.FLASH_MODE_TORCH else CaptureRequest.FLASH_MODE_OFF,
        )
    }

    fun setExposureCompensation(ev: Float) = update { b ->
        val step = characteristics.get(CameraCharacteristics.CONTROL_AE_COMPENSATION_STEP)
        val range = characteristics.get(CameraCharacteristics.CONTROL_AE_COMPENSATION_RANGE)
        if (step != null && range != null) {
            val steps = (ev / step.toFloat()).toInt().coerceIn(range.lower, range.upper)
            b.set(CaptureRequest.CONTROL_AE_EXPOSURE_COMPENSATION, steps)
        }
    }

    /** [x] and [y] are normalised to [0,1] across the active sensor area. */
    fun focusAt(x: Float, y: Float) = update { b ->
        val active = characteristics.get(
            CameraCharacteristics.SENSOR_INFO_ACTIVE_ARRAY_SIZE
        ) ?: return@update

        val half = 100
        val px = (active.left + x * active.width()).toInt()
        val py = (active.top + y * active.height()).toInt()
        val region = MeteringRectangle(
            Rect(
                (px - half).coerceAtLeast(active.left),
                (py - half).coerceAtLeast(active.top),
                (px + half).coerceAtMost(active.right),
                (py + half).coerceAtMost(active.bottom),
            ),
            MeteringRectangle.METERING_WEIGHT_MAX,
        )
        b.set(CaptureRequest.CONTROL_AF_REGIONS, arrayOf(region))
        b.set(CaptureRequest.CONTROL_AE_REGIONS, arrayOf(region))
        b.set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_AUTO)
        b.set(CaptureRequest.CONTROL_AF_TRIGGER, CaptureRequest.CONTROL_AF_TRIGGER_START)
    }

    fun setAutoFocus() = update { b ->
        b.set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_VIDEO)
        b.set(CaptureRequest.CONTROL_AF_TRIGGER, CaptureRequest.CONTROL_AF_TRIGGER_CANCEL)
    }

    // ---- manual controls -----------------------------------------------

    /**
     * Full manual exposure. Returns the exposure time actually applied, which is
     * not always the one asked for: an exposure longer than the frame interval
     * cannot be delivered at the requested rate, and a camera given one quietly
     * drops to half speed rather than refusing. Clamping here and reporting it
     * back keeps that trade visible instead of surprising.
     */
    fun setManualExposure(iso: Int, shutterNs: Long): Long {
        val isoRange = characteristics.get(CameraCharacteristics.SENSOR_INFO_SENSITIVITY_RANGE)
        val exposureRange = characteristics.get(
            CameraCharacteristics.SENSOR_INFO_EXPOSURE_TIME_RANGE)

        val frameDurationNs = 1_000_000_000L / fps.coerceAtLeast(1)
        var applied = shutterNs.coerceAtMost(frameDurationNs)
        if (exposureRange != null) applied = applied.coerceIn(exposureRange.lower, exposureRange.upper)

        val appliedIso = if (isoRange != null) iso.coerceIn(isoRange.lower, isoRange.upper) else iso

        update { b ->
            b.set(CaptureRequest.CONTROL_AE_MODE, CaptureRequest.CONTROL_AE_MODE_OFF)
            b.set(CaptureRequest.SENSOR_SENSITIVITY, appliedIso)
            b.set(CaptureRequest.SENSOR_EXPOSURE_TIME, applied)
            // Pinning the frame duration stops the HAL from stretching it to suit
            // the exposure and dragging the frame rate down with it.
            b.set(CaptureRequest.SENSOR_FRAME_DURATION, frameDurationNs)
        }
        Log.i(TAG, "manual exposure: ISO $appliedIso, ${applied / 1000}us " +
                "(requested ${shutterNs / 1000}us)")
        return applied
    }

    fun setAutoExposure() = update { b ->
        b.set(CaptureRequest.CONTROL_AE_MODE, CaptureRequest.CONTROL_AE_MODE_ON)
        b.set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, pickFpsRange())
    }

    /** [dioptres] runs from 0 (infinity) to the lens minimum focus distance. */
    fun setManualFocus(dioptres: Float): Float {
        val minDistance = characteristics.get(
            CameraCharacteristics.LENS_INFO_MINIMUM_FOCUS_DISTANCE) ?: 0f
        if (minDistance <= 0f) return -1f      // fixed-focus lens

        cancelRamp("focus")
        val applied = dioptres.coerceIn(0f, minDistance)
        applyFocus(applied)
        return applied
    }

    /** The same without the clamp or the cancel, for the ramp to drive. */
    private fun applyFocus(dioptres: Float) {
        lastFocusDioptres = dioptres
        update { b ->
            b.set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_OFF)
            b.set(CaptureRequest.LENS_FOCUS_DISTANCE, dioptres)
        }
    }

    fun setWhiteBalancePreset(mode: String): Boolean {
        val value = when (mode) {
            "auto" -> CaptureRequest.CONTROL_AWB_MODE_AUTO
            "incandescent" -> CaptureRequest.CONTROL_AWB_MODE_INCANDESCENT
            "fluorescent" -> CaptureRequest.CONTROL_AWB_MODE_FLUORESCENT
            "daylight" -> CaptureRequest.CONTROL_AWB_MODE_DAYLIGHT
            "cloudy" -> CaptureRequest.CONTROL_AWB_MODE_CLOUDY_DAYLIGHT
            "shade" -> CaptureRequest.CONTROL_AWB_MODE_SHADE
            else -> return false
        }
        update { b ->
            b.set(CaptureRequest.CONTROL_AWB_MODE, value)
            b.set(CaptureRequest.COLOR_CORRECTION_MODE,
                CaptureRequest.COLOR_CORRECTION_MODE_FAST)
        }
        return true
    }

    /**
     * Manual white balance from a colour temperature. The gains are ignored
     * unless COLOR_CORRECTION_MODE is switched to TRANSFORM_MATRIX in the same
     * request, which is the usual reason manual WB appears to do nothing.
     */
    fun setManualWhiteBalance(kelvin: Int): Int {
        val applied = kelvin.coerceIn(2000, 10000)
        val gains = gainsForTemperature(applied)

        update { b ->
            b.set(CaptureRequest.CONTROL_AWB_MODE, CaptureRequest.CONTROL_AWB_MODE_OFF)
            b.set(CaptureRequest.COLOR_CORRECTION_MODE,
                CaptureRequest.COLOR_CORRECTION_MODE_TRANSFORM_MATRIX)
            b.set(CaptureRequest.COLOR_CORRECTION_GAINS, gains)
        }
        return applied
    }

    /**
     * Flat "log" picture profile.
     *
     * The point is to keep highlight and shadow detail that the default tonemap
     * throws away, so the footage can be graded afterwards rather than arriving
     * already baked. Camera2 has no log mode, so this is built the only way it
     * can be: a custom tonemap curve that lifts the shadows and rolls off the
     * top, plus a desaturating colour matrix, since a log image that is still
     * fully saturated grades badly.
     *
     * Requires MANUAL_POST_PROCESSING and CONTRAST_CURVE support. Returns false
     * when the camera cannot do it, so the UI can say so instead of showing a
     * control that does nothing.
     */
    fun setPictureProfile(log: Boolean): Boolean {
        val modes = characteristics.get(
            CameraCharacteristics.TONEMAP_AVAILABLE_TONE_MAP_MODES)?.toSet() ?: emptySet()
        if (log && !modes.contains(CameraCharacteristics.TONEMAP_MODE_CONTRAST_CURVE)) {
            return false
        }

        update { b ->
            if (log) {
                b.set(CaptureRequest.TONEMAP_MODE, CaptureRequest.TONEMAP_MODE_CONTRAST_CURVE)
                b.set(CaptureRequest.TONEMAP_CURVE, logToneCurve())
                b.set(CaptureRequest.COLOR_CORRECTION_MODE,
                    CaptureRequest.COLOR_CORRECTION_MODE_TRANSFORM_MATRIX)
                b.set(CaptureRequest.COLOR_CORRECTION_TRANSFORM, desaturationMatrix())
                // Sharpening and noise reduction bake decisions into the image
                // that grading cannot undo. Not every HAL accepts either key,
                // and a rejected one must not take the whole profile with it.
                ignoringFailure { b.set(CaptureRequest.EDGE_MODE, CaptureRequest.EDGE_MODE_OFF) }
                ignoringFailure {
                    b.set(CaptureRequest.NOISE_REDUCTION_MODE,
                        CaptureRequest.NOISE_REDUCTION_MODE_MINIMAL)
                }
            } else {
                // Back to exactly what the template had, so turning log off
                // returns the picture the camera gives on its own.
                //
                // The curve and the colour transform stay in the request even
                // once the mode no longer selects them, and whether a HAL then
                // ignores them is its own business. Neutralising both means the
                // standard profile cannot inherit anything from the log one.
                b.set(CaptureRequest.TONEMAP_CURVE, identityToneCurve())
                b.set(CaptureRequest.COLOR_CORRECTION_TRANSFORM, identityColorTransform())
                b.set(CaptureRequest.COLOR_CORRECTION_GAINS,
                    android.hardware.camera2.params.RggbChannelVector(1f, 1f, 1f, 1f))

                defaultTonemapMode?.let { b.set(CaptureRequest.TONEMAP_MODE, it) }
                defaultColorCorrectionMode?.let {
                    b.set(CaptureRequest.COLOR_CORRECTION_MODE, it)
                }
                ignoringFailure { defaultEdgeMode?.let { b.set(CaptureRequest.EDGE_MODE, it) } }
                ignoringFailure {
                    defaultNoiseReductionMode?.let {
                        b.set(CaptureRequest.NOISE_REDUCTION_MODE, it)
                    }
                }
            }
        }
        Log.i(TAG, "picture profile: ${if (log) "log" else "standard"}")
        return true
    }

    /**
     * A flat log-style transfer curve.
     *
     *     out = lo + (hi - lo) * log10(1 + in * (base - 1)) / log10(base)
     *
     * Black lands at 0.075 rather than 0 and white at 0.92 rather than 1: a log
     * image is supposed to hold detail either side of the visible range, and
     * clipping it at the ends is what a grade cannot recover. Middle grey ends
     * up near 0.45, in the same territory as S-Log3 and C-Log.
     *
     * An earlier version of this used the Cineon code-value formula with its
     * 685/1023 white point added as an offset, which mapped black to 0.67 and
     * squeezed the whole image into the top third. That does not look like log,
     * it looks like the ISO has been run up, which is exactly how it was caught.
     */
    private fun logToneCurve(): android.hardware.camera2.params.TonemapCurve {
        val points = 32
        val lo = 0.075f
        val hi = 0.92f
        val base = 12.0

        val curve = FloatArray(points * 2)
        for (i in 0 until points) {
            val input = i.toFloat() / (points - 1)
            val shaped = Math.log10(1.0 + input * (base - 1.0)) / Math.log10(base)
            curve[i * 2] = input
            curve[i * 2 + 1] = (lo + (hi - lo) * shaped.toFloat()).coerceIn(0f, 1f)
        }
        // Same curve on all three channels: any per-channel difference here is a
        // colour cast the grade would have to remove.
        return android.hardware.camera2.params.TonemapCurve(curve, curve, curve)
    }

    /** A straight line: input maps to output unchanged. */
    private fun identityToneCurve(): android.hardware.camera2.params.TonemapCurve {
        val curve = floatArrayOf(0f, 0f, 1f, 1f)
        return android.hardware.camera2.params.TonemapCurve(curve, curve, curve)
    }

    /** The identity matrix, leaving colour exactly as the pipeline produced it. */
    private fun identityColorTransform(): android.hardware.camera2.params.ColorSpaceTransform {
        return android.hardware.camera2.params.ColorSpaceTransform(intArrayOf(
            1, 1, 0, 1, 0, 1,
            0, 1, 1, 1, 0, 1,
            0, 1, 0, 1, 1, 1,
        ))
    }

    private inline fun ignoringFailure(block: () -> Unit) {
        try { block() } catch (_: Throwable) {}
    }

    /**
     * Pulls saturation back towards neutral so the log image grades cleanly.
     * ColorSpaceTransform takes nine rationals as numerator/denominator pairs,
     * row-major, which is why this is a flat list of eighteen integers.
     */
    private fun desaturationMatrix(): android.hardware.camera2.params.ColorSpaceTransform {
        val s = 0.75f      // retained saturation
        val r = (1 - s) * 0.2126f
        val g = (1 - s) * 0.7152f
        val bl = (1 - s) * 0.0722f

        val d = 1000
        fun num(v: Float) = (v * d).toInt()

        return android.hardware.camera2.params.ColorSpaceTransform(intArrayOf(
            num(r + s), d, num(g),     d, num(bl),     d,
            num(r),     d, num(g + s), d, num(bl),     d,
            num(r),     d, num(g),     d, num(bl + s), d,
        ))
    }

    /**
     * Approximates per-channel gains for a colour temperature using a fit of the
     * Planckian locus. Exact colorimetry would need the sensor's calibration
     * matrices; this is close enough that the slider behaves the way the eye
     * expects, which is what a live preview control is for.
     */
    private fun gainsForTemperature(kelvin: Int): android.hardware.camera2.params.RggbChannelVector {
        val t = kelvin / 100.0

        val red = if (t <= 66) 255.0 else
            (329.698727446 * Math.pow(t - 60, -0.1332047592)).coerceIn(0.0, 255.0)
        val green = if (t <= 66)
            (99.4708025861 * Math.log(t) - 161.1195681661).coerceIn(0.0, 255.0)
        else
            (288.1221695283 * Math.pow(t - 60, -0.0755148492)).coerceIn(0.0, 255.0)
        val blue = when {
            t >= 66 -> 255.0
            t <= 19 -> 0.0
            else -> (138.5177312231 * Math.log(t - 10) - 305.0447927307).coerceIn(0.0, 255.0)
        }

        // Camera gains are the inverse of the illuminant: warm light needs the
        // blue channel lifted, not the red.
        val rGain = (255.0 / red.coerceAtLeast(1.0)).toFloat()
        val gGain = (255.0 / green.coerceAtLeast(1.0)).toFloat()
        val bGain = (255.0 / blue.coerceAtLeast(1.0)).toFloat()

        // Normalise so green sits at 1.0, which is what the HAL expects as the
        // neutral reference.
        return android.hardware.camera2.params.RggbChannelVector(
            (rGain / gGain).coerceIn(1.0f, 4.0f),
            1.0f,
            1.0f,
            (bGain / gGain).coerceIn(1.0f, 4.0f),
        )
    }

    /** Digital-zoom fallback for pre-R devices that lack CONTROL_ZOOM_RATIO. */
    private fun cropRegionFor(ratio: Float): Rect {
        val active = characteristics.get(CameraCharacteristics.SENSOR_INFO_ACTIVE_ARRAY_SIZE)
            ?: return Rect()
        val r = ratio.coerceAtLeast(1.0f)
        val w = (active.width() / r).toInt()
        val h = (active.height() / r).toInt()
        val cx = active.centerX()
        val cy = active.centerY()
        return Rect(cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2)
    }

    /**
     * Controls asked for before the capture session existed.
     *
     * A pipeline restart -- any change of resolution, frame rate, codec or
     * camera -- tears the session down and builds a new one, and opening a
     * camera is asynchronous. The desktop re-sends everything it believes is in
     * force the moment it sees the new CONFIG, but CONFIG comes from the
     * encoder announcing its output format, which MediaCodec does almost
     * immediately and well before the camera has finished configuring.
     *
     * So the commands arrived at a session that did not exist yet and were
     * dropped without a word, and the picture came back on auto with the panel
     * still showing the values it had asked for. They are held here instead and
     * replayed the moment there is something to apply them to.
     */
    private val pending = mutableListOf<(CaptureRequest.Builder) -> Unit>()

    private fun update(block: (CaptureRequest.Builder) -> Unit) {
        val b = requestBuilder
        val s = session
        if (b == null || s == null) {
            synchronized(pending) {
                // A hundred queued controls would mean something else is wrong;
                // the cap is there so a camera that never opens cannot grow this
                // without bound.
                if (pending.size < 100) pending += block
            }
            return
        }
        try {
            block(b)
            submit(s, b)
        } catch (t: Throwable) {
            Log.w(TAG, "control update failed", t)
        }
    }

    // ---- ramps ---------------------------------------------------------

    /**
     * A control moved from where it is to somewhere else, over time.
     *
     * The curve runs here rather than on the desktop, and that is the whole
     * point. A desktop sending sixty positions a second would put a focus pull
     * at the mercy of the link: one late packet on Wi-Fi and the move stutters
     * in a way no amount of easing hides. Sent as an intention -- go there, take
     * this long -- it cannot stutter, because nothing arrives while it happens.
     *
     * One at a time, and any manual change to the same control cancels it. A
     * pull someone has decided to interrupt is over.
     */
    private inner class Ramp(
        val what: String,
        val from: Float,
        val to: Float,
        val durationMs: Long,
        val apply: (Float) -> Unit,
    ) : Runnable {
        val startedAt = SystemClock.elapsedRealtime()

        override fun run() {
            if (activeRamp !== this) return

            val elapsed = SystemClock.elapsedRealtime() - startedAt
            val t = if (durationMs <= 0) 1f
                    else (elapsed.toFloat() / durationMs).coerceIn(0f, 1f)

            // Smoothstep: starts and stops at rest, which is what a hand on a
            // follow focus does and a linear ramp conspicuously does not.
            val eased = t * t * (3f - 2f * t)
            apply(from + (to - from) * eased)

            if (t >= 1f) {
                Log.i(TAG, "$what ramp finished at $to")
                activeRamp = null
                return
            }
            // Every other frame at 60, every frame at 30. Finer than the sensor
            // can act on would be work with nothing to show for it.
            handler.postDelayed(this, 16)
        }
    }

    @Volatile private var activeRamp: Ramp? = null

    /** True while a ramp is running, so the client can show it. */
    fun ramping(): String? = activeRamp?.what

    /**
     * Cancels any ramp on [what], or all of them when null.
     *
     * Called by every manual setter below: reaching for a control during a move
     * is how a person says they want the control back.
     */
    fun cancelRamp(what: String? = null) {
        val ramp = activeRamp ?: return
        if (what == null || ramp.what == what) {
            Log.i(TAG, "${ramp.what} ramp cancelled")
            activeRamp = null
        }
    }

    /**
     * Starts a ramp. Returns false when this camera cannot do it at all -- a
     * fixed-focus lens, or a zoom the device does not support -- which the
     * client needs as a refusal rather than as a move that never happens.
     */
    fun startRamp(what: String, target: Float, durationMs: Long): Boolean {
        val from: Float
        val apply: (Float) -> Unit

        when (what) {
            "focus" -> {
                val minDistance = characteristics.get(
                    CameraCharacteristics.LENS_INFO_MINIMUM_FOCUS_DISTANCE) ?: 0f
                if (minDistance <= 0f) return false
                from = lastFocusDioptres
                apply = { value -> applyFocus(value.coerceIn(0f, minDistance)) }
            }
            "zoom" -> {
                from = lastZoom
                apply = { value -> applyZoom(value) }
            }
            else -> return false
        }

        activeRamp = null
        val ramp = Ramp(what, from, target, durationMs, apply)
        activeRamp = ramp
        Log.i(TAG, "$what ramp $from -> $target over ${durationMs}ms")
        handler.post(ramp)
        return true
    }

    // ---- framing -------------------------------------------------------

    /**
     * Where the framing should be, in the sensor's own coordinates, normalised
     * to 0..1 of the active array.
     *
     * The desktop decides this -- it has the decoded picture and can look at it
     * -- but the crop happens here, in the sensor. That is the whole reason this
     * exists: the sensor is larger than the stream, so framing inside it costs
     * nothing at all. A desktop that cropped and rescaled its own copy would
     * throw away resolution to do the same job, and pay an encode for it.
     */
    @Volatile private var cropTarget: FloatArray? = null
    @Volatile private var cropNow: FloatArray? = null
    @Volatile private var cropDriving = false

    /**
     * Asks for a framing. Null hands the sensor back.
     *
     * The move is eased here rather than obeyed on arrival: a detector that
     * jitters by a few pixels between frames would otherwise show up as a camera
     * that trembles. What arrives is where to end up, and this decides how fast
     * to get there -- which is the difference between an operator and a servo.
     */
    fun setFraming(x: Float, y: Float, w: Float, h: Float) {
        cropTarget = floatArrayOf(x, y, w, h)
        if (!cropDriving) {
            cropDriving = true
            handler.post(cropDriver)
        }
    }

    fun clearFraming() {
        cropTarget = null
        cropNow = null
        cropDriving = false
        update { b -> b.set(CaptureRequest.SCALER_CROP_REGION, fullSensorRect()) }
    }

    private fun fullSensorRect(): Rect =
        characteristics.get(CameraCharacteristics.SENSOR_INFO_ACTIVE_ARRAY_SIZE)
            ?: Rect(0, 0, 1, 1)

    private val cropDriver = object : Runnable {
        override fun run() {
            if (!cropDriving) return
            val target = cropTarget ?: run { cropDriving = false; return }

            // A fixed fraction of the remaining distance each tick, which is an
            // exponential approach: fast while far, slow as it arrives, and it
            // never overshoots. Thirty percent at sixty hertz settles a large
            // move in about a fifth of a second.
            val current = cropNow ?: target.copyOf().also { cropNow = it }
            var moved = false
            for (i in 0 until 4) {
                val delta = target[i] - current[i]
                if (kotlin.math.abs(delta) > 0.0005f) {
                    current[i] += delta * 0.3f
                    moved = true
                }
            }

            val sensor = fullSensorRect()
            val left = (current[0] * sensor.width()).toInt().coerceIn(0, sensor.width() - 2)
            val top = (current[1] * sensor.height()).toInt().coerceIn(0, sensor.height() - 2)
            val width = (current[2] * sensor.width()).toInt()
                .coerceIn(2, sensor.width() - left)
            val height = (current[3] * sensor.height()).toInt()
                .coerceIn(2, sensor.height() - top)

            update { b ->
                b.set(CaptureRequest.SCALER_CROP_REGION,
                      Rect(left, top, left + width, top + height))
            }

            // Stops posting once it has arrived. A driver that keeps running
            // after the picture has settled is a wakeup per frame for nothing.
            if (moved) handler.postDelayed(this, 16) else cropDriving = false
        }
    }

    // ---- recording target ----------------------------------------------

    @Volatile private var recordActive = false
    @Volatile private var recordDivisor = 1

    /**
     * Adds or removes the recorder from the repeating request.
     *
     * [divisor] is how many streamed frames pass per recorded one. One sensor
     * produces both, so it runs at a single rate and the only way to record
     * slower is to leave the record target out of some requests -- which is what
     * the burst below does. A divisor of 1 records every frame.
     *
     * Returns the divisor actually in force.
     */
    fun setRecording(active: Boolean, divisor: Int): Int {
        if (recordTarget == null) return 0
        recordActive = active
        recordDivisor = divisor.coerceAtLeast(1)
        update { }
        Log.i(TAG, "record target ${if (active) "on" else "off"} (every ${recordDivisor} frames)")
        return recordDivisor
    }

    /**
     * Submits the repeating work, as a plain request when nothing is being
     * recorded and as a burst when the recording rate is a fraction of the
     * streaming one.
     *
     * The record target is always removed again before returning, so the
     * builder's target set means one thing only: what the *stream* needs. Any
     * other arrangement makes every caller of [update] responsible for
     * remembering the recorder's state.
     */
    /**
     * Reports capture failures.
     *
     * Without this the session fails silently: a request the HAL rejects simply
     * produces no frame, and every symptom -- a stream that stops, an encoder
     * with nothing to drain -- points somewhere other than the camera. Passing
     * null here cost an afternoon.
     */
    private val captureCallback = object : CameraCaptureSession.CaptureCallback() {
        private var failures = 0

        override fun onCaptureFailed(
            s: CameraCaptureSession,
            request: CaptureRequest,
            failure: android.hardware.camera2.CaptureFailure,
        ) {
            // One line per failure would fill the log at frame rate.
            if (failures++ % 30 == 0) {
                Log.w(TAG, "capture failed (#$failures): reason=${failure.reason} " +
                        "frame=${failure.frameNumber} " +
                        "imageCaptured=${failure.wasImageCaptured()}")
            }
        }

        override fun onCaptureBufferLost(
            s: CameraCaptureSession,
            request: CaptureRequest,
            target: Surface,
            frameNumber: Long,
        ) {
            Log.w(TAG, "buffer lost on frame $frameNumber " +
                    "(${if (target === recordTarget) "record" else "stream"})")
        }
    }

    private fun submit(s: CameraCaptureSession, b: CaptureRequest.Builder) {
        val rec = recordTarget
        if (!recordActive || rec == null) {
            s.setRepeatingRequest(b.build(), captureCallback, handler)
            return
        }

        if (recordDivisor <= 1) {
            b.addTarget(rec)
            val request = b.build()
            b.removeTarget(rec)
            s.setRepeatingRequest(request, captureCallback, handler)
            return
        }

        val burst = ArrayList<CaptureRequest>(recordDivisor)
        b.addTarget(rec)
        burst += b.build()
        b.removeTarget(rec)
        repeat(recordDivisor - 1) { burst += b.build() }
        s.setRepeatingBurst(burst, captureCallback, handler)
    }

    /**
     * Shuts the session down and does not return until the camera device is
     * actually free.
     *
     * The wait is the point. close() is asynchronous, and starting the next
     * session before the last device has gone gets ERROR_MAX_CAMERAS_IN_USE --
     * so every resolution change and camera switch would be a coin toss.
     */
    fun close() {
        closed = true
        try { session?.stopRepeating() } catch (_: Throwable) {}
        try { session?.close() } catch (_: Throwable) {}
        val hadDevice = device != null || everOpened
        try { device?.close() } catch (_: Throwable) {}
        session = null
        device = null

        if (hadDevice && !deviceClosed.await(CLOSE_TIMEOUT_MS, TimeUnit.MILLISECONDS)) {
            Log.w(TAG, "camera did not report closed within ${CLOSE_TIMEOUT_MS}ms")
        }
        thread.quitSafely()
    }

    companion object {
        private const val TAG = "XCam/Camera"
        private const val CLOSE_TIMEOUT_MS = 2000L
    }
}
