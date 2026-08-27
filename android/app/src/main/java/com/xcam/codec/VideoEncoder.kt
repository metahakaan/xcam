package com.xcam.codec

import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.os.Build
import android.util.Log
import android.view.Surface
import java.nio.ByteBuffer

/**
 * Hardware H.264/HEVC encoder fed by a Surface, so camera frames never touch
 * the CPU: Camera2 renders straight into [inputSurface] and MediaCodec consumes
 * it on the GPU.
 */
class VideoEncoder(
    private val config: Config,
    private val sink: Sink,
) {

    data class Config(
        val width: Int,
        val height: Int,
        val fps: Int,
        val bitrate: Int,
        val codec: String,          // "h264" | "hevc"
        val iFrameIntervalSec: Float = 1.0f,
    ) {
        val mime: String get() = if (codec == "hevc") MediaFormat.MIMETYPE_VIDEO_HEVC
                                 else MediaFormat.MIMETYPE_VIDEO_AVC
    }

    /** Called on the encoder's own thread. Implementations must not block long. */
    interface Sink {
        fun onConfig(csd: ByteArray)
        fun onFrame(data: ByteArray, isKeyFrame: Boolean, ptsUs: Long)
        fun onError(t: Throwable)
    }

    @Volatile private var codec: MediaCodec? = null
    @Volatile private var running = false

    /**
     * Most encoders hand the same SPS/PPS over twice -- once through
     * INFO_OUTPUT_FORMAT_CHANGED and again as a BUFFER_FLAG_CODEC_CONFIG buffer.
     * The protocol has receivers reset their decoder on every CONFIG, so passing
     * both along would cost a visible stall for no gain. First one wins.
     */
    @Volatile private var csdSent = false
    @Volatile private var lastCsd: ByteArray? = null

    lateinit var inputSurface: Surface
        private set

    private var thread: Thread? = null

    fun start() {
        val format = MediaFormat.createVideoFormat(config.mime, config.width, config.height).apply {
            setInteger(MediaFormat.KEY_COLOR_FORMAT,
                MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface)
            setInteger(MediaFormat.KEY_BIT_RATE, config.bitrate)
            setInteger(MediaFormat.KEY_FRAME_RATE, config.fps)
            setFloat(MediaFormat.KEY_I_FRAME_INTERVAL, config.iFrameIntervalSec)
            setInteger(MediaFormat.KEY_BITRATE_MODE,
                MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR)

            // Profile/level are advisory; some Xiaomi encoders reject the pair and
            // fall back on their own, which is fine.
            if (config.mime == MediaFormat.MIMETYPE_VIDEO_AVC) {
                setInteger(MediaFormat.KEY_PROFILE,
                    MediaCodecInfo.CodecProfileLevel.AVCProfileHigh)
                setInteger(MediaFormat.KEY_LEVEL,
                    MediaCodecInfo.CodecProfileLevel.AVCLevel51)
            }

            // Repeat SPS/PPS ahead of every IDR. Decoders that attach mid-stream
            // -- anything opening the virtual camera after capture started --
            // cannot decode a single frame without them, and the one-shot
            // codec-config buffer is long gone by then.
            trySet { setInteger(MediaFormat.KEY_PREPEND_HEADER_TO_SYNC_FRAMES, 1) }

            // Low-latency hints. Not every vendor honours them, hence the guards.
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                trySet { setInteger(MediaFormat.KEY_LATENCY, 1) }
                trySet { setInteger(MediaFormat.KEY_PRIORITY, 0) }
            }
            trySet { setInteger("vendor.qti-ext-enc-low-latency.enable", 1) }
        }

        val mc = MediaCodec.createEncoderByType(config.mime)
        mc.configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
        inputSurface = mc.createInputSurface()
        mc.start()

        codec = mc
        running = true
        csdSent = false
        thread = Thread({ drainLoop(mc) }, "xcam-encoder").apply {
            priority = Thread.MAX_PRIORITY
            start()
        }
        Log.i(TAG, "encoder started ${config.width}x${config.height}@${config.fps} " +
                "${config.codec} ${config.bitrate / 1_000_000}Mbps")
    }

    /** Asks the encoder to emit an IDR on the next frame. */
    fun requestKeyFrame() {
        val mc = codec ?: return
        try {
            mc.setParameters(android.os.Bundle().apply {
                putInt(MediaCodec.PARAMETER_KEY_REQUEST_SYNC_FRAME, 0)
            })
        } catch (t: Throwable) {
            Log.w(TAG, "requestKeyFrame failed", t)
        }
    }

    /** Adjusts the target bitrate without restarting the pipeline. */
    fun setBitrate(bps: Int) {
        val mc = codec ?: return
        try {
            mc.setParameters(android.os.Bundle().apply {
                putInt(MediaCodec.PARAMETER_KEY_VIDEO_BITRATE, bps)
            })
        } catch (t: Throwable) {
            Log.w(TAG, "setBitrate failed", t)
        }
    }

    fun stop() {
        running = false
        thread?.join(1000)
        thread = null
        try { codec?.signalEndOfInputStream() } catch (_: Throwable) {}
        try { codec?.stop() } catch (_: Throwable) {}
        try { codec?.release() } catch (_: Throwable) {}
        codec = null
        if (::inputSurface.isInitialized) {
            try { inputSurface.release() } catch (_: Throwable) {}
        }
        Log.i(TAG, "encoder stopped")
    }

    private fun drainLoop(mc: MediaCodec) {
        val info = MediaCodec.BufferInfo()
        try {
            while (running) {
                val index = mc.dequeueOutputBuffer(info, TIMEOUT_US)
                when {
                    index == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED -> {
                        // csd-0/csd-1 arrive here for most encoders.
                        emitCsd(mc.outputFormat)
                    }
                    index == MediaCodec.INFO_TRY_AGAIN_LATER -> Unit
                    index >= 0 -> {
                        val buf = mc.getOutputBuffer(index)
                        if (buf != null && info.size > 0) {
                            buf.position(info.offset)
                            buf.limit(info.offset + info.size)
                            val bytes = ByteArray(info.size)
                            buf.get(bytes)

                            if (info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG != 0) {
                                emitConfigOnce(bytes)
                            } else {
                                val key = info.flags and MediaCodec.BUFFER_FLAG_KEY_FRAME != 0
                                sink.onFrame(bytes, key, info.presentationTimeUs)
                            }
                        }
                        mc.releaseOutputBuffer(index, false)
                        if (info.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM != 0) break
                    }
                }
            }
        } catch (t: Throwable) {
            if (running) sink.onError(t)
        }
    }

    private fun emitCsd(format: MediaFormat) {
        val parts = mutableListOf<ByteBuffer>()
        // H.264 uses csd-0 (SPS) + csd-1 (PPS); HEVC packs VPS+SPS+PPS into csd-0.
        for (key in arrayOf("csd-0", "csd-1", "csd-2")) {
            if (format.containsKey(key)) parts += format.getByteBuffer(key)!!
        }
        if (parts.isEmpty()) return

        val total = parts.sumOf { it.remaining() }
        val out = ByteArray(total)
        var off = 0
        for (p in parts) {
            val n = p.remaining()
            p.duplicate().get(out, off, n)
            off += n
        }
        emitConfigOnce(out)
    }

    /**
     * Re-announces the codec configuration.
     *
     * A client that joined part-way through a session cannot decode a single
     * frame without it, and the encoder says it exactly once. Kept rather than
     * regenerated: asking the encoder again would mean restarting it, which is
     * the one thing a hand-over exists to avoid.
     */
    fun resendConfig() {
        lastCsd?.let { sink.onConfig(it) }
    }

    private fun emitConfigOnce(csd: ByteArray) {
        lastCsd = csd
        if (csdSent) return
        csdSent = true
        sink.onConfig(csd)
    }

    private inline fun trySet(block: () -> Unit) {
        try { block() } catch (_: Throwable) {}
    }

    companion object {
        private const val TAG = "XCam/Encoder"
        private const val TIMEOUT_US = 10_000L
    }
}
