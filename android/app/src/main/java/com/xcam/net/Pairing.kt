package com.xcam.net

import android.content.Context
import java.security.SecureRandom

/**
 * Whether this phone may be reached over the network, and the code that has to
 * be said by anything that tries.
 *
 * Both live in one small file because they are one decision. Until this
 * existed, [FrameServer] bound to every interface the moment the service
 * started and handed the camera, the microphone and the recordings folder to
 * whoever connected first -- no code, no prompt, nothing. Anyone on the same
 * Wi-Fi could take the picture, and a second connection displaced the first, so
 * they could take it *from* you.
 *
 * The shape of the fix is two layers, and the first is the one that matters:
 *
 *  1. Off by default, and "off" means the listener is bound to loopback. Not
 *     filtered after accepting -- bound. A socket on 127.0.0.1 is unreachable
 *     from another machine as a matter of kernel routing, so there is no code
 *     path here that a mistake could open. USB is unaffected: `adb forward`
 *     connects to the phone's own loopback, which is why the cable never
 *     needed the network in the first place.
 *
 *  2. When it is switched on, a client that did not arrive over loopback says
 *     a six-digit code before it is told anything -- not the camera list, not
 *     the model, not a frame.
 *
 * The code is generated once and kept, rather than made fresh per session:
 * somebody who has paired their own desktop should not have to re-pair it
 * every morning. [rotate] is there for when they want to.
 */
object Pairing {

    private const val PREFS = "xcam.pairing"
    private const val KEY_WIFI = "wifi"
    private const val KEY_CODE = "code"

    private fun prefs(context: Context) =
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)

    /** False unless somebody has deliberately turned it on. */
    fun wifiAllowed(context: Context): Boolean = prefs(context).getBoolean(KEY_WIFI, false)

    fun setWifiAllowed(context: Context, allowed: Boolean) {
        prefs(context).edit().putBoolean(KEY_WIFI, allowed).apply()
    }

    /** The code to read off the screen, made on first use and kept after that. */
    fun code(context: Context): String {
        val p = prefs(context)
        p.getString(KEY_CODE, null)?.let { if (it.length == DIGITS) return it }
        val fresh = generate()
        p.edit().putString(KEY_CODE, fresh).apply()
        return fresh
    }

    /** A new code. Every paired desktop has to be told it again. */
    fun rotate(context: Context): String {
        val fresh = generate()
        prefs(context).edit().putString(KEY_CODE, fresh).apply()
        return fresh
    }

    /**
     * Compared in constant time.
     *
     * Six digits is a small space, and an attacker who could measure how long a
     * comparison took would learn a digit at a time -- which turns a million
     * guesses into sixty. The network's own jitter probably buries the
     * difference, but "probably" is not a reason to write the version that
     * leaks.
     */
    fun check(context: Context, offered: String): Boolean {
        val expected = code(context)
        if (offered.length != expected.length) return false
        var diff = 0
        for (i in expected.indices) diff = diff or (expected[i].code xor offered[i].code)
        return diff == 0
    }

    private fun generate(): String {
        val random = SecureRandom()
        val sb = StringBuilder(DIGITS)
        for (i in 0 until DIGITS) sb.append(random.nextInt(10))
        return sb.toString()
    }

    private const val DIGITS = 6
}
