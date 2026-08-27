package com.xcam.codec

import android.media.AudioFormat
import android.media.AudioRecord
import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.media.MediaRecorder
import android.util.Log
import java.nio.ByteBuffer

/**
 * Microphone to AAC, for both destinations at once: the PC gets the frames as
 * AUDIO packets, and the local recording gets the same frames muxed into its
 * MP4. One capture, one encode, two consumers -- the alternative would be two
 * AudioRecords fighting over one microphone.
 *
 * Unlike the video path this one is not zero-copy: PCM has to reach the CPU to
 * be encoded at all. It is cheap enough that this does not matter -- 48kHz
 * stereo is 192 KB/s, against 7 MB/s for the video it accompanies.
 */
class AudioEncoder(
    private val config: Config,
    private val sink: Sink,
) {

    data class Config(
        val sampleRate: Int = 48_000,
        val channels: Int = 2,
        val bitrate: Int = 192_000,
        // CAMCORDER rather than MIC: it selects the microphone facing the same
        // way as the camera and applies the tuning meant for video, which is
        // what this is. Falls back to MIC where the device has no such source.
        val source: Int = MediaRecorder.AudioSource.CAMCORDER,

        /**
         * Whether the camera stamps frames with SystemClock.elapsedRealtimeNanos
         * rather than System.nanoTime -- SENSOR_INFO_TIMESTAMP_SOURCE REALTIME.
         *
         * The two clocks differ by however long the device has been suspended,
         * so audio timed against the wrong one lands minutes away from the video
         * it belongs to. Which one the camera uses is a property of the device,
         * so it is read rather than assumed.
         */
        val bootTimeClock: Boolean = false,
    ) {
        val channelMask: Int get() =
            if (channels >= 2) AudioFormat.CHANNEL_IN_STEREO else AudioFormat.CHANNEL_IN_MONO
    }

    /** Called on the encoder's own thread. Implementations must not block long. */
    interface Sink {
        /** The AudioSpecificConfig. Needed once, before any frame can be decoded. */
        fun onAudioConfig(csd: ByteArray)

        /** One AAC access unit. [ptsUs] shares the camera's clock. */
        fun onAudioFrame(data: ByteArray, ptsUs: Long)

        /** The MediaFormat the muxer needs to open an audio track. */
        fun onAudioFormat(format: MediaFormat)

        fun onAudioError(t: Throwable)
    }

    @Volatile private var running = false
    private var record: AudioRecord? = null
    private var codec: MediaCodec? = null
    private var readThread: Thread? = null
    private var drainThread: Thread? = null

    @Volatile private var csdSent = false
    @Volatile private var lastCsd: ByteArray? = null

    /**
     * Where the audio timeline starts, on the same monotonic clock the camera
     * stamps frames with.
     *
     * Every later timestamp is derived from the sample count rather than from
     * the clock. Reading the clock per buffer would let scheduling jitter into
     * the timeline, and a muxer given non-monotonic or unevenly spaced audio
     * produces a file that drifts out of sync with its own video.
     */
    @Volatile private var baseNanos = -1L
    @Volatile private var samplesRead = 0L

    private var silentFrames = 0L
    private var silencePeak = 0
    private var silenceReported = false
    private val peakSinceRead = java.util.concurrent.atomic.AtomicInteger(0)

    val sampleRate: Int get() = config.sampleRate
    val channels: Int get() = config.channels

    fun start() {
        val minBuffer = AudioRecord.getMinBufferSize(
            config.sampleRate, config.channelMask, AudioFormat.ENCODING_PCM_16BIT)
        if (minBuffer <= 0) throw IllegalStateException("no usable audio input configuration")

        // Four times the minimum. The reader competes with an encoder and a
        // camera for the CPU, and a buffer sized to the minimum overruns during
        // any hiccup -- which AudioRecord reports as a gap in the timeline, not
        // as an error.
        val bufferBytes = minBuffer * 4

        val rec = try {
            AudioRecord(config.source, config.sampleRate, config.channelMask,
                AudioFormat.ENCODING_PCM_16BIT, bufferBytes)
        } catch (t: Throwable) {
            throw IllegalStateException("could not open the microphone", t)
        }
        if (rec.state != AudioRecord.STATE_INITIALIZED) {
            rec.release()
            throw IllegalStateException("microphone unavailable (permission or in use)")
        }

        val format = MediaFormat.createAudioFormat(
            MediaFormat.MIMETYPE_AUDIO_AAC, config.sampleRate, config.channels).apply {
            setInteger(MediaFormat.KEY_AAC_PROFILE,
                MediaCodecInfo.CodecProfileLevel.AACObjectLC)
            setInteger(MediaFormat.KEY_BIT_RATE, config.bitrate)
            setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, bufferBytes)
        }

        val mc = MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_AUDIO_AAC)
        mc.configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
        mc.start()

        record = rec
        codec = mc
        running = true
        csdSent = false
        baseNanos = -1L
        samplesRead = 0L

        rec.startRecording()
        readThread = Thread({ readLoop(rec, mc, bufferBytes) }, "xcam-audio-in").apply {
            priority = Thread.MAX_PRIORITY
            start()
        }
        drainThread = Thread({ drainLoop(mc) }, "xcam-audio-out").apply { start() }

        Log.i(TAG, "audio started ${config.sampleRate}Hz " +
                "${config.channels}ch AAC ${config.bitrate / 1000}kbps")
    }

    fun stop() {
        running = false
        readThread?.join(500)
        drainThread?.join(500)
        readThread = null
        drainThread = null

        try { record?.stop() } catch (_: Throwable) {}
        try { record?.release() } catch (_: Throwable) {}
        try { codec?.stop() } catch (_: Throwable) {}
        try { codec?.release() } catch (_: Throwable) {}
        record = null
        codec = null
        Log.i(TAG, "audio stopped")
    }

    private fun readLoop(rec: AudioRecord, mc: MediaCodec, bufferBytes: Int) {
        val pcm = ByteArray(bufferBytes / 4)
        val bytesPerFrame = 2 * config.channels
        try {
            while (running) {
                val read = rec.read(pcm, 0, pcm.size)
                if (read <= 0) {
                    if (read == AudioRecord.ERROR_INVALID_OPERATION ||
                        read == AudioRecord.ERROR_BAD_VALUE) {
                        throw IllegalStateException("microphone read failed ($read)")
                    }
                    continue
                }

                val frames = read / bytesPerFrame
                if (baseNanos < 0) anchor(rec, frames)
                watchForSilence(pcm, read)
                val ptsUs = baseNanos / 1000 + samplesRead * 1_000_000L / config.sampleRate
                samplesRead += frames

                val index = mc.dequeueInputBuffer(TIMEOUT_US)
                if (index < 0) continue          // encoder is behind; drop rather than stall
                val buf = mc.getInputBuffer(index) ?: continue
                buf.clear()
                buf.put(pcm, 0, read)
                mc.queueInputBuffer(index, 0, read, ptsUs, 0)
            }
            // Let the encoder flush what it holds, so the tail of a recording is
            // not cut off mid-word.
            val index = mc.dequeueInputBuffer(TIMEOUT_US)
            if (index >= 0) {
                mc.queueInputBuffer(index, 0, 0, 0, MediaCodec.BUFFER_FLAG_END_OF_STREAM)
            }
        } catch (t: Throwable) {
            if (running) sink.onAudioError(t)
        }
    }

    private fun drainLoop(mc: MediaCodec) {
        val info = MediaCodec.BufferInfo()
        try {
            while (running) {
                val index = mc.dequeueOutputBuffer(info, TIMEOUT_US)
                when {
                    index == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED -> {
                        sink.onAudioFormat(mc.outputFormat)
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
                                emitCsdOnce(bytes)
                            } else {
                                sink.onAudioFrame(bytes, info.presentationTimeUs)
                            }
                        }
                        mc.releaseOutputBuffer(index, false)
                        if (info.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM != 0) break
                    }
                }
            }
        } catch (t: Throwable) {
            if (running) sink.onAudioError(t)
        }
    }

    private fun emitCsd(format: MediaFormat) {
        if (!format.containsKey("csd-0")) return
        val csd: ByteBuffer = format.getByteBuffer("csd-0") ?: return
        val bytes = ByteArray(csd.remaining())
        csd.duplicate().get(bytes)
        emitCsdOnce(bytes)
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
        lastCsd?.let { sink.onAudioConfig(it) }
    }

    private fun emitCsdOnce(csd: ByteArray) {
        lastCsd = csd
        if (csdSent) return
        csdSent = true
        sink.onAudioConfig(csd)
    }

    /**
     * Fixes where sample zero sits on the camera's clock.
     *
     * The driver hands over audio that was captured some tens of milliseconds
     * earlier, and that latency varies by device and by route. Assuming the
     * samples were captured just before the read returned makes audio
     * systematically late against video by exactly that unknown.
     *
     * AudioRecord.getTimestamp answers it directly -- "frame N was captured at
     * time T" -- on whichever of the two system clocks is asked for, which is
     * the same choice the camera made. Where the device will not answer, the
     * estimate below is what is left, and it is late rather than wrong.
     */
    private fun anchor(rec: AudioRecord, framesJustRead: Int) {
        val stamp = android.media.AudioTimestamp()
        val timebase = if (config.bootTimeClock) android.media.AudioTimestamp.TIMEBASE_BOOTTIME
                       else android.media.AudioTimestamp.TIMEBASE_MONOTONIC
        val ok = try {
            rec.getTimestamp(stamp, timebase) == AudioRecord.SUCCESS
        } catch (_: Throwable) {
            false
        }

        if (ok && stamp.framePosition > 0) {
            baseNanos = stamp.nanoTime -
                stamp.framePosition * 1_000_000_000L / config.sampleRate
            // What the estimate would have said, so the correction is a number
            // rather than a belief. Positive means the estimate placed sample
            // zero late, which is audio lagging video by that much.
            val estimate = nowNanos() - framesJustRead * 1_000_000_000L / config.sampleRate
            Log.i(TAG, "audio anchored from the driver's own timestamp " +
                    "(frame ${stamp.framePosition}); the estimate would have been " +
                    "${(estimate - baseNanos) / 1_000_000}ms late")
        } else {
            baseNanos = nowNanos() - framesJustRead * 1_000_000_000L / config.sampleRate
            Log.i(TAG, "audio anchored by estimate; the device reports no capture " +
                    "timestamp, so sound may lag by the input latency")
        }
    }

    /**
     * Reports a microphone that is delivering nothing.
     *
     * Android does not fail a denied microphone, it hands over well-formed
     * digital silence -- so the symptom of a permission problem, a privacy
     * switch or another app holding the input is a stream that connects, runs
     * at the right rate and records nothing. Sampling the peak turns that into
     * something the log can say out loud, once, rather than something to be
     * discovered later in a file.
     */
    /**
     * The loudest sample since the last read, 0..1, and reading resets it.
     *
     * There is exactly one reader -- the stats tick -- because two would each
     * see half a meter. A level that is measured and thrown away is how this
     * project has twice shipped a microphone that was silently dead, so the
     * number now leaves the phone.
     */
    fun takePeak(): Float = peakSinceRead.getAndSet(0) / 32767.0f

    private fun watchForSilence(pcm: ByteArray, bytes: Int) {
        var peak = 0
        var i = 0
        while (i + 1 < bytes) {
            val sample = ((pcm[i + 1].toInt() shl 8) or (pcm[i].toInt() and 0xFF)).toShort().toInt()
            val magnitude = if (sample < 0) -sample else sample
            if (magnitude > peak) peak = magnitude
            i += 2
        }
        // Always, even after the silence check has had its answer: this is the
        // meter's number now, not just the warning's.
        peakSinceRead.updateAndGet { if (peak > it) peak else it }

        if (silenceReported) return
        if (peak > silencePeak) silencePeak = peak

        silentFrames += bytes / (2 * config.channels)
        if (silentFrames < config.sampleRate * SILENCE_SECONDS) return

        silenceReported = true
        if (silencePeak <= 1) {
            Log.w(TAG, "the microphone has delivered nothing but silence for " +
                    "${SILENCE_SECONDS}s (peak $silencePeak). Android does not refuse a " +
                    "microphone it will not give you -- check the permission, the " +
                    "privacy switch, and whether another app is holding the input.")
        } else {
            Log.i(TAG, "microphone level looks healthy (peak $silencePeak of 32767)")
        }
    }

    private fun nowNanos(): Long =
        if (config.bootTimeClock) android.os.SystemClock.elapsedRealtimeNanos()
        else System.nanoTime()

    companion object {
        private const val TAG = "XCam/Audio"
        private const val TIMEOUT_US = 10_000L

        /** How long to listen before saying anything about the level. */
        private const val SILENCE_SECONDS = 3
    }
}
