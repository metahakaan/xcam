// XCam desktop preview.
//
// Two threads. The worker reads the socket and decodes, copying each frame into
// renderer memory and going straight back to reading. The UI thread owns the
// window, collects input, and does all the presenting.
//
// The split is the whole point: Present blocks while the GPU catches up, and
// anything that blocks the socket reader backs up into the phone's send queue
// and grows latency without bound. Decoding stays in the read path because it
// is cheap and skipping it would cost a frame; presenting does not.
//
// For the same reason the read path takes no shared lock. Guarding it with the
// same mutex the presenter holds would reintroduce the coupling through the
// back door: the reader would simply wait on the lock instead of on the GPU.
//
// Both take AppState::lock before touching the renderer or the model, which is
// the only shared state there is.

#include "app/camera_model.h"
#include "app/lut.h"
#include "app/preview_d3d11.h"
#include "app/pro_panel.h"
#include "app/connect_panel.h"
#include "app/settings_panel.h"
#include "app/take_sidecar.h"
#include "app/takes_panel.h"
#include "app/strings.h"
#include "app/ui/ui_context.h"
#include "app/xcam_resource.h"
#include "core/adb.h"
#include "core/discovery.h"
#include "core/log.h"
#include "core/mf_audio_decoder.h"
#include "core/mf_decoder.h"
#include "core/face_finder.h"
#include "core/mf_aac_encoder.h"
#include "core/wasapi_capture.h"
#include "core/mf_encoder.h"
#include "core/mp4_writer.h"
#include "core/net_client.h"
#include "core/protocol.h"
#include "core/settings.h"
#include "core/shared_audio.h"
#include "core/shared_frames.h"

#include <windows.h>
#include <windowsx.h>
#include <d2d1_1.h>
#include <d3d11.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shobjidl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <fstream>
#include <filesystem>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace xcam;

namespace {

constexpr wchar_t kWindowClass[] = L"XCamPreviewWindow";
constexpr UINT kIdleRenderTimer = 1;

// The one angle there is.
//
// Named rather than written as 0 so that every place still assuming a single
// camera says so. The renderer, the model and the panel are ready for several;
// the connection loop is not, and this constant is the list of what it would
// take -- grep for it and you have the work, rather than having to recognise a
// bare zero as an assumption.
constexpr size_t kSoleAngle = 0;

// Starting with Windows.
//
// The difference between a tool and a webcam is whether anyone has to remember
// to run it. A camera that only works if you first start a desktop application
// is a tool; one that is simply there when Discord opens is a webcam.
//
// HKCU\...\Run rather than a scheduled task or a service: it is per-user, needs
// no elevation, and a person who wants it gone can see it in Task Manager's
// Startup tab and switch it off there. Anything more powerful would be harder
// to remove than to install, which is not a trade to make on someone's behalf.
namespace autostart {

constexpr wchar_t kKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kValue[] = L"XCam";

std::wstring CommandLine() {
    wchar_t path[MAX_PATH] = L"";
    if (!GetModuleFileNameW(nullptr, path, MAX_PATH)) return {};
    // Quoted: the executable usually lives under a path with a space in it, and
    // an unquoted one runs the first word and fails silently at every boot.
    return L"\"" + std::wstring(path) + L"\" --minimised";
}

bool Enabled() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kKey, 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return false;
    }
    wchar_t stored[MAX_PATH * 2] = L"";
    DWORD bytes = sizeof(stored);
    DWORD type = 0;
    const LSTATUS status =
        RegQueryValueExW(key, kValue, nullptr, &type,
                         reinterpret_cast<BYTE*>(stored), &bytes);
    RegCloseKey(key);
    return status == ERROR_SUCCESS && type == REG_SZ;
}

void Set(bool enabled) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kKey, 0, nullptr, 0, KEY_WRITE, nullptr,
                        &key, nullptr) != ERROR_SUCCESS) {
        XCAM_LOG_WARN("could not open the Run key");
        return;
    }

    if (enabled) {
        // Rewritten every time rather than only when absent: the executable
        // moves when someone rebuilds it somewhere else, and a stale entry
        // fails at boot in a way nobody would connect to this switch.
        const std::wstring command = CommandLine();
        RegSetValueExW(key, kValue, 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(command.c_str()),
                       static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, kValue);
    }
    RegCloseKey(key);
    XCAM_LOG_INFO("start with Windows: %s", enabled ? "on" : "off");
}

}  // namespace autostart


// How often the phone reports. Mirrors STATS_INTERVAL_MS on the other side; the
// only thing this side does with it is turn a count of ticks into a duration.
constexpr int64_t kStatsIntervalMs = 1000;

double NowSeconds();

// Wall clock, unlike NowSeconds, which is a monotonic timer and says nothing
// about what day it is. A sidecar records when a take happened, and that is a
// date rather than an interval.
double UnixSeconds();

// Keeps the encoder's bitrate inside what the link can actually carry.
//
// Mostly this does nothing: over USB the chosen rate is carried without
// complaint, and the only correct adaptation is none. It exists for Wi-Fi,
// where 60 Mbit/s is optimistic and the failure mode without it is not a softer
// picture but a growing queue -- the phone sheds frames, latency climbs, and
// the stream falls apart while every number still says the bitrate was applied.
//
// Two signals, both of which mean the link is behind rather than the encoder:
// frames the phone dropped, and queueing delay on this side. Backing off is
// sharp and recovering is slow, because guessing high costs a visible stall and
// guessing low costs only detail.
class BitrateGovernor {
public:
    void SetTarget(int bitrate) {
        target_ = bitrate;
        active_ = bitrate;
        lastChange_ = NowSeconds();
    }

    // Returns the new bitrate to request, or 0 for "leave it alone".
    int Update(int droppedFrames, double queueMs) {
        if (!enabled_ || target_ <= 0) return 0;
        const double now = NowSeconds();

        const bool struggling = droppedFrames > 0 || queueMs > kQueueLimitMs;
        if (struggling) {
            const int next = (std::max)(static_cast<int>(active_ * 0.75), kFloor);
            if (next < active_) {
                active_ = next;
                lastChange_ = now;
                XCAM_LOG_INFO("link is behind (%d dropped, %.0fms queued); "
                              "bitrate down to %d Mb/s",
                              droppedFrames, queueMs, active_ / 1000000);
                return active_;
            }
            // Already at the floor: holding here is the honest answer, since
            // going lower would trade a picture for a link that is broken
            // anyway.
            lastChange_ = now;
            return 0;
        }

        // Recover only after a stretch of calm, and only part of the way back.
        // Jumping straight to the target re-creates the congestion that caused
        // the drop in the first place.
        if (active_ < target_ && now - lastChange_ >= kRecoverySeconds) {
            const int next = (std::min)(static_cast<int>(active_ * 1.25), target_);
            active_ = next;
            lastChange_ = now;
            XCAM_LOG_INFO("link is calm; bitrate up to %d Mb/s", active_ / 1000000);
            return active_;
        }
        return 0;
    }

    void Disable() { enabled_ = false; }
    bool Limited() const { return enabled_ && active_ < target_; }
    int Active() const { return active_; }

private:
    static constexpr int kFloor = 4'000'000;
    static constexpr double kQueueLimitMs = 250.0;
    static constexpr double kRecoverySeconds = 6.0;

    bool enabled_ = true;
    int target_ = 0;
    int active_ = 0;
    double lastChange_ = 0;
};

// One encoded frame from the desk microphone, waiting for the thread that owns
// the file.
struct DeskAudioFrame {
    std::vector<uint8_t> bytes;
    uint64_t ptsUs = 0;
};

// One connection, and everything that belongs to it rather than to the
// application.
//
// Split out with exactly one of them, on purpose. Multicam needs several, and
// the alternative -- reaching into a single client and decoder from a loop that
// would then have to know which phone it was talking about -- is the version
// that goes wrong. What is still shared, and is the remaining cost of getting
// there, is the model: it describes one camera's exposure, format and state,
// and a second angle needs its own.
struct Device {
    NetClient client;
    MfDecoder decoder;
    BitrateGovernor governor;

    // Where this one was found, for the panel and for the log.
    std::string host;
    bool overUsb = false;
    std::string serial;
};

// One angle: a phone, what this side knows about it, and everything that
// arrives down its own socket.
//
// Everything in here exists once per camera. The connection, obviously -- but
// also the recording, because every angle records its own file at its own
// quality, which is what makes a multicam set worth having rather than a
// switcher's single output. And the clock offset, because two phones are two
// clocks and mapping one of them onto this machine says nothing about the
// other.
//
// Held in a deque and never erased while running: a thread holds a reference to
// its own angle for the length of the session.
struct Angle {
    // Which slot in the renderer this one decodes into, and which entry in the
    // panel's row it is. Fixed for the life of the angle.
    size_t index = 0;

    Device link;
    CameraModel model;

    // The local end of this angle's link. Over USB every phone gets its own
    // adb forward, because a forward is a port on this machine and two of them
    // cannot share one -- the second phone would silently reach the first.
    uint16_t port = kDefaultPort;

    // The phone this angle is for, by adb serial. Empty means "whichever one
    // adb offers", which is what a single-camera setup wants and what the
    // first angle is left as.
    std::string wantSerial;

    // An address this angle was told to use, which outranks everything the
    // connection loop would otherwise work out. --host sets it for the first
    // angle; --angle adds another with one of its own.
    std::string host;

    // Whether this angle has ever had a streaming session. A phone will offer
    // to resume one to anybody who connects in time; only a client that was
    // already talking to *this* phone can accept.
    bool hadSession = false;

    // Sound arrives, is decoded and is published in one place, with nothing for
    // the UI thread to contend on. The decoder is per angle because each one
    // sends its own audio; only the angle on program reaches the virtual
    // microphone, the same way only it reaches the virtual camera.
    MfAudioDecoder audioDecoder;

    // The recording, when it is being written here rather than on the phone.
    // Owned by this angle's network thread, which is where both streams that
    // feed it arrive.
    Mp4Writer recording;
    std::vector<uint8_t> audioAsc;    // kept so a take can open its audio track

    // The file being fetched off this phone, when one is.
    //
    // Owned by the network thread, which is where the chunks arrive. Held open
    // for the length of the transfer rather than reopened per chunk: a fetch is
    // thousands of packets, and the destination is often a spinning disk.
    std::ofstream fetchFile;

    // How far this machine's clock is from this phone's, in microseconds, as a
    // running minimum of what the arriving frames say. INT64_MIN means nothing
    // has been measured yet, and nothing can be placed on the phone's timeline
    // until something has.
    int64_t clockOffsetUs = INT64_MIN;
    double clockOffsetSince = 0.0;

    // What this angle's own connection loop has already said, so it says each
    // thing once. These were static locals, which was correct while there was
    // one worker thread and quietly wrong the moment there were two: one
    // angle's "trying 127.0.0.1" would suppress the other's, and one angle's
    // refusals would count against the other's remembered address.
    int wifiRefusals = 0;
    bool askedAdb = false;
    std::string reportedStatus;
    std::string announcedTarget;

    std::thread thread;
};

struct AppState {
    ID3D11Device* device = nullptr;
    PreviewRenderer renderer;

    // The angles. There is always at least one, made before anything runs, so
    // that every path can ask for the program without checking whether a phone
    // has been found yet.
    std::deque<Angle> angles;

    AppState() { angles.emplace_back(); }

    // The angle on air: what the panel addresses, what the virtual camera
    // publishes, and what the tally light is lit for.
    Angle& Program() {
        return angles[shared.program < angles.size() ? shared.program : 0];
    }

    ui::UiContext ui;
    ui::Input input;
    ProPanel panel;
    SettingsPanel settingsPanel;
    ConnectPanel connectPanel;
    TakesPanel takesPanel;

    // The renderer and the models are both touched from either thread, so they
    // share one lock rather than two that would have to be taken in order.
    // One lock for every angle, not one each: the panel reads several models in
    // a single draw, and a lock per angle would be a lock order to get wrong.
    std::mutex lock;

    // What every angle shares: the folder, the virtual camera, the presets, the
    // desk microphone, the look. One of these however many cameras there are,
    // which is the whole reason it is not part of the model above.
    AppModel shared;

    std::atomic<bool> quit{false};

    // Publishes decoded frames to the DirectShow filter. Fed from the
    // presenting thread, since the readback it needs stalls on the GPU.
    SharedFrameWriter publisher;
    std::vector<uint8_t> publishBuffer;

    // Sound takes the same shape as the picture: decode on the angle, publish
    // through a named section, and let a filter inside the consuming
    // application read it. One publisher, like one virtual camera -- the
    // program angle writes to it and the others decode for their own files.
    SharedAudioWriter audioPublisher;

    // Recording the graded picture. The only path in this project that
    // re-encodes, because a grade cannot be applied to a stream without
    // decoding it -- so it is opt-in, and it records the live stream rather
    // than the phone's separate high-bitrate encode, which never reaches this
    // machine as pixels.
    //
    // Lives on the presenting thread with the rest of the readback work.
    MfEncoder gradeEncoder;
    Mp4Writer gradeWriter;
    std::vector<uint8_t> gradeBuffer;
    bool gradeActive = false;
    // What the open encoder was opened at. The shape can be changed in the
    // middle of a take, and an encoder fed a buffer of a size it was not opened
    // for reads it at the old one -- so this is checked every frame.
    uint32_t gradeWidth = 0, gradeHeight = 0;

    // The tally, raised by the presenter and sent by the message loop. -1 means
    // there is nothing to send.
    int tallyPending = -1;

    // The angle the tally has just come off, when a cut has happened. Its light
    // has to be put out as deliberately as the new one's is lit: a phone whose
    // tally nobody cleared goes on telling the person in front of it that it is
    // being seen. -1 means there is nobody to darken.
    int tallyCutFrom = -1;

    // The microphone on this machine, its encoder, and what it has produced
    // that the file has not taken yet.
    WasapiCapture deskMic;
    MfAacEncoder deskAac;
    std::mutex deskMicLock;
    std::deque<DeskAudioFrame> deskMicQueue;
    uint64_t deskMicFirstQpc = 0;
    int64_t deskMicAnchorUs = 0;

    // Auto-framing: the detector, the frame it last looked at, when it last
    // looked, and the command the presenter wants sent.
    FaceFinder faceFinder;
    std::vector<uint8_t> detectBuffer;
    double lastDetectAt = 0.0;
    bool framingActive = false;
    std::string framingPending;

    // A take the browser asked for, acted on outside the draw: fetching it
    // decides between adb and the link, and one of those starts a process.
    std::string fetchPending;

    // Raised by the panel, acted on by the message loop once the frame it was
    // raised in has finished drawing.
    bool lutDialogPending = false;
    bool prompterDialogPending = false;

    // When the prompter was last moved on, so it scrolls by wall time.
    double lastPrompterTick = 0.0;
    bool folderDialogPending = false;

    // Set when the connection settings changed, so the worker drops what it has
    // and looks again rather than waiting for the current attempt to time out.
    std::atomic<bool> reconnect{false};

    // Set by the decoder when a new frame has been copied in, cleared by the
    // presenter. A frame that arrives while the previous one is still being
    // shown simply replaces it: dropping on the display side is free, whereas
    // making the socket wait is what causes runaway latency.
    std::atomic<bool> frameReady{false};

    std::string adbPath;
    std::string serial;

    Settings settings;
    DeviceDiscovery discovery;

    // Where to connect. Empty means USB: find a phone on adb, put up the
    // forward, and talk to the local end of it. A host given on the command
    // line means Wi-Fi, and adb is left out of it entirely -- there may not be
    // a cable at all.
    std::string host;

    BitrateGovernor governor;

    // An explicit format on the command line outranks the automatic choice;
    // otherwise asking for 4K and silently getting 1080p would be baffling.
    bool formatFromCommandLine = false;

    // Set by --minimised, which is what the Run entry passes. Starting with
    // Windows and then taking the screen would be worse than not starting.
    bool startMinimised = false;
};

AppState* g_app = nullptr;

double UnixSeconds() {
    return static_cast<double>(std::time(nullptr));
}

double NowSeconds() {
    using clock = std::chrono::steady_clock;
    static const auto start = clock::now();
    return std::chrono::duration<double>(clock::now() - start).count();
}

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                      nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

ID3D11Device* CreateDevice() {
    ID3D11Device* device = nullptr;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};

    if (FAILED(D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
            levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &device, nullptr, nullptr))) {
        return nullptr;
    }

    // The decoder writes textures on the worker thread while the swap chain is
    // resized from the UI thread; without this D3D makes no promises.
    ID3D10Multithread* mt = nullptr;
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&mt)))) {
        mt->SetMultithreadProtected(TRUE);
        mt->Release();
    }
    return device;
}

// Applies what the phone reports it actually did. Clamping arrives here, so the
// panel shows the truth rather than the request.
void ApplyAck(CameraModel& model, const std::string& payload) {
    Ack ack;
    if (!ParseAck(payload, ack) || ack.appliedJson.empty()) return;

    double value = 0;
    if (ack.cmd == "set") {
        // The phone lowers the frame rate when the configured recording size
        // cannot be driven any faster. The panel has to follow, or it shows a
        // rate the stream stopped running at.
        if (ReadAppliedNumber(ack.appliedJson, "fps", value) && value > 0) {
            model.fps = static_cast<int>(value);
        }
    } else if (ack.cmd == "exposure") {
        if (ReadAppliedNumber(ack.appliedJson, "iso", value)) {
            model.iso = static_cast<int>(value);
        }
        if (ReadAppliedNumber(ack.appliedJson, "shutterNs", value) && value > 0) {
            const int64_t reported = static_cast<int64_t>(value);
            model.shutterClamped = reported < model.shutterNs;
            model.shutterNs = reported;
        }
    } else if (ack.cmd == "focus") {
        if (ReadAppliedNumber(ack.appliedJson, "distance", value)) {
            model.focusDistance = static_cast<float>(value);
        }
    } else if (ack.cmd == "wb") {
        if (ReadAppliedNumber(ack.appliedJson, "temperature", value)) {
            model.wbKelvin = static_cast<int>(value);
        }
    } else if (ack.cmd == "audio") {
        // The phone reports whether sound is actually running, which is not the
        // same as what was asked for: a permission the user has revoked comes
        // back as off rather than as an error.
        model.micEnabled = ack.appliedJson.find("\"enabled\":true") != std::string::npos;
    } else if (!ack.ok && (ack.cmd == "framing" || ack.cmd == "ramp")) {
        // The phone refused something this side is driving continuously. Turn it
        // off rather than repeat it: a feature that does nothing while the log
        // fills with the same refusal is the worst of both.
        if (ack.cmd == "framing") {
            model.autoFrame = false;
            model.frameX = model.frameY = 0.0f;
            model.frameW = model.frameH = 1.0f;
        }
        XCAM_LOG_WARN("the phone refused %s: %s -- switching it off",
                      ack.cmd.c_str(), ack.error.c_str());
    } else if (ack.cmd == "ramp") {
        // Shown while it runs so the marks light up: a move nobody can see
        // starting looks like a button that did nothing.
        model.rampingFocus = ack.ok &&
                             ack.appliedJson.find("\"cancelled\"") == std::string::npos;
        model.rampEndsAt = model.rampingFocus ? NowSeconds() + model.rampMs / 1000.0 : 0.0;
    } else if (ack.cmd == "takes") {
        model.takesPending = false;

        std::vector<TakeInfo> takes;
        std::string dir;
        if (ParseTakes(ack.appliedJson, takes, dir)) {
            model.takes = std::move(takes);
            model.takesDir = dir;
            model.takesError.clear();
        }

        std::string name;
        if (ReadAppliedString(ack.appliedJson, "fetching", name)) {
            model.fetchName = name;
            model.fetchReceived = 0;
            if (ReadAppliedNumber(ack.appliedJson, "bytes", value)) {
                model.fetchBytes = static_cast<int64_t>(value);
            }
        }
        if (ReadAppliedString(ack.appliedJson, "deleted", name)) {
            // Dropped here rather than by asking again: the phone has already
            // done it, and a second round trip to learn what we were just told
            // would leave the row on screen for the length of it.
            model.takes.erase(
                std::remove_if(model.takes.begin(), model.takes.end(),
                               [&](const TakeInfo& t) { return t.name == name; }),
                model.takes.end());
        }
    } else if (ack.cmd == "record") {
        // The phone picks the recording size when asked for "the camera's
        // best", and snaps the frame rate to one that divides the streaming
        // rate. Both come back here, so the panel shows the file that is
        // actually being written rather than the one that was requested.
        if (ReadAppliedNumber(ack.appliedJson, "width", value)) {
            model.recordWidth = static_cast<int>(value);
        }
        if (ReadAppliedNumber(ack.appliedJson, "height", value)) {
            model.recordHeight = static_cast<int>(value);
        }
        if (ReadAppliedNumber(ack.appliedJson, "fps", value)) {
            model.recordFps = static_cast<int>(value);
        }
        ReadAppliedString(ack.appliedJson, "file", model.recordFile);
        model.recordEnabled = ack.appliedJson.find("\"enabled\":false") == std::string::npos;
        model.recordToPc = ack.appliedJson.find("\"target\":\"phone\"") == std::string::npos;

        // Granted rather than obeyed: the ring lives in the phone's heap at a
        // bitrate chosen for a file, so twenty seconds asked for can come back
        // as six. The panel shows what was granted, not what was requested.
        if (ReadAppliedNumber(ack.appliedJson, "preroll", value)) {
            model.preRollGranted = static_cast<int>(value);
        } else {
            // No field at all is not the same as a field saying zero. A phone
            // built before pre-roll existed answers this way, and reporting it
            // as "granted nothing" would have the panel explain a shortage of
            // memory that is not the reason.
            model.preRollGranted = -1;
        }

        // A larger recording size can pull the streaming rate down with it,
        // since one sensor drives both.
        if (ReadAppliedNumber(ack.appliedJson, "streamFps", value) && value > 0) {
            model.fps = static_cast<int>(value);
        }
        // "state" is authoritative the moment it arrives; STATS then keeps the
        // elapsed time moving.
        const bool wasRecording = model.recording;
        model.recording = ack.appliedJson.find("\"state\":\"recording\"") != std::string::npos;

        if (!wasRecording && model.recording) model.takeStartedAt = UnixSeconds();

        // A take that just ended is a file the listing has never seen, and the
        // one moment everything needed to describe it is still in hand.
        if (wasRecording && !model.recording) {
            model.takesStale = true;
            // Named after the file that exists here, not after the take.
            //
            // The phone names its own take from its own clock, and a recording
            // written on this side gets a name from this one -- a second or two
            // apart. Naming the sidecar after the phone's take left it sitting
            // beside nothing, describing a file whose name it did not share.
            model.describePending = model.recordToPc && !model.recordLocalPath.empty()
                                        ? model.recordLocalPath
                                        : model.recordFile;
            if (ReadAppliedNumber(ack.appliedJson, "durationMs", value)) {
                model.describeMs = static_cast<int64_t>(value);
            }
        }
        if (!model.recording) {
            model.recordMs = 0;
            model.recordBytes = 0;
        }
    }
}

// Picks a sane starting format from what the camera actually offers: 1080p at
// the highest rate it can sustain, falling back to the largest mode available.
void ChooseDefaultFormat(CameraModel& model) {
    const auto modes = model.PreferredModes();
    if (modes.empty()) return;

    const CaptureMode* chosen = nullptr;
    for (const CaptureMode& mode : modes) {
        if (mode.height == 1080) { chosen = &mode; break; }
    }
    if (!chosen) chosen = &modes.front();

    model.width = chosen->width;
    model.height = chosen->height;
    model.fps = (std::min)(60, chosen->maxFps);
}

// ---- remembering ------------------------------------------------------------
//
// Everything a person chose, kept for the next run. The alternative is dialling
// in ISO, shutter, white balance, a LUT and a recording format every single
// time, which for a camera is not a small annoyance -- those settings are the
// work.
//
// Settings::Set marks the file dirty only when a value actually changes, so
// this can be called every second and writes only when there is something to
// write.

void StoreSettings(AppState& app) {
    Angle& angle = app.Program();

    Settings& s = app.settings;
    const CameraModel& m = angle.model;

    s.Set("format.width", m.width);
    s.Set("format.height", m.height);
    s.Set("format.fps", m.fps);
    s.Set("format.bitrate", m.bitrate);
    s.Set("format.codec", m.codec);
    s.Set("camera.index", static_cast<int>(m.cameraIndex));

    s.Set("picture.exposureManual", m.exposureMode == ExposureMode::Manual);
    s.Set("picture.iso", m.iso);
    s.Set("picture.shutterNs", static_cast<int>(m.shutterNs));
    s.Set("picture.focusManual", m.focusMode == FocusMode::Manual);
    s.Set("picture.focusDistance", m.focusDistance);
    s.Set("picture.wbMode", m.wbMode);
    s.Set("picture.wbKelvin", m.wbKelvin);
    s.Set("picture.ev", m.ev);
    s.Set("picture.logProfile", m.logProfile);
    s.Set("picture.zoom", m.zoom);
    s.Set("picture.lut", app.shared.lutPath);

    s.Set("record.enabled", m.recordEnabled);
    s.Set("record.toPc", m.recordToPc);
    s.Set("record.width", m.recordWantWidth);
    s.Set("record.height", m.recordWantHeight);
    s.Set("record.fps", m.recordFps);
    s.Set("record.codec", m.recordCodec);

    s.Set("mic.enabled", m.micEnabled);
    s.Set("record.folder", app.shared.recordFolder);
    s.Set("record.interval", m.recordInterval);
    s.Set("record.graded", app.shared.recordGraded);
    s.Set("look.matte", app.shared.matte);
    s.Set("out.shape", app.shared.shape == Shape::Vertical ? "vertical"
                     : app.shared.shape == Shape::Square   ? "square"
                                                           : "wide");
    s.Set("out.autoRotate", app.shared.autoRotate);
    s.Set("prompter.path", app.shared.prompterPath);
    s.Set("prompter.speed", app.shared.prompterSpeed);
    s.Set("prompter.size", app.shared.prompterSize);
    s.Set("prompter.mirror", app.shared.prompterMirror);
    s.Set("record.preroll", m.preRoll);
    s.Set("look.flipX", m.flipX);
    s.Set("look.flipY", m.flipY);
    s.Set("focus.markA", m.focusA);
    s.Set("focus.markB", m.focusB);
    s.Set("focus.rampMs", m.rampMs);
    s.Set("frame.auto", m.autoFrame);
    s.Set("look.zebra", app.shared.zebra);
    s.Set("look.peaking", app.shared.peaking);
    s.Set("look.gain", app.shared.gain);
    s.Set("look.contrast", app.shared.contrast);
    s.Set("look.saturation", app.shared.saturation);
    s.Set("look.warmth", app.shared.warmth);
    // Saved at last. It has been in the model since the LUT arrived and was
    // written nowhere, so a grade dialled back to half came back at full.
    s.Set("look.lutAmount", app.shared.lutAmount);
    s.Set("audio.deskMic", app.shared.deskMic);
    s.Set("audio.deskMicId", app.shared.deskMicId);
    s.Set("net.transport", app.shared.transport == Transport::Usb  ? "usb"
                         : app.shared.transport == Transport::WiFi ? "wifi"
                                                          : "auto");
    if (!app.shared.host.empty()) s.Set("net.host", app.shared.host);
    if (!app.shared.pairCode.empty()) s.Set("net.pairCode", app.shared.pairCode);
    s.Set("ui.language", Strings::Current() == Lang::Turkish ? "tr" : "en");

    for (size_t i = 0; i < kPresetSlots; ++i) {
        const Preset& p = app.shared.presets[i];
        const std::string key = "preset." + std::to_string(i) + ".";
        s.Set(key + "name", p.name);
        if (p.Empty()) continue;
        s.Set(key + "exposureManual", p.exposureManual);
        s.Set(key + "iso", p.iso);
        s.Set(key + "shutterNs", static_cast<int>(p.shutterNs));
        s.Set(key + "focusManual", p.focusManual);
        s.Set(key + "focusDistance", p.focusDistance);
        s.Set(key + "wbMode", p.wbMode);
        s.Set(key + "wbKelvin", p.wbKelvin);
        s.Set(key + "ev", p.ev);
        s.Set(key + "logProfile", p.logProfile);
        s.Set(key + "zoom", p.zoom);
        s.Set(key + "lut", p.lutPath);
        s.Set(key + "matte", p.matte);
    }
    s.Set("virtualCamera", app.shared.virtualCamera);
    s.Set("ui.pro", app.panel.IsPro());
    s.Set("ui.proTab", app.panel.ProTab());

    // The address that worked last time, so a Wi-Fi setup does not need
    // --host on every launch. Only written when Wi-Fi was actually used;
    // over USB there is nothing to remember.
    if (!app.host.empty()) s.Set("net.host", app.host);
}

// Applies what was remembered. Called before the first connection, so the very
// first `set` already carries the format the person last chose rather than a
// default they would have to correct.
void ApplySettings(AppState& app) {
    Angle& angle = app.Program();

    const Settings& s = app.settings;
    CameraModel& m = angle.model;

    m.width = s.GetInt("format.width", m.width);
    m.height = s.GetInt("format.height", m.height);
    m.fps = s.GetInt("format.fps", m.fps);
    m.bitrate = s.GetInt("format.bitrate", m.bitrate);
    m.codec = s.GetString("format.codec", m.codec);
    m.cameraIndex = static_cast<size_t>(s.GetInt("camera.index", 0));

    m.exposureMode = s.GetBool("picture.exposureManual", false) ? ExposureMode::Manual
                                                                : ExposureMode::Auto;
    m.iso = s.GetInt("picture.iso", m.iso);
    m.shutterNs = s.GetInt("picture.shutterNs", static_cast<int>(m.shutterNs));
    m.focusMode = s.GetBool("picture.focusManual", false) ? FocusMode::Manual
                                                          : FocusMode::Continuous;
    m.focusDistance = s.GetFloat("picture.focusDistance", m.focusDistance);
    m.wbMode = s.GetString("picture.wbMode", m.wbMode);
    m.wbKelvin = s.GetInt("picture.wbKelvin", m.wbKelvin);
    m.ev = s.GetFloat("picture.ev", m.ev);
    m.logProfile = s.GetBool("picture.logProfile", false);
    m.zoom = s.GetFloat("picture.zoom", 1.0f);
    app.shared.lutPath = s.GetString("picture.lut");

    m.recordEnabled = s.GetBool("record.enabled", true);
    m.recordToPc = s.GetBool("record.toPc", true);
    m.recordWantWidth = s.GetInt("record.width", 0);
    m.recordWantHeight = s.GetInt("record.height", 0);
    m.recordFps = s.GetInt("record.fps", 0);
    m.recordCodec = s.GetString("record.codec", m.recordCodec);

    m.micEnabled = s.GetBool("mic.enabled", true);
    app.shared.recordFolder = s.GetString("record.folder");
    m.recordInterval = (std::max)(1, s.GetInt("record.interval", 1));
    app.shared.recordGraded = s.GetBool("record.graded", false);
    app.shared.matte = s.GetFloat("look.matte", 0.0f);
    const std::string shape = s.GetString("out.shape", "wide");
    app.shared.shape = shape == "vertical" ? Shape::Vertical
                     : shape == "square"   ? Shape::Square
                                           : Shape::Wide;
    app.shared.autoRotate = s.GetBool("out.autoRotate", true);
    app.shared.prompterPath = s.GetString("prompter.path");
    app.shared.prompterSpeed = s.GetFloat("prompter.speed", 40.0f);
    app.shared.prompterSize = s.GetFloat("prompter.size", 34.0f);
    app.shared.prompterMirror = s.GetBool("prompter.mirror", false);
    SizeOf(app.shared.shape, app.shared.virtualCameraWidth,
           app.shared.virtualCameraHeight);
    m.preRoll = (std::max)(0, s.GetInt("record.preroll", 0));
    m.flipX = s.GetBool("look.flipX", false);
    m.flipY = s.GetBool("look.flipY", false);
    m.focusA = s.GetFloat("focus.markA", -1.0f);
    m.focusB = s.GetFloat("focus.markB", -1.0f);
    m.rampMs = (std::max)(250, s.GetInt("focus.rampMs", 2000));
    m.autoFrame = s.GetBool("frame.auto", false);
    app.shared.zebra = s.GetFloat("look.zebra", 0.0f);
    app.shared.peaking = s.GetFloat("look.peaking", 0.0f);
    app.shared.gain = s.GetFloat("look.gain", 0.0f);
    app.shared.contrast = s.GetFloat("look.contrast", 0.0f);
    app.shared.saturation = s.GetFloat("look.saturation", 0.0f);
    app.shared.warmth = s.GetFloat("look.warmth", 0.0f);
    app.shared.lutAmount = s.GetFloat("look.lutAmount", 1.0f);
    app.shared.deskMic = s.GetBool("audio.deskMic", false);
    app.shared.deskMicId = s.GetString("audio.deskMicId");

    const std::string transport = s.GetString("net.transport", "auto");
    app.shared.transport = transport == "usb"  ? Transport::Usb
                : transport == "wifi" ? Transport::WiFi
                                      : Transport::Auto;

    // Asked once, on a first run, and never again.
    //
    // The key's presence is the record of having been asked -- not its value.
    // A previous answer of "decide for me" stores "auto", which is
    // indistinguishable from the default if you go by the value, and somebody
    // who said that once should not be asked every morning.
    if (s.Has("net.transport")) app.connectPanel.Close();
    app.shared.host = s.GetString("net.host");
    app.shared.pairCode = s.GetString("net.pairCode");

    // A stored language beats the system one; the system one is only the first
    // guess, made before anybody has said otherwise.
    if (s.Has("ui.language")) {
        Strings::Set(s.GetString("ui.language") == "tr" ? Lang::Turkish : Lang::English);
    }

    for (size_t i = 0; i < kPresetSlots; ++i) {
        Preset& p = app.shared.presets[i];
        const std::string key = "preset." + std::to_string(i) + ".";
        p.name = s.GetString(key + "name");
        if (p.Empty()) continue;
        p.exposureManual = s.GetBool(key + "exposureManual", false);
        p.iso = s.GetInt(key + "iso", 400);
        p.shutterNs = s.GetInt(key + "shutterNs", 16'666'666);
        p.focusManual = s.GetBool(key + "focusManual", false);
        p.focusDistance = s.GetFloat(key + "focusDistance", 0.0f);
        p.wbMode = s.GetString(key + "wbMode", "auto");
        p.wbKelvin = s.GetInt(key + "wbKelvin", 5600);
        p.ev = s.GetFloat(key + "ev", 0.0f);
        p.logProfile = s.GetBool(key + "logProfile", false);
        p.zoom = s.GetFloat(key + "zoom", 1.0f);
        p.lutPath = s.GetString(key + "lut");
        p.matte = s.GetFloat(key + "matte", 0.0f);
    }
    app.shared.virtualCamera = s.GetBool("virtualCamera", true);

    // Off by default, so a first run is the short panel. Somebody who wants
    // every control gets it back next time without asking again.
    app.panel.SetPro(s.GetBool("ui.pro", false));
    app.panel.SetProTab(s.GetInt("ui.proTab", 0));

    // A format restored from a file counts as chosen, so the automatic pick
    // does not quietly overwrite it when the handshake arrives.
    if (s.Has("format.width")) app.formatFromCommandLine = true;
}

// Re-asserts everything the picture depends on.
//
// A pipeline restart -- any change of resolution, frame rate, codec or camera --
// rebuilds the capture request on the phone from the template defaults, so
// manual exposure, focus, white balance and the picture profile are all lost.
// The panel goes on showing the values it last set, which is how the same
// numbers end up producing two visibly different pictures.
void SendPictureState(AppState& app, Angle& angle) {
    (void)app;
    const CameraModel& model = angle.model;

    if (model.exposureMode == ExposureMode::Manual) {
        angle.link.client.SendControl(MakeExposureCommand(true, model.iso, model.shutterNs));
    } else if (model.ev != 0.0f) {
        angle.link.client.SendControl(MakeEvCommand(model.ev));
    }

    if (model.focusMode == FocusMode::Manual) {
        angle.link.client.SendControl(MakeFocusCommand("manual", model.focusDistance));
    }

    if (model.wbMode == "manual") {
        angle.link.client.SendControl(MakeWhiteBalanceCommand("manual", model.wbKelvin));
    } else if (model.wbMode != "auto") {
        angle.link.client.SendControl(MakeWhiteBalanceCommand(model.wbMode, 0));
    }

    // The profile goes last: it drives the tonemap, and re-asserting exposure
    // afterwards would be harmless but re-asserting it before is not, since a
    // profile change rewrites the colour correction the white balance just set.
    if (model.logProfile) {
        angle.link.client.SendControl(MakePictureProfileCommand(true));
    }

    if (model.zoom != 1.0f) angle.link.client.SendControl(MakeZoomCommand(model.zoom));
    if (model.torch) angle.link.client.SendControl(MakeTorchCommand(true));

    XCAM_LOG_INFO("re-applied picture state after a pipeline restart");
}

void SendFormat(AppState& app, Angle& angle) {
    (void)app;
    // A format change re-states the chosen bitrate, so the governor starts
    // again from the target rather than from wherever it had backed off to.
    angle.link.governor.SetTarget(angle.model.bitrate);
    angle.model.activeBitrate = angle.model.bitrate;
    angle.model.bitrateLimited = false;

    const CameraInfo* camera = angle.model.Camera();
    angle.link.client.SendControl(MakeSetCommand(camera ? camera->id : "",
                                          angle.model.width, angle.model.height,
                                          angle.model.fps, angle.model.bitrate, angle.model.codec));
}

// Asks for a .cube and loads it. Runs on the UI thread, where a modal dialog
// belongs; the panel only raises the request.
void LoadLutInteractively(AppState& app, HWND window) {
    wchar_t path[MAX_PATH] = L"";

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = window;
    // Null-separated and double-null terminated -- the one Win32 API that
    // still asks for a string built this way.
    ofn.lpstrFilter = L"Cube LUT (*.cube)\0*.cube\0All files (*.*)\0*.*\0\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"Load a .cube LUT";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (!GetOpenFileNameW(&ofn)) return;

    char narrow[MAX_PATH * 2] = "";
    WideCharToMultiByte(CP_UTF8, 0, path, -1, narrow, sizeof(narrow), nullptr, nullptr);

    CubeLut lut;
    std::string error;
    if (!LoadCubeLut(narrow, lut, error)) {
        XCAM_LOG_ERROR("LUT load failed: %s", error.c_str());
        MessageBoxW(window, Widen(error).c_str(), L"XCam", MB_ICONWARNING);
        return;
    }

    if (!app.renderer.SetLut(lut)) {
        XCAM_LOG_ERROR("LUT upload failed: %s", app.renderer.LastError().c_str());
        return;
    }

    // The file name is what the button shows, so keep it short enough to fit.
    std::string name = narrow;
    const size_t slash = name.find_last_of("\\/");
    if (slash != std::string::npos) name = name.substr(slash + 1);
    // The chip shows this beside a lamp, so it has less room than it did when
    // the label read "LOAD LUT". The extension carries no information here.
    const size_t dot = name.find_last_of('.');
    if (dot != std::string::npos) name = name.substr(0, dot);
    if (name.size() > 11) name = name.substr(0, 10) + "…";

    app.shared.lutName = name;
    app.shared.lutPath = narrow;
    XCAM_LOG_INFO("LUT loaded: %s (%dx%dx%d)%s%s", narrow, lut.size, lut.size, lut.size,
                  lut.title.empty() ? "" : " -- ", lut.title.c_str());
}

// Asks for a folder. IFileDialog with FOS_PICKFOLDERS rather than the old
// SHBrowseForFolder, which still shows the tree from Windows 2000 and cannot be
// typed into.
// Brings one take off the phone.
//
// Two ways, and which one is available decides. Over USB, `adb pull` moves a
// gigabyte in seconds and never touches the stream; over Wi-Fi there is no adb,
// so the phone sends the file through the same socket as the picture -- paced
// to run only while that socket is idle, which makes it slow and harmless
// rather than fast and disruptive.
//
// Runs off the message thread either way: one of these spawns a process and the
// other waits on a network, and a window that stops repainting while a file
// copies is a window that looks crashed.
// Reads a script off disk into the model. Shared by the file dialog and by
// startup, so a script that was loaded last time comes back with the path that
// was remembered rather than only the name of a file nobody read.
bool ReadScript(AppState& app, const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;

    std::string text((std::istreambuf_iterator<char>(file)),
                     std::istreambuf_iterator<char>());

    // A UTF-8 byte order mark would draw as three characters at the top of the
    // script, which is the first thing anybody would read out loud.
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF) {
        text.erase(0, 3);
    }
    // Windows line endings would each leave a stray glyph in DirectWrite.
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());

    std::lock_guard<std::mutex> guard(app.lock);
    app.shared.prompterPath = path;
    app.shared.prompterText = std::move(text);
    app.shared.prompterOffset = 0.0f;
    app.shared.prompterRunning = false;
    return true;
}

// Asks for a script and reads it in. Modelled on the LUT loader above, and on
// the same thread for the same reason: a modal dialog belongs on the one that
// owns the window.
void LoadPrompterScript(AppState& app, HWND window) {
    wchar_t path[MAX_PATH] = L"";

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = window;
    ofn.lpstrFilter = L"Text (*.txt)\0*.txt\0All files (*.*)\0*.*\0\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"Load a script";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (!GetOpenFileNameW(&ofn)) return;

    char narrow[MAX_PATH * 2] = "";
    WideCharToMultiByte(CP_UTF8, 0, path, -1, narrow, sizeof(narrow), nullptr, nullptr);

    if (!ReadScript(app, narrow)) {
        XCAM_LOG_ERROR("could not open the script %s", narrow);
        return;
    }
    {
        std::lock_guard<std::mutex> guard(app.lock);
        app.shared.prompterOn = true;
    }
    XCAM_LOG_INFO("script loaded: %s (%zu bytes)", narrow, app.shared.prompterText.size());
}

void StartTakeFetch(AppState& app, const std::string& name) {
    Angle& angle = app.Program();

    std::string remoteDir, localDir, adbPath, serial;
    int64_t bytes = 0;
    {
        std::lock_guard<std::mutex> guard(app.lock);
        if (angle.model.Fetching()) return;

        for (const TakeInfo& take : angle.model.takes) {
            if (take.name == name) bytes = take.bytes;
        }
        remoteDir = angle.model.takesDir;
        localDir = app.shared.recordFolder;
        adbPath = app.adbPath;
        serial = angle.link.serial;
    }

    if (localDir.empty()) localDir = Mp4Writer::DefaultDirectory();
    const std::string local = localDir.empty() ? name : localDir + "\\" + name;

    // adb only when there is actually a phone on the cable. Over Wi-Fi the
    // path exists and the device does not, and adb would fail slowly.
    const bool overUsb = !adbPath.empty() && !serial.empty() && !remoteDir.empty();

    {
        std::lock_guard<std::mutex> guard(app.lock);
        angle.model.fetchName = name;
        angle.model.fetchLocalPath = local;
        angle.model.fetchBytes = bytes;
        angle.model.fetchReceived = 0;
        angle.model.takesError.clear();
    }

    if (!overUsb) {
        // The phone answers with an ACK naming the file, then sends it as FILE
        // packets. Everything else is handled where those arrive.
        XCAM_LOG_INFO("fetching %s over the link", name.c_str());
        angle.link.client.SendControl(MakeTakesCommand("fetch", name));
        return;
    }

    XCAM_LOG_INFO("pulling %s with adb", name.c_str());
    std::thread([&app, target = &angle, adbPath, serial, remoteDir, name, local] {
        Angle& angle = *target;
        const std::string remote = remoteDir + "/" + name;

        // Progress comes from the file itself. adb prints its own, but only to
        // a pipe read after it exits, which is a progress bar that arrives once
        // the wait is over.
        std::atomic<bool> done{false};
        std::thread watcher([&app, &angle, &done, local] {
            while (!done.load()) {
                std::error_code ec;
                const auto size = std::filesystem::file_size(local, ec);
                if (!ec) {
                    std::lock_guard<std::mutex> guard(app.lock);
                    angle.model.fetchReceived = static_cast<int64_t>(size);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
        });

        std::string output;
        const bool ok = RunAdb(adbPath, {"-s", serial, "pull", remote, local}, output);

        done = true;
        watcher.join();

        std::lock_guard<std::mutex> guard(app.lock);
        angle.model.fetchName.clear();
        angle.model.fetchBytes = 0;
        angle.model.fetchReceived = 0;
        if (ok) {
            XCAM_LOG_INFO("pulled %s", local.c_str());
        } else {
            XCAM_LOG_ERROR("adb pull failed: %s", output.c_str());
            angle.model.takesError = "adb pull failed";
        }
    }).detach();
}

void ShowFolderDialog(AppState& app, HWND window);

// The folder picker, on a thread of its own.
//
// IFileDialog is a shell object and the shell wants an apartment-threaded
// caller. This process initialises COM as multi-threaded -- everything else it
// does with COM is happier that way -- and calling Show from here deadlocks
// against the shell's own apartment. The window stops repainting the instant
// the row is clicked, which is exactly what it looks like: an application that
// froze when you asked it where to put recordings.
//
// So the dialog gets an STA thread. It is detached rather than waited on: the
// owner window is disabled by Windows for as long as the dialog is up, which is
// the modality anyone expects, while this side keeps drawing.
void ChooseRecordingFolder(AppState& app, HWND window) {
    std::thread([&app, window] {
        // Apartment-threaded, and only for this thread.
        if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return;
        ShowFolderDialog(app, window);
        CoUninitialize();
    }).detach();
}

void ShowFolderDialog(AppState& app, HWND window) {
    IFileDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dialog)))) {
        return;
    }

    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM);
    dialog->SetTitle(L"Where should recordings go?");

    if (SUCCEEDED(dialog->Show(window))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR wide = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &wide))) {
                char narrow[MAX_PATH * 2] = "";
                WideCharToMultiByte(CP_UTF8, 0, wide, -1, narrow, sizeof(narrow),
                                    nullptr, nullptr);
                std::lock_guard<std::mutex> guard(app.lock);
                app.shared.recordFolder = narrow;
                XCAM_LOG_INFO("recordings will go to %s", narrow);
                CoTaskMemFree(wide);
            }
            item->Release();
        }
    }
    dialog->Release();
}

// Draws the most recently uploaded frame with the panel over it. Runs on the UI
// thread only, so nothing here can stall the socket. Must hold AppState::lock.
// Defined below, beside the virtual camera's readback: both belong on the
// presenting thread, and this one is written where its neighbours are.
void PumpGradedRecording(AppState& app);
void PumpAutoFraming(AppState& app);
void DrainDeskMic(AppState& app);
void StartDeskMic(AppState& app);
void StopDeskMic(AppState& app);

void PresentFrame(AppState& app) {
    Angle& angle = app.Program();

    const double now = NowSeconds();

    // The rig, as the panel needs to see it. Rebuilt each frame rather than
    // kept in step: an angle can appear at any moment, and a list maintained by
    // whoever remembered to is a list that goes stale.
    //
    // No lock taken here. Both callers already hold it -- this runs inside the
    // message loop's guard -- and taking it again is not a wait but a throw,
    // which is what a plain std::mutex does when the thread that owns it asks
    // for it a second time.
    app.shared.rig.clear();
    for (const Angle& each : app.angles) {
        AngleSummary summary;
        summary.label = each.model.device.deviceName.empty()
                            ? "CAM " + std::to_string(each.index + 1)
                            : each.model.device.deviceName;
        summary.connected = each.model.connected;
        summary.hasPicture = app.renderer.HasPicture(each.index);
        app.shared.rig.push_back(std::move(summary));
    }

    // Said once per change rather than per frame. Which angles exist, and which
    // of them could actually be cut to, is the first thing anybody debugging a
    // rig wants and the last thing a preview window shows.
    {
        static std::string reported;
        std::string state;
        for (const AngleSummary& each : app.shared.rig) {
            state += each.label + (each.hasPicture ? "[picture]" : "[waiting]") +
                     (each.connected ? "[up] " : "[down] ");
        }
        if (state != reported) {
            reported = state;
            XCAM_LOG_INFO("rig: %s", state.c_str());
        }
    }

    auto overlay = [&app, &angle, now]() {
        ID2D1DeviceContext* d2d = app.renderer.BeginOverlay();
        if (!d2d) return;

        const D2D1_SIZE_F size = d2d->GetSize();
        // The connect card does not fade.
        //
        // Everything drawn through UiContext is multiplied by this, and the
        // panel dims itself to nothing when the pointer has been still -- which
        // is right for controls floating over a picture and wrong for the one
        // card standing between somebody and a picture existing at all. It
        // faded out while they read it.
        const float opacity = app.connectPanel.IsOpen() ? 1.0f : app.panel.Opacity(now);
        app.ui.BeginFrame(app.input, size.width, size.height, opacity);

        // The prompter, under the panel and over the picture.
        //
        // Drawn before the controls so a chip is never hidden behind a line of
        // script, and drawn here rather than anywhere downstream because this
        // is the overlay: it lands on the back buffer after the video, while
        // the virtual camera and the recording read the decoded texture. That
        // is the whole guarantee that only the reader sees it.
        if (app.shared.prompterOn && app.shared.HasScript()) {
            const float column = (std::max)(320.0f, size.width * app.shared.prompterWidth);
            const float top = 70.0f;
            const float bottom = (std::max)(top + 80.0f, size.height - 190.0f);
            const D2D1_RECT_F band = D2D1::RectF((size.width - column) * 0.5f, top,
                                                 (size.width + column) * 0.5f, bottom);

            // A wash behind it. White text over an unknown picture is the same
            // legibility gamble the panel used to lose, and a script is the one
            // thing on screen somebody is reading word by word.
            app.ui.Rect(band, theme::Rgba(0x000000, 0.45f), 10.0f);

            app.shared.prompterExtent = app.ui.TextBlock(
                app.shared.prompterText, band, app.shared.prompterSize,
                theme::kText, app.shared.prompterOffset, app.shared.prompterMirror);

            // Stops at the end rather than scrolling the script off the top and
            // leaving an empty box.
            const float last = (std::max)(0.0f, app.shared.prompterExtent -
                                                    (band.bottom - band.top) * 0.5f);
            if (app.shared.prompterOffset >= last) {
                app.shared.prompterOffset = last;
                app.shared.prompterRunning = false;
            }
        }

        const ProPanel::Output out = app.panel.Draw(app.ui, angle.model, app.shared, now);

        // Drawn after the panel so it sits over it, and its own hit testing
        // then takes the pointer from anything underneath.
        const SettingsPanel::Output settingsOut =
            app.settingsPanel.Draw(app.ui, angle.model, app.shared, app.settings);
        for (const std::string& command : settingsOut.commands) {
            XCAM_LOG_INFO("control -> %s", command.c_str());
            angle.link.client.SendControl(command);
        }
        const TakesPanel::Output takesOut = app.takesPanel.Draw(app.ui, angle.model);
        for (const std::string& command : takesOut.commands) {
            XCAM_LOG_INFO("control -> %s", command.c_str());
            angle.link.client.SendControl(command);
        }
        if (!takesOut.fetch.empty()) app.fetchPending = takesOut.fetch;

        // Last, which is what makes it topmost.
        //
        // A hotspot claims the pointer for whichever widget drew it most
        // recently, so a card meant to block everything underneath has to be
        // the final thing drawn -- otherwise a click lands on the card and on
        // whatever chip happens to sit beneath it.
        if (app.connectPanel.IsOpen()) {
            const ConnectPanel::Output connectOut = app.connectPanel.Draw(
                app.ui, app.shared, app.discovery.Devices(), !app.adbPath.empty());
            if (connectOut.chose) {
                app.settings.Set("net.transport",
                                 app.shared.transport == Transport::Usb  ? "usb"
                               : app.shared.transport == Transport::WiFi ? "wifi"
                                                                         : "auto");
                if (!app.shared.host.empty()) app.settings.Set("net.host", app.shared.host);
                app.reconnect = true;
                angle.link.client.Disconnect();
                XCAM_LOG_INFO("transport chosen: %s",
                              app.shared.transport == Transport::Usb  ? "USB"
                            : app.shared.transport == Transport::WiFi ? "Wi-Fi"
                                                                      : "auto");
            }
            if (connectOut.openSettings) app.settingsPanel.Open();
        }

        if (settingsOut.openTakes) app.takesPanel.Open();
        if (settingsOut.autostartChanged) autostart::Set(app.shared.autostart);
        if (settingsOut.restartDeskMic && app.deskMic.IsRunning()) {
            StopDeskMic(app);
            StartDeskMic(app);
        }
        if (settingsOut.chooseFolder) app.folderDialogPending = true;
        if (settingsOut.reconnect) {
            app.reconnect = true;
            angle.link.client.Disconnect();
        }

        app.ui.EndFrame();
        app.input.EndFrame();
        app.renderer.EndOverlay();

        for (const std::string& command : out.commands) {
            XCAM_LOG_INFO("control -> %s", command.c_str());
            angle.link.client.SendControl(command);
        }

        // Deferred rather than handled here: a modal dialog inside the draw
        // would block the presenter mid-frame with the lock held.
        // The cut. Everything that follows the program follows from this one
        // line: the preview, the virtual camera's readback and the graded
        // encode all draw whichever slot the renderer is pointed at.
        if (out.cutTo >= 0 && static_cast<size_t>(out.cutTo) != app.shared.program &&
            static_cast<size_t>(out.cutTo) < app.angles.size()) {
            app.shared.program = static_cast<size_t>(out.cutTo);
            app.renderer.SetProgram(app.shared.program);

            // The tally follows the cut, and it has to be sent twice: the angle
            // coming off air needs its light put out as much as the one coming
            // on needs it lit. Raised rather than sent, like every other tally
            // -- a socket write under the presenter's lock froze the window
            // once already.
            app.tallyPending = app.shared.tally ? 1 : 0;
            app.tallyCutFrom = static_cast<int>(angle.index);
            XCAM_LOG_INFO("cut to angle %d", out.cutTo);
        }

        if (out.openSettings) app.settingsPanel.Open();
        if (out.openLutDialog) app.lutDialogPending = true;
        if (out.openPrompterFile) app.prompterDialogPending = true;
        if (out.clearLut) {
            app.renderer.ClearLut();
            app.shared.lutName.clear();
            app.shared.lutPath.clear();
            XCAM_LOG_INFO("LUT cleared");
        }
    };

    if (angle.model.rampingFocus && now >= angle.model.rampEndsAt) {
        angle.model.rampingFocus = false;
    }

    // Advanced by elapsed time rather than per frame, so a script reads at the
    // speed it says whatever the window is managing to draw at.
    if (app.shared.prompterRunning) {
        const double elapsed = app.lastPrompterTick > 0 ? now - app.lastPrompterTick : 0.0;
        if (elapsed > 0.0 && elapsed < 1.0) {
            app.shared.prompterOffset +=
                static_cast<float>(elapsed) * app.shared.prompterSpeed;
        }
    }
    app.lastPrompterTick = now;

    app.renderer.SetMatte(app.shared.matte);
    app.renderer.SetFlip(angle.model.flipX, angle.model.flipY);
    app.renderer.SetMonitorAids(app.shared.zebra, app.shared.peaking);
    app.renderer.SetGrade(app.shared.gain, app.shared.contrast, app.shared.saturation,
                          app.shared.warmth);
    // Pushed at last. SetLutAmount existed and was called from nowhere, so a
    // loaded LUT was always at full strength and the model's value did nothing.
    app.renderer.SetLutAmount(app.shared.lutAmount);
    app.renderer.SetShape(AspectOf(app.shared.shape), app.shared.RotationForShape());

    // The published size follows the shape. Re-opening is what tells a consumer
    // the picture changed shape; one that is already connected will not hear it,
    // which is the limitation the control warns about.
    uint32_t wantWidth = 0, wantHeight = 0;
    SizeOf(app.shared.shape, wantWidth, wantHeight);
    if (wantWidth != app.shared.virtualCameraWidth ||
        wantHeight != app.shared.virtualCameraHeight) {
        app.shared.virtualCameraWidth = wantWidth;
        app.shared.virtualCameraHeight = wantHeight;
        if (app.publisher.IsOpen()) {
            app.publisher.Close();
            XCAM_LOG_INFO("shape changed; republishing at %ux%u", wantWidth, wantHeight);
        }
    }

    // The desk microphone follows its switch here rather than at a take, since
    // the file needs its encoder's configuration before it opens.
    if (app.shared.deskMic && !app.deskMic.IsRunning()) StartDeskMic(app);
    else if (!app.shared.deskMic && app.deskMic.IsRunning()) StopDeskMic(app);

    if (app.deskMic.IsRunning()) {
        app.shared.deskMicPeak = app.deskMic.TakePeak();
        app.shared.deskMicHold =
            (std::max)(app.shared.deskMicPeak, app.shared.deskMicHold * 0.75f);
    }
    app.renderer.PresentLast(overlay);

    PumpGradedRecording(app);
    PumpAutoFraming(app);

    if (!app.shared.virtualCamera) {
        if (app.publisher.IsOpen()) {
            app.publisher.Close();
            XCAM_LOG_INFO("virtual camera stopped");
        }
        return;
    }

    // The published size is a setting and stays one.
    //
    // Closing used to zero it, and nothing ever put it back -- so switching the
    // webcam off and on again inside one session left the readback failing its
    // "target width is zero" guard for ever, and the camera never came back
    // until the application was restarted.

    const uint32_t width = app.shared.virtualCameraWidth;
    const uint32_t height = app.shared.virtualCameraHeight;

    // Through the shader, so what a call sees is what the preview shows: the
    // LUT, the colour, the mirror and the matte all arrive together instead of
    // the first two reaching only this window and the last two being written a
    // second time by hand below.
    uint32_t stride = 0;
    if (!app.renderer.PublishToNv12(width, height, app.publishBuffer, stride)) {
        // Once, not every frame: a broken path would otherwise fill the log at
        // sixty lines a second.
        static std::string reported;
        if (reported != app.renderer.LastError()) {
            reported = app.renderer.LastError();
            XCAM_LOG_WARN("virtual camera readback: %s", reported.c_str());
        }
        return;
    }

    if (!app.publisher.IsOpen()) {
        if (app.publisher.Open(width, height, static_cast<uint32_t>(angle.model.fps), 1)) {
            XCAM_LOG_INFO("virtual camera publishing %ux%u", width, height);
        } else {
            XCAM_LOG_ERROR("virtual camera: %s", app.publisher.LastError().c_str());
            app.shared.virtualCamera = false;
            return;
        }
    }

    // The mirror and the matte used to be re-implemented here, byte by byte, on
    // the NV12 buffer -- luma reversed one way, chroma reversed in pairs, and
    // two black bars memset over the rows. They are gone: this path goes
    // through the pixel shader now, which already did both for the preview.
    // Doing them twice would have cancelled the mirror out.

    app.publisher.Publish(app.publishBuffer.data(), stride,
                          static_cast<uint64_t>(now * 1e6));

    // The tally.
    //
    // "Connected" is not the question a person in front of the camera is asking;
    // "is anyone looking" is. The desktop can sit here all day with the filter
    // registered and nobody consuming it, and the phone has no way to tell the
    // difference -- except that a consumer reads frames continuously and marks
    // the section as it goes.
    //
    // A second of grace before it goes out: a consumer that misses one read
    // because its thread was descheduled has not stopped looking.
    const int64_t sinceRead = app.publisher.MillisecondsSinceRead();
    const bool live = sinceRead >= 0 && sinceRead < 1000;
    if (live != app.shared.tally) {
        app.shared.tally = live;
        // Raised, not sent.
        //
        // This runs inside the presenter with the model lock held, and a socket
        // write there blocks the window on the far end's willingness to read.
        // Every other command in this file goes out after the frame for exactly
        // that reason, and putting this one on the socket here froze the window
        // the first time the link went away underneath it.
        app.tallyPending = live ? 1 : 0;
        XCAM_LOG_INFO("tally %s", live ? "on" : "off");
    }
}

// Auto-framing.
//
// The detector looks at the decoded picture; the phone crops its sensor. Those
// are two different coordinate systems and the difference is the whole problem:
// a face at the centre-left of the *stream* is not at the centre-left of the
// *sensor* once a crop is already in force. So the crop this side asks for is
// composed with the crop already applied, rather than computed afresh from a
// picture that is itself the result of one -- which would send the framing
// walking a little further off every time it looked.
//
// Runs on the presenting thread, five times a second. Faster would find the same
// faces; the phone is easing towards the last answer for a good deal longer than
// 200ms anyway.
void PumpAutoFraming(AppState& app) {
    Angle& angle = app.Program();

    if (!angle.model.autoFrame) {
        if (app.framingActive) {
            app.framingActive = false;
            angle.model.frameX = 0.0f;
            angle.model.frameY = 0.0f;
            angle.model.frameW = 1.0f;
            angle.model.frameH = 1.0f;
            // Raised, not sent: this runs inside the presenter with the model
            // lock held, and a socket write there blocks the window on the far
            // end's willingness to read.
            app.framingPending = MakeFramingOffCommand();
            XCAM_LOG_INFO("auto-framing off; sensor handed back");
        }
        return;
    }

    const CameraInfo* camera = angle.model.Camera();
    if (!camera || camera->maxWidth <= 0 || camera->maxHeight <= 0) return;

    const double now = NowSeconds();
    if (now - app.lastDetectAt < 0.2) return;
    app.lastDetectAt = now;

    if (!app.faceFinder.IsOpen()) {
        if (!app.faceFinder.Open()) {
            XCAM_LOG_WARN("auto-framing unavailable: %s", app.faceFinder.LastError().c_str());
            angle.model.autoFrame = false;
            return;
        }
        XCAM_LOG_INFO("auto-framing ready");
    }

    // Its own readback, and deliberately the ungraded one.
    //
    // This used to borrow the virtual camera's buffer, which cost nothing while
    // that buffer was a plain copy of the decoded frame. It is the graded,
    // mirrored, matted picture now -- and a detector looking at a mirror sends
    // mirror-image framing to the phone, which moves the crop the wrong way.
    // ReadbackScaledNv12 is the hardware blit off the decoded texture with
    // nothing applied, which is exactly what a measurement wants. It runs five
    // times a second, so the second readback is cheap.
    // And deliberately a small one.
    //
    // This used to ask for the capture size, which on this phone is 3840x2160 --
    // twelve megabytes of NV12 pulled across the bus and run through the
    // detector five times a second, for an answer given back in fractions of the
    // frame. A face that is findable at 4K is findable at 640 wide, the readback
    // is a hardware blit that scales for free, and the detector's own cost falls
    // with the area. Nothing downstream changes: FaceBox is normalised.
    constexpr uint32_t kDetectWidth = 640;
    const uint32_t captureW = static_cast<uint32_t>(angle.model.width);
    const uint32_t captureH = static_cast<uint32_t>(angle.model.height);
    if (captureW == 0 || captureH == 0) return;

    const uint32_t width = (std::min)(captureW, kDetectWidth) & ~1u;
    const uint32_t height =
        static_cast<uint32_t>(static_cast<uint64_t>(width) * captureH / captureW) & ~1u;
    if (width == 0 || height == 0) return;

    uint32_t stride = 0;
    if (!app.renderer.ReadbackScaledNv12(width, height, app.detectBuffer, stride)) return;
    const uint8_t* luma = app.detectBuffer.data();

    std::vector<FaceBox> faces;
    if (!app.faceFinder.Detect(luma, stride, width, height, faces) || faces.empty()) {
        // Nothing found is not a reason to move. A framing that drifted home
        // every time someone turned their head would be worse than none.
        return;
    }

    // Everyone in shot, not just the nearest. A second person walking in should
    // widen the frame rather than be cropped out of it.
    float left = 1.0f, top = 1.0f, right = 0.0f, bottom = 0.0f;
    for (const FaceBox& face : faces) {
        left = (std::min)(left, face.x);
        top = (std::min)(top, face.y);
        right = (std::max)(right, face.x + face.w);
        bottom = (std::max)(bottom, face.y + face.h);
    }

    // Headroom above and body below: a face centred in the frame is a portrait
    // of a forehead. These are the proportions a camera operator uses without
    // thinking, and the reason the box is not simply the faces.
    const float faceH = bottom - top;
    const float centreX = (left + right) * 0.5f;
    const float centreY = top - faceH * 0.6f + (faceH * 2.4f) * 0.5f;

    // How much of the frame the group should fill.
    const float wanted = 0.34f;
    float scale = faceH > 0.0f ? (faceH * 2.4f) / wanted : 1.0f;
    scale = (std::max)(0.25f, (std::min)(1.0f, scale));

    // In sensor fractions, composed with the crop already in force.
    const float sensorAspect = static_cast<float>(camera->maxWidth) /
                               static_cast<float>(camera->maxHeight);
    const float outAspect = angle.model.height > 0
        ? static_cast<float>(angle.model.width) / static_cast<float>(angle.model.height)
        : sensorAspect;

    float w = angle.model.frameW * scale;
    float h = w * (sensorAspect / outAspect);
    if (h > 1.0f) { h = 1.0f; w = h * (outAspect / sensorAspect); }

    const float cx = angle.model.frameX + centreX * angle.model.frameW;
    const float cy = angle.model.frameY + centreY * angle.model.frameH;

    float x = (std::max)(0.0f, (std::min)(1.0f - w, cx - w * 0.5f));
    float y = (std::max)(0.0f, (std::min)(1.0f - h, cy - h * 0.5f));

    // Only when it has actually moved. Every command is a capture request
    // rebuilt on the phone, and asking for the framing it already has is a
    // wakeup for nothing -- while a threshold this side stops a detector that
    // wobbles by a pixel from becoming a camera that does.
    const float moved = std::fabs(x - angle.model.frameX) + std::fabs(y - angle.model.frameY) +
                        std::fabs(w - angle.model.frameW);
    if (moved < 0.01f) return;

    angle.model.frameX = x;
    angle.model.frameY = y;
    angle.model.frameW = w;
    angle.model.frameH = h;
    app.framingActive = true;
    app.framingPending = MakeFramingCommand(x, y, w, h);
}

// The microphone plugged into this machine.
//
// Started when it is switched on rather than when a take begins, for one
// reason: the file needs the encoder's AudioSpecificConfig *before* it opens,
// because a sink writer takes its streams before it starts and none after. An
// interface switched on halfway through a take cannot be added to that take.
//
// Samples are encoded on the capture thread and queued; the thread that owns
// the file drains them. Two threads writing to one sink writer is a race, and
// the capture thread is the one that must not wait.
void StartDeskMic(AppState& app) {
    if (app.deskMic.IsRunning()) return;

    if (!app.deskMic.Start(app.shared.deskMicId, [&app](const int16_t* samples,
                                                       size_t frames, uint64_t qpc) {
        // Anchored to this machine's clock at the first callback, and advanced
        // by the device's own timestamps after that.
        //
        // The two clocks are not mixed: QPC positions from the endpoint and
        // steady_clock have different origins, and subtracting one from the
        // other would be a number with no meaning. The difference between two
        // QPC positions does have one, so that is all that is used.
        if (app.deskMicFirstQpc == 0) {
            app.deskMicFirstQpc = qpc;
            app.deskMicAnchorUs = static_cast<int64_t>(NowSeconds() * 1e6);
        }
        const int64_t localUs =
            app.deskMicAnchorUs + static_cast<int64_t>((qpc - app.deskMicFirstQpc) / 10);

        app.deskAac.Encode(samples, frames, static_cast<uint64_t>(localUs),
                           [&app](const uint8_t* data, size_t bytes, uint64_t ptsUs) {
            std::lock_guard<std::mutex> guard(app.deskMicLock);
            // Bounded. If nothing is draining -- no take running -- this would
            // otherwise grow for as long as the microphone is switched on.
            if (app.deskMicQueue.size() > 500) app.deskMicQueue.pop_front();
            app.deskMicQueue.push_back({std::vector<uint8_t>(data, data + bytes), ptsUs});
        });
    })) {
        XCAM_LOG_WARN("no desk microphone: %s", app.deskMic.LastError().c_str());
        app.shared.deskMic = false;
        return;
    }

    if (!app.deskAac.Open(app.deskMic.SampleRate(), app.deskMic.Channels(), 192000)) {
        XCAM_LOG_ERROR("desk microphone cannot be encoded: %s",
                       app.deskAac.LastError().c_str());
        app.deskMic.Stop();
        app.shared.deskMic = false;
        return;
    }

    app.shared.deskMicName = app.deskMic.DeviceName();
    XCAM_LOG_INFO("desk microphone: %s at %u Hz, %u ch",
                  app.shared.deskMicName.c_str(), app.deskMic.SampleRate(),
                  app.deskMic.Channels());
}

void StopDeskMic(AppState& app) {
    if (!app.deskMic.IsRunning()) return;
    app.deskMic.Stop();
    app.deskAac.Close();
    app.deskMicFirstQpc = 0;
    {
        std::lock_guard<std::mutex> guard(app.deskMicLock);
        app.deskMicQueue.clear();
    }
    XCAM_LOG_INFO("desk microphone stopped");
}

// Everything queued, onto the second track.
//
// The timeline is the phone's, so each sample has to cross from this machine's
// clock to that one. The offset between them is measured rather than assumed:
// every recorded frame arrives with the phone's timestamp on it, and the
// difference from the moment it lands here is that offset plus however long the
// link took. Taking the *smallest* difference seen recently is taking the
// least-delayed packet, which is the closest thing to the truth the link
// offers -- and it is why this is a running minimum rather than an average.
void DrainDeskMic(AppState& app) {
    Angle& angle = app.Program();
    if (!angle.recording.IsOpen() || !angle.recording.HasAudio2()) return;
    if (angle.clockOffsetUs == INT64_MIN) return;      // nothing measured yet

    std::deque<DeskAudioFrame> ready;
    {
        std::lock_guard<std::mutex> guard(app.deskMicLock);
        ready.swap(app.deskMicQueue);
    }
    for (const DeskAudioFrame& frame : ready) {
        const int64_t phonePts = static_cast<int64_t>(frame.ptsUs) - angle.clockOffsetUs;
        if (phonePts < 0) continue;
        angle.recording.WriteAudio2(frame.bytes.data(), frame.bytes.size(),
                                  static_cast<uint64_t>(phonePts));
    }
}

// Records the graded picture, when that is what was asked for.
//
// Runs on the presenting thread beside the virtual camera's readback, for the
// same reason: both stall on the GPU, and doing that on the socket thread is
// what made 4K reach sixteen seconds of latency in phase two.
//
// What it records is the live stream, not the phone's second encode. The phone
// keeps the full-quality file ungraded; this is the other choice, and the
// settings sheet says which is which.
void PumpGradedRecording(AppState& app) {
    Angle& angle = app.Program();

    // A LUT *or* a colour setting. It used to be the LUT alone, which meant
    // somebody who dialled contrast and saturation without loading a .cube saw
    // it in the preview and in the call and then got a flat file -- the one
    // place the look was actually wanted for keeps.
    const bool anyLook = app.renderer.HasLut() || app.renderer.HasGrade();
    const bool wanted = angle.model.recording && app.shared.recordGraded &&
                        angle.model.recordToPc && anyLook;

    if (!wanted) {
        if (app.gradeActive) {
            app.gradeEncoder.Drain([&](const uint8_t* data, size_t bytes,
                                       uint64_t ptsUs, bool key) {
                app.gradeWriter.WriteVideo(data, bytes, ptsUs, key);
            });
            const std::string path = app.gradeWriter.Path();
            const uint64_t written = app.gradeWriter.Bytes();
            app.gradeWriter.Close();
            app.gradeEncoder.Close();
            app.gradeActive = false;
            XCAM_LOG_INFO("graded recording finished: %.0f MB -> %s",
                          static_cast<double>(written) / 1e6, path.c_str());
        }
        return;
    }

    // The shape being produced, not the shape that arrived.
    //
    // This used to take the capture size, which was right until the output
    // could be vertical -- then the shader cropped to 9:16 and the encoder was
    // still opened at 16:9, so the file came out squashed. Stood up, the
    // capture's long edge becomes the height and the whole frame survives;
    // cropped, the height stays and the width narrows to what is really there,
    // rather than upscaling to a number that promises detail the sensor did not
    // give.
    const int rotate = app.shared.RotationForShape();
    const uint32_t captureW = static_cast<uint32_t>(angle.model.width);
    const uint32_t captureH = static_cast<uint32_t>(angle.model.height);

    const uint32_t height = (rotate != 0 ? captureW : captureH) & ~1u;
    const uint32_t width =
        static_cast<uint32_t>(height * AspectOf(app.shared.shape) + 0.5f) & ~1u;
    const uint32_t fps = static_cast<uint32_t>(angle.model.fps ? angle.model.fps : 30);

    // The shape changed under an open take. Nothing can be done to the frames
    // already written -- an MP4 track has one size -- so this one is finished
    // properly and the next opens beside it at the new shape.
    if (app.gradeActive && (width != app.gradeWidth || height != app.gradeHeight)) {
        app.gradeEncoder.Drain([&](const uint8_t* data, size_t bytes,
                                   uint64_t ptsUs, bool key) {
            app.gradeWriter.WriteVideo(data, bytes, ptsUs, key);
        });
        app.gradeWriter.Close();
        app.gradeEncoder.Close();
        app.gradeActive = false;
        XCAM_LOG_INFO("shape changed mid-take; graded recording continues in a new file");
    }

    if (!app.gradeActive) {
        // H.264 rather than the recording codec: this machine may have no HEVC
        // encoder at all, and a graded take that refuses to start is worse than
        // one in a slightly larger file.
        if (!app.gradeEncoder.Open("h264", width, height, fps, angle.model.bitrate)) {
            XCAM_LOG_ERROR("graded recording unavailable: %s",
                           app.gradeEncoder.LastError().c_str());
            app.shared.recordGraded = false;
            return;
        }
        app.gradeActive = true;
        app.gradeWidth = width;
        app.gradeHeight = height;
        XCAM_LOG_INFO("graded recording started at %ux%u@%u", width, height, fps);
    }

    uint32_t stride = 0;
    if (!app.renderer.GradeToNv12(width, height, app.gradeBuffer, stride)) return;

    const uint64_t ptsUs = static_cast<uint64_t>(NowSeconds() * 1e6);
    app.gradeEncoder.Encode(
        app.gradeBuffer.data(), ptsUs,
        [&](const uint8_t* data, size_t bytes, uint64_t samplePts, bool key) {
            if (!app.gradeWriter.IsOpen()) {
                std::string dir = app.shared.recordFolder;
                if (dir.empty()) dir = Mp4Writer::DefaultDirectory();
                const std::string name = "XCam_graded_" + Mp4Writer::TimestampedName().substr(5);
                const std::string path = dir.empty() ? name : dir + "\\" + name;

                const std::string& csd = app.gradeEncoder.CodecPrivateData();
                if (!app.gradeWriter.Open(path, "h264", width, height, fps,
                                          reinterpret_cast<const uint8_t*>(csd.data()),
                                          csd.size(), nullptr, 0, 0, 0)) {
                    XCAM_LOG_ERROR("could not open the graded file: %s",
                                   app.gradeWriter.LastError().c_str());
                    return;
                }
                XCAM_LOG_INFO("graded recording to %s", path.c_str());
            }
            app.gradeWriter.WriteVideo(data, bytes, samplePts, key);
        });
}

// Watches for a cable while the stream is on Wi-Fi.
//
// The connection loop only reconsiders the transport when the current one
// breaks, so plugging a cable in used to do nothing until the Wi-Fi link
// happened to drop. Dropping it deliberately was never worth it before: it cost
// a full pipeline restart. Now that the phone lingers and the session resumes,
// the switch costs a key frame, and a cable that is plugged in is almost always
// meant to be used -- it is the difference between 22 Mbit/s and 156.
//
// Only in Auto. Someone who pinned the transport has said what they want.
void CableWatchThread(AppState& app) {
    Angle& angle = app.Program();

    while (!app.quit.load()) {
        // Slept in slices so that quitting does not wait out the interval. A
        // window that takes five seconds to close is a window people report as
        // hung.
        for (int i = 0; i < 20 && !app.quit.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        if (app.quit.load() || app.adbPath.empty()) continue;

        bool worthLooking = false;
        {
            std::lock_guard<std::mutex> guard(app.lock);
            worthLooking = angle.model.connected &&
                           app.shared.transport == Transport::Auto &&
                           angle.model.transportLabel.rfind("Wi-Fi", 0) == 0;
        }
        if (!worthLooking) continue;

        // One adb call every five seconds, and only while on Wi-Fi. Cheap
        // enough not to matter, and the alternative is a person wondering why
        // the cable they just plugged in did nothing.
        const auto devices = ListDevices(app.adbPath);
        const bool cable = std::any_of(devices.begin(), devices.end(),
                                       [](const AdbDevice& d) { return d.IsUsable(); });
        if (!cable) continue;

        XCAM_LOG_INFO("a cable appeared; moving off Wi-Fi");
        app.reconnect = true;
        angle.link.client.Disconnect();
    }
}

// ---- the worker ------------------------------------------------------------

void StreamThread(AppState& app, Angle& angle, HWND window) {
    XCAM_LOG_INFO("worker started");
    while (!app.quit.load()) {
        {
            std::lock_guard<std::mutex> guard(app.lock);
            angle.model.connected = false;
        }

        // Where to look, in order of how much it was asked for: an address on
        // the command line, then a cable, then a phone that announced itself,
        // then the address that worked last time. Discovery comes before the
        // remembered one because a phone that is broadcasting now is a better
        // answer than an address that may have been reassigned since.
        Transport transport = Transport::Auto;
        std::string chosenHost;
        int* wifiRefusals = nullptr;
        {
            std::lock_guard<std::mutex> guard(app.lock);
            transport = app.shared.transport;
            chosenHost = app.shared.host;
        }
        app.reconnect = false;

        // An address on the command line still outranks everything: it is the
        // most deliberate thing anyone can say.
        std::string networkHost = angle.host;
        if (networkHost.empty() && transport == Transport::WiFi) {
            networkHost = chosenHost;

            // A remembered address that keeps refusing is worse than no address:
            // it is checked first, it never answers, and it hides a phone that is
            // announcing itself from the same network. After two failures the
            // broadcast wins -- addresses are handed out by a router and change
            // without telling anyone, which is the whole reason discovery exists.
            int& refusals = angle.wifiRefusals;
            if (refusals >= 2) {
                const auto found = app.discovery.Devices();
                if (!found.empty() && found.front().address != networkHost) {
                    XCAM_LOG_INFO("%s is not answering; taking %s from the broadcast",
                                  networkHost.c_str(), found.front().address.c_str());
                    networkHost = found.front().address;
                    refusals = 0;
                }
            }
            wifiRefusals = &refusals;
        }
        bool overUsb = false;

        if (networkHost.empty() && transport != Transport::WiFi && !app.adbPath.empty()) {
            if (!angle.askedAdb) {
                angle.askedAdb = true;
                XCAM_LOG_INFO("asking adb for devices");
            }
            const auto devices = ListDevices(app.adbPath);
            XCAM_LOG_INFO("adb listed %zu device(s)", devices.size());

            // An angle that was given a phone takes that one and waits if it is
            // not there. Anything else and two angles would race for whichever
            // device adb happened to list first, and the rig would shuffle
            // itself every time a cable was replugged.
            const auto usable = std::find_if(
                devices.begin(), devices.end(), [&angle](const AdbDevice& d) {
                    if (!d.IsUsable()) return false;
                    return angle.wantSerial.empty() || d.serial == angle.wantSerial;
                });
            if (usable != devices.end()) {
                angle.link.serial = usable->serial;
                EnsureForward(app.adbPath, angle.port, angle.link.serial);
                overUsb = true;
            }
        }

        if (!overUsb && networkHost.empty() && transport != Transport::Usb) {
            const auto found = app.discovery.Devices();
            if (!found.empty()) {
                networkHost = found.front().address;
                std::lock_guard<std::mutex> guard(app.lock);
                angle.model.status = "found " + found.front().name;
            } else {
                networkHost = app.settings.GetString("net.host");
            }
        }

        if (!overUsb && networkHost.empty()) {
            std::lock_guard<std::mutex> guard(app.lock);
            // Logged, not only shown. A status that lives in a window nobody is
            // looking at explains nothing afterwards, and this is the branch a
            // desktop sits in when it cannot find the phone at all.
            if (angle.reportedStatus != "looking") {
                angle.reportedStatus = "looking";
                XCAM_LOG_INFO("looking for a phone (transport %d, adb %s)",
                              static_cast<int>(transport),
                              app.adbPath.empty() ? "missing" : "found");
            }
            angle.model.status =
                transport == Transport::Usb   ? T("waiting for a phone on adb")
              : transport == Transport::WiFi  ? T("looking for a phone on the network")
              : app.adbPath.empty()           ? T("looking for a phone on the network")
                                              : T("looking for a phone on USB or the network");
            for (int i = 0; i < 10 && !app.quit.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            continue;
        }

        const std::string target = overUsb ? std::string("127.0.0.1") : networkHost;
        {
            const std::string what = target + (overUsb ? " (USB)" : " (Wi-Fi)");
            if (angle.announcedTarget != what) {
                angle.announcedTarget = what;
                XCAM_LOG_INFO("trying %s", what.c_str());
            }
        }

        // A second and a half, not five.
        //
        // A phone that is there answers in milliseconds, on loopback or on a
        // LAN; the timeout only ever governs how long a failure takes. Five
        // seconds made two things worse: quitting waited out the attempt in
        // progress, which is why closing the window with no phone attached took
        // fifteen seconds, and a hand-over to Wi-Fi could queue behind a stale
        // address that was never going to answer.
        if (!angle.link.client.Connect(target, angle.port, 1500)) {
            {
                std::lock_guard<std::mutex> guard(app.lock);
                angle.model.status = overUsb ? "starting the phone app"
                                           : "waiting for " + networkHost;
                XCAM_LOG_WARN("connect to %s failed: %s", target.c_str(),
                              angle.link.client.LastError().c_str());
                if (wifiRefusals) ++*wifiRefusals;
            }
            // Over USB the app may simply not be up yet; bring it forward and
            // retry. Over Wi-Fi there is no way to reach in and start it, so
            // waiting is all there is.
            if (overUsb && !app.adbPath.empty()) LaunchApp(app.adbPath, angle.link.serial);
            for (int i = 0; i < 10 && !app.quit.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            continue;
        }

        // Nothing is logged as connected yet. Over USB the adb forward accepts
        // the socket whether or not the phone app is running, so a TCP
        // connection on its own means very little; the handshake below is the
        // first thing that proves there is a phone at the other end.

        DeviceInfo info;
        bool shook = angle.link.client.ReadHandshake(info);

        // A phone reached over the network answers with a challenge rather than
        // a description until the code is said. The code is a setting, so this
        // costs nothing on every connection after the first -- and a phone on
        // the cable never asks, because holding the cable is the better proof.
        if (!shook && angle.link.client.NeedsPairing()) {
            std::string code;
            {
                std::lock_guard<std::mutex> guard(app.lock);
                code = app.shared.pairCode;
            }
            if (code.empty()) {
                XCAM_LOG_WARN("this phone wants a pairing code; none is set");
                std::lock_guard<std::mutex> guard(app.lock);
                angle.model.status = "type the phone's pairing code in Settings";
            } else if (angle.link.client.SendPairing(code)) {
                shook = angle.link.client.ReadHandshake(info);
                // The phone closes the socket on a wrong code rather than
                // saying so, which is right of it and unhelpful here: the one
                // thing worth telling somebody is which of their two problems
                // this is.
                if (!shook && !angle.link.client.NeedsPairing()) {
                    std::lock_guard<std::mutex> guard(app.lock);
                    angle.model.status = "the phone refused that pairing code";
                }
            }
        }

        if (!shook) {
            XCAM_LOG_ERROR("handshake failed: %s", angle.link.client.LastError().c_str());
            {
                std::lock_guard<std::mutex> guard(app.lock);
                if (angle.model.status.rfind("type the phone", 0) != 0 &&
                    angle.model.status.rfind("the phone refused", 0) != 0) {
                    angle.model.status = "handshake failed: " + angle.link.client.LastError();
                }
            }
            angle.link.client.Disconnect();
            for (int i = 0; i < 10 && !app.quit.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            continue;
        }

        {
            std::lock_guard<std::mutex> guard(app.lock);
            XCAM_LOG_INFO("connected to the phone over %s",
                          overUsb ? "USB" : networkHost.c_str());
            angle.model.transportLabel = overUsb ? "USB" : ("Wi-Fi " + networkHost);
            // Only a connection that got as far as a handshake is worth
            // remembering as somewhere to look next time.
            if (!overUsb) app.settings.Set("net.host", networkHost);

            angle.model.device = info;
            // The chosen camera survives a reconnect and a restart -- someone
            // who works on the front camera should not be put back on the rear
            // one by a cable coming loose. Clamped rather than trusted: a
            // different phone may not have as many.
            if (angle.model.cameraIndex >= info.cameras.size()) angle.model.cameraIndex = 0;
            angle.model.connected = true;
            angle.hadSession = true;
            angle.model.status = info.deviceName;
            // Not on a resume: the pipeline on the phone is still running the
            // format it was given, and choosing a "default" here would send it
            // a change it does not need.
            if (!info.resumed && !app.formatFromCommandLine) ChooseDefaultFormat(angle.model);
            XCAM_LOG_INFO("handshake: %s, %zu cameras, starting %dx%d@%d",
                          info.deviceName.c_str(), info.cameras.size(),
                          angle.model.width, angle.model.height, angle.model.fps);
        }
        // The whole point of the hand-over.
        //
        // A `set` command rebuilds the capture session and the encoder on the
        // phone, which is exactly what a cable coming loose must not cost. On a
        // resumed session everything asked for is already running; the phone has
        // re-sent the codec configuration and a key frame, and there is nothing
        // for this side to do but carry on decoding.
        // A resume this side never had.
        //
        // The phone offers a session to whoever connects inside its linger
        // window, and it cannot tell a link that came back from a client that
        // has just started. This one can: an application that has never had a
        // session has nothing to continue, and accepting the offer would leave
        // it running a pipeline configured by whoever was here before -- which
        // is how a recording size chosen in one session survived restarting the
        // desktop entirely.
        if (info.resumed && !angle.hadSession) {
            XCAM_LOG_INFO("the phone offered a session this application never had");
        }

        if (info.resumed && angle.hadSession) {
            XCAM_LOG_INFO("session resumed; leaving the pipeline alone");
        } else {
            // The recording configuration first.
            //
            // The phone remembers what it was last told, and that memory
            // outlives this application: a size chosen in one session was still
            // in force in the next, so a recording clamped to 720p once stayed
            // there no matter what this side believed. Re-stated on every fresh
            // connection, the phone's idea and the panel's cannot drift apart.
            // The phone ignores a configuration that changes nothing.
            angle.link.client.SendControl(MakeRecordConfigCommand(
                angle.model.recordEnabled, angle.model.recordToPc,
                angle.model.recordWantWidth, angle.model.recordWantHeight,
                angle.model.recordRate(), 0, angle.model.recordCodec, angle.model.preRoll));
            SendFormat(app, angle);
        }

        double lastReport = NowSeconds();
        uint64_t frames = 0, bytes = 0;
        uint32_t expectedSeq = 0;
        bool haveSeq = false;
        int gaps = 0;
        bool haveConfig = false;

        uint64_t audioBytesIn = 0, audioBytesOut = 0;
        int audioPeak = 0;

        double latencyBase = 0;
        bool haveBase = false;
        std::vector<double> latencyWindow;

        Packet packet;
        while (!app.quit.load() && angle.link.client.ReadPacket(packet)) {
            const auto& header = packet.header;

            if (header.type == PacketType::Config) {
                std::string codec;
                {
                    std::lock_guard<std::mutex> guard(app.lock);
                    codec = angle.model.codec;
                }
                haveConfig = angle.link.decoder.Open(codec, app.device) &&
                             angle.link.decoder.SetCodecConfig(packet.payload.data(),
                                                        packet.payload.size());
                XCAM_LOG_INFO("stream restart: CONFIG %zu bytes, decoder %s",
                              packet.payload.size(), haveConfig ? "ready" : "FAILED");

                // Put back everything the panel believes is in force. After a
                // pipeline restart the capture request was rebuilt from
                // defaults and this is the only thing that recovers it; after a
                // resume nothing was lost and this is merely idempotent, which
                // is cheaper than working out which of the two happened.
                {
                    std::lock_guard<std::mutex> guard(app.lock);
                    SendPictureState(app, angle);
                }
                // A new encoder session restarts both the timestamp clock and
                // the frame counter. Carrying either across would misreport the
                // age of the session as latency, and the counter going back to
                // zero as lost frames.
                haveBase = false;
                latencyWindow.clear();
                haveSeq = false;
                if (!haveConfig) {
                    std::lock_guard<std::mutex> guard(app.lock);
                    angle.model.status = "decoder: " + angle.link.decoder.LastError();
                }
                continue;
            }

            if (header.type == PacketType::Stats) {
                StatsInfo stats;
                const std::string payload = packet.PayloadAsString();
                if (ParseStats(payload, stats)) {
                    double queueMs = 0;
                    {
                        std::lock_guard<std::mutex> guard(app.lock);
                        angle.model.recording = stats.recording;
                        angle.model.recordMs = stats.recordMs;
                        angle.model.recordBytes = stats.recordBytes;
                        angle.model.storageFreeMb = stats.storageFreeMb;
                        // Which way up the phone is, which decides whether a
                        // vertical output can be stood up rather than cropped.
                        // Shared rather than per-angle: it is the shape leaving
                        // this machine, and there is one of those.
                        if (stats.surfaceRotation >= 0) {
                            app.shared.surfaceRotation = stats.surfaceRotation;
                        }

                        // Peak now, and a hold that falls back slowly. A meter
                        // drawn straight from the last tick flickers; one that
                        // decays is how every level meter ever built behaves.
                        angle.model.audioPeak = stats.audioPeak;
                        angle.model.audioHold = (std::max)(stats.audioPeak,
                                                         angle.model.audioHold * 0.75f);

                        // Silence is only a fault when something is listening.
                        // A microphone that is off is not broken, and saying so
                        // would train people to ignore the one warning here
                        // that has twice been right.
                        if (angle.model.micEnabled && stats.audioPeak <= 0.0f) {
                            angle.model.micSilentMs += kStatsIntervalMs;
                        } else {
                            angle.model.micSilentMs = 0;
                        }

                        // The phone can disarm the ring on its own when it gets
                        // hot, and says so only by reporting zero here. Taking
                        // the phone's word rather than our own request is the
                        // difference between a panel that shows the setting and
                        // one that shows the state.
                        const int armed = static_cast<int>(stats.preRollArmedMs / 1000);
                        if (angle.model.preRollGranted >= 0 &&
                            armed != angle.model.preRollGranted) {
                            XCAM_LOG_INFO("pre-roll now %ds (was %ds, thermal %s)", armed,
                                          angle.model.preRollGranted, stats.thermal.c_str());
                            angle.model.preRollGranted = armed;
                        }
                        angle.model.preRollFillMs = stats.preRollFillMs;
                        queueMs = angle.model.statLatencyMs;
                    }

                    const int adjusted = angle.link.governor.Update(stats.droppedFrames, queueMs);
                    if (adjusted > 0) {
                        std::ostringstream cmd;
                        cmd << R"({"cmd":"bitrate","value":)" << adjusted << '}';
                        angle.link.client.SendControl(cmd.str());
                    }
                    {
                        std::lock_guard<std::mutex> guard(app.lock);
                        angle.model.activeBitrate = angle.link.governor.Active();
                        angle.model.bitrateLimited = angle.link.governor.Limited();
                    }
                }
                continue;
            }

            if (header.type == PacketType::File) {
                // A chunk of a take being fetched. The phone only sends these
                // while the link is otherwise idle, so nothing here needs to
                // yield to the stream -- it already has.
                std::string finished;
                {
                    std::lock_guard<std::mutex> guard(app.lock);
                    if (angle.model.Fetching()) {
                        if (!angle.fetchFile.is_open()) {
                            angle.fetchFile.open(angle.model.fetchLocalPath,
                                               std::ios::binary | std::ios::trunc);
                            if (!angle.fetchFile) {
                                XCAM_LOG_ERROR("could not write %s",
                                               angle.model.fetchLocalPath.c_str());
                                angle.model.takesError = "could not write the file";
                                angle.model.fetchName.clear();
                            }
                        }
                        if (angle.fetchFile.is_open() && !packet.payload.empty()) {
                            angle.fetchFile.write(
                                reinterpret_cast<const char*>(packet.payload.data()),
                                static_cast<std::streamsize>(packet.payload.size()));
                            angle.model.fetchReceived +=
                                static_cast<int64_t>(packet.payload.size());
                        }
                        if (header.flags & kFlagLastFragment) {
                            angle.fetchFile.close();
                            finished = angle.model.fetchLocalPath;
                            angle.model.fetchName.clear();
                            angle.model.fetchBytes = 0;
                            angle.model.fetchReceived = 0;
                        }
                    }
                }
                if (!finished.empty()) XCAM_LOG_INFO("fetched %s", finished.c_str());
                continue;
            }

            if (header.type == PacketType::Record) {
                if (header.flags & kFlagCodecConfig) {
                    // A take is starting, and everything needed to describe the
                    // file is known by now: the format from the record ACK, the
                    // parameter sets here, and the audio configuration from
                    // earlier in the session.
                    uint32_t width = 0, height = 0, fps = 0;
                    std::string codec;
                    {
                        std::lock_guard<std::mutex> guard(app.lock);
                        width = static_cast<uint32_t>(angle.model.recordWidth);
                        height = static_cast<uint32_t>(angle.model.recordHeight);
                        fps = static_cast<uint32_t>(angle.model.recordFps);
                        codec = angle.model.recordCodec;
                    }

                    std::string dir;
                    {
                        std::lock_guard<std::mutex> guard(app.lock);
                        dir = app.shared.recordFolder;
                    }
                    if (dir.empty()) dir = Mp4Writer::DefaultDirectory();
                    const std::string path =
                        dir.empty() ? Mp4Writer::TimestampedName()
                                    : dir + "\\" + Mp4Writer::TimestampedName();

                    const std::string& deskAsc = app.deskAac.AudioSpecificConfig();
                    const bool withDesk = app.deskMic.IsRunning() && !deskAsc.empty();

                    if (angle.recording.Open(
                            path, codec, width, height, fps,
                            packet.payload.data(), packet.payload.size(),
                            angle.audioAsc.data(), angle.audioAsc.size(),
                            kAudioSampleRate, kAudioChannels,
                            withDesk ? reinterpret_cast<const uint8_t*>(deskAsc.data())
                                     : nullptr,
                            withDesk ? deskAsc.size() : 0,
                            withDesk ? app.deskMic.SampleRate() : 0,
                            withDesk ? app.deskMic.Channels() : 0)) {
                        std::lock_guard<std::mutex> guard(app.lock);
                        angle.model.recordLocalPath = path;
                        XCAM_LOG_INFO("recording to %s (%ux%u@%u %s, audio %s%s)",
                                      path.c_str(), width, height, fps, codec.c_str(),
                                      angle.recording.HasAudio() ? "phone" : "none",
                                      angle.recording.HasAudio2() ? " + desk" : "");
                    } else {
                        XCAM_LOG_ERROR("could not start the recording: %s",
                                       angle.recording.LastError().c_str());
                    }
                } else if (angle.recording.IsOpen()) {
                    // Every frame is a measurement of the gap between the two
                    // clocks: it left the phone at header.ptsUs and arrived
                    // here now, so the difference is the offset plus whatever
                    // the link took. The smallest difference seen recently is
                    // the least-delayed packet, which is the closest to the
                    // truth the link will give -- hence a running minimum, reset
                    // every few seconds so it can follow a drifting clock rather
                    // than latching onto one lucky packet forever.
                    const double now = NowSeconds();
                    const int64_t observed =
                        static_cast<int64_t>(now * 1e6) - static_cast<int64_t>(header.ptsUs);
                    if (angle.clockOffsetUs == INT64_MIN ||
                        observed < angle.clockOffsetUs ||
                        now - angle.clockOffsetSince > 5.0) {
                        if (now - angle.clockOffsetSince > 5.0) angle.clockOffsetSince = now;
                        angle.clockOffsetUs = observed;
                    }

                    angle.recording.WriteVideo(packet.payload.data(), packet.payload.size(),
                                             header.ptsUs,
                                             (header.flags & kFlagKeyFrame) != 0);
                    DrainDeskMic(app);
                }
                continue;
            }

            if (header.type == PacketType::Audio) {
                if (header.flags & kFlagCodecConfig) {
                    // Everything the decoder needs is in these two bytes, and
                    // nothing before them can be decoded at all.
                    // Kept as well as decoded: a recording started later needs
                    // it to describe its audio track, and the phone sends it
                    // once per session.
                    angle.audioAsc = packet.payload;

                    const bool ok = angle.audioDecoder.Open(
                        packet.payload.data(), packet.payload.size(),
                        kAudioSampleRate, kAudioChannels);
                    XCAM_LOG_INFO("audio config %zu bytes, decoder %s",
                                  packet.payload.size(),
                                  ok ? "ready" : angle.audioDecoder.LastError().c_str());
                    if (ok && !app.audioPublisher.IsOpen()) {
                        if (app.audioPublisher.Open()) {
                            XCAM_LOG_INFO("virtual microphone publishing %u Hz %u ch",
                                          kAudioSampleRate, kAudioChannels);
                        } else {
                            XCAM_LOG_ERROR("could not publish audio: %s",
                                           app.audioPublisher.LastError().c_str());
                        }
                    }
                } else if (angle.audioDecoder.IsOpen()) {
                    audioBytesIn += packet.payload.size();
                    angle.audioDecoder.Decode(
                        packet.payload.data(), packet.payload.size(), header.ptsUs,
                        [&](const uint8_t* pcm, size_t bytes, uint64_t) {
                            audioBytesOut += bytes;
                            // Peak level of what we publish. Silence reaching a
                            // virtual microphone can come from three places --
                            // the phone, the decoder, or the ring -- and this
                            // says which of them still has a signal.
                            const auto* samples = reinterpret_cast<const int16_t*>(pcm);
                            for (size_t i = 0; i < bytes / 2; ++i) {
                                const int v = samples[i] < 0 ? -samples[i] : samples[i];
                                if (v > audioPeak) audioPeak = v;
                            }
                            // Only the angle on air reaches the virtual
                            // microphone -- two phones publishing into one
                            // section would interleave two rooms. The others
                            // still decode, because their own files want the
                            // level and the sync.
                            if (angle.index == app.shared.program) {
                                app.audioPublisher.Publish(pcm, bytes);
                            }
                        });

                    // The file takes the AAC as it arrived rather than the PCM
                    // just decoded for the virtual microphone: re-encoding
                    // sound already in the right format would be a pure loss.
                    if (angle.recording.IsOpen()) {
                        angle.recording.WriteAudio(packet.payload.data(),
                                                 packet.payload.size(), header.ptsUs);
                    }
                }
                continue;
            }

            if (header.type == PacketType::Ack) {
                const std::string payload = packet.PayloadAsString();
                XCAM_LOG_INFO("ack   <- %s", payload.c_str());

                // A take that has ended is finalised here rather than left to
                // the next one: an MP4 without its index has every byte of
                // footage in it and no player will open it.
                Ack ack;
                if (ParseAck(payload, ack) && ack.cmd == "record" &&
                    ack.appliedJson.find("\"state\":\"recording\"") == std::string::npos &&
                    angle.recording.IsOpen()) {
                    const std::string path = angle.recording.Path();
                    const uint64_t fileBytes = angle.recording.Bytes();
                    const uint64_t ms = angle.recording.DurationMs();
                    angle.recording.Close();
                    XCAM_LOG_INFO("recorded %llums, %.0f MB -> %s",
                                  static_cast<unsigned long long>(ms),
                                  static_cast<double>(fileBytes) / 1e6, path.c_str());
                }

                std::lock_guard<std::mutex> guard(app.lock);
                ApplyAck(angle.model, payload);
                continue;
            }

            if (!packet.IsVideo() || !haveConfig) continue;

            if (haveSeq && header.seq != expectedSeq) {
                ++gaps;
                // Frames were shed upstream; ask for a key frame so the picture
                // recovers now instead of at the next scheduled one.
                angle.link.client.SendControl(MakeSimpleCommand("idr"));
            }
            expectedSeq = header.seq + 1;
            haveSeq = true;

            const double arrival = NowSeconds();
            const double delta = arrival * 1000.0 - static_cast<double>(header.ptsUs) / 1000.0;
            if (!haveBase || delta < latencyBase) { latencyBase = delta; haveBase = true; }
            latencyWindow.push_back(delta - latencyBase);

            ++frames;
            bytes += packet.payload.size();

            // No shared lock here on purpose. The decoder belongs to this
            // thread and the renderer guards its own texture, so nothing in the
            // read path can be held up by the UI thread sitting in Present.
            angle.link.decoder.Decode(packet.payload.data(), packet.payload.size(), header.ptsUs,
                               [&](const DecodedFrame& decoded) {
                                   if (!decoded.texture) return;
                                   app.renderer.UploadFrame(
                                       angle.index, decoded.texture, decoded.textureIndex,
                                       decoded.width, decoded.height,
                                       decoded.visibleWidth, decoded.visibleHeight);
                                   app.frameReady = true;
                               });

            const double now = NowSeconds();
            if (now - lastReport >= 0.5) {
                const double elapsed = now - lastReport;
                std::sort(latencyWindow.begin(), latencyWindow.end());

                std::lock_guard<std::mutex> guard(app.lock);
                angle.model.statFps = frames / elapsed;
                angle.model.statMbps = static_cast<double>(bytes) * 8.0 / elapsed / 1e6;
                angle.model.statLatencyMs = latencyWindow.empty()
                    ? 0.0 : latencyWindow[latencyWindow.size() / 2];
                angle.model.statGaps = gaps;

                XCAM_LOG_DEBUG("%.0f fps  %.1f Mb/s  %.0f ms  %d gaps  "
                               "audio %.0f kbit/s in, %.0f kB/s out, peak %d",
                               angle.model.statFps, angle.model.statMbps,
                               angle.model.statLatencyMs, gaps,
                               static_cast<double>(audioBytesIn) * 8.0 / elapsed / 1000.0,
                               static_cast<double>(audioBytesOut) / elapsed / 1000.0,
                               audioPeak);
                audioBytesIn = 0;
                audioBytesOut = 0;
                audioPeak = 0;

                latencyWindow.clear();
                frames = 0;
                bytes = 0;
                lastReport = now;
            }
        }

        {
            std::lock_guard<std::mutex> guard(app.lock);
            angle.model.connected = false;
            angle.model.status = angle.link.client.LastError().empty() ? "disconnected"
                                                              : angle.link.client.LastError();
            XCAM_LOG_WARN("stream ended: %s", angle.model.status.c_str());
        }
        // Losing the phone mid-take still has to leave a playable file, which
        // is the whole difference between a recording and a pile of bytes.
        if (angle.recording.IsOpen()) {
            const std::string path = angle.recording.Path();
            angle.recording.Close();
            XCAM_LOG_WARN("connection lost mid-recording; finalised %s", path.c_str());
        }

        angle.link.client.Disconnect();
        PostMessageW(window, WM_APP, 0, 0);
        if (!app.quit.load()) std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

// ---- window ----------------------------------------------------------------

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    AppState* app = g_app;
    if (!app) return DefWindowProcW(window, message, wparam, lparam);
    Angle& angle = app->Program();

    switch (message) {
        case WM_SIZE:
            if (wparam != SIZE_MINIMIZED) {
                std::lock_guard<std::mutex> guard(app->lock);
                app->renderer.Resize(LOWORD(lparam), HIWORD(lparam));
            }
            return 0;

        case WM_MOUSEMOVE: {
            std::lock_guard<std::mutex> guard(app->lock);
            app->input.mouseX = static_cast<float>(GET_X_LPARAM(lparam));
            app->input.mouseY = static_cast<float>(GET_Y_LPARAM(lparam));
            app->panel.NotePointerActivity(NowSeconds());
            return 0;
        }

        case WM_LBUTTONDOWN: {
            SetCapture(window);
            std::lock_guard<std::mutex> guard(app->lock);
            app->input.mouseX = static_cast<float>(GET_X_LPARAM(lparam));
            app->input.mouseY = static_cast<float>(GET_Y_LPARAM(lparam));
            app->input.mouseDown = true;
            app->input.mousePressed = true;
            app->panel.NotePointerActivity(NowSeconds());
            return 0;
        }

        case WM_LBUTTONUP: {
            ReleaseCapture();
            std::lock_guard<std::mutex> guard(app->lock);
            app->input.mouseDown = false;
            app->input.mouseReleased = true;

            // A click on the picture rather than on a control means tap to
            // focus, the same gesture a phone camera uses.
            if (!app->panel.WantsMouse() && angle.model.connected) {
                const float x = app->input.mouseX / (std::max)(app->ui.Width(), 1.0f);
                const float y = app->input.mouseY / (std::max)(app->ui.Height(), 1.0f);
                angle.link.client.SendControl(MakeFocusCommand("tap", 0.0f, x, y));
                angle.model.focusMode = FocusMode::Continuous;
            }
            app->panel.NotePointerActivity(NowSeconds());
            return 0;
        }

        case WM_MOUSEWHEEL: {
            std::lock_guard<std::mutex> guard(app->lock);
            app->input.wheel += GET_WHEEL_DELTA_WPARAM(wparam) / 120.0f;
            app->panel.NotePointerActivity(NowSeconds());
            return 0;
        }

        case WM_TIMER: {
            // The only place anything is presented. Draws when a new frame has
            // landed, and otherwise only while the panel still has something to
            // animate, so an idle stream costs nothing.
            if (wparam != kIdleRenderTimer) return 0;

            const bool newFrame = app->frameReady.exchange(false);
            const bool panelAwake = app->panel.Opacity(NowSeconds()) > 0.0f;
            if (newFrame || panelAwake) {
                std::lock_guard<std::mutex> guard(app->lock);
                PresentFrame(*app);
            }

            // Write settings as they change rather than only on exit: a crash
            // or a pulled cable should not cost an evening of dialling in.
            // Nothing is written unless something actually changed.
            static double lastSettingsSave = 0;
            const double now = NowSeconds();
            if (now - lastSettingsSave >= 1.0) {
                lastSettingsSave = now;
                std::lock_guard<std::mutex> guard(app->lock);
                StoreSettings(*app);
                if (app->settings.Dirty()) app->settings.Save();
            }

            bool wantsDialog = false;
            bool wantsFolder = false;
            bool wantsScript = false;
            {
                std::lock_guard<std::mutex> guard(app->lock);
                std::swap(wantsDialog, app->lutDialogPending);
                std::swap(wantsFolder, app->folderDialogPending);
                std::swap(wantsScript, app->prompterDialogPending);
            }
            if (wantsScript) {
                LoadPrompterScript(*app, window);
                // The panel faded out while the dialog was up, and coming back
                // to an empty screen would look like the load had failed.
                app->panel.NotePointerActivity(NowSeconds());
            }
            if (wantsDialog) {
                LoadLutInteractively(*app, window);
                app->panel.NotePointerActivity(NowSeconds());
            }
            if (wantsFolder) {
                ChooseRecordingFolder(*app, window);
                app->panel.NotePointerActivity(NowSeconds());
            }

            std::string wantsFetch;
            int wantsTally = -1;
            int darken = -1;
            {
                std::lock_guard<std::mutex> guard(app->lock);
                std::swap(wantsFetch, app->fetchPending);
                std::swap(wantsTally, app->tallyPending);
                std::swap(darken, app->tallyCutFrom);
            }
            if (!wantsFetch.empty()) StartTakeFetch(*app, wantsFetch);
            if (darken >= 0 && static_cast<size_t>(darken) < app->angles.size()) {
                app->angles[static_cast<size_t>(darken)].link.client.SendControl(
                    MakeTallyCommand(false));
            }
            if (wantsTally >= 0) angle.link.client.SendControl(MakeTallyCommand(wantsTally != 0));

            std::string describe;
            int64_t describeMs = 0;
            double startedAt = 0;
            {
                std::lock_guard<std::mutex> guard(app->lock);
                std::swap(describe, angle.model.describePending);
                describeMs = angle.model.describeMs;
                startedAt = angle.model.takeStartedAt;
            }
            if (!describe.empty()) {
                // The take's own directory when it was written here, and the
                // recordings folder when it is still on the phone -- which is
                // where the takes browser will put it when it is fetched, so
                // the sidecar and the file end up together either way.
                std::string directory = app->shared.recordFolder;
                std::string name = describe;
                // Two backslashes in the source, not one. A lone backslash in
                // front of a slash is not an escape sequence at all, and MSVC
                // quietly drops it -- so this set was "/" by itself and no
                // Windows path in it ever split. The take kept its whole path as
                // its name, and its sidecar went to the wrong folder.
                const size_t slash = describe.find_last_of("\\/");
                if (slash != std::string::npos) {
                    name = describe.substr(slash + 1);
                    if (angle.model.recordToPc) directory = describe.substr(0, slash);
                }
                if (directory.empty()) directory = Mp4Writer::DefaultDirectory();

                std::string error;
                std::lock_guard<std::mutex> guard(app->lock);
                if (!WriteTakeSidecar(directory, name, angle.model, app->shared, startedAt,
                                      // The graded file is the proxy, when one
                                      // was made. The take's own path is not a
                                      // proxy for itself.
                                      app->shared.recordGraded && !angle.model.recordToPc
                                          ? angle.model.recordLocalPath
                                          : std::string(),
                                      error)) {
                    XCAM_LOG_WARN("%s", error.c_str());
                } else if (!WriteTakeEdl(directory, name, angle.model, describeMs, error)) {
                    XCAM_LOG_WARN("%s", error.c_str());
                } else {
                    XCAM_LOG_INFO("described %s", name.c_str());
                }
            }

            std::string wantsFraming;
            {
                std::lock_guard<std::mutex> guard(app->lock);
                std::swap(wantsFraming, app->framingPending);
            }
            if (!wantsFraming.empty()) angle.link.client.SendControl(wantsFraming);
            return 0;
        }

        case WM_PAINT: {
            std::lock_guard<std::mutex> guard(app->lock);
            PresentFrame(*app);
            ValidateRect(window, nullptr);
            return 0;
        }

        case WM_CHAR: {
            // Printable characters go to the panel, which decides whether a
            // control is open to being typed into.
            if (wparam >= 0x20 && wparam < 0x7F) {
                std::lock_guard<std::mutex> guard(app->lock);
                app->input.typed += static_cast<char>(wparam);
                app->panel.NotePointerActivity(NowSeconds());
            }
            return 0;
        }

        case WM_KEYDOWN: {
            std::lock_guard<std::mutex> guard(app->lock);
            app->panel.NotePointerActivity(NowSeconds());

            // While a field is open, the keyboard belongs to it.
            //
            // Every shortcut below is a bare key, and TranslateMessage queues
            // the WM_CHAR from the message loop whatever this does -- so typing
            // an address put the dot in the field *and* toggled the takes sheet
            // over the top of it, and every letter in a preset name fired
            // whatever it was bound to as well. Editing keeps backspace, Enter
            // and Escape, which belong to the field; the rest is left to
            // WM_CHAR alone.
            const bool typing = app->panel.IsEditing() || app->settingsPanel.IsEditing();
            if (typing && wparam != VK_BACK && wparam != VK_RETURN &&
                wparam != VK_ESCAPE) {
                return 0;
            }

            switch (wparam) {
                case VK_BACK:   app->input.backspace = true; return 0;
                case VK_RETURN: app->input.commit = true;    return 0;

                case VK_ESCAPE:
                    // Escape backs out of the innermost thing first, which is
                    // the order anyone would expect: an edit, then the sheet,
                    // then the window.
                    if (app->panel.IsEditing() || app->settingsPanel.IsEditing()) {
                        app->input.cancel = true;
                        return 0;
                    }
                    if (app->takesPanel.IsOpen()) {
                        app->takesPanel.Close();
                        return 0;
                    }
                    if (app->settingsPanel.IsOpen()) {
                        app->settingsPanel.Close();
                        return 0;
                    }
                    PostMessageW(window, WM_CLOSE, 0, 0);
                    break;
                case VK_OEM_PERIOD:
                    // Next to the settings key, because it is the other sheet.
                    app->takesPanel.Toggle();
                    return 0;
                case VK_OEM_COMMA:
                    // The shortcut every application on this machine uses for
                    // settings.
                    app->settingsPanel.Toggle();
                    return 0;
                // The follow focus, on the function keys.
                //
                // A pull is a thing you do while looking at the subject, not at
                // a panel, and the four keys are what a control surface would be
                // bound to later. The buttons on the lens bar do the same.
                case VK_F1: angle.model.focusA = angle.model.focusDistance; return 0;
                case VK_F2: angle.model.focusB = angle.model.focusDistance; return 0;
                case VK_F3:
                case VK_F4: {
                    const float target = wparam == VK_F3 ? angle.model.focusA
                                                         : angle.model.focusB;
                    if (target < 0.0f) return 0;      // that mark is not set
                    angle.model.focusDistance = target;
                    angle.link.client.SendControl(
                        MakeRampCommand("focus", target, angle.model.rampMs));
                    return 0;
                }
                case 'Z': {
                    // Cycles rather than toggles: the level matters as much as
                    // whether it is on, and off is part of the cycle.
                    const float steps[] = {0.0f, 0.70f, 0.95f};
                    int next = 0;
                    for (int i = 0; i < 3; ++i) {
                        if (app->shared.zebra == steps[i]) next = (i + 1) % 3;
                    }
                    app->shared.zebra = steps[next];
                    return 0;
                }
                case 'P': {
                    const float steps[] = {0.0f, 0.10f, 0.05f};
                    int next = 0;
                    for (int i = 0; i < 3; ++i) {
                        if (app->shared.peaking == steps[i]) next = (i + 1) % 3;
                    }
                    app->shared.peaking = steps[next];
                    return 0;
                }
                case 'C': {
                    const size_t count = angle.model.device.cameras.size();
                    if (count > 1) {
                        angle.model.cameraIndex = (angle.model.cameraIndex + 1) % count;
                        angle.model.zoom = 1.0f;
                        ChooseDefaultFormat(angle.model);
                        SendFormat(*app, angle);
                    }
                    break;
                }
                case 'T':
                    angle.model.torch = !angle.model.torch;
                    angle.link.client.SendControl(MakeTorchCommand(angle.model.torch));
                    break;
                case 'L':
                    if (angle.model.CanLogProfile()) {
                        angle.model.logProfile = !angle.model.logProfile;
                        angle.link.client.SendControl(
                            MakePictureProfileCommand(angle.model.logProfile));
                    }
                    break;
                case 'R':
                    angle.link.client.SendControl(MakeSimpleCommand("idr"));
                    break;
                case 'V':
                    // Recording, not a key frame -- 'R' was already taken by the
                    // time this existed, and moving it would break the habit of
                    // anyone using it.
                    if (angle.model.CanRecord() && angle.model.recordEnabled) {
                        angle.link.client.SendControl(
                            MakeRecordCommand(angle.model.recording ? "stop" : "start"));
                    }
                    break;
                case 'M':
                    if (angle.model.CanUseMic()) {
                        angle.model.micEnabled = !angle.model.micEnabled;
                        angle.link.client.SendControl(MakeAudioCommand(angle.model.micEnabled));
                    }
                    break;
                default:
                    break;
            }
            return 0;
        }

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;

        default:
            return DefWindowProcW(window, message, wparam, lparam);
    }
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR commandLine, int) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    LogOpen();
    LogInstallCrashHandler();
    XCAM_LOG_INFO("XCam starting");

    AppState app;
    g_app = &app;

    // The one angle there is. A second would be another Angle in the deque with
    // a thread of its own; what is missing before that can happen is not here
    // but in the connection loop, which finds a phone rather than the phones.
    Angle& angle = app.angles.front();
    angle.index = kSoleAngle;

    // Remembered first, command line second: an explicit argument should
    // outrank what was left over from last time.
    Strings::Init();
    app.settings.Load();
    ApplySettings(app);
    XCAM_LOG_INFO("settings: %s", Settings::Path().c_str());
    app.shared.autostart = autostart::Enabled();

    std::string startupLut = app.shared.lutPath;

    // The remembered script, read back off disk. Only the path is saved -- a
    // script belongs in its file, not in the settings -- so this is what turns
    // that path back into something to read.
    if (!app.shared.prompterPath.empty()) {
        const std::string path = app.shared.prompterPath;
        if (ReadScript(app, path)) {
            XCAM_LOG_INFO("script restored: %s (%zu bytes)", path.c_str(),
                          app.shared.prompterText.size());
        } else {
            XCAM_LOG_WARN("the remembered script is gone: %s", path.c_str());
            app.shared.prompterPath.clear();
        }
    }
    bool startupLogProfile = angle.model.logProfile;

    int argc = 0;
    if (LPWSTR* argv = CommandLineToArgvW(commandLine, &argc)) {
        for (int i = 0; i < argc; ++i) {
            const std::wstring arg = argv[i];
            auto value = [&](int& out) {
                if (i + 1 < argc) { out = _wtoi(argv[++i]); app.formatFromCommandLine = true; }
            };
            if (arg == L"--minimised")    app.startMinimised = true;
            else if (arg == L"--width")   value(angle.model.width);
            else if (arg == L"--height")  value(angle.model.height);
            else if (arg == L"--fps")     value(angle.model.fps);
            else if (arg == L"--bitrate") value(angle.model.bitrate);
            else if (arg == L"--codec" && i + 1 < argc) {
                angle.model.codec = (std::wstring(argv[++i]) == L"hevc") ? "hevc" : "h264";
            }
            else if (arg == L"--lut" && i + 1 < argc) {
                char narrow[MAX_PATH * 2] = "";
                WideCharToMultiByte(CP_UTF8, 0, argv[++i], -1, narrow, sizeof(narrow),
                                    nullptr, nullptr);
                startupLut = narrow;
            }
            else if (arg == L"--log-profile") startupLogProfile = true;
            else if (arg == L"--fixed-bitrate") angle.link.governor.Disable();
            else if (arg == L"--host" && i + 1 < argc) {
                char narrow[256] = "";
                WideCharToMultiByte(CP_UTF8, 0, argv[++i], -1, narrow, sizeof(narrow),
                                    nullptr, nullptr);
                app.host = narrow;
                angle.host = narrow;
            }
            // Which phone the first angle is, when there is more than one on
            // the cable. Without it adb's listing order decides, and that is
            // not a decision anybody made.
            else if (arg == L"--serial" && i + 1 < argc) {
                char narrow[128] = "";
                WideCharToMultiByte(CP_UTF8, 0, argv[++i], -1, narrow, sizeof(narrow),
                                    nullptr, nullptr);
                angle.wantSerial = narrow;
            }
            // A second phone at a known address. Repeatable, and the way a rig
            // is assembled over Wi-Fi -- over USB the angles are found rather
            // than named, because adb already knows which phones are there.
            //
            //   --angle 192.168.1.42        another phone, default port
            //   --angle 127.0.0.1:27184     a second phantom, for testing a cut
            //                               with nothing plugged in
            //   --angle usb:R58M12ABCDE    another phone on the cable, named
            //
            // Named rather than found, even over USB. adb will happily list two
            // phones, but which is camera one and which is camera two is a
            // decision about a shot, and a rig that reshuffled itself every
            // time a cable was replugged would be worse than no rig.
            else if (arg == L"--angle" && i + 1 < argc) {
                char narrow[256] = "";
                WideCharToMultiByte(CP_UTF8, 0, argv[++i], -1, narrow, sizeof(narrow),
                                    nullptr, nullptr);
                std::string spec = narrow;

                Angle& extra = app.angles.emplace_back();
                extra.index = app.angles.size() - 1;
                extra.model = angle.model;      // the same format to start from

                if (spec.rfind("usb:", 0) == 0) {
                    extra.wantSerial = spec.substr(4);
                    // Its own forward. A forward is a port on this machine and
                    // two phones cannot share one -- the second would silently
                    // reach the first.
                    extra.port = static_cast<uint16_t>(kDefaultPort + extra.index);
                    XCAM_LOG_INFO("angle %zu is %s on USB, forwarded on %u",
                                  extra.index, extra.wantSerial.c_str(), extra.port);
                } else {
                    const size_t colon = spec.rfind(':');
                    if (colon != std::string::npos) {
                        extra.port = static_cast<uint16_t>(std::atoi(spec.c_str() + colon + 1));
                        spec.resize(colon);
                    }
                    extra.host = spec;
                    XCAM_LOG_INFO("angle %zu at %s:%u", extra.index, spec.c_str(),
                                  extra.port);
                }
            }
        }
        LocalFree(argv);
    }

    app.device = CreateDevice();
    if (!app.device) {
        MessageBoxW(nullptr, L"Could not create a Direct3D 11 device.", L"XCam", MB_ICONERROR);
        return 1;
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = instance;
    // Both sizes explicitly. Given only hIcon, Windows shrinks the large frame
    // for the title bar instead of using the 16px one that was drawn for it,
    // and the mark's amber arm is the first thing lost to that.
    wc.hIcon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_XCAM),
                                             IMAGE_ICON, 0, 0, LR_DEFAULTSIZE));
    wc.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_XCAM),
                                               IMAGE_ICON,
                                               GetSystemMetrics(SM_CXSMICON),
                                               GetSystemMetrics(SM_CYSMICON), 0));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;      // the renderer owns every pixel
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);

    RECT rect{0, 0, 1280, 720};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);

    HWND window = CreateWindowExW(
        0, kWindowClass, L"XCam", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top,
        nullptr, nullptr, instance, nullptr);
    if (!window) return 1;

    if (!app.renderer.Init(app.device, window)) {
        MessageBoxW(window, Widen(app.renderer.LastError()).c_str(), L"XCam", MB_ICONERROR);
        return 1;
    }

    ID2D1DeviceContext* d2d = app.renderer.BeginOverlay();
    const bool uiReady = d2d && app.ui.Init(d2d, app.renderer.DWrite());
    app.renderer.EndOverlay();
    if (!uiReady) {
        MessageBoxW(window, L"Could not create the Direct2D overlay.", L"XCam", MB_ICONERROR);
        return 1;
    }

    if (!startupLut.empty()) {
        CubeLut lut;
        std::string error;
        if (LoadCubeLut(startupLut, lut, error) && app.renderer.SetLut(lut)) {
            std::string name = startupLut;
            const size_t slash = name.find_last_of("\\/");
            if (slash != std::string::npos) name = name.substr(slash + 1);
            app.shared.lutName = name.size() > 14 ? name.substr(0, 13) + "..." : name;
            app.shared.lutPath = startupLut;
            XCAM_LOG_INFO("LUT loaded: %s", startupLut.c_str());
        } else {
            XCAM_LOG_ERROR("LUT load failed: %s", error.c_str());
        }
    }
    angle.model.logProfile = startupLogProfile;

    // Beacons cost one socket and one thread, and finding a phone before
    // anyone asks is the whole point, so this runs from the start rather than
    // being turned on when a connection fails.
    if (app.discovery.Start()) {
        XCAM_LOG_INFO("listening for phones on UDP %d", kDiscoveryPort);
    } else {
        XCAM_LOG_WARN("device discovery unavailable: %s", app.discovery.LastError().c_str());
    }

    // Minimised, not hidden. A window with no way back to it is a process
    // people find in Task Manager and kill, and the taskbar button is the way
    // back.
    ShowWindow(window, app.startMinimised ? SW_SHOWMINNOACTIVE : SW_SHOW);
    SetTimer(window, kIdleRenderTimer, 8, nullptr);

    // Find out what this machine can decode before offering the choice. HEVC
    // in particular is absent on plenty of Windows installs.
    for (const char* codec : {"h264", "hevc"}) {
        const auto decoders = MfDecoder::ListDecoders(codec);
        bool usable = false;
        for (const auto& decoder : decoders) {
            if (!decoder.async) usable = true;      // async MFTs need an event loop
            XCAM_LOG_INFO("%s decoder: %s (%s%s)", codec, decoder.name.c_str(),
                          decoder.hardware ? "hardware" : "software",
                          decoder.async ? ", async" : "");
        }
        if (usable) app.shared.decodableCodecs.push_back(codec);
        else XCAM_LOG_WARN("no usable %s decoder on this system", codec);
    }
    if (!app.shared.CanDecode(angle.model.codec)) {
        angle.model.codec = app.shared.decodableCodecs.empty() ? "h264"
                                                            : app.shared.decodableCodecs.front();
    }

    if (app.host.empty()) {
        app.adbPath = FindAdb();
        if (app.adbPath.empty()) angle.model.status = "adb not found; set ANDROID_HOME";
    } else {
        angle.model.status = "connecting to " + app.host;
        XCAM_LOG_INFO("connecting over the network to %s:%d", app.host.c_str(), kDefaultPort);
    }

    std::thread worker(StreamThread, std::ref(app), std::ref(angle), window);

    // Every other angle gets a thread of its own. A shared one would mean two
    // phones taking turns to be read, and a socket that is not being read is a
    // send queue filling on the far end.
    for (size_t i = 1; i < app.angles.size(); ++i) {
        app.angles[i].thread =
            std::thread(StreamThread, std::ref(app), std::ref(app.angles[i]), window);
    }
    if (app.angles.size() > 1) app.renderer.SetProgram(app.shared.program);

    std::thread cableWatch(CableWatchThread, std::ref(app));

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    app.quit = true;
    // Unblocks each worker parked in recv. Every angle, not only the program:
    // the one nobody is watching is just as parked as the one on air.
    for (Angle& each : app.angles) each.link.client.Disconnect();
    worker.join();
    for (size_t i = 1; i < app.angles.size(); ++i) {
        if (app.angles[i].thread.joinable()) app.angles[i].thread.join();
    }
    cableWatch.join();

    KillTimer(window, kIdleRenderTimer);
    {
        std::lock_guard<std::mutex> guard(app.lock);
        app.ui.Shutdown();
        app.renderer.Shutdown();
    }
    for (Angle& each : app.angles) each.link.decoder.Close();
    app.discovery.Stop();
    StoreSettings(app);
    app.settings.Save();
    if (app.device) app.device->Release();
    for (Angle& each : app.angles) {
        if (!app.adbPath.empty()) RemoveForward(app.adbPath, each.port, each.link.serial);
    }

    XCAM_LOG_INFO("XCam exiting");
    LogClose();
    CoUninitialize();
    return 0;
}
