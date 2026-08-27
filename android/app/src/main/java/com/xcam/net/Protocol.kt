package com.xcam.net

import java.io.DataInputStream
import java.io.OutputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * Wire format shared with the Windows client. See docs/PROTOCOL.md — that file
 * is the spec; this is one of its two implementations. Keep them in lockstep.
 */
object Protocol {

    const val VERSION = 6
    const val PORT = 27183

    /** "XCAM" read back as a little-endian u32. */
    const val MAGIC = 0x4D414358
    val HANDSHAKE_MAGIC = byteArrayOf(0x58, 0x43, 0x41, 0x4D)

    const val HEADER_SIZE = 24

    object Type {
        const val CONFIG: Byte = 1
        const val KEYFRAME: Byte = 2
        const val DELTA: Byte = 3
        const val CONTROL: Byte = 4
        const val STATS: Byte = 5
        const val AUDIO: Byte = 6
        const val ACK: Byte = 7
        const val RECORD: Byte = 8

        /**
         * A chunk of a file being fetched off the phone.
         *
         * Paced rather than queued at will: the send queue sheds DELTA packets
         * when it fills, so a transfer that simply enqueued chunks would spend
         * the live picture to move a file. Chunks go out only when the queue is
         * already empty, which makes a fetch take whatever bandwidth is left
         * over and never a frame that was not.
         */
        const val FILE: Byte = 9
    }

    const val FLAG_LAST_FRAGMENT: Byte = 0x01

    /**
     * Marks an AUDIO packet as the codec configuration rather than a frame.
     *
     * Video gets its own packet type for this because a video CONFIG also means
     * "reset the decoder". Audio needs no such signal -- the AudioSpecificConfig
     * arrives once and is simply required before the first frame -- so a flag on
     * the packet costs less than a type that would mean two different things.
     */
    const val FLAG_CODEC_CONFIG: Byte = 0x02

    /**
     * RECORD only: this access unit is a key frame.
     *
     * The streaming path does not need a flag because it has a packet type per
     * kind. Recording does need one, because the receiver is muxing rather than
     * decoding and an MP4 has to know which samples it can seek to.
     */
    const val FLAG_KEY_FRAME: Byte = 0x04

    /**
     * Serialises a packet header into [dst] (which must hold at least
     * [HEADER_SIZE] bytes at offset 0) and returns it, so callers can reuse a
     * single scratch array per connection instead of allocating per frame.
     */
    fun writeHeader(
        dst: ByteArray,
        type: Byte,
        payloadLen: Int,
        ptsUs: Long,
        seq: Int,
        flags: Byte = FLAG_LAST_FRAGMENT,
    ): ByteArray {
        val bb = ByteBuffer.wrap(dst).order(ByteOrder.LITTLE_ENDIAN)
        bb.putInt(MAGIC)
        bb.put(type)
        bb.put(flags)
        bb.putShort(0)
        bb.putInt(payloadLen)
        bb.putLong(ptsUs)
        bb.putInt(seq)
        return dst
    }

    /** Builds the handshake blob that precedes all packets. */
    fun buildHandshake(json: String): ByteArray {
        val body = json.toByteArray(Charsets.UTF_8)
        val bb = ByteBuffer.allocate(12 + body.size).order(ByteOrder.LITTLE_ENDIAN)
        bb.put(HANDSHAKE_MAGIC)
        bb.putShort(VERSION.toShort())
        bb.putShort(0)
        bb.putInt(body.size)
        bb.put(body)
        return bb.array()
    }

    /** A single inbound packet; only CONTROL is expected from the PC in v1. */
    data class Packet(val type: Byte, val seq: Int, val ptsUs: Long, val payload: ByteArray) {
        fun payloadAsString(): String = String(payload, Charsets.UTF_8)

        override fun equals(other: Any?): Boolean =
            this === other || (other is Packet && type == other.type && seq == other.seq)

        override fun hashCode(): Int = 31 * type + seq
    }

    /**
     * Reads one packet, blocking until it is complete. Throws [java.io.IOException]
     * on a malformed stream so the caller drops the connection rather than trying
     * to resynchronise mid-stream.
     */
    fun readPacket(input: DataInputStream): Packet {
        val header = ByteArray(HEADER_SIZE)
        input.readFully(header)
        val bb = ByteBuffer.wrap(header).order(ByteOrder.LITTLE_ENDIAN)

        val magic = bb.int
        if (magic != MAGIC) throw java.io.IOException("bad packet magic 0x%08x".format(magic))

        val type = bb.get()
        bb.get()          // flags
        bb.short          // reserved
        val len = bb.int
        val ptsUs = bb.long
        val seq = bb.int

        if (len < 0 || len > MAX_PAYLOAD) throw java.io.IOException("payload length $len out of range")

        val payload = ByteArray(len)
        if (len > 0) input.readFully(payload)
        return Packet(type, seq, ptsUs, payload)
    }

    /** Guards against a desynchronised stream turning into an OOM. */
    private const val MAX_PAYLOAD = 64 * 1024 * 1024

    fun writePacket(
        out: OutputStream,
        scratch: ByteArray,
        type: Byte,
        payload: ByteArray,
        offset: Int = 0,
        length: Int = payload.size,
        ptsUs: Long = 0,
        seq: Int = 0,
        flags: Byte = FLAG_LAST_FRAGMENT,
    ) {
        out.write(writeHeader(scratch, type, length, ptsUs, seq, flags), 0, HEADER_SIZE)
        if (length > 0) out.write(payload, offset, length)
    }
}
