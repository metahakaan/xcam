#include "core/mp4_writer.h"

#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <shlobj.h>

#include <cstdio>
#include <cstring>
#include <ctime>
#include <vector>

namespace xcam {
namespace {

struct MediaFoundationScope {
    MediaFoundationScope() { MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET); }
    ~MediaFoundationScope() { MFShutdown(); }
};

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                      nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::string Narrow(const wchar_t* s) {
    char buffer[MAX_PATH * 2] = "";
    WideCharToMultiByte(CP_UTF8, 0, s, -1, buffer, sizeof(buffer), nullptr, nullptr);
    return buffer;
}

template <typename T>
void SafeRelease(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

}  // namespace

Mp4Writer::Mp4Writer() {
    static MediaFoundationScope scope;
}

Mp4Writer::~Mp4Writer() { Close(); }

std::string Mp4Writer::DefaultDirectory() {
    PWSTR wide = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Videos, 0, nullptr, &wide))) return {};
    std::string dir = Narrow(wide);
    CoTaskMemFree(wide);

    dir += "\\XCam";
    SHCreateDirectoryExW(nullptr, Widen(dir).c_str(), nullptr);
    return dir;
}

std::string Mp4Writer::TimestampedName() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &now);

    char buffer[64];
    std::strftime(buffer, sizeof(buffer), "XCam_%Y%m%d-%H%M%S.mp4", &local);
    return buffer;
}

bool Mp4Writer::Open(const std::string& path, const std::string& codec,
                     uint32_t width, uint32_t height, uint32_t fps,
                     const uint8_t* csd, size_t csdBytes,
                     const uint8_t* audioAsc, size_t audioAscBytes,
                     uint32_t sampleRate, uint32_t channels,
                     const uint8_t* audio2Asc, size_t audio2AscBytes,
                     uint32_t sampleRate2, uint32_t channels2) {
    Close();

    if (width == 0 || height == 0) {
        lastError_ = "no recording size";
        return false;
    }
    fps_ = fps ? fps : 30;

    IMFAttributes* attributes = nullptr;
    if (FAILED(MFCreateAttributes(&attributes, 2))) {
        lastError_ = "MFCreateAttributes failed";
        return false;
    }
    // The samples arrive at the rate the phone produces them, which is already
    // real time; throttling on top of that would only build a queue.
    attributes->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);

    IMFSinkWriter* writer = nullptr;
    HRESULT hr = MFCreateSinkWriterFromURL(Widen(path).c_str(), nullptr, attributes, &writer);
    attributes->Release();
    if (FAILED(hr)) {
        lastError_ = "could not create " + path;
        return false;
    }

    // ---- video --------------------------------------------------------
    IMFMediaType* video = nullptr;
    if (FAILED(MFCreateMediaType(&video))) {
        writer->Release();
        lastError_ = "MFCreateMediaType failed";
        return false;
    }
    video->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    video->SetGUID(MF_MT_SUBTYPE, codec == "hevc" ? MFVideoFormat_HEVC : MFVideoFormat_H264);
    MFSetAttributeSize(video, MF_MT_FRAME_SIZE, width, height);
    MFSetAttributeRatio(video, MF_MT_FRAME_RATE, fps_, 1);
    MFSetAttributeRatio(video, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    video->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    // The bitrate is advisory in a pass-through mux, but the sink writes it
    // into the header and players show it.
    video->SetUINT32(MF_MT_AVG_BITRATE, 120'000'000);
    if (csd && csdBytes) {
        // The parameter sets. Without them the sink cannot describe the track,
        // and the file opens as a stream nothing will decode.
        video->SetBlob(MF_MT_MPEG_SEQUENCE_HEADER, csd, static_cast<UINT32>(csdBytes));
    }

    DWORD stream = 0;
    hr = writer->AddStream(video, &stream);
    if (SUCCEEDED(hr)) {
        // Input equal to output is what makes this a mux rather than a
        // transcode: no transform is inserted, and the bytes reach the file
        // exactly as the phone encoded them.
        hr = writer->SetInputMediaType(stream, video, nullptr);
    }
    video->Release();
    if (FAILED(hr)) {
        writer->Release();
        lastError_ = "the MP4 sink refused the video track";
        return false;
    }
    videoStream_ = static_cast<long>(stream);

    // ---- audio --------------------------------------------------------
    //
    // Added now or never: a sink writer takes its streams before it starts, so
    // a file opened without an audio track cannot grow one later.
    //
    // Written once and used for both tracks. They differ only in where their
    // samples come from, and a second copy of this would be a second place for
    // the HEAACWAVEINFO prefix to go wrong.
    auto addAudioTrack = [&](const uint8_t* asc, size_t ascBytes, uint32_t rate,
                             uint32_t count) -> long {
        if (!asc || ascBytes == 0 || rate == 0 || count == 0) return -1;

        IMFMediaType* audio = nullptr;
        if (FAILED(MFCreateMediaType(&audio))) return -1;

        audio->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        audio->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
        audio->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
        audio->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, count);
        audio->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        audio->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 192000 / 8);
        audio->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
        audio->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29);

        // Same layout the decoder wants: the HEAACWAVEINFO tail, then the
        // AudioSpecificConfig.
        std::vector<uint8_t> userData(12 + ascBytes, 0);
        auto* words = reinterpret_cast<WORD*>(userData.data());
        words[0] = 0;        // raw AAC
        words[1] = 0x29;     // LC
        words[2] = 0;
        std::memcpy(userData.data() + 12, asc, ascBytes);
        audio->SetBlob(MF_MT_USER_DATA, userData.data(),
                       static_cast<UINT32>(userData.size()));

        DWORD stream = 0;
        HRESULT trackHr = writer->AddStream(audio, &stream);
        if (SUCCEEDED(trackHr)) trackHr = writer->SetInputMediaType(stream, audio, nullptr);
        audio->Release();

        // A refused audio track is not a refused recording. Silent footage is
        // worth far more than no footage.
        return SUCCEEDED(trackHr) ? static_cast<long>(stream) : -1;
    };

    audioStream_ = addAudioTrack(audioAsc, audioAscBytes, sampleRate, channels);
    audio2Stream_ = addAudioTrack(audio2Asc, audio2AscBytes, sampleRate2, channels2);


    hr = writer->BeginWriting();
    if (FAILED(hr)) {
        writer->Release();
        videoStream_ = audioStream_ = audio2Stream_ = -1;
        lastError_ = "the MP4 sink would not start";
        return false;
    }

    writer_ = writer;
    path_ = path;
    bytes_ = 0;
    baseUs_ = -1;
    lastUs_ = 0;
    return true;
}

void Mp4Writer::Close() {
    auto* writer = static_cast<IMFSinkWriter*>(writer_);
    if (writer) {
        // Finalize writes the index. Without it the file has every byte of
        // footage in it and no player will open it.
        writer->Finalize();
        writer->Release();
        writer_ = nullptr;
    }
    videoStream_ = -1;
    audioStream_ = -1;
    audio2Stream_ = -1;
}

uint64_t Mp4Writer::DurationMs() const {
    if (baseUs_ < 0) return 0;
    return static_cast<uint64_t>((lastUs_ - baseUs_) / 1000);
}

bool Mp4Writer::WriteVideo(const uint8_t* data, size_t bytes, uint64_t ptsUs, bool keyFrame) {
    auto* writer = static_cast<IMFSinkWriter*>(writer_);
    if (!writer || videoStream_ < 0 || !data || bytes == 0) return false;

    if (baseUs_ < 0) {
        if (!keyFrame) return false;      // a file must open on a key frame
        baseUs_ = static_cast<int64_t>(ptsUs);
    }
    const int64_t relative = static_cast<int64_t>(ptsUs) - baseUs_;
    if (relative < 0) return false;
    lastUs_ = static_cast<int64_t>(ptsUs);

    IMFSample* sample = nullptr;
    IMFMediaBuffer* buffer = nullptr;
    if (FAILED(MFCreateSample(&sample))) return false;
    if (FAILED(MFCreateMemoryBuffer(static_cast<DWORD>(bytes), &buffer))) {
        sample->Release();
        return false;
    }

    BYTE* dst = nullptr;
    if (SUCCEEDED(buffer->Lock(&dst, nullptr, nullptr))) {
        std::memcpy(dst, data, bytes);
        buffer->Unlock();
    }
    buffer->SetCurrentLength(static_cast<DWORD>(bytes));
    sample->AddBuffer(buffer);

    sample->SetSampleTime(relative * 10);          // Media Foundation counts 100ns
    sample->SetSampleDuration(10'000'000 / fps_);
    sample->SetUINT32(MFSampleExtension_CleanPoint, keyFrame ? TRUE : FALSE);

    const HRESULT hr = writer->WriteSample(static_cast<DWORD>(videoStream_), sample);
    buffer->Release();
    sample->Release();

    if (FAILED(hr)) {
        lastError_ = "WriteSample failed for video";
        return false;
    }
    bytes_ += bytes;
    return true;
}

// Both audio tracks, which differ only in which stream they land on.
static bool WriteAudioSample(void* writerPtr, long stream, int64_t baseUs,
                             const uint8_t* data, size_t bytes, uint64_t ptsUs,
                             uint64_t& total, std::string& error) {
    auto* writer = static_cast<IMFSinkWriter*>(writerPtr);
    if (!writer || stream < 0 || !data || bytes == 0) return false;

    // Sound that predates the first picture belongs to no file. Writing it at a
    // negative time makes players disagree about where the recording starts.
    if (baseUs < 0) return false;
    const int64_t relative = static_cast<int64_t>(ptsUs) - baseUs;
    if (relative < 0) return false;

    IMFSample* sample = nullptr;
    IMFMediaBuffer* buffer = nullptr;
    if (FAILED(MFCreateSample(&sample))) return false;
    if (FAILED(MFCreateMemoryBuffer(static_cast<DWORD>(bytes), &buffer))) {
        sample->Release();
        return false;
    }

    BYTE* dst = nullptr;
    if (SUCCEEDED(buffer->Lock(&dst, nullptr, nullptr))) {
        std::memcpy(dst, data, bytes);
        buffer->Unlock();
    }
    buffer->SetCurrentLength(static_cast<DWORD>(bytes));
    sample->AddBuffer(buffer);

    sample->SetSampleTime(relative * 10);
    // One AAC-LC access unit is always 1024 samples, whatever its byte size.
    sample->SetSampleDuration(10'000'000LL * 1024 / 48000);
    sample->SetUINT32(MFSampleExtension_CleanPoint, TRUE);

    const HRESULT hr = writer->WriteSample(static_cast<DWORD>(stream), sample);
    buffer->Release();
    sample->Release();

    if (FAILED(hr)) {
        error = "WriteSample failed for audio";
        return false;
    }
    total += bytes;
    return true;
}

bool Mp4Writer::WriteAudio(const uint8_t* data, size_t bytes, uint64_t ptsUs) {
    return WriteAudioSample(writer_, audioStream_, baseUs_, data, bytes, ptsUs,
                            bytes_, lastError_);
}

bool Mp4Writer::WriteAudio2(const uint8_t* data, size_t bytes, uint64_t ptsUs) {
    return WriteAudioSample(writer_, audio2Stream_, baseUs_, data, bytes, ptsUs,
                            bytes_, lastError_);
}

}  // namespace xcam
