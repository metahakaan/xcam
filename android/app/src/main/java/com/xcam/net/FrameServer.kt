package com.xcam.net

import android.util.Log
import java.io.BufferedOutputStream
import java.io.DataInputStream
import java.net.InetAddress
import java.net.ServerSocket
import java.net.Socket
import java.util.ArrayDeque
import java.util.concurrent.atomic.AtomicInteger

/**
 * Accepts one PC client at a time on [Protocol.PORT] and pumps encoded frames to
 * it. Reached over USB via `adb forward tcp:27183 tcp:27183`.
 *
 * Sending happens on a dedicated thread behind a bounded queue. When the link
 * cannot keep up we shed DELTA packets oldest-first and never touch CONFIG or
 * KEYFRAME, so the picture degrades in frame rate rather than falling apart.
 */
class FrameServer(
    /**
     * The handshake, built per connection. Takes `resumed` because the client
     * has to be told: it decides whether to configure the pipeline or leave a
     * running one alone, and it has no other way to know which of the two it
     * just connected to.
     */
    private val handshakeJson: (resumed: Boolean) -> String,
    private val onControl: (String) -> Unit,
    /**
     * Called when the session starts and when it really ends.
     *
     * `resumed` distinguishes a client that arrived within the linger window of
     * one that just left -- a cable pulled out and the same desktop coming back
     * over Wi-Fi -- from a genuinely new one. The pipeline must survive the
     * first and be rebuilt for the second, and telling them apart here is what
     * makes a hand-over a hand-over rather than a restart.
     */
    private val onClientChange: (connected: Boolean, resumed: Boolean) -> Unit,

    /**
     * Whether this phone is willing to be reached over the network at all.
     *
     * False -- the default -- binds the listener to the loopback address, and
     * that is the whole of the defence: a socket bound to 127.0.0.1 cannot be
     * reached from another machine, so no amount of scanning finds it. USB is
     * unaffected, because `adb forward` connects to the phone's own loopback.
     */
    private val wifiAllowed: () -> Boolean,

    /**
     * Checks a pairing code sent by a client that did not come in over
     * loopback. Returning false closes the connection before anything is
     * handed over.
     */
    private val checkPairing: (String) -> Boolean,
) {

    private data class Outgoing(
        val type: Byte,
        val data: ByteArray,
        val ptsUs: Long,
        val seq: Int,
        val flags: Byte = Protocol.FLAG_LAST_FRAGMENT,
    )

    @Volatile private var running = false
    @Volatile private var client: Socket? = null
    private var serverSocket: ServerSocket? = null

    private val queue = ArrayDeque<Outgoing>()
    private val queueLock = Object()
    private val dropped = AtomicInteger(0)

    // When the last client vanished, and a counter that invalidates a pending
    // linger the moment anything connects.
    @Volatile private var lostAt = 0L
    @Volatile private var lingerEpoch = 0

    private var acceptThread: Thread? = null
    private var sendThread: Thread? = null
    private var readThread: Thread? = null

    val isConnected: Boolean get() = client != null
    fun takeDroppedCount(): Int = dropped.getAndSet(0)

    fun start() {
        running = true
        acceptThread = Thread({ acceptLoop() }, "xcam-accept").apply { start() }
    }

    fun stop() {
        running = false
        try { serverSocket?.close() } catch (_: Throwable) {}
        closeClient()
        synchronized(queueLock) { queueLock.notifyAll() }
        acceptThread?.join(500)
        acceptThread = null
    }

    // ---- outbound ------------------------------------------------------

    fun sendConfig(csd: ByteArray) = enqueue(Outgoing(Protocol.Type.CONFIG, csd, 0, 0), critical = true)

    fun sendFrame(data: ByteArray, isKey: Boolean, ptsUs: Long, seq: Int) {
        val type = if (isKey) Protocol.Type.KEYFRAME else Protocol.Type.DELTA
        enqueue(Outgoing(type, data, ptsUs, seq), critical = isKey)
    }

    /**
     * Audio is critical in the queue-shedding sense: dropping a frame of it is
     * an audible click, where a dropped video frame is a moment of staleness
     * nobody notices. It is also tiny -- a few hundred bytes against tens of
     * kilobytes for a picture -- so protecting it costs almost no latency.
     */
    fun sendAudio(data: ByteArray, ptsUs: Long, isConfig: Boolean) {
        val flags = if (isConfig) (Protocol.FLAG_LAST_FRAGMENT.toInt() or
                                   Protocol.FLAG_CODEC_CONFIG.toInt()).toByte()
                    else Protocol.FLAG_LAST_FRAGMENT
        enqueue(Outgoing(Protocol.Type.AUDIO, data, ptsUs, 0, flags), critical = true)
    }

    /**
     * The recording encoder's output, when the file is being written on the PC.
     *
     * Critical in the queue-shedding sense, unlike the live stream. A dropped
     * frame of the webcam is a moment of staleness; a dropped frame of the
     * recording is a hole in the only copy worth keeping, and it cannot be
     * asked for again.
     */
    fun sendRecord(data: ByteArray, ptsUs: Long, isConfig: Boolean, isKey: Boolean) {
        var flags = Protocol.FLAG_LAST_FRAGMENT.toInt()
        if (isConfig) flags = flags or Protocol.FLAG_CODEC_CONFIG.toInt()
        if (isKey) flags = flags or Protocol.FLAG_KEY_FRAME.toInt()
        enqueue(Outgoing(Protocol.Type.RECORD, data, ptsUs, 0, flags.toByte()), critical = true)
    }

    /**
     * One chunk of a file being fetched.
     *
     * Droppable, and deliberately so -- but the sender only ever offers a chunk
     * when [queueDepth] is zero, so it is never actually in a position to be
     * shed. The flag marks the last chunk.
     */
    fun sendFile(data: ByteArray, offset: Long, isLast: Boolean) {
        val flags = if (isLast) Protocol.FLAG_LAST_FRAGMENT else 0.toByte()
        enqueue(Outgoing(Protocol.Type.FILE, data, offset, 0, flags), critical = false)
    }

    /**
     * How many packets are waiting to go out.
     *
     * The one thing a background transfer needs to know: an empty queue means
     * the link has spare capacity right now, and anything else means the live
     * picture is using it.
     */
    fun queueDepth(): Int = synchronized(queueLock) { queue.size }

    fun sendStats(json: String) =
        enqueue(Outgoing(Protocol.Type.STATS, json.toByteArray(Charsets.UTF_8), 0, 0), critical = false)

    fun sendAck(json: String, requestSeq: Int) =
        enqueue(Outgoing(Protocol.Type.ACK, json.toByteArray(Charsets.UTF_8), 0, requestSeq), critical = true)

    private fun enqueue(item: Outgoing, critical: Boolean) {
        if (client == null) return
        synchronized(queueLock) {
            if (queue.size >= MAX_QUEUE) {
                // Shed the oldest droppable packet. A queue made entirely of
                // critical packets means the link is hopeless, so drop this one.
                val victim = queue.firstOrNull { it.type == Protocol.Type.DELTA }
                if (victim != null) {
                    queue.remove(victim)
                    dropped.incrementAndGet()
                } else if (!critical) {
                    dropped.incrementAndGet()
                    return
                }
            }
            queue.addLast(item)
            queueLock.notifyAll()
        }
    }

    // ---- threads -------------------------------------------------------

    private fun acceptLoop() {
        while (running) {
            // Rebound rather than filtered.
            //
            // Refusing unwanted peers after accepting them would still leave the
            // port answering on every interface -- visible to a scan, and one
            // logic slip away from answering properly. A listener bound to
            // loopback is not reachable from another machine at all, which is a
            // guarantee from the kernel rather than from this file.
            val wifi = wifiAllowed()
            val server = try {
                ServerSocket(Protocol.PORT, BACKLOG,
                             if (wifi) null else InetAddress.getLoopbackAddress())
                    .apply { reuseAddress = true; soTimeout = REBIND_CHECK_MS }
            } catch (t: Throwable) {
                Log.w(TAG, "could not listen", t)
                try { Thread.sleep(500) } catch (_: InterruptedException) { return }
                continue
            }
            serverSocket = server
            Log.i(TAG, "listening on ${Protocol.PORT} (${if (wifi) "any" else "loopback"})")

            try {
                acceptOn(server) { wifiAllowed() != wifi }
            } finally {
                try { server.close() } catch (_: Throwable) {}
            }
        }
    }

    /** Accepts until [switched] says the binding is no longer the right one. */
    private fun acceptOn(server: ServerSocket, switched: () -> Boolean) {
            while (running) {
                val socket = try {
                    server.accept()
                } catch (_: java.net.SocketTimeoutException) {
                    // The idle tick. Nothing arrived; the only question is
                    // whether the switch moved while we were waiting.
                    if (switched()) return
                    continue
                } catch (t: Throwable) {
                    if (running) Log.w(TAG, "accept failed", t)
                    return
                }

                Log.i(TAG, "client connected from ${socket.inetAddress}")

                // Anything that did not come in over loopback has to say the
                // code before it is told a single thing about this phone --
                // not the camera list, not the model, and certainly not a
                // frame. Loopback is exempt because reaching it already means
                // holding the cable, which is a stronger proof than a code.
                if (!socket.inetAddress.isLoopbackAddress && !pairOrClose(socket)) continue
                socket.tcpNoDelay = true
                socket.sendBufferSize = 1 shl 20

                // Displace whoever was here before and go straight back to
                // accepting. Waiting for the previous session to finish would
                // leave a new client sitting in the OS backlog -- connected as
                // far as TCP is concerned, but never handed a handshake -- for
                // as long as a half-open socket took to time out.
                // Within the linger window of a client that just left, this
                // is the same session coming back on the other transport. The
                // queue still holds packets aimed at a socket that is gone.
                val resumed = lostAt != 0L &&
                        android.os.SystemClock.elapsedRealtime() - lostAt < LINGER_MS

                closeClient()
                synchronized(queueLock) { queue.clear() }
                lostAt = 0L
                ++lingerEpoch
                client = socket

                try {
                    val out = BufferedOutputStream(socket.getOutputStream(), 1 shl 18)
                    out.write(Protocol.buildHandshake(handshakeJson(resumed)))
                    out.flush()

                    onClientChange(true, resumed)
                    sendThread = Thread({ sendLoop(socket, out) }, "xcam-send").apply {
                        priority = Thread.MAX_PRIORITY
                        start()
                    }
                    readThread = Thread({ readLoop(socket) }, "xcam-read").apply { start() }
                } catch (t: Throwable) {
                    Log.w(TAG, "handshake failed", t)
                    try { socket.close() } catch (_: Throwable) {}
                    if (client === socket) client = null
                    onClientChange(false, false)
                }
            }
    }

    /**
     * Makes a networked client prove it knows the code.
     *
     * The exchange is deliberately one-sided and tells the caller nothing it
     * could learn from: a short JSON saying a code is wanted, then one control
     * packet, then either the session begins or the socket closes. A wrong code
     * gets the same silence as no code at all.
     */
    private fun pairOrClose(socket: Socket): Boolean {
        try {
            socket.soTimeout = PAIR_TIMEOUT_MS

            val out = BufferedOutputStream(socket.getOutputStream(), 1 shl 12)
            out.write(Protocol.buildHandshake("""{"pairing":"required"}"""))
            out.flush()

            val packet = Protocol.readPacket(DataInputStream(socket.getInputStream()))
            if (packet.type != Protocol.Type.CONTROL) throw java.io.IOException("not a control packet")

            val json = org.json.JSONObject(packet.payloadAsString())
            if (json.optString("cmd") != "pair") throw java.io.IOException("not a pair command")

            if (!checkPairing(json.optString("code"))) {
                Log.w(TAG, "wrong pairing code from ${socket.inetAddress}")
                try { socket.close() } catch (_: Throwable) {}
                return false
            }

            // Back to blocking for the session itself: a stream that goes quiet
            // for a moment is not a stream that has gone away.
            socket.soTimeout = 0
            Log.i(TAG, "paired with ${socket.inetAddress}")
            return true
        } catch (t: Throwable) {
            Log.w(TAG, "pairing failed: ${t.message}")
            try { socket.close() } catch (_: Throwable) {}
            return false
        }
    }

    private fun sendLoop(socket: Socket, out: BufferedOutputStream) {
        val scratch = ByteArray(Protocol.HEADER_SIZE)
        try {
            // A displaced session must stop pulling from the queue immediately:
            // it is shared, so two senders draining it at once would split the
            // stream between two sockets and corrupt both.
            while (running && !socket.isClosed && client === socket) {
                val item: Outgoing = synchronized(queueLock) {
                    // The displacement check belongs *inside* the lock, next to
                    // the poll.
                    //
                    // Checking it only at the top of the loop leaves a window: a
                    // sender parked in wait() is woken by the enqueue that the
                    // new session just made, and takes that packet before it
                    // ever re-evaluates whose socket it is holding. It then
                    // writes it to a socket that is closed, and the packet is
                    // gone. During a full restart that cost a frame nobody
                    // missed; a resumed session has exactly one CONFIG, and
                    // losing it leaves the client with audio and no picture at
                    // all -- which is what it did.
                    while (running && client === socket && queue.isEmpty()) {
                        queueLock.wait(200)
                    }
                    if (!running || client !== socket) return
                    queue.pollFirst()
                } ?: continue

                Protocol.writePacket(
                    out, scratch, item.type, item.data,
                    ptsUs = item.ptsUs, seq = item.seq, flags = item.flags,
                )
                // Flush once the queue drains, so a burst still goes out as one
                // write but a lone frame is never held back.
                if (synchronized(queueLock) { queue.isEmpty() }) out.flush()
            }
        } catch (t: Throwable) {
            if (running) Log.w(TAG, "send loop ended: ${t.message}")
        } finally {
            try { socket.close() } catch (_: Throwable) {}
        }
    }

    private fun readLoop(socket: Socket) {
        try {
            val input = DataInputStream(socket.getInputStream())
            while (running && !socket.isClosed) {
                val packet = Protocol.readPacket(input)
                if (packet.type == Protocol.Type.CONTROL) {
                    onControl(packet.payloadAsString())
                }
            }
        } catch (t: Throwable) {
            if (running) Log.i(TAG, "client disconnected: ${t.message}")
        } finally {
            try { socket.close() } catch (_: Throwable) {}
            // Only tear the pipeline down if this is still the live session; a
            // newer client may already have replaced us.
            if (client === socket) {
                client = null
                lingerThenDisconnect()
            }
        }
    }

    /**
     * Waits a moment before declaring the session over.
     *
     * A USB cable pulled out mid-call kills the socket instantly, and the same
     * desktop is usually back over Wi-Fi within half a second. Tearing the
     * camera and both encoders down in between would make that a restart --
     * several seconds of black, a new encoder session, and every manual setting
     * re-applied -- for a link that was never really gone.
     *
     * So the pipeline lingers. The cost is a few seconds of camera and encoder
     * after a genuine disconnect, which is battery nobody asked to spend; the
     * alternative is paying a full restart every time a cable moves.
     */
    private fun lingerThenDisconnect() {
        val epoch = ++lingerEpoch
        lostAt = android.os.SystemClock.elapsedRealtime()

        Thread({
            try { Thread.sleep(LINGER_MS) } catch (_: InterruptedException) { return@Thread }
            // Only if nothing has connected since. A client that arrived took
            // the epoch with it and this one is stale.
            if (running && client == null && lingerEpoch == epoch) {
                Log.i(TAG, "session ended: nothing came back within ${LINGER_MS}ms")
                lostAt = 0L
                onClientChange(false, false)
            }
        }, "xcam-linger").start()
    }

    /**
     * Drops the current client without waiting on its threads. Closing the
     * socket is what makes them exit: the sender's next write throws and the
     * reader's recv returns end-of-stream.
     */
    private fun closeClient() {
        val c = client ?: return
        client = null
        try { c.close() } catch (_: Throwable) {}
        sendThread = null
        readThread = null
    }

    companion object {
        private const val TAG = "XCam/Server"

        /**
         * How long the pipeline outlives a client.
         *
         * Long enough for a desktop that lost USB to find its way back over
         * Wi-Fi -- connect, handshake, and resume -- and short enough that a
         * genuine disconnect does not leave the camera running for long. Five
         * seconds is roughly ten times the observed hand-over.
         */
        private const val LINGER_MS = 5_000L

        /**
         * The queue is a latency budget, not a safety margin: anything sitting in
         * it is a frame the viewer will see late. Ten frames caps the backlog at
         * roughly 160ms at 60fps, and past that a live camera is better served by
         * dropping to the newest frame than by delivering a stale one.
         */
        private const val MAX_QUEUE = 10

        /** Kept short: one desktop at a time is the whole design. */
        private const val BACKLOG = 4

        /**
         * How long an accept waits before looking at the Wi-Fi switch again.
         * Only a rebinding check, so a second of lag costs nothing.
         */
        private const val REBIND_CHECK_MS = 1_000

        /**
         * A person reading a code off a phone and typing it has plenty of time
         * in fifteen seconds; a script working through six digits does not.
         */
        private const val PAIR_TIMEOUT_MS = 15_000
    }
}
