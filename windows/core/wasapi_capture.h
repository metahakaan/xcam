#pragma once

// A microphone on this machine.
//
// Nobody serious records a shoot on a phone's microphone. They do want it in
// the file, because it is the track that carries sync -- but the sound anyone
// will actually use comes from an interface plugged in here.
//
// So this captures one, and the recording carries both: the phone's as the
// reference, this one as the take. Two tracks, one clock, one file.
//
// Shared mode rather than exclusive: an interface someone is also monitoring
// through must stay usable, and a capture that seized the device would make the
// desktop the only application allowed to hear it.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace xcam {

struct AudioInput {
    std::string id;                  // the endpoint's own id, for remembering
    std::string name;                // what a person would recognise
    bool isDefault = false;
};

class WasapiCapture {
public:
    // Called from the capture thread with interleaved 16-bit samples, and the
    // performance-counter time the first of them was captured at.
    using Callback = std::function<void(const int16_t* samples, size_t frames,
                                        uint64_t captureQpc)>;

    WasapiCapture();
    ~WasapiCapture();

    WasapiCapture(const WasapiCapture&) = delete;
    WasapiCapture& operator=(const WasapiCapture&) = delete;

    // Every capture endpoint that is plugged in and switched on.
    static std::vector<AudioInput> List();

    // An empty id takes the default device, which is what someone who has not
    // chosen means.
    bool Start(const std::string& id, const Callback& onSamples);
    void Stop();
    bool IsRunning() const { return running_; }

    uint32_t SampleRate() const { return sampleRate_; }
    uint32_t Channels() const { return channels_; }
    const std::string& DeviceName() const { return deviceName_; }

    // The loudest sample since the last call, 0..1, and reading resets it. The
    // same arrangement as the phone's, and for the same reason: a level that is
    // measured and thrown away is how a dead microphone ships.
    float TakePeak();

    // How many buffers the endpoint marked as silent since the last call.
    //
    // Worth telling apart from a quiet room: Windows hands a blocked or muted
    // input to an application as digital silence with this flag set, exactly as
    // Android hands over a denied microphone. A meter that never moves looks the
    // same either way, and the difference is what somebody needs to be told.
    uint32_t TakeSilentBuffers();

    const std::string& LastError() const { return lastError_; }

private:
    // Takes the endpoint id it was started with. Opening "the default" again
    // in here is how the chosen device came to be ignored: Start looked one up
    // to read its name, and the capture thread quietly used another.
    void Run(std::string id, Callback callback);

    struct Impl;
    Impl* impl_ = nullptr;

    volatile bool running_ = false;
    uint32_t sampleRate_ = 48000;
    uint32_t channels_ = 2;
    std::string deviceName_;
    std::string lastError_;
};

}  // namespace xcam
