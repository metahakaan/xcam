#pragma once

// Writes the phone's recording into an MP4 on this machine.
//
// Nothing here re-encodes. The phone's second encoder has already produced
// exactly the stream that belongs in the file, and the sound is already AAC, so
// this is a muxer and only a muxer -- Media Foundation's MPEG-4 sink with the
// input type set equal to the output type, which makes it a pass-through.
//
// That matters more than it sounds: re-encoding on the desktop would undo the
// whole point of recording at 120 Mbit/s in the first place.

#include <cstdint>
#include <string>

namespace xcam {

class Mp4Writer {
public:
    Mp4Writer();
    ~Mp4Writer();

    Mp4Writer(const Mp4Writer&) = delete;
    Mp4Writer& operator=(const Mp4Writer&) = delete;

    // `codec` is "h264" or "hevc"; `csd` is the parameter-set blob the phone
    // sent, which the sink needs before it can describe the track.
    //
    // Audio is optional and settled here rather than later: a sink writer takes
    // its streams before it starts and none after, so a file opened without an
    // audio track cannot grow one when the first sound arrives.
    bool Open(const std::string& path, const std::string& codec,
              uint32_t width, uint32_t height, uint32_t fps,
              const uint8_t* csd, size_t csdBytes,
              const uint8_t* audioAsc, size_t audioAscBytes,
              uint32_t sampleRate, uint32_t channels,
              // A second audio track, for a microphone plugged into this
              // machine. Settled here for the same reason as the first: a sink
              // writer takes its streams before it starts and none after, so a
              // file that opened with one track cannot grow another when the
              // interface is switched on halfway through a take.
              const uint8_t* audio2Asc = nullptr, size_t audio2AscBytes = 0,
              uint32_t sampleRate2 = 0, uint32_t channels2 = 0);

    // Finishes the file. A recording that is not finalised has no index and no
    // player will open it, so this runs even on the failure paths.
    void Close();
    bool IsOpen() const { return writer_ != nullptr; }

    // `ptsUs` is on the phone's session timeline; the first sample written
    // defines zero for the file.
    bool WriteVideo(const uint8_t* data, size_t bytes, uint64_t ptsUs, bool keyFrame);
    bool WriteAudio(const uint8_t* data, size_t bytes, uint64_t ptsUs);

    // The second track. The phone's microphone goes on the first because it is
    // the one that carries sync; this is the one anyone will actually use.
    bool WriteAudio2(const uint8_t* data, size_t bytes, uint64_t ptsUs);

    const std::string& Path() const { return path_; }
    uint64_t Bytes() const { return bytes_; }
    uint64_t DurationMs() const;
    bool HasAudio() const { return audioStream_ >= 0; }
    bool HasAudio2() const { return audio2Stream_ >= 0; }

    const std::string& LastError() const { return lastError_; }

    // Where recordings go, created if missing: %USERPROFILE%\Videos\XCam.
    static std::string DefaultDirectory();

    // A name of the same shape the phone uses, so a mixed set of takes sorts
    // together.
    static std::string TimestampedName();

private:
    void* writer_ = nullptr;          // IMFSinkWriter*
    long videoStream_ = -1;
    long audioStream_ = -1;
    long audio2Stream_ = -1;

    std::string path_;
    std::string lastError_;

    uint32_t fps_ = 30;
    uint64_t bytes_ = 0;

    // The file's zero, taken from the first video sample. Audio that predates
    // it is dropped rather than written at a negative time.
    int64_t baseUs_ = -1;
    int64_t lastUs_ = 0;
};

}  // namespace xcam
