#pragma once

// AAC-LC to 16-bit PCM, on the Media Foundation decoder MFT.
//
// The counterpart to MfDecoder, and much smaller: audio has no surfaces, no
// GPU, no display aperture and no frame-rate negotiation. What it does have is a
// codec configuration that must arrive before anything can be decoded -- the
// two-byte AudioSpecificConfig the phone sends as its first AUDIO packet -- and
// a sample rate and channel count that come from that configuration rather than
// from the frames.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace xcam {

class MfAudioDecoder {
public:
    // Called with interleaved 16-bit PCM. The buffer is borrowed and valid only
    // for the duration of the call.
    using PcmCallback = std::function<void(const uint8_t* pcm, size_t bytes, uint64_t ptsUs)>;

    MfAudioDecoder();
    ~MfAudioDecoder();

    MfAudioDecoder(const MfAudioDecoder&) = delete;
    MfAudioDecoder& operator=(const MfAudioDecoder&) = delete;

    // `asc` is the AudioSpecificConfig. Everything the decoder needs to know
    // about the stream is in it, including the rate and channel count, so this
    // both opens the decoder and settles the output format.
    bool Open(const uint8_t* asc, size_t ascBytes, uint32_t sampleRate, uint32_t channels);
    void Close();
    bool IsOpen() const { return transform_ != nullptr; }

    // One raw AAC access unit, no ADTS header.
    bool Decode(const uint8_t* aac, size_t bytes, uint64_t ptsUs, const PcmCallback& onPcm);

    uint32_t SampleRate() const { return sampleRate_; }
    uint32_t Channels() const { return channels_; }
    const std::string& LastError() const { return lastError_; }

private:
    struct Impl;
    Impl* impl_ = nullptr;
    void* transform_ = nullptr;      // IMFTransform*, kept opaque here
    uint32_t sampleRate_ = 0;
    uint32_t channels_ = 0;
    std::string lastError_;
};

}  // namespace xcam
