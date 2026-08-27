#pragma once

// One camera, as this desktop knows it.
//
// What the panel draws from and what it sends. Kept apart from both the window
// and the socket so the two threads that touch it have one obvious contract:
// the UI thread edits `pending` and emits commands, the network thread applies
// what the phone says it actually did.
//
// Everything in here belongs to one angle. What is the same whichever angle is
// on air -- the recordings folder, the virtual camera, the presets, the desk
// microphone -- lives in AppModel next door, so that a second phone gets its
// own exposure without also getting its own copy of where files go.

#include "app/app_model.h"
#include "core/protocol.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace xcam {

enum class ExposureMode { Auto, Manual };
enum class FocusMode { Continuous, Manual };

// The parameter the pro column has expanded for editing. Only one at a time,
// the way a camera's mode dial works.
enum class ProControl {
    None, Iso, Shutter, WhiteBalance, Focus, Ev,
    Gain, Contrast, Saturation, Warmth, LutAmount,
};

struct CameraModel {
    // ---- from the handshake -----------------------------------------------
    DeviceInfo device;
    size_t cameraIndex = 0;

    const CameraInfo* Camera() const {
        return cameraIndex < device.cameras.size() ? &device.cameras[cameraIndex] : nullptr;
    }

    // ---- stream format ----------------------------------------------------
    int width = 1920;
    int height = 1080;
    int fps = 60;
    int bitrate = 60'000'000;
    std::string codec = "h264";

    // What the encoder is actually being asked for, which drops below `bitrate`
    // when the link cannot carry it. Shown separately because a stream running
    // at half the chosen rate is worth knowing about, and silently rewriting the
    // chosen value would lose the target to come back to.
    int activeBitrate = 0;
    bool bitrateLimited = false;

    // ---- picture ----------------------------------------------------------
    float zoom = 1.0f;
    bool torch = false;

    ExposureMode exposureMode = ExposureMode::Auto;
    int iso = 400;
    int64_t shutterNs = 16'666'666;      // 1/60

    // Set when the phone clamps a shutter longer than the frame interval. The
    // panel shows this rather than the request, because the alternative is a
    // control that appears to work while the frame rate quietly halves.
    bool shutterClamped = false;

    FocusMode focusMode = FocusMode::Continuous;
    float focusDistance = 0.0f;          // dioptres, 0 = infinity

    // Flat capture profile. The picture looks wrong until a LUT is applied,
    // which is the point: what the camera discards cannot be graded back.
    bool logProfile = false;

    // ---- local recording --------------------------------------------------
    //
    // Separate from the stream on purpose. What arrives over USB is sized for a
    // webcam and compressed to reach here on time; what the phone writes is what
    // the sensor actually gave. These fields describe the second one, and every
    // number in them comes from the phone rather than from what was asked for.

    bool recording = false;
    int64_t recordMs = 0;
    int64_t recordBytes = 0;
    int64_t storageFreeMb = -1;
    std::string recordFile;         // absolute path on the phone, for adb pull

    // Whether a recording encoder exists on the phone at all. One sitting ready
    // costs a few milliseconds of stream latency, and a 4K one costs the frame
    // rate as well, so this is something to be able to switch off rather than a
    // permanent tax on being a webcam.
    bool recordEnabled = true;

    // Where the file is written. The desktop by default: it is where the person
    // using the recording already is, and the alternative leaves something to be
    // collected afterwards. The phone stays the answer when the link cannot
    // carry a second stream, which over Wi-Fi it cannot.
    bool recordToPc = true;

    // The file this side is writing, while it is writing one.
    std::string recordLocalPath;

    // 0 means "whatever the camera does best", which is the honest default for a
    // file whose whole purpose is to keep what the stream throws away.
    int recordWidth = 0;
    int recordHeight = 0;
    int recordFps = 0;              // 0 means "the streaming rate"
    std::string recordCodec = "hevc";

    bool CanRecord() const { return device.recorder; }

    // The phone's microphone, which feeds both the recording and the virtual
    // microphone on this side. Off is a real choice: a webcam pointed at a room
    // where someone else is already carrying the sound does not want it.
    bool micEnabled = true;
    bool CanUseMic() const { return device.audioAvailable; }

    // ---- settings that outlive a session ----------------------------------

    // What was *asked* for, as against what came back.
    //
    // The two used to be one field, and that was a ratchet. A 4K stream leaves
    // the sensor nothing to record with, so the phone clamps the take to 720p
    // and says so; writing that answer back over the request meant the next
    // record configuration asked for 720p, and the one after that, for ever --
    // a one-time consequence of a stream size became a permanent setting nobody
    // chose. Zero means "the camera at its best", which is the phone's to decide.
    int recordWantWidth = 0;
    int recordWantHeight = 0;

    // Record every n-th frame. One sensor produces both streams, so a recording
    // slower than the stream is reached by leaving the record target out of
    // some requests -- which is also what makes a time-lapse.
    int recordInterval = 1;

    // The microphone's level, and how long it has been at nothing.
    //
    // Held rather than shown raw: a meter that only ever draws the last tick
    // flickers, and a peak that falls back slowly is how every level meter ever
    // built behaves. `micSilentMs` is the number that matters -- a microphone
    // that is on and has been at zero for seconds is broken, not quiet.
    float audioPeak = 0.0f;
    float audioHold = 0.0f;
    int64_t micSilentMs = 0;

    // What the phone has on it, as of the last time it was asked.
    //
    // Not kept in sync automatically: a listing costs a round trip and a
    // MediaMetadataRetriever open per file, so it is refreshed when the browser
    // is opened and after anything that would change it.
    std::vector<TakeInfo> takes;
    std::string takesDir;
    std::string takesError;
    bool takesPending = false;

    // Set when something happened that the listing does not know about -- a take
    // finished, most of the time. The browser asks again when it sees this,
    // rather than every second: a listing opens every file on the phone to read
    // its duration, which is not free.
    bool takesStale = false;

    // The fetch in flight, if any. `fetchBytes` is what the phone said the file
    // was, `fetchReceived` how much has arrived -- the two are what a progress
    // bar is made of, and their difference is the only honest estimate of how
    // much longer it will take.
    std::string fetchName;
    std::string fetchLocalPath;
    int64_t fetchBytes = 0;
    int64_t fetchReceived = 0;

    bool Fetching() const { return !fetchName.empty(); }

    // Pre-roll: seconds of footage the phone keeps ready so a take can begin
    // before the button was pressed. `preRollGranted` is what the phone said it
    // could actually hold, which is not always what was asked for, and
    // `preRollFillMs` is how full the ring is right now.
    //
    // Local takes only. The ring is on the phone, and shipping it across when a
    // take starts would stall the live picture for as long as the ring is deep.
    int preRoll = 0;
    int preRollGranted = 0;
    int64_t preRollFillMs = 0;

    // The follow focus: two marks, in dioptres, and how long a move between
    // them should take. -1 means the mark has not been set.
    //
    // The marks live here rather than on the phone because they belong to the
    // shot, not to the camera -- they survive a reconnect, a camera switch and
    // a format change, all of which rebuild the capture request.
    float focusA = -1.0f;
    float focusB = -1.0f;
    int rampMs = 2000;
    bool rampingFocus = false;

    // When the move should be over, by this machine's clock. The phone does not
    // report finishing -- it would be a packet to say nothing is happening -- so
    // the indicator goes out on the duration that was asked for.
    double rampEndsAt = 0.0;

    // The take being described: when it started, what it is called and how long
    // it ran. Raised when a take ends and cleared once the sidecar is written --
    // the file system does not belong on the thread reading the socket.
    double takeStartedAt = 0.0;
    std::string describePending;
    int64_t describeMs = 0;

    // Auto-framing, and the crop it currently has in force, as fractions of the
    // sensor's active array. The crop lives here because the next one is
    // composed with it: the detector sees a picture that is already the result
    // of this rectangle, so a framing computed afresh each time would walk.
    bool autoFrame = false;
    float frameX = 0.0f, frameY = 0.0f, frameW = 1.0f, frameH = 1.0f;

    // Mirroring. Left to right is what people reach for; top to bottom is for a
    // phone hanging upside down in a clamp.
    bool flipX = false;
    bool flipY = false;

    // The rate a record config should ask for. Derived rather than stored: the
    // interval is the setting, and every place that reconfigures the recorder
    // has to honour it or changing the record size quietly undoes a time-lapse.
    int recordRate() const {
        // Derived from the stream rate, not from the last applied one: the ACK
        // writes recordFps back, so reading it here would make 1/4 followed by
        // 1x settle at a quarter rate and stay there.
        if (fps <= 0) return recordFps;
        return fps / (recordInterval > 1 ? recordInterval : 1);
    }

    std::string wbMode = "auto";
    int wbKelvin = 5600;

    float ev = 0.0f;

    // ---- live stats -------------------------------------------------------
    double statFps = 0;
    double statMbps = 0;
    double statLatencyMs = 0;
    int statGaps = 0;
    bool connected = false;
    std::string status = "connecting";

    // "USB" or "Wi-Fi 192.168.1.10", as it appears on screen -- as against
    // `transport` below, which is what the person asked for. Worth showing:
    // the two behave differently enough under load that knowing which one is
    // carrying the picture explains most of what the numbers above are doing.
    std::string transportLabel;

    // ---- derived helpers --------------------------------------------------

    bool CanManualExposure() const {
        const CameraInfo* c = Camera();
        return c && c->SupportsManualExposure();
    }
    bool CanManualFocus() const {
        const CameraInfo* c = Camera();
        return c && c->SupportsManualFocus();
    }
    bool CanManualWhiteBalance() const {
        const CameraInfo* c = Camera();
        return c && c->SupportsManualWhiteBalance();
    }
    bool CanLogProfile() const {
        const CameraInfo* c = Camera();
        return c && c->logProfile;
    }

    // Resolutions worth offering, largest first and de-duplicated. The raw list
    // runs to thirty-odd entries including sensor-native crops nobody wants for
    // a webcam, so the panel shows the common ones and nothing else.
    std::vector<CaptureMode> PreferredModes() const {
        static const int kHeights[] = {2160, 1440, 1080, 720, 480};
        std::vector<CaptureMode> out;
        const CameraInfo* c = Camera();
        if (!c) return out;

        for (int wanted : kHeights) {
            const CaptureMode* best = nullptr;
            for (const CaptureMode& mode : c->modes) {
                // 16:9 only -- a webcam that hands out 4:3 or square surprises
                // every downstream app.
                if (mode.height != wanted) continue;
                if (mode.width * 9 != mode.height * 16) continue;
                if (!best || mode.maxFps > best->maxFps) best = &mode;
            }
            if (best) out.push_back(*best);
        }
        return out;
    }

    // Frame rates offered for the current resolution, capped by what that mode
    // can actually sustain.
    std::vector<int> FrameRatesForCurrentSize() const {
        static const int kRates[] = {24, 30, 60};
        std::vector<int> out;
        const CameraInfo* c = Camera();
        if (!c) return {30};

        int cap = 30;
        for (const CaptureMode& mode : c->modes) {
            if (mode.width == width && mode.height == height) cap = (std::max)(cap, mode.maxFps);
        }
        for (int rate : kRates) {
            if (rate <= cap) out.push_back(rate);
        }
        return out.empty() ? std::vector<int>{30} : out;
    }

    // The lens stops worth putting on screen, from the camera's zoom range.
    // Xiaomi's logical camera crosses between ultra-wide, main and tele on zoom
    // ratio alone, so these double as lens buttons.
    std::vector<float> LensStops() const {
        const CameraInfo* c = Camera();
        if (!c) return {1.0f};

        std::vector<float> stops;
        if (c->zoomMin < 0.95f) stops.push_back(c->zoomMin);
        stops.push_back(1.0f);
        for (float stop : {2.0f, 3.0f, 5.0f, 10.0f}) {
            if (stop <= c->zoomMax + 0.01f) stops.push_back(stop);
        }
        return stops;
    }
};

// Elapsed recording time, in the mm:ss a camera shows.
inline std::string FormatDuration(int64_t ms) {
    const int64_t total = ms / 1000;
    char buffer[32];
    if (total >= 3600) {
        sprintf_s(buffer, "%lld:%02lld:%02lld", total / 3600, (total / 60) % 60, total % 60);
    } else {
        sprintf_s(buffer, "%lld:%02lld", total / 60, total % 60);
    }
    return buffer;
}

// File sizes in the unit that keeps the number small enough to read at a glance.
inline std::string FormatBytes(int64_t bytes) {
    char buffer[32];
    if (bytes >= 1000LL * 1000 * 1000) sprintf_s(buffer, "%.1f GB", bytes / 1e9);
    else sprintf_s(buffer, "%lld MB", bytes / 1000000);
    return buffer;
}

// Human-readable shutter speed: cameras speak in fractions of a second, not
// nanoseconds, and "1/60" is the only form anyone reads fluently.
inline std::string FormatShutter(int64_t ns) {
    if (ns <= 0) return "--";
    const double seconds = static_cast<double>(ns) / 1e9;
    if (seconds >= 1.0) {
        char buffer[32];
        sprintf_s(buffer, "%.1fs", seconds);
        return buffer;
    }
    char buffer[32];
    sprintf_s(buffer, "1/%d", static_cast<int>(1.0 / seconds + 0.5));
    return buffer;
}

// Focus distance reads as a distance, not the dioptres Camera2 works in.
inline std::string FormatFocus(float dioptres) {
    if (dioptres <= 0.01f) return "INF";
    const float metres = 1.0f / dioptres;
    char buffer[32];
    if (metres < 1.0f) sprintf_s(buffer, "%.0fcm", metres * 100.0f);
    else sprintf_s(buffer, "%.1fm", metres);
    return buffer;
}

}  // namespace xcam
