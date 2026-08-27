#include "core/mf_audio_decoder.h"

#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mftransform.h>
#include <mmreg.h>
#include <wmcodecdsp.h>

#include <algorithm>
#include <cstring>

namespace xcam {
namespace {

// MFStartup/MFShutdown are refcounted per process; mf_decoder.cpp owns one
// scope and this one owns another, which is exactly what the refcount is for.
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

struct MfAudioDecoder::Impl {
    MediaFoundationScope mf;
    IMFSample* inputSample = nullptr;
    IMFMediaBuffer* inputBuffer = nullptr;
    DWORD inputCapacity = 0;
    std::vector<uint8_t> pcm;
};

MfAudioDecoder::MfAudioDecoder() : impl_(new Impl()) {}

MfAudioDecoder::~MfAudioDecoder() {
    Close();
    delete impl_;
}

void MfAudioDecoder::Close() {
    auto* transform = static_cast<IMFTransform*>(transform_);
    if (transform) {
        transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        transform->Release();
        transform_ = nullptr;
    }
    if (impl_) {
        SafeRelease(impl_->inputSample);
        SafeRelease(impl_->inputBuffer);
        impl_->inputCapacity = 0;
    }
    sampleRate_ = 0;
    channels_ = 0;
}

bool MfAudioDecoder::Open(const uint8_t* asc, size_t ascBytes,
                          uint32_t sampleRate, uint32_t channels) {
    Close();

    if (sampleRate == 0 || channels == 0) {
        lastError_ = "no sample rate or channel count";
        return false;
    }

    IMFTransform* transform = nullptr;
    if (FAILED(CoCreateInstance(CLSID_CMSAACDecMFT, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&transform)))) {
        lastError_ = "no AAC decoder on this system";
        return false;
    }

    // The decoder is told the stream is raw AAC by a payload type of 0, and it
    // takes the AudioSpecificConfig through MF_MT_USER_DATA -- but preceded by
    // the three HEAACWAVEINFO fields that follow WAVEFORMATEX. Handing it the
    // bare config is the usual reason the decoder refuses the input type.
    std::vector<uint8_t> userData(12 + ascBytes, 0);
    auto* payloadType = reinterpret_cast<WORD*>(userData.data());
    payloadType[0] = 0;      // wPayloadType: raw AAC
    payloadType[1] = 0x29;   // wAudioProfileLevelIndication: LC
    payloadType[2] = 0;      // wStructType: must be zero
    if (asc && ascBytes) std::memcpy(userData.data() + 12, asc, ascBytes);

    IMFMediaType* input = nullptr;
    if (FAILED(MFCreateMediaType(&input))) {
        transform->Release();
        lastError_ = "MFCreateMediaType failed";
        return false;
    }
    input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    input->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
    input->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, sampleRate);
    input->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);
    input->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    input->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
    input->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29);
    input->SetBlob(MF_MT_USER_DATA, userData.data(),
                   static_cast<UINT32>(userData.size()));

    HRESULT hr = transform->SetInputType(0, input, 0);
    input->Release();
    if (FAILED(hr)) {
        transform->Release();
        lastError_ = "AAC decoder refused the input type";
        return false;
    }

    // Take an output type the decoder itself offers rather than describing one.
    //
    // A constructed PCM type is accepted here and then decodes to digital
    // silence: the decoder answers with the right number of bytes per frame and
    // nothing in them. Whatever it disagrees with is not reported, so the only
    // reliable move is to ask what it is prepared to produce and pick from that.
    IMFMediaType* output = nullptr;
    for (DWORD i = 0;; ++i) {
        IMFMediaType* candidate = nullptr;
        if (FAILED(transform->GetOutputAvailableType(0, i, &candidate))) break;

        GUID subtype{};
        UINT32 bits = 0;
        candidate->GetGUID(MF_MT_SUBTYPE, &subtype);
        candidate->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bits);
        if (subtype == MFAudioFormat_PCM && bits == 16) {
            output = candidate;
            break;
        }
        candidate->Release();
    }
    if (!output) {
        transform->Release();
        lastError_ = "the AAC decoder offers no 16-bit PCM output";
        return false;
    }

    UINT32 outRate = sampleRate;
    UINT32 outChannels = channels;
    output->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &outRate);
    output->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &outChannels);

    hr = transform->SetOutputType(0, output, 0);
    output->Release();
    if (FAILED(hr)) {
        transform->Release();
        lastError_ = "AAC decoder refused its own PCM output type";
        return false;
    }
    sampleRate = outRate;
    channels = outChannels;

    transform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

    transform_ = transform;
    sampleRate_ = sampleRate;
    channels_ = channels;
    return true;
}

bool MfAudioDecoder::Decode(const uint8_t* aac, size_t bytes, uint64_t ptsUs,
                            const PcmCallback& onPcm) {
    auto* transform = static_cast<IMFTransform*>(transform_);
    if (!transform || !aac || bytes == 0) return false;

    if (impl_->inputCapacity < bytes) {
        SafeRelease(impl_->inputSample);
        SafeRelease(impl_->inputBuffer);
        const DWORD size = static_cast<DWORD>(bytes * 2 + 1024);
        if (FAILED(MFCreateMemoryBuffer(size, &impl_->inputBuffer)) ||
            FAILED(MFCreateSample(&impl_->inputSample))) {
            lastError_ = "could not allocate an input sample";
            return false;
        }
        impl_->inputSample->AddBuffer(impl_->inputBuffer);
        impl_->inputCapacity = size;
    }

    BYTE* dst = nullptr;
    DWORD maxLength = 0;
    if (FAILED(impl_->inputBuffer->Lock(&dst, &maxLength, nullptr))) return false;
    std::memcpy(dst, aac, bytes);
    impl_->inputBuffer->Unlock();
    impl_->inputBuffer->SetCurrentLength(static_cast<DWORD>(bytes));

    // Media Foundation counts in 100ns units throughout.
    impl_->inputSample->SetSampleTime(static_cast<LONGLONG>(ptsUs) * 10);

    HRESULT hr = transform->ProcessInput(0, impl_->inputSample, 0);
    if (FAILED(hr)) {
        lastError_ = "ProcessInput failed";
        return false;
    }

    MFT_OUTPUT_STREAM_INFO streamInfo{};
    transform->GetOutputStreamInfo(0, &streamInfo);

    for (;;) {
        IMFSample* sample = nullptr;
        IMFMediaBuffer* buffer = nullptr;

        // Unlike the video decoder, the AAC MFT does not allocate its own output
        // samples, so one has to be supplied every time.
        const DWORD size = (std::max)(streamInfo.cbSize, DWORD{16384});
        if (FAILED(MFCreateSample(&sample))) break;
        if (FAILED(MFCreateMemoryBuffer(size, &buffer))) {
            sample->Release();
            break;
        }
        sample->AddBuffer(buffer);

        MFT_OUTPUT_DATA_BUFFER out{};
        out.pSample = sample;
        DWORD status = 0;

        hr = transform->ProcessOutput(0, 1, &out, &status);
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            buffer->Release();
            sample->Release();
            break;
        }
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            // The decoder settled on a different output format than the one it
            // was given, which for AAC means the configuration disagreed with
            // what the handshake said. Renegotiating would mean republishing a
            // new sample rate to consumers that have already fixed theirs, so
            // this is reported rather than absorbed.
            buffer->Release();
            sample->Release();
            lastError_ = "the decoder changed its output format mid-stream";
            return false;
        }
        if (FAILED(hr)) {
            buffer->Release();
            sample->Release();
            lastError_ = "ProcessOutput failed";
            return false;
        }

        BYTE* pcm = nullptr;
        DWORD length = 0;
        if (SUCCEEDED(buffer->Lock(&pcm, nullptr, &length)) && length > 0) {
            LONGLONG sampleTime = 0;
            sample->GetSampleTime(&sampleTime);
            onPcm(pcm, length, static_cast<uint64_t>(sampleTime / 10));
            buffer->Unlock();
        }

        buffer->Release();
        sample->Release();

        if (out.pEvents) out.pEvents->Release();
    }

    return true;
}

}  // namespace xcam
