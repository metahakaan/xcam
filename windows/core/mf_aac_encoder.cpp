#include "core/mf_aac_encoder.h"

#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wmcodecdsp.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace xcam {
namespace {

template <typename T>
void SafeRelease(T*& p) {
    if (p) { p->Release(); p = nullptr; }
}

IMFTransform* AsTransform(void* p) { return static_cast<IMFTransform*>(p); }

}  // namespace

MfAacEncoder::MfAacEncoder() { MFStartup(MF_VERSION, MFSTARTUP_LITE); }

MfAacEncoder::~MfAacEncoder() { Close(); }

bool MfAacEncoder::Open(uint32_t sampleRate, uint32_t channels, uint32_t bitsPerSecond) {
    Close();
    sampleRate_ = sampleRate;
    channels_ = channels;

    IMFTransform* encoder = nullptr;
    if (FAILED(CoCreateInstance(CLSID_AACMFTEncoder, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&encoder)))) {
        lastError_ = "no AAC encoder on this system";
        return false;
    }

    // Output first. The encoder settles its parameters here, and the input type
    // it will then accept follows from them -- the other order leaves it
    // rejecting the very format it is about to ask for.
    IMFMediaType* output = nullptr;
    if (FAILED(MFCreateMediaType(&output))) {
        SafeRelease(encoder);
        lastError_ = "could not describe the encoded audio";
        return false;
    }
    output->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    output->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
    output->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    output->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, sampleRate_);
    output->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels_);
    output->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, bitsPerSecond / 8);
    output->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);      // raw, which is what a muxer wants

    HRESULT hr = encoder->SetOutputType(0, output, 0);
    if (FAILED(hr)) {
        SafeRelease(output);
        SafeRelease(encoder);
        lastError_ = "the AAC encoder refused this rate or channel count";
        return false;
    }

    IMFMediaType* input = nullptr;
    if (FAILED(MFCreateMediaType(&input))) {
        SafeRelease(output);
        SafeRelease(encoder);
        lastError_ = "could not describe the raw audio";
        return false;
    }
    input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    input->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    input->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    input->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, sampleRate_);
    input->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels_);

    hr = encoder->SetInputType(0, input, 0);
    SafeRelease(input);
    if (FAILED(hr)) {
        SafeRelease(output);
        SafeRelease(encoder);
        lastError_ = "the AAC encoder refused 16-bit PCM";
        return false;
    }

    // The AudioSpecificConfig, taken from the type the encoder settled on.
    // Media Foundation hands it back with a twelve-byte HEAACWAVEINFO in front
    // of the two bytes a muxer actually wants.
    IMFMediaType* settled = nullptr;
    if (SUCCEEDED(encoder->GetOutputCurrentType(0, &settled)) && settled) {
        UINT32 size = 0;
        if (SUCCEEDED(settled->GetBlobSize(MF_MT_USER_DATA, &size)) && size > 12) {
            std::vector<uint8_t> blob(size);
            if (SUCCEEDED(settled->GetBlob(MF_MT_USER_DATA, blob.data(), size, &size))) {
                asc_.assign(reinterpret_cast<const char*>(blob.data()) + 12, size - 12);
            }
        }
        settled->Release();
    }

    SafeRelease(output);
    encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

    transform_ = encoder;
    lastError_.clear();
    return true;
}

void MfAacEncoder::Close() {
    if (!transform_) return;
    IMFTransform* encoder = AsTransform(transform_);
    encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
    encoder->Release();
    transform_ = nullptr;
    asc_.clear();
}

bool MfAacEncoder::Encode(const int16_t* samples, size_t frames, uint64_t ptsUs,
                          const FrameCallback& onFrame) {
    if (!transform_ || !samples || frames == 0) return false;

    const size_t bytes = frames * channels_ * sizeof(int16_t);

    IMFMediaBuffer* buffer = nullptr;
    if (FAILED(MFCreateMemoryBuffer(static_cast<DWORD>(bytes), &buffer))) return false;

    BYTE* dst = nullptr;
    if (FAILED(buffer->Lock(&dst, nullptr, nullptr))) {
        SafeRelease(buffer);
        return false;
    }
    std::memcpy(dst, samples, bytes);
    buffer->Unlock();
    buffer->SetCurrentLength(static_cast<DWORD>(bytes));

    IMFSample* sample = nullptr;
    if (FAILED(MFCreateSample(&sample))) {
        SafeRelease(buffer);
        return false;
    }
    sample->AddBuffer(buffer);
    sample->SetSampleTime(static_cast<LONGLONG>(ptsUs) * 10);
    sample->SetSampleDuration(
        static_cast<LONGLONG>(frames) * 10'000'000 / sampleRate_);
    SafeRelease(buffer);

    const HRESULT hr = AsTransform(transform_)->ProcessInput(0, sample, 0);
    SafeRelease(sample);
    if (FAILED(hr)) {
        lastError_ = "the AAC encoder would not take the samples";
        return false;
    }
    return PullFrames(onFrame);
}

void MfAacEncoder::Drain(const FrameCallback& onFrame) {
    if (!transform_) return;
    AsTransform(transform_)->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    PullFrames(onFrame);
}

bool MfAacEncoder::PullFrames(const FrameCallback& onFrame) {
    IMFTransform* encoder = AsTransform(transform_);

    MFT_OUTPUT_STREAM_INFO info{};
    encoder->GetOutputStreamInfo(0, &info);

    // Bounded. An encoder that keeps saying it has more is a loop this thread
    // never leaves, and one frame of audio is eight milliseconds -- a hundred
    // of them is a second, which nothing here can legitimately be holding.
    for (int guard = 0; guard < 100; ++guard) {
        IMFSample* sample = nullptr;
        const bool allocate = (info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) == 0;
        if (allocate) {
            IMFMediaBuffer* buffer = nullptr;
            if (FAILED(MFCreateMemoryBuffer(info.cbSize > 4096 ? info.cbSize : 4096, &buffer))) {
                return false;
            }
            if (FAILED(MFCreateSample(&sample))) {
                SafeRelease(buffer);
                return false;
            }
            sample->AddBuffer(buffer);
            SafeRelease(buffer);
        }

        MFT_OUTPUT_DATA_BUFFER out{};
        out.pSample = sample;
        DWORD status = 0;

        const HRESULT hr = encoder->ProcessOutput(0, 1, &out, &status);
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            SafeRelease(sample);
            return true;
        }
        if (FAILED(hr)) {
            SafeRelease(sample);
            lastError_ = "the AAC encoder failed";
            return false;
        }

        IMFSample* produced = out.pSample;
        IMFMediaBuffer* buffer = nullptr;
        if (SUCCEEDED(produced->ConvertToContiguousBuffer(&buffer))) {
            BYTE* data = nullptr;
            DWORD length = 0;
            if (SUCCEEDED(buffer->Lock(&data, nullptr, &length)) && length > 0) {
                LONGLONG time = 0;
                produced->GetSampleTime(&time);
                if (onFrame) onFrame(data, length, static_cast<uint64_t>(time) / 10);
                buffer->Unlock();
            }
            SafeRelease(buffer);
        }
        SafeRelease(produced);
        if (out.pEvents) out.pEvents->Release();
    }
    return true;
}

}  // namespace xcam
