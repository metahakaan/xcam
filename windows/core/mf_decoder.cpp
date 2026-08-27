#include "core/mf_decoder.h"

#include <windows.h>
#include <d3d11.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wmcodecdsp.h>
#include <codecapi.h>

#include <cstring>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")

namespace xcam {
namespace {

template <typename T>
void SafeRelease(T*& p) {
    if (p) { p->Release(); p = nullptr; }
}

// MFStartup/MFShutdown are refcounted per process; one owner keeps it simple.
struct MediaFoundationScope {
    MediaFoundationScope() { MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET); }
    ~MediaFoundationScope() { MFShutdown(); }
};

void EnsureMediaFoundation() {
    static MediaFoundationScope scope;
    (void)scope;
}

// 100-nanosecond units, the currency of everything in Media Foundation.
constexpr int64_t kHundredNanosPerMicro = 10;

}  // namespace

struct MfDecoder::Impl {
    ID3D11Device* device = nullptr;
    IMFDXGIDeviceManager* deviceManager = nullptr;
    UINT resetToken = 0;

    IMFMediaType* inputType = nullptr;
    IMFMediaType* outputType = nullptr;

    // Reused across calls so the steady state allocates nothing.
    IMFSample* inputSample = nullptr;
    IMFMediaBuffer* inputBuffer = nullptr;
    size_t inputCapacity = 0;

    std::vector<uint8_t> systemCopy;
};

MfDecoder::MfDecoder() : impl_(new Impl) {}

MfDecoder::~MfDecoder() {
    Close();
    delete impl_;
}

void MfDecoder::Close() {
    auto* transform = static_cast<IMFTransform*>(transform_);
    if (transform) {
        if (streaming_) {
            transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
            transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        }
        transform->Release();
        transform_ = nullptr;
    }
    streaming_ = false;

    if (impl_) {
        SafeRelease(impl_->inputBuffer);
        SafeRelease(impl_->inputSample);
        SafeRelease(impl_->inputType);
        SafeRelease(impl_->outputType);
        SafeRelease(impl_->deviceManager);
        impl_->inputCapacity = 0;
        if (impl_->device) { impl_->device->Release(); impl_->device = nullptr; }
    }
    width_ = height_ = 0;
    visibleWidth_ = visibleHeight_ = 0;
}

namespace {

std::string Narrow(const wchar_t* wide) {
    if (!wide) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string out(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), n, nullptr, nullptr);
    return out;
}

const GUID& SubtypeFor(const std::string& codec) {
    return codec == "hevc" ? MFVideoFormat_HEVC : MFVideoFormat_H264;
}

}  // namespace

std::vector<MfDecoder::DecoderInfo> MfDecoder::ListDecoders(const std::string& codec) {
    EnsureMediaFoundation();
    std::vector<DecoderInfo> out;

    MFT_REGISTER_TYPE_INFO input{MFMediaType_Video, SubtypeFor(codec)};
    IMFActivate** activates = nullptr;
    UINT32 count = 0;

    const UINT32 flags = MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT |
                         MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER;
    if (FAILED(MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, flags, &input, nullptr,
                         &activates, &count))) {
        return out;
    }

    for (UINT32 i = 0; i < count; ++i) {
        DecoderInfo info;

        wchar_t* name = nullptr;
        UINT32 length = 0;
        if (SUCCEEDED(activates[i]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute,
                                                       &name, &length))) {
            info.name = Narrow(name);
            CoTaskMemFree(name);
        }
        info.hardware = activates[i]->GetUINT32(MF_TRANSFORM_FLAGS_Attribute, &length) == S_OK &&
                        (length & MFT_ENUM_FLAG_HARDWARE) != 0;

        IMFTransform* probe = nullptr;
        if (SUCCEEDED(activates[i]->ActivateObject(IID_PPV_ARGS(&probe))) && probe) {
            IMFAttributes* attributes = nullptr;
            if (SUCCEEDED(probe->GetAttributes(&attributes)) && attributes) {
                UINT32 isAsync = 0;
                attributes->GetUINT32(MF_TRANSFORM_ASYNC, &isAsync);
                info.async = isAsync != 0;
                attributes->Release();
            }
            probe->Release();
            activates[i]->ShutdownObject();
        }
        out.push_back(std::move(info));
        activates[i]->Release();
    }
    CoTaskMemFree(activates);
    return out;
}

bool MfDecoder::Open(const std::string& codec, ID3D11Device* device) {
    EnsureMediaFoundation();
    Close();

    codec_ = codec;

    // Ask Media Foundation what it has rather than naming one CLSID. The stock
    // HEVC decoder ships as a Store extension that is simply absent on many
    // machines, while a GPU driver may register its own -- and hardcoding
    // CLSID_MSH265DecoderMFT means seeing none of them.
    IMFTransform* transform = nullptr;

    MFT_REGISTER_TYPE_INFO input{MFMediaType_Video, SubtypeFor(codec)};
    IMFActivate** activates = nullptr;
    UINT32 count = 0;

    // Synchronous only: an asynchronous MFT needs an event loop this decoder
    // does not have, and would sit there producing nothing.
    const UINT32 flags = MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_HARDWARE |
                         MFT_ENUM_FLAG_SORTANDFILTER;

    if (SUCCEEDED(MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, flags, &input, nullptr,
                            &activates, &count))) {
        for (UINT32 i = 0; i < count && !transform; ++i) {
            IMFTransform* candidate = nullptr;
            if (SUCCEEDED(activates[i]->ActivateObject(IID_PPV_ARGS(&candidate))) &&
                candidate) {
                IMFAttributes* attributes = nullptr;
                UINT32 isAsync = 0;
                if (SUCCEEDED(candidate->GetAttributes(&attributes)) && attributes) {
                    attributes->GetUINT32(MF_TRANSFORM_ASYNC, &isAsync);
                    attributes->Release();
                }
                if (isAsync) {
                    candidate->Release();
                    activates[i]->ShutdownObject();
                } else {
                    transform = candidate;
                }
            }
        }
        for (UINT32 i = 0; i < count; ++i) activates[i]->Release();
        CoTaskMemFree(activates);
    }

    if (!transform) {
        // Last resort: the well-known CLSIDs, in case enumeration is filtered.
        const CLSID clsid = (codec == "hevc") ? CLSID_MSH265DecoderMFT
                                              : CLSID_MSH264DecoderMFT;
        CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&transform));
    }

    if (!transform) {
        lastError_ = (codec == "hevc")
            ? "no HEVC decoder on this system -- install \"HEVC Video Extensions\" "
              "from the Microsoft Store, or stay on H.264"
            : "no H.264 decoder on this system";
        return false;
    }
    transform_ = transform;

    if (device) {
        device->AddRef();
        impl_->device = device;

        // Handing the MFT a DXGI device manager is what turns this from a
        // software decode into a GPU one, and keeps output frames as D3D11
        // textures the preview can sample without a readback.
        if (SUCCEEDED(MFCreateDXGIDeviceManager(&impl_->resetToken, &impl_->deviceManager))) {
            impl_->deviceManager->ResetDevice(device, impl_->resetToken);
            transform->ProcessMessage(
                MFT_MESSAGE_SET_D3D_MANAGER,
                reinterpret_cast<ULONG_PTR>(impl_->deviceManager));
        }
    }

    // Low latency matters more than throughput: without this the decoder is
    // free to hold frames back for reordering it does not need on a live feed.
    IMFAttributes* attributes = nullptr;
    if (SUCCEEDED(transform->GetAttributes(&attributes)) && attributes) {
        attributes->SetUINT32(MF_LOW_LATENCY, TRUE);
        attributes->SetUINT32(CODECAPI_AVLowLatencyMode, TRUE);
        attributes->SetUINT32(CODECAPI_AVDecNumWorkerThreads, 0);
        attributes->Release();
    }

    lastError_.clear();
    return true;
}

bool MfDecoder::ConfigureTypes() {
    auto* transform = static_cast<IMFTransform*>(transform_);
    if (!transform) return false;

    SafeRelease(impl_->inputType);
    if (FAILED(MFCreateMediaType(&impl_->inputType))) {
        lastError_ = "MFCreateMediaType failed";
        return false;
    }

    impl_->inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    impl_->inputType->SetGUID(MF_MT_SUBTYPE,
                              codec_ == "hevc" ? MFVideoFormat_HEVC : MFVideoFormat_H264);
    impl_->inputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    impl_->inputType->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, FALSE);

    // The decoder can pick the frame size out of the SPS, so leaving it unset
    // avoids having to guess before the first frame arrives.
    if (!codecConfig_.empty()) {
        impl_->inputType->SetBlob(MF_MT_USER_DATA,
                                  codecConfig_.data(),
                                  static_cast<UINT32>(codecConfig_.size()));
    }

    HRESULT hr = transform->SetInputType(0, impl_->inputType, 0);
    if (FAILED(hr)) {
        lastError_ = "SetInputType failed (hr=0x" + std::to_string(static_cast<uint32_t>(hr)) + ")";
        return false;
    }

    if (!NegotiateOutputType()) return false;

    transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    streaming_ = true;
    return true;
}

bool MfDecoder::NegotiateOutputType() {
    auto* transform = static_cast<IMFTransform*>(transform_);

    // Walk the offered types and take NV12: it is what the hardware decoder
    // produces natively, so anything else would insert a conversion.
    for (DWORD i = 0;; ++i) {
        IMFMediaType* candidate = nullptr;
        HRESULT hr = transform->GetOutputAvailableType(0, i, &candidate);
        if (hr == MF_E_NO_MORE_TYPES || FAILED(hr)) break;

        GUID subtype{};
        candidate->GetGUID(MF_MT_SUBTYPE, &subtype);
        if (subtype == MFVideoFormat_NV12) {
            hr = transform->SetOutputType(0, candidate, 0);
            if (SUCCEEDED(hr)) {
                SafeRelease(impl_->outputType);
                impl_->outputType = candidate;

                UINT32 w = 0, h = 0;
                MFGetAttributeSize(candidate, MF_MT_FRAME_SIZE, &w, &h);
                width_ = w;
                height_ = h;

                // Default to the coded size, then narrow to the display aperture
                // when the stream carries one.
                visibleWidth_ = w;
                visibleHeight_ = h;

                MFVideoArea area{};
                if (SUCCEEDED(candidate->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE,
                                                 reinterpret_cast<UINT8*>(&area),
                                                 sizeof(area), nullptr))) {
                    if (area.Area.cx > 0 && area.Area.cy > 0) {
                        visibleWidth_ = static_cast<uint32_t>(area.Area.cx);
                        visibleHeight_ = static_cast<uint32_t>(area.Area.cy);
                    }
                }
                return true;
            }
        }
        candidate->Release();
    }

    lastError_ = "decoder offered no NV12 output type";
    return false;
}

bool MfDecoder::SetCodecConfig(const uint8_t* data, size_t size) {
    codecConfig_.assign(data, data + size);

    // A fresh CONFIG means a new stream: resolution, profile or codec may all
    // have changed, so rebuild rather than trying to reconfigure in place.
    ID3D11Device* device = impl_->device;
    if (device) device->AddRef();

    const std::string codec = codec_;
    bool ok = Open(codec, device) && ConfigureTypes();

    // MF_MT_USER_DATA alone is not enough for an Annex-B elementary stream: the
    // Microsoft decoder wants the parameter sets in-band. MediaCodec emits them
    // once, as the codec-config buffer, and does not repeat them before every
    // IDR, so without pushing them through as a sample here the decoder never
    // sees an SPS and silently produces nothing at all.
    if (ok) {
        auto discard = [](const DecodedFrame&) {};
        ok = Decode(codecConfig_.data(), codecConfig_.size(), 0, discard);
    }

    if (device) device->Release();
    return ok;
}

bool MfDecoder::Decode(const uint8_t* data, size_t size, uint64_t ptsUs,
                       const FrameCallback& onFrame) {
    auto* transform = static_cast<IMFTransform*>(transform_);
    if (!transform || !streaming_) {
        lastError_ = "decode before CONFIG";
        return false;
    }

    if (impl_->inputCapacity < size) {
        SafeRelease(impl_->inputBuffer);
        SafeRelease(impl_->inputSample);
        if (FAILED(MFCreateMemoryBuffer(static_cast<DWORD>(size), &impl_->inputBuffer)) ||
            FAILED(MFCreateSample(&impl_->inputSample))) {
            lastError_ = "could not allocate an input sample";
            return false;
        }
        impl_->inputSample->AddBuffer(impl_->inputBuffer);
        impl_->inputCapacity = size;
    }

    BYTE* dst = nullptr;
    DWORD maxLength = 0;
    if (FAILED(impl_->inputBuffer->Lock(&dst, &maxLength, nullptr))) {
        lastError_ = "could not lock the input buffer";
        return false;
    }
    std::memcpy(dst, data, size);
    impl_->inputBuffer->Unlock();
    impl_->inputBuffer->SetCurrentLength(static_cast<DWORD>(size));

    impl_->inputSample->SetSampleTime(static_cast<LONGLONG>(ptsUs) * kHundredNanosPerMicro);
    impl_->inputSample->SetSampleDuration(0);

    HRESULT hr = transform->ProcessInput(0, impl_->inputSample, 0);
    if (hr == MF_E_NOTACCEPTING) {
        // The decoder wants its output collected before it will take more.
        DrainOutput(onFrame);
        hr = transform->ProcessInput(0, impl_->inputSample, 0);
    }
    if (FAILED(hr)) {
        lastError_ = "ProcessInput failed (hr=0x" + std::to_string(static_cast<uint32_t>(hr)) + ")";
        return false;
    }

    DrainOutput(onFrame);
    return true;
}

void MfDecoder::DrainOutput(const FrameCallback& onFrame) {
    auto* transform = static_cast<IMFTransform*>(transform_);
    if (!transform) return;

    MFT_OUTPUT_STREAM_INFO streamInfo{};
    transform->GetOutputStreamInfo(0, &streamInfo);
    const bool decoderAllocates =
        (streamInfo.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
                               MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;

    for (;;) {
        MFT_OUTPUT_DATA_BUFFER output{};
        DWORD status = 0;

        IMFSample* sample = nullptr;
        if (!decoderAllocates) {
            if (FAILED(MFCreateSample(&sample))) return;
            IMFMediaBuffer* buffer = nullptr;
            if (FAILED(MFCreateMemoryBuffer(streamInfo.cbSize, &buffer))) {
                sample->Release();
                return;
            }
            sample->AddBuffer(buffer);
            buffer->Release();
            output.pSample = sample;
        }

        HRESULT hr = transform->ProcessOutput(0, 1, &output, &status);

        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            SafeRelease(output.pSample);
            SafeRelease(output.pEvents);
            return;
        }

        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            // Resolution or format changed mid-stream; renegotiate and retry.
            SafeRelease(output.pSample);
            SafeRelease(output.pEvents);
            if (!NegotiateOutputType()) return;
            continue;
        }

        if (FAILED(hr) || output.pSample == nullptr) {
            SafeRelease(output.pSample);
            SafeRelease(output.pEvents);
            return;
        }

        LONGLONG sampleTime = 0;
        output.pSample->GetSampleTime(&sampleTime);

        IMFMediaBuffer* buffer = nullptr;
        if (SUCCEEDED(output.pSample->ConvertToContiguousBuffer(&buffer)) && buffer) {
            DecodedFrame frame;
            frame.width = width_;
            frame.height = height_;
            frame.visibleWidth = visibleWidth_;
            frame.visibleHeight = visibleHeight_;
            frame.ptsUs = static_cast<uint64_t>(sampleTime / kHundredNanosPerMicro);

            // Preferred path: the frame is already a D3D11 texture, so the
            // preview can sample it without ever touching system memory.
            IMFDXGIBuffer* dxgiBuffer = nullptr;
            if (SUCCEEDED(buffer->QueryInterface(IID_PPV_ARGS(&dxgiBuffer)))) {
                ID3D11Texture2D* texture = nullptr;
                if (SUCCEEDED(dxgiBuffer->GetResource(IID_PPV_ARGS(&texture)))) {
                    UINT subresource = 0;
                    dxgiBuffer->GetSubresourceIndex(&subresource);

                    frame.texture = texture;
                    frame.textureIndex = subresource;
                    onFrame(frame);
                    texture->Release();
                }
                dxgiBuffer->Release();
            } else {
                BYTE* data = nullptr;
                DWORD maxLength = 0, currentLength = 0;
                if (SUCCEEDED(buffer->Lock(&data, &maxLength, &currentLength))) {
                    frame.nv12 = data;
                    frame.nv12Stride = width_;
                    onFrame(frame);
                    buffer->Unlock();
                }
            }
            buffer->Release();
        }

        SafeRelease(output.pSample);
        SafeRelease(output.pEvents);
    }
}

void MfDecoder::Flush(const FrameCallback& onFrame) {
    auto* transform = static_cast<IMFTransform*>(transform_);
    if (!transform || !streaming_) return;
    transform->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    DrainOutput(onFrame);
}

}  // namespace xcam
