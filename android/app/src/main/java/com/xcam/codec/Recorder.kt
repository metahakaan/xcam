package com.xcam.codec

import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.media.MediaMuxer
import android.os.Build
import android.util.Log
import android.view.Surface
import java.io.File
import java.nio.ByteBuffer
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.concurrent.atomic.AtomicLong

/**
 * The second encoder: writes a full-quality MP4 on the phone while the first one
 * streams to the PC.
 *
 * The two exist separately because they want opposite things. The stream is
 * sized for a webcam and compressed hard enough to arrive on time; the file is
 * whatever the sensor can actually give, at a bitrate no USB link needs to
 * carry. One encode cannot be both, and that trade is the reason this exists --
 * the live picture is a convenience, the file is the work.
 *
 * **The encoder runs for the whole capture session, not for the length of a
 * take.** A take is delimited by the muxer and by whether the camera is
 * targeting [surface] at all, and the codec itself is never stopped in between.
 *
 * That is not an optimisation, it is the only arrangement that works. Starting a
 * MediaCodec re-establishes its input surface's buffer queue, and doing so after
 * the camera has already been configured against that surface leaves the camera
 * holding buffers the consumer no longer recognises. The HAL says so plainly --
 *
 *     Camera3-Stream: returnBuffer: Stream 1: Returning an unknown buffer
 *
 * -- and then stops producing frames on every stream, taking the live picture
 * down with it. So the codec starts before the capture session is built and
 * outlives every take within it. It costs an idle encoder instance; frames only
 * reach it while the record target is in the repeating request.
 */
class Recorder(
    private val outputDir: File,
    private val sink: Sink? = null,
) {

    /**
     * Where the file is written.
     *
     * [Target.Phone] muxes locally and needs nothing from the link, which is
     * what makes it work over Wi-Fi. [Target.Pc] sends the same encoded frames
     * across instead and lets the desktop mux them, which puts the file where
     * the person using it already is at the cost of carrying two streams at
     * once. Neither is better in general.
     */
    enum class Target { Phone, Pc }

    /** Receives encoded frames when the target is the PC. */
    interface Sink {
        fun onRecordFrame(data: ByteArray, ptsUs: Long, isConfig: Boolean, isKey: Boolean)
    }

    data class Config(
        val width: Int,
        val height: Int,
        val fps: Int,
        val bitrate: Int,
        val codec: String,
    ) {
        val mime: String get() = if (codec == "hevc") MediaFormat.MIMETYPE_VIDEO_HEVC
                                 else MediaFormat.MIMETYPE_VIDEO_AVC
    }

    /** What the client asked for, after clamping. Null until [prepare]. */
    var config: Config? = null
        private set

    var target: Target = Target.Phone
        private set

    /**
     * The codec configuration, kept because the desktop needs it at the start
     * of every take while the encoder only announces it once per session.
     */
    private var videoCsd: ByteArray? = null

    /** The camera target. Valid from [prepare] until [release]. */
    var surface: Surface? = null
        private set

    @Volatile var isRecording = false
        private set

    /** Absolute path of the current or most recent take, for `adb pull`. */
    @Volatile var currentFile: String = ""
        private set

    private val bytesWritten = AtomicLong(0)
    @Volatile private var firstPtsUs = -1L
    @Volatile private var lastPtsUs = 0L

    private var codec: MediaCodec? = null
    private var drainThread: Thread? = null
    @Volatile private var draining = false

    /**
     * The muxer and everything that describes the take in progress. Touched by
     * the control thread on start/stop and by the drain thread on every frame,
     * so all of it lives behind [takeLock].
     */
    private val takeLock = Object()
    private var muxer: MediaMuxer? = null
    private var trackIndex = -1
    private var audioTrackIndex = -1
    private var muxerStarted = false

    /**
     * The audio encoder's output format, once it has one.
     *
     * A muxer takes all its tracks before it starts and none after, so a take
     * cannot begin until this has arrived -- otherwise the file would open
     * video-only and the sound would have nowhere to go. It is set once per
     * session and reused by every take.
     */
    private var audioFormat: MediaFormat? = null
    @Volatile private var wantAudio = false

    /**
     * The encoder's output format, kept from the first time the codec reported
     * it. Later takes need it to add their track before any frame arrives, and
     * the codec only announces it once per session.
     */
    private var outputFormat: MediaFormat? = null

    /**
     * Declares whether this session will have sound.
     *
     * Set before the format is known, because a take that begins in the first
     * moments of a session would otherwise open its muxer video-only and shut
     * the door on the audio track for its whole length. Set back to false if the
     * microphone turns out to be unavailable, or a take would wait forever for a
     * track that is never coming.
     */
    fun expectAudio(expected: Boolean) {
        synchronized(takeLock) {
            wantAudio = expected
            if (!expected) audioFormat = null
        }
    }

    /** The format the audio track needs, once the encoder has reported it. */
    fun setAudioFormat(format: MediaFormat) {
        synchronized(takeLock) {
            audioFormat = format
            wantAudio = true
            // A take may already be waiting on exactly this.
            openTrackIfNeeded()
        }
    }

    /** Milliseconds of footage written so far. */
    fun elapsedMs(): Long =
        if (firstPtsUs < 0) 0L else (lastPtsUs - firstPtsUs) / 1000L

    fun bytes(): Long = bytesWritten.get()

    /**
     * Creates the encoder, its input surface, and the thread that drains it, and
     * starts all three. The caller must do this *before* configuring the capture
     * session against [surface] -- see the class comment for what happens
     * otherwise.
     *
     * Throws when the device will not give a second encoder instance at this
     * size, which is a real limit on some SoCs and has to reach the client as a
     * refusal rather than as a recording that silently never starts.
     */
    fun prepare(cfg: Config, recordTarget: Target = Target.Phone) {
        release()
        target = recordTarget

        val mc = MediaCodec.createEncoderByType(cfg.mime)
        mc.configure(formatFor(cfg), null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
        val input = mc.createInputSurface()
        mc.start()

        codec = mc
        surface = input
        config = cfg
        draining = true
        drainThread = Thread({ drainLoop(mc) }, "xcam-recorder").apply {
            priority = Thread.NORM_PRIORITY + 2
            start()
        }
        Log.i(TAG, "recorder ready ${cfg.width}x${cfg.height}@${cfg.fps} " +
                "${cfg.codec} ${cfg.bitrate / 1_000_000}Mbps")
    }

    /**
     * Opens a file and begins a take. The caller adds the record target to the
     * capture request afterwards, so the first frame the encoder sees belongs to
     * this take rather than to the gap before it.
     */
    fun start(): String {
        val mc = codec ?: throw IllegalStateException("recorder not prepared")
        if (isRecording) return currentFile

        val stamp = SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(Date())

        synchronized(takeLock) {
            if (target == Target.Phone) {
                outputDir.mkdirs()
                val file = File(outputDir, "XCam_$stamp.mp4")
                muxer = MediaMuxer(file.absolutePath,
                    MediaMuxer.OutputFormat.MUXER_OUTPUT_MPEG_4)
                currentFile = file.absolutePath
            } else {
                // The desktop names the file; this is only what the phone calls
                // the take when it reports on it.
                currentFile = "XCam_$stamp.mp4"
            }
            trackIndex = -1
            audioTrackIndex = -1
            muxerStarted = false
            bytesWritten.set(0)
            firstPtsUs = -1L
            lastPtsUs = 0L
            isRecording = true

            // Everything the ring is holding belongs to this take, and it has
            // to go in before the first live frame does or the file would open
            // in the present and the past would arrive after it.
            openTrackIfNeeded()
            flushPreRoll()
        }

        // The desktop opens a fresh file per take and cannot decode a frame
        // without this, but the encoder announces it once per session -- so it
        // is kept and re-sent rather than waited for.
        if (target == Target.Pc) {
            videoCsd?.let { sink?.onRecordFrame(it, 0, isConfig = true, isKey = false) }
        }

        // The file has to open on a key frame, or nothing can decode its first
        // second. Between takes the encoder has been idle, so the next frame it
        // produces would otherwise be whatever the GOP schedule had planned.
        try {
            mc.setParameters(android.os.Bundle().apply {
                putInt(MediaCodec.PARAMETER_KEY_REQUEST_SYNC_FRAME, 0)
            })
        } catch (t: Throwable) {
            Log.w(TAG, "could not request a sync frame", t)
        }

        Log.i(TAG, if (target == Target.Pc) "recording to the desktop"
                   else "recording to $currentFile")
        return currentFile
    }

    /**
     * Ends the take and closes the file. The codec keeps running for the next
     * one; only the muxer is torn down.
     */
    fun stop(): String {
        if (!isRecording) return currentFile

        synchronized(takeLock) {
            isRecording = false
            try {
                if (muxerStarted) muxer?.stop()
            } catch (t: Throwable) {
                // A muxer that never received a sample throws here, and an
                // escaping exception would take the release below with it and
                // leak the file handle.
                Log.w(TAG, "muxer stop failed", t)
            }
            try { muxer?.release() } catch (_: Throwable) {}
            muxer = null
            muxerStarted = false
            trackIndex = -1
            audioTrackIndex = -1
        }

        Log.i(TAG, "recorded ${elapsedMs()}ms, ${bytes() / 1_000_000}MB -> $currentFile")
        return currentFile
    }

    fun release() {
        stop()
        synchronized(takeLock) { clearPreRoll() }
        draining = false
        drainThread?.join(1000)
        drainThread = null

        val mc = codec
        try { mc?.signalEndOfInputStream() } catch (_: Throwable) {}
        try { mc?.stop() } catch (_: Throwable) {}
        try { mc?.release() } catch (_: Throwable) {}
        try { surface?.release() } catch (_: Throwable) {}
        codec = null
        surface = null
        config = null
        outputFormat = null
    }

    private fun formatFor(cfg: Config): MediaFormat =
        MediaFormat.createVideoFormat(cfg.mime, cfg.width, cfg.height).apply {
            setInteger(MediaFormat.KEY_COLOR_FORMAT,
                MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface)
            setInteger(MediaFormat.KEY_BIT_RATE, cfg.bitrate)
            setInteger(MediaFormat.KEY_FRAME_RATE, cfg.fps)
            // A second between key frames. This file gets scrubbed, not
            // streamed, and a long GOP makes seeking coarse for a saving that
            // does not matter at this bitrate.
            setFloat(MediaFormat.KEY_I_FRAME_INTERVAL, 1.0f)
            // VBR, unlike the stream. Nothing here has to fit a link budget, so
            // spending bits where the picture needs them is strictly better.
            setInteger(MediaFormat.KEY_BITRATE_MODE,
                MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_VBR)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N) {
                try { setInteger(MediaFormat.KEY_PROFILE, profileFor(cfg)) } catch (_: Throwable) {}
            }
        }

    private fun drainLoop(mc: MediaCodec) {
        val info = MediaCodec.BufferInfo()
        try {
            while (draining) {
                val index = mc.dequeueOutputBuffer(info, TIMEOUT_US)
                when {
                    index == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED -> {
                        synchronized(takeLock) {
                            outputFormat = mc.outputFormat
                            if (videoCsd == null) videoCsd = extractCsd(mc.outputFormat)
                            openTrackIfNeeded()
                        }
                    }
                    index == MediaCodec.INFO_TRY_AGAIN_LATER -> Unit
                    index >= 0 -> {
                        if (target == Target.Pc) sendIfRecording(mc, index, info)
                        else writeIfRecording(mc, index, info)
                        mc.releaseOutputBuffer(index, false)
                        if (info.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM != 0) break
                    }
                }
            }
        } catch (t: Throwable) {
            if (draining) Log.e(TAG, "recorder drain failed", t)
        }
    }

    /**
     * Writes one AAC frame into the take, on the same timeline as the video.
     *
     * Audio that arrives before the take's first key frame is dropped rather
     * than written at a negative timestamp: the file has to begin at zero on
     * both tracks or players disagree about where it starts.
     */
    fun writeAudio(data: ByteArray, ptsUs: Long) {
        // The desktop is already receiving AUDIO packets on their own timeline;
        // sending the same bytes twice would be pure waste.
        if (target == Target.Pc) return
        synchronized(takeLock) {
            if (!isRecording) {
                // A take that opens ten seconds in the past with ten seconds of
                // silence is not the feature anyone asked for. Sound is small
                // enough that keeping it costs nothing worth counting.
                bufferAudioForPreRoll(data, ptsUs)
                return
            }
            if (!muxerStarted || audioTrackIndex < 0) return
            if (firstPtsUs < 0 || ptsUs < firstPtsUs) return

            val info = MediaCodec.BufferInfo()
            info.set(0, data.size, ptsUs - firstPtsUs, 0)
            try {
                muxer?.writeSampleData(audioTrackIndex, ByteBuffer.wrap(data), info)
                bytesWritten.addAndGet(data.size.toLong())
            } catch (t: Throwable) {
                Log.w(TAG, "audio write failed", t)
            }
        }
    }

    private fun writeIfRecording(mc: MediaCodec, index: Int, info: MediaCodec.BufferInfo) {
        if (info.size <= 0) return
        if (info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG != 0) return

        val buf = mc.getOutputBuffer(index) ?: return
        val isKey = info.flags and MediaCodec.BUFFER_FLAG_KEY_FRAME != 0
        synchronized(takeLock) {
            if (!isRecording) {
                // Not discarded any more when the ring is armed: this is the
                // only moment these frames exist, and the whole point of
                // pre-roll is that the decision to keep them comes later.
                bufferForPreRoll(buf, info, isKey)
                return
            }
            openTrackIfNeeded()
            if (!muxerStarted) return

            // Wait for the take's first key frame. Anything before it references
            // pictures from the gap between takes and cannot be decoded.
            if (firstPtsUs < 0) {
                if (!isKey) return
                firstPtsUs = info.presentationTimeUs
            }
            lastPtsUs = info.presentationTimeUs
            // The muxer wants a timeline that starts at zero; the camera clock
            // starts wherever the phone booted.
            info.presentationTimeUs -= firstPtsUs
            muxer?.writeSampleData(trackIndex, buf, info)
            bytesWritten.addAndGet(info.size.toLong())
        }
    }

    /**
     * Hands one access unit to the desktop instead of to a muxer.
     *
     * Same gating as the local path: nothing goes out before the take's first
     * key frame, because a file that opens on a mid-GOP frame cannot be decoded
     * from its own beginning.
     */
    private fun sendIfRecording(mc: MediaCodec, index: Int, info: MediaCodec.BufferInfo) {
        if (info.size <= 0) return
        val buf = mc.getOutputBuffer(index) ?: return

        val isConfig = info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG != 0
        val isKey = info.flags and MediaCodec.BUFFER_FLAG_KEY_FRAME != 0

        buf.position(info.offset)
        buf.limit(info.offset + info.size)
        val bytes = ByteArray(info.size)
        buf.get(bytes)

        if (isConfig) {
            // Keep it for the next take as well; the encoder says it once.
            if (videoCsd == null) videoCsd = bytes
            if (isRecording) sink?.onRecordFrame(bytes, 0, isConfig = true, isKey = false)
            return
        }

        synchronized(takeLock) {
            if (!isRecording) return
            if (firstPtsUs < 0) {
                if (!isKey) return
                firstPtsUs = info.presentationTimeUs
            }
            lastPtsUs = info.presentationTimeUs
            bytesWritten.addAndGet(info.size.toLong())
        }
        sink?.onRecordFrame(bytes, info.presentationTimeUs, isConfig = false, isKey = isKey)
    }

    /** The csd the desktop needs, flattened out of the encoder's format. */
    private fun extractCsd(format: MediaFormat): ByteArray? {
        val parts = mutableListOf<ByteBuffer>()
        for (key in arrayOf("csd-0", "csd-1", "csd-2")) {
            if (format.containsKey(key)) parts += format.getByteBuffer(key) ?: continue
        }
        if (parts.isEmpty()) return null

        val out = ByteArray(parts.sumOf { it.remaining() })
        var offset = 0
        for (part in parts) {
            val n = part.remaining()
            part.duplicate().get(out, offset, n)
            offset += n
        }
        return out
    }

    /**
     * Adds every track and starts the muxer, once all of them are known.
     *
     * Caller must hold [takeLock].
     */
    private fun openTrackIfNeeded() {
        if (target == Target.Pc) return
        val m = muxer ?: return
        val video = outputFormat ?: return
        if (muxerStarted) return
        // A muxer accepts tracks only before it starts, so starting with video
        // alone would close the door on the sound for the whole take.
        if (wantAudio && audioFormat == null) return

        trackIndex = m.addTrack(video)
        audioTrackIndex = audioFormat?.let { m.addTrack(it) } ?: -1
        m.start()
        muxerStarted = true
    }

    private fun profileFor(cfg: Config): Int =
        if (cfg.mime == MediaFormat.MIMETYPE_VIDEO_HEVC)
            MediaCodecInfo.CodecProfileLevel.HEVCProfileMain
        else
            MediaCodecInfo.CodecProfileLevel.AVCProfileHigh

    // ---- pre-roll ----------------------------------------------------------

    /**
     * One encoded access unit, kept so a take can begin before the button was
     * pressed.
     */
    private class Buffered(val bytes: ByteArray, val ptsUs: Long, val isKey: Boolean)

    private val preRollVideo = ArrayDeque<Buffered>()
    private val preRollAudio = ArrayDeque<Buffered>()

    /**
     * Timestamps of the key frames still in the ring, in order.
     *
     * Kept beside the frames so trimming does not have to scan for the next one
     * on every frame. A ring can only be handed to a muxer starting at a key
     * frame, so the second key frame is the only place the front can be cut to.
     */
    private val preRollKeys = ArrayDeque<Long>()

    @Volatile private var preRollUs = 0L
    private var preRollByteCap = 0L
    private var preRollBytes = 0L

    /** How much footage the ring is holding right now, in milliseconds. */
    fun preRollFillMs(): Long = synchronized(takeLock) {
        val first = preRollVideo.firstOrNull() ?: return 0L
        val last = preRollVideo.lastOrNull() ?: return 0L
        (last.ptsUs - first.ptsUs) / 1000L
    }

    fun preRollArmedMs(): Long = preRollUs / 1000L

    /**
     * Arms the ring for [seconds] of footage, and returns what was actually
     * granted.
     *
     * Granted rather than obeyed, because the ring lives in the heap and this
     * encoder runs at a bitrate chosen for a file rather than a link: ten
     * seconds at 120 Mbit/s is 150 MB, which on most phones is the difference
     * between a feature and an OutOfMemoryError. So the request is clamped to a
     * quarter of the heap and the client is told what it got -- the same
     * arrangement as a record size the sensor cannot drive.
     *
     * Local takes only. The ring is on the phone, and shipping it across the
     * link when a take starts would stall the live picture for as long as the
     * ring is deep, which is the one thing a webcam may not do.
     */
    fun armPreRoll(seconds: Int, bitrate: Int): Int {
        synchronized(takeLock) {
            if (seconds <= 0 || target == Target.Pc) {
                clearPreRoll()
                preRollUs = 0L
                preRollByteCap = 0L
                return 0
            }

            val budget = (Runtime.getRuntime().maxMemory() / 4)
                .coerceAtMost(384L * 1024 * 1024)
            // A quarter over the nominal rate: this encoder is VBR, so a busy
            // second costs more than the average says it will.
            val perSecond = (bitrate.coerceAtLeast(1_000_000) / 8L) * 5 / 4
            val granted = (budget / perSecond).toInt().coerceIn(0, seconds)

            preRollUs = granted * 1_000_000L
            preRollByteCap = granted * perSecond
            if (granted == 0) clearPreRoll()
            if (granted < seconds) {
                Log.i(TAG, "pre-roll clamped to ${granted}s of the ${seconds}s asked for " +
                        "(${budget / 1_000_000}MB for ${perSecond / 1_000_000}MB/s)")
            }
            return granted
        }
    }

    private fun clearPreRoll() {
        preRollVideo.clear()
        preRollAudio.clear()
        preRollKeys.clear()
        preRollBytes = 0L
    }

    /** Copies one encoded frame into the ring. Caller holds [takeLock]. */
    private fun bufferForPreRoll(buf: ByteBuffer, info: MediaCodec.BufferInfo, isKey: Boolean) {
        if (preRollUs <= 0L) return

        buf.position(info.offset)
        buf.limit(info.offset + info.size)
        val bytes = ByteArray(info.size)
        buf.get(bytes)

        preRollVideo.addLast(Buffered(bytes, info.presentationTimeUs, isKey))
        if (isKey) preRollKeys.addLast(info.presentationTimeUs)
        preRollBytes += bytes.size
        trimPreRoll()
    }

    /** Copies one AAC frame into the ring. Caller holds [takeLock]. */
    private fun bufferAudioForPreRoll(data: ByteArray, ptsUs: Long) {
        if (preRollUs <= 0L) return
        preRollAudio.addLast(Buffered(data, ptsUs, isKey = true))
        preRollBytes += data.size
        trimPreRoll()
    }

    private fun trimPreRoll() {
        val newest = preRollVideo.lastOrNull()?.ptsUs ?: return

        // Cut only at a key frame, and only when what remains still covers the
        // whole window. Cutting anywhere else leaves frames referencing pictures
        // that are no longer in the ring.
        while (preRollKeys.size >= 2) {
            val nextKey = preRollKeys[1]
            if (newest - nextKey < preRollUs && preRollBytes <= preRollByteCap) break
            while (preRollVideo.isNotEmpty() && preRollVideo.first().ptsUs < nextKey) {
                preRollBytes -= preRollVideo.removeFirst().bytes.size
            }
            preRollKeys.removeFirst()
        }

        // Nothing to cut to and still growing. One GOP has outgrown the whole
        // budget on its own, which means the encoder is not producing the key
        // frames the format asked for -- keeping the ring would be an
        // OutOfMemoryError with extra steps.
        if (preRollBytes > preRollByteCap * 2 && preRollKeys.size < 2) {
            Log.w(TAG, "pre-roll dropped: ${preRollBytes / 1_000_000}MB with " +
                    "${preRollKeys.size} key frames in it")
            clearPreRoll()
            return
        }

        // Sound follows the picture's window. It is small enough that its own
        // budget would be noise.
        val floor = preRollVideo.firstOrNull()?.ptsUs ?: return
        while (preRollAudio.isNotEmpty() && preRollAudio.first().ptsUs < floor) {
            preRollBytes -= preRollAudio.removeFirst().bytes.size
        }
    }

    /**
     * Writes the ring into the take that is just starting. Caller holds
     * [takeLock] and the muxer is already running.
     *
     * The two tracks are merged by timestamp rather than written one after the
     * other: a muxer will take them either way, but a file whose first ten
     * seconds are all video and then all audio makes every player seek badly.
     */
    private fun flushPreRoll() {
        if (preRollVideo.isEmpty() || !muxerStarted) return

        val opening = preRollVideo.first()
        firstPtsUs = opening.ptsUs

        val audio = preRollAudio.filter { it.ptsUs >= firstPtsUs }
        val info = MediaCodec.BufferInfo()
        var next = 0

        for (frame in preRollVideo) {
            while (next < audio.size && audio[next].ptsUs <= frame.ptsUs) {
                writeBuffered(audioTrackIndex, audio[next], info)
                next++
            }
            writeBuffered(trackIndex, frame, info)
            lastPtsUs = frame.ptsUs
        }
        while (next < audio.size) {
            writeBuffered(audioTrackIndex, audio[next], info)
            next++
        }

        val span = (lastPtsUs - firstPtsUs) / 1000L
        Log.i(TAG, "take opened with ${span}ms of pre-roll, ${preRollBytes / 1_000_000}MB")
        clearPreRoll()
    }

    private fun writeBuffered(track: Int, frame: Buffered, info: MediaCodec.BufferInfo) {
        if (track < 0) return
        info.set(0, frame.bytes.size, frame.ptsUs - firstPtsUs,
                 if (frame.isKey) MediaCodec.BUFFER_FLAG_KEY_FRAME else 0)
        try {
            muxer?.writeSampleData(track, ByteBuffer.wrap(frame.bytes), info)
            bytesWritten.addAndGet(frame.bytes.size.toLong())
        } catch (t: Throwable) {
            Log.w(TAG, "pre-roll write failed", t)
        }
    }

    companion object {
        private const val TAG = "XCam/Recorder"
        private const val TIMEOUT_US = 10_000L
    }
}
