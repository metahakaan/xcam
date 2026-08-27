#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct ID3D11Device;
struct ID3D11Texture2D;

namespace xcam {

// Decoded frame handed to the consumer. Either `texture` is set (the decoder
// kept the frame on the GPU, which is the fast path the preview wants) or
// `nv12` holds a system-memory copy. Never both.
struct DecodedFrame {
    ID3D11Texture2D* texture = nullptr;   // borrowed; valid only during the callback
    uint32_t textureIndex = 0;            // subresource within a decoder array texture

    const uint8_t* nv12 = nullptr;
    size_t nv12Stride = 0;

    // Coded size, which H.264 rounds up to whole macroblocks -- 1080 arrives as
    // 1088. Sampling that padding shows up as a corrupt band along the bottom.
    uint32_t width = 0;
    uint32_t height = 0;

    // The region actually meant to be shown, from the stream's display aperture.
    uint32_t visibleWidth = 0;
    uint32_t visibleHeight = 0;

    uint64_t ptsUs = 0;
};

// Hardware H.264/HEVC decoder built on the Media Foundation decoder MFT with a
// D3D11 manager attached, which is what routes the work onto the GPU's video
// engine (NVDEC on this machine).
//
// The synchronous MFT is used deliberately. Async hardware MFTs need an event
// loop with METransformNeedInput/METransformHaveOutput, and buy nothing here:
// with a D3D manager set, the synchronous decoder is DXVA-accelerated all the
// same, and its straight-line ProcessInput/ProcessOutput is far easier to keep
// correct and to reason about for latency.
class MfDecoder {
public:
    using FrameCallback = std::function<void(const DecodedFrame&)>;

    MfDecoder();
    ~MfDecoder();

    MfDecoder(const MfDecoder&) = delete;
    MfDecoder& operator=(const MfDecoder&) = delete;

    // `codec` is "h264" or "hevc". The device is shared with the preview so
    // decoded textures never leave the GPU.
    bool Open(const std::string& codec, ID3D11Device* device);
    void Close();
    bool IsOpen() const { return transform_ != nullptr; }

    // Feeds the CONFIG payload (SPS/PPS, Annex-B). Safe to call again on a
    // stream change; the decoder is rebuilt so no state survives across it.
    bool SetCodecConfig(const uint8_t* data, size_t size);

    // Feeds one access unit. Frames come back through `onFrame`, which may fire
    // zero, one, or several times per call.
    bool Decode(const uint8_t* data, size_t size, uint64_t ptsUs,
                const FrameCallback& onFrame);

    // Pushes out whatever the decoder is still holding.
    void Flush(const FrameCallback& onFrame);

    // Every decoder Media Foundation can offer for this codec, with the name
    // and whether it is asynchronous. Exposed for diagnostics: "HEVC does not
    // work" is almost always a question of which decoders exist on the machine.
    struct DecoderInfo {
        std::string name;
        bool hardware = false;
        bool async = false;
    };
    static std::vector<DecoderInfo> ListDecoders(const std::string& codec);

    uint32_t Width() const { return width_; }
    uint32_t Height() const { return height_; }
    uint32_t VisibleWidth() const { return visibleWidth_; }
    uint32_t VisibleHeight() const { return visibleHeight_; }
    const std::string& LastError() const { return lastError_; }

private:
    bool ConfigureTypes();
    bool NegotiateOutputType();
    void DrainOutput(const FrameCallback& onFrame);

    struct Impl;
    Impl* impl_ = nullptr;

    void* transform_ = nullptr;      // IMFTransform*
    std::string codec_;
    std::string lastError_;
    std::vector<uint8_t> codecConfig_;

    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint32_t visibleWidth_ = 0;
    uint32_t visibleHeight_ = 0;
    bool streaming_ = false;
};

}  // namespace xcam
