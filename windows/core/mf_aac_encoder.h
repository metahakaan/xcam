#pragma once

// PCM to AAC, on this machine.
//
// The counterpart of MfAudioDecoder, and it exists for one reason: the second
// audio track. The phone's sound arrives already encoded and is muxed as it is;
// a microphone plugged in here arrives as samples and has to become something
// an MP4 can hold.
//
// Media Foundation's own AAC encoder, so there is nothing to bundle. It is
// fussy about what it will take -- 16-bit PCM at 44.1 or 48 kHz, mono or stereo
// -- which is exactly what a capture endpoint gives.

#include <cstdint>
#include <functional>
#include <string>

namespace xcam {

class MfAacEncoder {
public:
    // One encoded frame, with the timestamp of the samples that made it.
    using FrameCallback =
        std::function<void(const uint8_t* data, size_t bytes, uint64_t ptsUs)>;

    MfAacEncoder();
    ~MfAacEncoder();

    MfAacEncoder(const MfAacEncoder&) = delete;
    MfAacEncoder& operator=(const MfAacEncoder&) = delete;

    bool Open(uint32_t sampleRate, uint32_t channels, uint32_t bitsPerSecond);
    void Close();
    bool IsOpen() const { return transform_ != nullptr; }

    // Interleaved 16-bit samples. `ptsUs` is the moment the first of them was
    // captured, on whatever timeline the caller is keeping.
    bool Encode(const int16_t* samples, size_t frames, uint64_t ptsUs,
                const FrameCallback& onFrame);

    void Drain(const FrameCallback& onFrame);

    // The AudioSpecificConfig an MP4 track needs. Empty until the encoder has
    // settled its output type, which happens during Open.
    const std::string& AudioSpecificConfig() const { return asc_; }

    const std::string& LastError() const { return lastError_; }

private:
    bool PullFrames(const FrameCallback& onFrame);

    void* transform_ = nullptr;      // IMFTransform*
    uint32_t sampleRate_ = 48000;
    uint32_t channels_ = 2;
    std::string asc_;
    std::string lastError_;
};

}  // namespace xcam
