package com.xcam.net

import android.content.Context
import android.os.Build
import android.util.Log
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import org.json.JSONObject

/**
 * Announces this phone on the local network, so a desktop on the same Wi-Fi can
 * find it without anyone typing an address.
 *
 * A UDP broadcast rather than mDNS. NsdManager would register a proper service
 * and is the conventional answer, but the desktop end of mDNS is a callback
 * driven resolver where this is a socket and a loop -- a great deal of
 * machinery on both sides to learn one address that fits in forty bytes.
 *
 * Only useful over Wi-Fi. Over USB the desktop already knows where the phone is,
 * because it built the tunnel itself.
 */
class Beacon(private val context: Context) {

    @Volatile private var running = false
    private var thread: Thread? = null

    fun start() {
        if (running) return
        running = true
        thread = Thread({ loop() }, "xcam-beacon").apply {
            // Nothing waits on this, and it must never compete with the encoder.
            priority = Thread.MIN_PRIORITY
            start()
        }
    }

    fun stop() {
        running = false
        thread?.join(500)
        thread = null
    }

    private fun loop() {
        var socket: DatagramSocket? = null
        try {
            socket = DatagramSocket().apply { broadcast = true }
            val broadcast = InetAddress.getByName("255.255.255.255")

            while (running) {
                val payload = JSONObject().apply {
                    // The marker a listener checks before parsing anything else,
                    // so a stray datagram on this port costs one string compare.
                    put("xcam", 1)
                    put("name", "${Build.MANUFACTURER} ${Build.MODEL}")
                    put("port", Protocol.PORT)
                    put("version", Protocol.VERSION)
                }.toString().toByteArray(Charsets.UTF_8)

                try {
                    socket.send(DatagramPacket(payload, payload.size, broadcast, PORT))
                } catch (t: Throwable) {
                    // A network that comes and goes is normal -- Wi-Fi dropping,
                    // a hotspot starting. Losing a beacon is not worth stopping
                    // over; the next one is half a second away.
                    Log.d(TAG, "beacon send failed: ${t.message}")
                }

                Thread.sleep(INTERVAL_MS)
            }
        } catch (t: Throwable) {
            if (running) Log.w(TAG, "beacon stopped", t)
        } finally {
            try { socket?.close() } catch (_: Throwable) {}
        }
    }

    companion object {
        private const val TAG = "XCam/Beacon"

        /** Deliberately one past the stream port, so the pair is easy to open. */
        const val PORT = Protocol.PORT + 1

        /**
         * Twice a second. Fast enough that starting the desktop app feels like
         * it found the phone rather than waited for it, and small enough that
         * nobody will notice the traffic.
         */
        private const val INTERVAL_MS = 500L
    }
}
