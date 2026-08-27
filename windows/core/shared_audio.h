#pragma once

// The audio half of the bridge between xcam-app and the DirectShow filters.
//
// Same constraint as shared_frames.h: the filter is a COM DLL Windows loads into
// the consuming application's process, so a named section is the only way to
// hand it anything. The shape is different, though, and deliberately so.
//
// Video is a sequence of discrete frames and a consumer that misses one should
// skip to the newest -- showing a stale picture is worse than showing none.
// Audio is a continuous stream where skipping is audible, so this is a plain
// byte ring that the reader walks in order, with a read position of its own. It
// falls behind only if it stops reading for longer than the ring holds, and then
// it resynchronises once rather than clicking on every buffer.

#include <cstdint>
#include <string>

namespace xcam {

inline constexpr uint32_t kSharedAudioMagic = 0x314D4158;   // "XAM1"
inline constexpr uint32_t kSharedAudioVersion = 1;

// What both ends assume. The phone is configured to match, so no resampling
// happens anywhere: a rate converter in the middle of a live path is latency
// and drift for nothing.
inline constexpr uint32_t kAudioSampleRate = 48000;
inline constexpr uint32_t kAudioChannels = 2;
inline constexpr uint32_t kAudioBitsPerSample = 16;
inline constexpr uint32_t kAudioBytesPerFrame = kAudioChannels * kAudioBitsPerSample / 8;

// Two seconds. Large enough that a consumer stalling for a moment loses nothing,
// small enough that a consumer which stops entirely does not accumulate two
// minutes of sound to catch up on.
inline constexpr uint32_t kAudioRingBytes = kAudioSampleRate * kAudioBytesPerFrame * 2;

struct SharedAudioHeader {
    uint32_t magic;
    uint32_t version;

    uint32_t sampleRate;
    uint32_t channels;
    uint32_t bitsPerSample;
    uint32_t ringBytes;

    // Total bytes ever written. The ring position is this modulo ringBytes;
    // keeping the running total is what lets a reader work out how far behind it
    // has fallen rather than merely that it has.
    volatile uint64_t writePos;

    volatile uint64_t lastWriteTick;
    volatile uint32_t producerAlive;
    uint32_t reserved;
};

// Writer side, owned by xcam-app.
class SharedAudioWriter {
public:
    SharedAudioWriter();
    ~SharedAudioWriter();

    SharedAudioWriter(const SharedAudioWriter&) = delete;
    SharedAudioWriter& operator=(const SharedAudioWriter&) = delete;

    bool Open();
    void Close();
    bool IsOpen() const { return header_ != nullptr; }

    // Appends interleaved 16-bit PCM. Never blocks and never fails for want of
    // room: a reader that cannot keep up is overwritten, which is the right
    // trade for a live stream.
    bool Publish(const uint8_t* pcm, size_t bytes);

    const std::string& LastError() const { return lastError_; }

private:
    void* mapping_ = nullptr;      // HANDLE
    void* view_ = nullptr;
    void* dataEvent_ = nullptr;    // HANDLE
    SharedAudioHeader* header_ = nullptr;
    uint8_t* ring_ = nullptr;
    std::string lastError_;
};

// Reader side, living inside whatever process loaded the filter.
class SharedAudioReader {
public:
    SharedAudioReader();
    ~SharedAudioReader();

    SharedAudioReader(const SharedAudioReader&) = delete;
    SharedAudioReader& operator=(const SharedAudioReader&) = delete;

    bool Attach();
    void Detach();
    bool IsAttached() const { return header_ != nullptr; }

    bool ProducerAlive() const;

    // Copies up to `bytes` of PCM into `dst`, waiting up to `timeoutMs` for the
    // producer to supply it. Returns how many bytes were actually written; the
    // caller is expected to pad the remainder with silence rather than stall,
    // because an audio capture device that stops delivering looks broken to
    // every application.
    size_t Read(uint8_t* dst, size_t bytes, uint32_t timeoutMs);

    // True when the last Read had to resynchronise because the producer had
    // lapped us. Worth surfacing: it means audio was lost, not merely late.
    bool TakeDiscontinuity();

private:
    void* mapping_ = nullptr;
    void* view_ = nullptr;
    void* dataEvent_ = nullptr;
    SharedAudioHeader* header_ = nullptr;
    uint8_t* ring_ = nullptr;
    uint64_t readPos_ = 0;
    bool haveReadPos_ = false;
    bool discontinuity_ = false;
};

}  // namespace xcam
