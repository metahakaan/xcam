#pragma once

// NV12 to H.264 or HEVC, on this machine.
//
// The counterpart to MfDecoder, and the piece that only exists because of one
// feature: recording with the LUT baked in. Everything else in this project
// deliberately never re-encodes -- the phone's second encoder already produces
// exactly the stream that belongs in the file, and a transcode on the desktop
// would undo the whole point of encoding at 120 Mbit/s over there.
//
// Grading is the exception, because a grade cannot be applied to a stream
// without decoding it. So this is opt-in, and the settings sheet says what it
// costs.

#include <cstdint>
#include <functional>
#include <string>

namespace xcam {

class MfEncoder {
public:
    // Called with one encoded access unit, Annex-B, on the calling thread.
    using SampleCallback =
        std::function<void(const uint8_t* data, size_t bytes, uint64_t ptsUs, bool keyFrame)>;

    MfEncoder();
    ~MfEncoder();

    MfEncoder(const MfEncoder&) = delete;
    MfEncoder& operator=(const MfEncoder&) = delete;

    // `codec` is "h264" or "hevc". Hardware encoders are preferred and a
    // software one is accepted: a grade that runs slowly is better than one
    // that refuses to run.
    bool Open(const std::string& codec, uint32_t width, uint32_t height,
              uint32_t fps, uint32_t bitrate);
    void Close();
    bool IsOpen() const { return transform_ != nullptr; }

    // One packed NV12 frame: luma rows of `width`, then interleaved chroma.
    bool Encode(const uint8_t* nv12, uint64_t ptsUs, const SampleCallback& onSample);

    // Pushes out whatever the encoder is still holding. A file finalised
    // without this loses the last frames, which at a one-second GOP can be a
    // second of footage.
    void Drain(const SampleCallback& onSample);

    // The codec private data, for whoever has to describe the track. Empty
    // until the first sample has come out.
    const std::string& CodecPrivateData() const { return codecPrivate_; }

    const std::string& LastError() const { return lastError_; }

private:
    bool PullSamples(const SampleCallback& onSample);

    struct Impl;
    Impl* impl_ = nullptr;
    void* transform_ = nullptr;      // IMFTransform*
    uint32_t width_ = 0, height_ = 0, fps_ = 30;
    std::string codecPrivate_;
    std::string lastError_;
};

}  // namespace xcam
