#include "core/mf_encoder.h"

#include <windows.h>
#include <codecapi.h>
#include <icodecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace xcam {
namespace {

struct MediaFoundationScope {
    MediaFoundationScope() { MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET); }
    ~MediaFoundationScope() { MFShutdown(); }
};

template <typename T>
void SafeRelease(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

}  // namespace

struct MfEncoder::Impl {
    MediaFoundationScope mf;
    std::vector<uint8_t> scratch;
};

MfEncoder::MfEncoder() : impl_(new Impl()) {}

MfEncoder::~MfEncoder() {
    Close();
    delete impl_;
}

void MfEncoder::Close() {
    auto* transform = static_cast<IMFTransform*>(transform_);
    if (transform) {
        transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        transform->Release();
        transform_ = nullptr;
    }
    codecPrivate_.clear();
    width_ = height_ = 0;
}

bool MfEncoder::Open(const std::string& codec, uint32_t width, uint32_t height,
                     uint32_t fps, uint32_t bitrate) {
    Close();
    if (width == 0 || height == 0) {
        lastError_ = "no size";
        return false;
    }
    // The encoder wants even dimensions for 4:2:0; a stream that is not is a
    // stream nothing will decode.
    if ((width & 1) || (height & 1)) {
        lastError_ = "size must be even";
        return false;
    }

    const GUID subtype = codec == "hevc" ? MFVideoFormat_HEVC : MFVideoFormat_H264;

    // Synchronous encoders only.
    //
    // Every hardware video encoder on Windows is an asynchronous MFT: it wants
    // MF_TRANSFORM_ASYNC_UNLOCK, an event loop, and usually a D3D manager on
    // top. That machinery buys speed, and speed is not what this path is for --
    // it exists so a grade can be baked in, which the settings sheet already
    // labels as re-encoding on this PC. A synchronous encoder is a straight
    // ProcessInput/ProcessOutput and far easier to keep correct.
    MFT_REGISTER_TYPE_INFO output{};
    output.guidMajorType = MFMediaType_Video;
    output.guidSubtype = subtype;

    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    if (FAILED(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                         MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT |
                             MFT_ENUM_FLAG_SORTANDFILTER,
                         nullptr, &output, &activates, &count)) ||
        count == 0) {
        lastError_ = "no " + codec + " encoder on this system";
        return false;
    }

    IMFTransform* transform = nullptr;
    for (UINT32 i = 0; i < count; ++i) {
        if (!transform && SUCCEEDED(activates[i]->ActivateObject(IID_PPV_ARGS(&transform)))) {
            // keep the first that activates
        }
        activates[i]->Release();
    }
    CoTaskMemFree(activates);

    if (!transform) {
        lastError_ = "could not activate a " + codec + " encoder";
        return false;
    }

    // No B-frames, and low latency -- set before the output type.
    //
    // Ordering matters here: several encoders latch these when the output type
    // is set and ignore anything said afterwards, which is why the same calls
    // made later had no effect at all.
    //
    // Left alone, the Microsoft encoder emits B-frames, so its output arrives
    // out of presentation order -- 33ms, 100ms, 66ms, 166ms, 133ms. An MP4 sink
    // fed that in the order it comes drops nearly half of it, which showed up as
    // a three-second recording holding 48 of 90 frames at an apparent 16fps.
    //
    // Reordering them properly would mean holding a window of samples and
    // computing composition offsets, for a small saving in bitrate that a
    // camera recording does not want anyway. Turning them off costs a few
    // percent of size and makes every frame arrive in the order it was shot.
    ICodecAPI* codecApi = nullptr;
    if (SUCCEEDED(transform->QueryInterface(IID_PPV_ARGS(&codecApi))) && codecApi) {
        // Set unconditionally. IsModifiable is not implemented by every
        // encoder, and gating on it silently skipped every one of these.
        auto set = [&](const GUID& property, VARIANT value) {
            codecApi->SetValue(&property, &value);
        };

        VARIANT v{};
        v.vt = VT_UI4;
        v.ulVal = 0;
        set(CODECAPI_AVEncMPVDefaultBPictureCount, v);

        v.vt = VT_BOOL;
        v.boolVal = VARIANT_TRUE;
        set(CODECAPI_AVEncCommonLowLatency, v);

        // A second between key frames: this file is scrubbed, not streamed, and
        // a longer GOP makes seeking coarse for a saving that does not matter.
        v.vt = VT_UI4;
        v.ulVal = fps ? fps : 30;
        set(CODECAPI_AVEncMPVGOPSize, v);

        v.vt = VT_UI4;
        v.ulVal = bitrate ? bitrate : 40'000'000;
        set(CODECAPI_AVEncCommonMeanBitRate, v);

        codecApi->Release();
    }

    // Output first. An encoder cannot be told what it takes until it knows what
    // it is being asked to produce.
    IMFMediaType* out = nullptr;
    if (FAILED(MFCreateMediaType(&out))) {
        transform->Release();
        lastError_ = "MFCreateMediaType failed";
        return false;
    }
    out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    out->SetGUID(MF_MT_SUBTYPE, subtype);
    out->SetUINT32(MF_MT_AVG_BITRATE, bitrate ? bitrate : 40'000'000);
    out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(out, MF_MT_FRAME_SIZE, width, height);
    MFSetAttributeRatio(out, MF_MT_FRAME_RATE, fps ? fps : 30, 1);
    MFSetAttributeRatio(out, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (codec != "hevc") {
        out->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
    }

    HRESULT hr = transform->SetOutputType(0, out, 0);
    out->Release();
    if (FAILED(hr)) {
        transform->Release();
        lastError_ = "the encoder refused that output format";
        return false;
    }

    // Take an input type the encoder itself offers rather than describing one.
    //
    // A constructed NV12 type is refused outright by the Microsoft encoder, and
    // it does not say which attribute it disliked. Asking what it accepts and
    // filling in the size is the only reliable move -- the same lesson the AAC
    // decoder taught, where a constructed output type was accepted and then
    // decoded to silence.
    IMFMediaType* in = nullptr;
    for (DWORD i = 0;; ++i) {
        IMFMediaType* candidate = nullptr;
        if (FAILED(transform->GetInputAvailableType(0, i, &candidate))) break;

        GUID candidateSubtype{};
        candidate->GetGUID(MF_MT_SUBTYPE, &candidateSubtype);
        if (candidateSubtype == MFVideoFormat_NV12) {
            in = candidate;
            break;
        }
        candidate->Release();
    }
    if (!in) {
        transform->Release();
        lastError_ = "the encoder offers no NV12 input";
        return false;
    }

    in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(in, MF_MT_FRAME_SIZE, width, height);
    MFSetAttributeRatio(in, MF_MT_FRAME_RATE, fps ? fps : 30, 1);
    MFSetAttributeRatio(in, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

    hr = transform->SetInputType(0, in, 0);
    in->Release();
    if (FAILED(hr)) {
        transform->Release();
        lastError_ = "the encoder refused NV12 at this size";
        return false;
    }

    transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

    transform_ = transform;
    width_ = width;
    height_ = height;
    fps_ = fps ? fps : 30;
    return true;
}

bool MfEncoder::Encode(const uint8_t* nv12, uint64_t ptsUs, const SampleCallback& onSample) {
    auto* transform = static_cast<IMFTransform*>(transform_);
    if (!transform || !nv12) return false;

    const DWORD bytes = static_cast<DWORD>(width_) * height_ * 3 / 2;

    IMFSample* sample = nullptr;
    IMFMediaBuffer* buffer = nullptr;
    if (FAILED(MFCreateSample(&sample))) return false;
    if (FAILED(MFCreateMemoryBuffer(bytes, &buffer))) {
        sample->Release();
        return false;
    }

    BYTE* dst = nullptr;
    if (SUCCEEDED(buffer->Lock(&dst, nullptr, nullptr))) {
        std::memcpy(dst, nv12, bytes);
        buffer->Unlock();
    }
    buffer->SetCurrentLength(bytes);
    sample->AddBuffer(buffer);
    sample->SetSampleTime(static_cast<LONGLONG>(ptsUs) * 10);
    sample->SetSampleDuration(10'000'000LL / fps_);

    const HRESULT hr = transform->ProcessInput(0, sample, 0);
    buffer->Release();
    sample->Release();

    if (hr == MF_E_NOTACCEPTING) {
        // The encoder is holding as much as it will; take what it has and try
        // once more rather than dropping the frame.
        PullSamples(onSample);
        return true;
    }
    if (FAILED(hr)) {
        lastError_ = "ProcessInput failed";
        return false;
    }
    return PullSamples(onSample);
}

void MfEncoder::Drain(const SampleCallback& onSample) {
    auto* transform = static_cast<IMFTransform*>(transform_);
    if (!transform) return;
    transform->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    PullSamples(onSample);
}

bool MfEncoder::PullSamples(const SampleCallback& onSample) {
    auto* transform = static_cast<IMFTransform*>(transform_);
    if (!transform) return false;

    MFT_OUTPUT_STREAM_INFO info{};
    transform->GetOutputStreamInfo(0, &info);
    const bool allocatesItsOwn =
        (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
                         MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;

    // Bounded. An encoder that keeps answering without ever saying it needs
    // more input would otherwise hang whatever is driving it, and a hang inside
    // someone's recording is worse than a dropped frame.
    for (int guard = 0; guard < 64; ++guard) {
        MFT_OUTPUT_DATA_BUFFER out{};
        DWORD status = 0;

        IMFSample* sample = nullptr;
        IMFMediaBuffer* buffer = nullptr;
        if (!allocatesItsOwn) {
            const DWORD size = (std::max)(info.cbSize, DWORD{1} << 20);
            if (FAILED(MFCreateSample(&sample))) break;
            if (FAILED(MFCreateMemoryBuffer(size, &buffer))) {
                sample->Release();
                break;
            }
            sample->AddBuffer(buffer);
            out.pSample = sample;
        }

        const HRESULT hr = transform->ProcessOutput(0, 1, &out, &status);

        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            // out.pSample is the one supplied above, so releasing both would be
            // releasing it twice.
            SafeRelease(buffer);
            SafeRelease(sample);
            break;
        }
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            // The encoder settled on a different output type. Accept whatever
            // it now offers rather than failing: the size and codec have not
            // changed, only details it is entitled to pick.
            SafeRelease(buffer);
            SafeRelease(sample);
            IMFMediaType* renegotiated = nullptr;
            if (SUCCEEDED(transform->GetOutputAvailableType(0, 0, &renegotiated))) {
                transform->SetOutputType(0, renegotiated, 0);
                renegotiated->Release();
            }
            continue;
        }
        if (FAILED(hr)) {
            SafeRelease(buffer);
            SafeRelease(sample);
            lastError_ = "ProcessOutput failed";
            return false;
        }

        IMFSample* produced = out.pSample;
        if (produced) {
            // The parameter sets, kept the first time they appear: whoever has
            // to describe the track needs them and the encoder says them once.
            if (codecPrivate_.empty()) {
                IMFMediaType* type = nullptr;
                if (SUCCEEDED(transform->GetOutputCurrentType(0, &type)) && type) {
                    UINT32 size = 0;
                    if (SUCCEEDED(type->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &size)) &&
                        size > 0) {
                        codecPrivate_.resize(size);
                        type->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER,
                                      reinterpret_cast<UINT8*>(codecPrivate_.data()),
                                      size, nullptr);
                    }
                    type->Release();
                }
            }

            IMFMediaBuffer* contiguous = nullptr;
            if (SUCCEEDED(produced->ConvertToContiguousBuffer(&contiguous))) {
                BYTE* data = nullptr;
                DWORD length = 0;
                if (SUCCEEDED(contiguous->Lock(&data, nullptr, &length)) && length > 0) {
                    LONGLONG time = 0;
                    produced->GetSampleTime(&time);
                    UINT32 clean = 0;
                    produced->GetUINT32(MFSampleExtension_CleanPoint, &clean);
                    onSample(data, length, static_cast<uint64_t>(time / 10), clean != 0);
                    contiguous->Unlock();
                }
                contiguous->Release();
            }
        }

        if (out.pEvents) out.pEvents->Release();
        SafeRelease(buffer);
        if (produced && produced != sample) produced->Release();
        SafeRelease(sample);
    }
    return true;
}

}  // namespace xcam
