package com.xcam.camera

import android.content.Context
import android.graphics.ImageFormat
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraManager
import android.media.MediaCodecList
import android.media.MediaFormat
import android.os.Build
import android.util.Log
import android.util.Size
import org.json.JSONArray
import org.json.JSONObject

/**
 * Builds the handshake payload the PC uses to populate its device pickers.
 *
 * Xiaomi exposes its ultra-wide and tele modules both as standalone camera ids
 * and as physical members of a logical camera, so we report both: the PC can
 * either open a dedicated id or stay on the logical camera and drive
 * CONTROL_ZOOM_RATIO across the lens range.
 */
object CameraEnumerator {

    private const val TAG = "XCam/Enum"

    fun describeDevice(context: Context): JSONObject {
        val manager = context.getSystemService(Context.CAMERA_SERVICE) as CameraManager

        val cameras = JSONArray()
        for (id in manager.cameraIdList) {
            try {
                cameras.put(describeCamera(manager, id))
            } catch (t: Throwable) {
                Log.w(TAG, "skipping camera $id", t)
            }
        }

        return JSONObject().apply {
            put("deviceName", "${Build.MANUFACTURER} ${Build.MODEL}")
            put("androidApi", Build.VERSION.SDK_INT)
            put("cameras", cameras)
            put("codecs", JSONArray(supportedCodecs()))
            put("maxBitrate", 200_000_000)

            // Recording needs a second encoder instance running alongside the
            // streaming one. Reported rather than assumed: whether a device can
            // give one is a property of the SoC, and a client that offers the
            // control anyway would show a button that fails on press.
            put("recorder", canRunSecondEncoder())
            // The client's only way to collect a file is `adb pull` of an
            // absolute path, so the path is part of the handshake.
            put("recordDir",
                java.io.File(context.getExternalFilesDir(null) ?: context.filesDir,
                             "recordings").absolutePath)

            // Sound is reported as available only when the permission is
            // actually held. A client that offered a microphone control on the
            // strength of the hardware existing would be offering something the
            // user has already declined.
            // Where a client on the same network can reach this phone. The
            // server has always listened on every interface; what was missing
            // was any way to find out the address, which is not something the
            // desktop can work out for itself.
            wifiAddress(context)?.let { put("wifiAddress", it) }

            put("audio", JSONObject().apply {
                val granted = context.checkSelfPermission(
                    android.Manifest.permission.RECORD_AUDIO) ==
                    android.content.pm.PackageManager.PERMISSION_GRANTED
                put("available", granted)
                put("codec", "aac")
                put("sampleRate", 48_000)
                put("channels", 2)
                put("bitrate", 192_000)
            })
        }
    }

    /**
     * The phone's IPv4 address on the active network, or null when there is no
     * usable one.
     *
     * Deliberately IPv4 only: this exists to be typed into `--host` on a desktop
     * by a person, and a link-local IPv6 address with a scope suffix is not that.
     */
    private fun wifiAddress(context: Context): String? = try {
        val cm = context.getSystemService(android.net.ConnectivityManager::class.java)
        val network = cm?.activeNetwork
        val links = network?.let { cm.getLinkProperties(it) }
        links?.linkAddresses
            ?.map { it.address }
            ?.firstOrNull { it is java.net.Inet4Address && !it.isLoopbackAddress }
            ?.hostAddress
    } catch (t: Throwable) {
        Log.w(TAG, "could not read the network address", t)
        null
    }

    /**
     * Whether the hardware can encode two streams at once.
     *
     * `maxSupportedInstances` is the encoder's own count of how many sessions it
     * will run concurrently. Anything less than two means the recording encoder
     * would take the streaming one's place, which is worse than not offering it.
     */
    private fun canRunSecondEncoder(): Boolean = try {
        MediaCodecList(MediaCodecList.REGULAR_CODECS).codecInfos.any { info ->
            info.isEncoder && info.supportedTypes.any { type ->
                (type.equals(MediaFormat.MIMETYPE_VIDEO_AVC, true) ||
                 type.equals(MediaFormat.MIMETYPE_VIDEO_HEVC, true)) &&
                (info.getCapabilitiesForType(type).maxSupportedInstances >= 2)
            }
        }
    } catch (t: Throwable) {
        Log.w(TAG, "could not count encoder instances", t)
        false
    }

    private fun describeCamera(manager: CameraManager, id: String): JSONObject {
        val c = manager.getCameraCharacteristics(id)
        val map = c.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)

        // Encoder input is a Surface, so the sizes that matter are the ones the
        // camera can deliver to a SurfaceTexture-class consumer.
        val sizes: List<Size> = map?.getOutputSizes(ImageFormat.PRIVATE)?.toList()
            ?: map?.getOutputSizes(android.graphics.SurfaceTexture::class.java)?.toList()
            ?: emptyList()

        val maxRes = sizes.maxByOrNull { it.width.toLong() * it.height } ?: Size(1920, 1080)

        val fpsRanges = c.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES)
        val maxFps = fpsRanges?.maxOfOrNull { it.upper } ?: 30

        val capabilities = c.get(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES)
            ?.toSet() ?: emptySet()
        val manualSensor = capabilities.contains(
            CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES_MANUAL_SENSOR)
        val manualPost = capabilities.contains(
            CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES_MANUAL_POST_PROCESSING)

        val facing = when (c.get(CameraCharacteristics.LENS_FACING)) {
            CameraCharacteristics.LENS_FACING_FRONT -> "front"
            CameraCharacteristics.LENS_FACING_BACK -> "back"
            else -> "external"
        }

        val zoomRange = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            c.get(CameraCharacteristics.CONTROL_ZOOM_RATIO_RANGE)
        } else null

        val physicalIds: Set<String> =
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) c.physicalCameraIds
            else emptySet()

        val iso = c.get(CameraCharacteristics.SENSOR_INFO_SENSITIVITY_RANGE)
        val exposure = c.get(CameraCharacteristics.SENSOR_INFO_EXPOSURE_TIME_RANGE)
        val evRange = c.get(CameraCharacteristics.CONTROL_AE_COMPENSATION_RANGE)
        val evStep = c.get(CameraCharacteristics.CONTROL_AE_COMPENSATION_STEP)

        // 0 dioptres means the lens is fixed focus, so manual focus is off the
        // table however capable the rest of the camera is.
        val minFocusDistance =
            c.get(CameraCharacteristics.LENS_INFO_MINIMUM_FOCUS_DISTANCE) ?: 0f

        return JSONObject().apply {
            put("id", id)
            put("facing", facing)
            put("label", labelFor(c, facing, id))
            put("maxRes", JSONArray(listOf(maxRes.width, maxRes.height)))
            put("maxFps", maxFps)
            put("modes", modesFor(map, sizes, maxFps))
            put("zoomRange", JSONArray(listOf(zoomRange?.lower ?: 1.0f, zoomRange?.upper ?: 1.0f)))
            put("hasTorch", c.get(CameraCharacteristics.FLASH_INFO_AVAILABLE) == true)
            put("logical", physicalIds.isNotEmpty())
            put("physicalIds", JSONArray(physicalIds.toList()))

            put("logProfile", c.get(CameraCharacteristics.TONEMAP_AVAILABLE_TONE_MAP_MODES)
                ?.contains(CameraCharacteristics.TONEMAP_MODE_CONTRAST_CURVE) == true &&
                manualPost)
            put("manualSensor", manualSensor)
            put("manualPostProcessing", manualPost)
            put("isoRange", JSONArray(listOf(iso?.lower ?: 0, iso?.upper ?: 0)))
            put("exposureRangeNs", JSONArray(listOf(exposure?.lower ?: 0L, exposure?.upper ?: 0L)))
            put("minFocusDistance", minFocusDistance)
            put("afModes", JSONArray(afModesFor(c, minFocusDistance)))
            put("awbModes", JSONArray(awbModesFor(c)))
            put("evRange", JSONArray(listOf(
                (evRange?.lower ?: 0) * (evStep?.toFloat() ?: 0f),
                (evRange?.upper ?: 0) * (evStep?.toFloat() ?: 0f),
            )))
            put("evStep", evStep?.toFloat() ?: 0f)
        }
    }

    /**
     * Pairs each offered size with the frame rate it can actually sustain.
     * getOutputMinFrameDuration is the honest source: a sensor may advertise
     * 60fps overall and still cap a large capture at 30, and a size without its
     * real frame rate is not something a UI can act on.
     */
    private fun modesFor(
        map: android.hardware.camera2.params.StreamConfigurationMap?,
        sizes: List<Size>,
        deviceMaxFps: Int,
    ): JSONArray {
        val out = JSONArray()
        for (size in sizes.sortedByDescending { it.width.toLong() * it.height }) {
            val fps = try {
                val minDurationNs = map?.getOutputMinFrameDuration(
                    android.graphics.SurfaceTexture::class.java, size) ?: 0L
                if (minDurationNs > 0) {
                    (1_000_000_000.0 / minDurationNs).toInt().coerceAtMost(deviceMaxFps)
                } else deviceMaxFps
            } catch (_: Throwable) {
                deviceMaxFps
            }
            out.put(JSONObject().apply {
                put("size", JSONArray(listOf(size.width, size.height)))
                put("maxFps", fps)
            })
        }
        return out
    }

    private fun afModesFor(c: CameraCharacteristics, minFocusDistance: Float): List<String> {
        val available = c.get(CameraCharacteristics.CONTROL_AF_AVAILABLE_MODES)?.toSet()
            ?: return listOf("continuous")

        val modes = mutableListOf<String>()
        if (available.contains(CameraCharacteristics.CONTROL_AF_MODE_CONTINUOUS_VIDEO)) {
            modes += "continuous"
        }
        if (available.contains(CameraCharacteristics.CONTROL_AF_MODE_AUTO)) modes += "auto"
        if (available.contains(CameraCharacteristics.CONTROL_AF_MODE_OFF) && minFocusDistance > 0f) {
            modes += "manual"
        }
        return modes.ifEmpty { listOf("continuous") }
    }

    private fun awbModesFor(c: CameraCharacteristics): List<String> {
        val available = c.get(CameraCharacteristics.CONTROL_AWB_AVAILABLE_MODES)?.toSet()
            ?: return listOf("auto")

        val names = mapOf(
            CameraCharacteristics.CONTROL_AWB_MODE_AUTO to "auto",
            CameraCharacteristics.CONTROL_AWB_MODE_INCANDESCENT to "incandescent",
            CameraCharacteristics.CONTROL_AWB_MODE_FLUORESCENT to "fluorescent",
            CameraCharacteristics.CONTROL_AWB_MODE_DAYLIGHT to "daylight",
            CameraCharacteristics.CONTROL_AWB_MODE_CLOUDY_DAYLIGHT to "cloudy",
            CameraCharacteristics.CONTROL_AWB_MODE_SHADE to "shade",
        )
        return names.filterKeys { available.contains(it) }.values.toList().ifEmpty { listOf("auto") }
    }

    /**
     * Derives a human label from focal length, since Camera2 has no name field.
     * Wider than ~20mm equivalent is the ultra-wide, longer than ~50mm the tele.
     */
    private fun labelFor(c: CameraCharacteristics, facing: String, id: String): String {
        if (facing == "front") return "Front"

        val focal = c.get(CameraCharacteristics.LENS_INFO_AVAILABLE_FOCAL_LENGTHS)
            ?.minOrNull() ?: return "Camera $id"
        val sensor = c.get(CameraCharacteristics.SENSOR_INFO_PHYSICAL_SIZE)
            ?: return "Camera $id"

        // 35mm-equivalent focal length from the sensor diagonal.
        val diagonal = kotlin.math.hypot(sensor.width, sensor.height)
        val equiv = focal * (43.27f / diagonal)

        return when {
            equiv < 20f -> "Ultra-wide"
            equiv < 40f -> "Main"
            else -> "Telephoto"
        }
    }

    private fun supportedCodecs(): List<String> {
        val list = MediaCodecList(MediaCodecList.REGULAR_CODECS)
        val out = mutableListOf<String>()
        val wanted = mapOf(
            MediaFormat.MIMETYPE_VIDEO_AVC to "h264",
            MediaFormat.MIMETYPE_VIDEO_HEVC to "hevc",
        )
        for ((mime, name) in wanted) {
            val hasHardwareEncoder = list.codecInfos.any { info ->
                info.isEncoder &&
                    info.supportedTypes.any { it.equals(mime, ignoreCase = true) } &&
                    (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q || info.isHardwareAccelerated)
            }
            if (hasHardwareEncoder) out += name
        }
        return out.ifEmpty { listOf("h264") }
    }
}
