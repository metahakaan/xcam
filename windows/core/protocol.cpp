#include "core/protocol.h"

#include <charconv>
#include <cstring>
#include <sstream>

namespace xcam {
namespace {

template <typename T>
T ReadLE(const uint8_t* p) {
    T value{};
    std::memcpy(&value, p, sizeof(T));   // x86/ARM64 Windows are little-endian
    return value;
}

template <typename T>
void WriteLE(uint8_t* p, T value) {
    std::memcpy(p, &value, sizeof(T));
}

// ---- minimal JSON reading ---------------------------------------------------
//
// The handshake is the only JSON this side ever parses, it comes from a peer we
// built ourselves, and its shape is pinned by docs/PROTOCOL.md. A dependency
// would have to be vendored into a project whose one hard requirement is that
// it builds from a clean checkout with nothing but MSVC and the Windows SDK, so
// these few hundred bytes of scanning earn their place.
//
// Deliberately not general: no escapes beyond \" and no nested-object recursion
// past what the handshake actually contains. Anything it cannot read comes back
// as the caller's default rather than an error, because a missing capability
// field should degrade the UI, not refuse the connection.

size_t SkipWhitespace(const std::string& s, size_t i) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
    return i;
}

// Finds "key" at the top level of the object starting at `from`, returning the
// index just past the colon.
size_t FindKey(const std::string& s, const std::string& key, size_t from = 0) {
    const std::string needle = "\"" + key + "\"";
    int depth = 0;
    bool inString = false;

    for (size_t i = from; i < s.size(); ++i) {
        const char c = s[i];
        if (inString) {
            if (c == '\\') { ++i; continue; }
            if (c == '"') inString = false;
            continue;
        }
        if (c == '"') {
            if (depth <= 1 && s.compare(i, needle.size(), needle) == 0) {
                size_t j = SkipWhitespace(s, i + needle.size());
                if (j < s.size() && s[j] == ':') return SkipWhitespace(s, j + 1);
            }
            inString = true;
            continue;
        }
        if (c == '{' || c == '[') ++depth;
        else if (c == '}' || c == ']') {
            if (--depth < 0) return std::string::npos;
        }
    }
    return std::string::npos;
}

std::string ReadString(const std::string& s, size_t at) {
    if (at == std::string::npos || at >= s.size() || s[at] != '"') return {};
    std::string out;
    for (size_t i = at + 1; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) { out += s[++i]; continue; }
        if (s[i] == '"') break;
        out += s[i];
    }
    return out;
}

double ReadNumber(const std::string& s, size_t at, double fallback) {
    if (at == std::string::npos || at >= s.size()) return fallback;
    size_t end = at;
    while (end < s.size() && (std::isdigit(static_cast<unsigned char>(s[end])) ||
                              s[end] == '-' || s[end] == '+' || s[end] == '.' ||
                              s[end] == 'e' || s[end] == 'E')) ++end;
    if (end == at) return fallback;
    try {
        return std::stod(s.substr(at, end - at));
    } catch (...) {
        return fallback;
    }
}

bool ReadBool(const std::string& s, size_t at, bool fallback) {
    if (at == std::string::npos) return fallback;
    if (s.compare(at, 4, "true") == 0) return true;
    if (s.compare(at, 5, "false") == 0) return false;
    return fallback;
}

// Returns the substring covering the array or object beginning at `at`.
std::string ReadSpan(const std::string& s, size_t at) {
    if (at == std::string::npos || at >= s.size()) return {};
    const char open = s[at];
    const char close = (open == '[') ? ']' : (open == '{') ? '}' : '\0';
    if (close == '\0') return {};

    int depth = 0;
    bool inString = false;
    for (size_t i = at; i < s.size(); ++i) {
        const char c = s[i];
        if (inString) {
            if (c == '\\') { ++i; continue; }
            if (c == '"') inString = false;
            continue;
        }
        if (c == '"') { inString = true; continue; }
        if (c == open) ++depth;
        else if (c == close && --depth == 0) return s.substr(at, i - at + 1);
    }
    return {};
}

// Splits the top-level elements of an array span (including its brackets).
std::vector<std::string> SplitArray(const std::string& span) {
    std::vector<std::string> out;
    if (span.size() < 2) return out;

    int depth = 0;
    bool inString = false;
    size_t start = 1;

    for (size_t i = 1; i + 1 < span.size(); ++i) {
        const char c = span[i];
        if (inString) {
            if (c == '\\') { ++i; continue; }
            if (c == '"') inString = false;
            continue;
        }
        if (c == '"') { inString = true; continue; }
        if (c == '{' || c == '[') ++depth;
        else if (c == '}' || c == ']') --depth;
        else if (c == ',' && depth == 0) {
            out.push_back(span.substr(start, i - start));
            start = i + 1;
        }
    }
    if (start + 1 <= span.size()) {
        std::string tail = span.substr(start, span.size() - 1 - start);
        if (SkipWhitespace(tail, 0) < tail.size()) out.push_back(tail);
    }
    return out;
}

std::pair<int, int> ReadIntPair(const std::string& s, size_t at) {
    const std::string span = ReadSpan(s, at);
    const auto parts = SplitArray(span);
    if (parts.size() < 2) return {0, 0};
    return {static_cast<int>(ReadNumber(parts[0], SkipWhitespace(parts[0], 0), 0)),
            static_cast<int>(ReadNumber(parts[1], SkipWhitespace(parts[1], 0), 0))};
}

}  // namespace

bool ParseHeader(const uint8_t* bytes, PacketHeader& out) {
    out.magic = ReadLE<uint32_t>(bytes);
    if (out.magic != kPacketMagic) return false;

    out.type     = static_cast<PacketType>(bytes[4]);
    out.flags    = bytes[5];
    out.reserved = ReadLE<uint16_t>(bytes + 6);
    out.length   = ReadLE<uint32_t>(bytes + 8);
    out.ptsUs    = ReadLE<uint64_t>(bytes + 12);
    out.seq      = ReadLE<uint32_t>(bytes + 20);
    return true;
}

void WriteHeader(const PacketHeader& header, uint8_t* out) {
    WriteLE<uint32_t>(out, header.magic);
    out[4] = static_cast<uint8_t>(header.type);
    out[5] = header.flags;
    WriteLE<uint16_t>(out + 6, header.reserved);
    WriteLE<uint32_t>(out + 8, header.length);
    WriteLE<uint64_t>(out + 12, header.ptsUs);
    WriteLE<uint32_t>(out + 20, header.seq);
}

bool ParseHandshakeJson(const std::string& json, DeviceInfo& out) {
    out.rawJson = json;
    out.deviceName = ReadString(json, FindKey(json, "deviceName"));
    out.androidApi = static_cast<int>(ReadNumber(json, FindKey(json, "androidApi"), 0));
    out.maxBitrate = static_cast<int64_t>(ReadNumber(json, FindKey(json, "maxBitrate"), 0));
    out.recorder = ReadBool(json, FindKey(json, "recorder"), false);
    out.recordDir = ReadString(json, FindKey(json, "recordDir"));
    out.resumed = ReadBool(json, FindKey(json, "resumed"), false);

    const std::string audio = ReadSpan(json, FindKey(json, "audio"));
    if (!audio.empty()) {
        out.audioAvailable = ReadBool(audio, FindKey(audio, "available"), false);
        out.audioSampleRate =
            static_cast<int>(ReadNumber(audio, FindKey(audio, "sampleRate"), 0));
        out.audioChannels = static_cast<int>(ReadNumber(audio, FindKey(audio, "channels"), 0));
    }

    for (const std::string& entry : SplitArray(ReadSpan(json, FindKey(json, "codecs")))) {
        std::string codec = ReadString(entry, SkipWhitespace(entry, 0));
        if (!codec.empty()) out.codecs.push_back(std::move(codec));
    }

    for (const std::string& entry : SplitArray(ReadSpan(json, FindKey(json, "cameras")))) {
        const size_t begin = SkipWhitespace(entry, 0);
        const std::string cam = ReadSpan(entry, begin);
        if (cam.empty()) continue;

        CameraInfo info;
        info.id       = ReadString(cam, FindKey(cam, "id"));
        info.facing   = ReadString(cam, FindKey(cam, "facing"));
        info.label    = ReadString(cam, FindKey(cam, "label"));
        info.maxFps   = static_cast<int>(ReadNumber(cam, FindKey(cam, "maxFps"), 0));
        info.hasTorch = ReadBool(cam, FindKey(cam, "hasTorch"), false);
        info.logical  = ReadBool(cam, FindKey(cam, "logical"), false);

        std::tie(info.maxWidth, info.maxHeight) = ReadIntPair(cam, FindKey(cam, "maxRes"));

        const std::string zoomSpan = ReadSpan(cam, FindKey(cam, "zoomRange"));
        const auto zoom = SplitArray(zoomSpan);
        if (zoom.size() >= 2) {
            info.zoomMin = static_cast<float>(ReadNumber(zoom[0], SkipWhitespace(zoom[0], 0), 1.0));
            info.zoomMax = static_cast<float>(ReadNumber(zoom[1], SkipWhitespace(zoom[1], 0), 1.0));
        }

        info.logProfile = ReadBool(cam, FindKey(cam, "logProfile"), false);
        info.manualSensor = ReadBool(cam, FindKey(cam, "manualSensor"), false);
        info.manualPostProcessing = ReadBool(cam, FindKey(cam, "manualPostProcessing"), false);
        info.minFocusDistance =
            static_cast<float>(ReadNumber(cam, FindKey(cam, "minFocusDistance"), 0.0));
        info.evStep = static_cast<float>(ReadNumber(cam, FindKey(cam, "evStep"), 0.0));

        std::tie(info.isoMin, info.isoMax) = ReadIntPair(cam, FindKey(cam, "isoRange"));

        const auto exposure = SplitArray(ReadSpan(cam, FindKey(cam, "exposureRangeNs")));
        if (exposure.size() >= 2) {
            info.exposureMinNs = static_cast<int64_t>(
                ReadNumber(exposure[0], SkipWhitespace(exposure[0], 0), 0));
            info.exposureMaxNs = static_cast<int64_t>(
                ReadNumber(exposure[1], SkipWhitespace(exposure[1], 0), 0));
        }

        const auto ev = SplitArray(ReadSpan(cam, FindKey(cam, "evRange")));
        if (ev.size() >= 2) {
            info.evMin = static_cast<float>(ReadNumber(ev[0], SkipWhitespace(ev[0], 0), 0.0));
            info.evMax = static_cast<float>(ReadNumber(ev[1], SkipWhitespace(ev[1], 0), 0.0));
        }

        for (const std::string& item : SplitArray(ReadSpan(cam, FindKey(cam, "afModes")))) {
            std::string mode = ReadString(item, SkipWhitespace(item, 0));
            if (!mode.empty()) info.afModes.push_back(std::move(mode));
        }
        for (const std::string& item : SplitArray(ReadSpan(cam, FindKey(cam, "awbModes")))) {
            std::string mode = ReadString(item, SkipWhitespace(item, 0));
            if (!mode.empty()) info.awbModes.push_back(std::move(mode));
        }

        for (const std::string& item : SplitArray(ReadSpan(cam, FindKey(cam, "modes")))) {
            const std::string modeJson = ReadSpan(item, SkipWhitespace(item, 0));
            if (modeJson.empty()) continue;
            CaptureMode mode;
            std::tie(mode.width, mode.height) = ReadIntPair(modeJson, FindKey(modeJson, "size"));
            mode.maxFps = static_cast<int>(ReadNumber(modeJson, FindKey(modeJson, "maxFps"), 0));
            if (mode.width > 0 && mode.height > 0) info.modes.push_back(mode);
        }

        out.cameras.push_back(std::move(info));
    }

    return !out.cameras.empty();
}

std::string MakeSetCommand(const std::string& cameraId, int width, int height,
                           int fps, int bitrate, const std::string& codec) {
    std::ostringstream os;
    os << R"({"cmd":"set")";
    if (!cameraId.empty()) os << R"(,"camera":")" << cameraId << '"';
    if (width > 0)   os << R"(,"width":)"   << width;
    if (height > 0)  os << R"(,"height":)"  << height;
    if (fps > 0)     os << R"(,"fps":)"     << fps;
    if (bitrate > 0) os << R"(,"bitrate":)" << bitrate;
    if (!codec.empty()) os << R"(,"codec":")" << codec << '"';
    os << '}';
    return os.str();
}

std::string MakeSimpleCommand(const std::string& cmd) {
    return R"({"cmd":")" + cmd + R"("})";
}

std::string MakeExposureCommand(bool manual, int iso, int64_t shutterNs) {
    if (!manual) return R"({"cmd":"exposure","mode":"auto"})";
    std::ostringstream os;
    os << R"({"cmd":"exposure","mode":"manual","iso":)" << iso
       << R"(,"shutterNs":)" << shutterNs << '}';
    return os.str();
}

std::string MakeFocusCommand(const std::string& mode, float distance, float x, float y) {
    std::ostringstream os;
    os << R"({"cmd":"focus","mode":")" << mode << '"';
    if (mode == "manual") os << R"(,"distance":)" << distance;
    if (mode == "tap")    os << R"(,"x":)" << x << R"(,"y":)" << y;
    os << '}';
    return os.str();
}

std::string MakeWhiteBalanceCommand(const std::string& mode, int kelvin) {
    std::ostringstream os;
    os << R"({"cmd":"wb","mode":")" << mode << '"';
    if (mode == "manual") os << R"(,"temperature":)" << kelvin;
    os << '}';
    return os.str();
}

std::string MakePictureProfileCommand(bool log) {
    return log ? R"({"cmd":"profile","mode":"log"})"
               : R"({"cmd":"profile","mode":"standard"})";
}

std::string MakeZoomCommand(float ratio) {
    std::ostringstream os;
    os << R"({"cmd":"zoom","ratio":)" << ratio << '}';
    return os.str();
}

std::string MakeTorchCommand(bool on) {
    return on ? R"({"cmd":"torch","on":true})" : R"({"cmd":"torch","on":false})";
}

std::string MakeEvCommand(float value) {
    std::ostringstream os;
    os << R"({"cmd":"ev","value":)" << value << '}';
    return os.str();
}

std::string MakeAudioCommand(bool enabled) {
    return enabled ? R"({"cmd":"audio","enabled":true})"
                   : R"({"cmd":"audio","enabled":false})";
}

std::string MakeRecordCommand(const std::string& action) {
    return R"({"cmd":"record","action":")" + action + R"("})";
}

std::string MakeRecordConfigCommand(bool enabled, bool toPc, int width, int height,
                                    int fps, int bitrate, const std::string& codec,
                                    int preRollSeconds) {
    std::ostringstream os;
    os << R"({"cmd":"record","action":"config","enabled":)"
       << (enabled ? "true" : "false")
       << R"(,"target":")" << (toPc ? "pc" : "phone") << '"';
    // Always sent, both of them, including zero.
    //
    // An omitted field means "keep what you have" to the phone, and zero here
    // means "the camera at its best" -- so leaving them out made the best size
    // impossible to ask for once any size had ever been set. The phone went on
    // recording at whatever it had been told last, through restarts, which is
    // most of what "stuck at 720p" was.
    os << R"(,"width":)" << (width > 0 ? width : 0);
    os << R"(,"height":)" << (height > 0 ? height : 0);
    if (fps > 0)     os << R"(,"fps":)"     << fps;
    if (bitrate > 0) os << R"(,"bitrate":)" << bitrate;
    if (!codec.empty()) os << R"(,"codec":")" << codec << '"';
    // Always sent, unlike the rest: zero is a meaningful value here -- it is how
    // the ring is disarmed -- so omitting it would leave it armed.
    os << R"(,"preroll":)" << (preRollSeconds > 0 ? preRollSeconds : 0);
    os << '}';
    return os.str();
}

bool ParseTakes(const std::string& appliedJson, std::vector<TakeInfo>& out,
                std::string& dir) {
    out.clear();
    dir = ReadString(appliedJson, FindKey(appliedJson, "dir"));

    const std::string span = ReadSpan(appliedJson, FindKey(appliedJson, "takes"));
    if (span.empty()) return false;

    for (const std::string& entry : SplitArray(span)) {
        TakeInfo take;
        take.name = ReadString(entry, FindKey(entry, "name"));
        if (take.name.empty()) continue;
        take.bytes = static_cast<int64_t>(ReadNumber(entry, FindKey(entry, "bytes"), 0));
        take.durationMs =
            static_cast<int64_t>(ReadNumber(entry, FindKey(entry, "durationMs"), 0));
        take.modified = static_cast<int64_t>(ReadNumber(entry, FindKey(entry, "modified"), 0));
        out.push_back(std::move(take));
    }
    return true;
}

std::string MakeTakesCommand(const std::string& action, const std::string& name) {
    std::ostringstream os;
    os << R"({"cmd":"takes","action":")" << action << '"';
    if (!name.empty()) os << R"(,"name":")" << name << '"';
    os << '}';
    return os.str();
}

std::string MakeTallyCommand(bool live) {
    return std::string(R"({"cmd":"tally","live":)") + (live ? "true" : "false") + "}";
}

std::string MakeRampCommand(const std::string& what, float target, int ms) {
    std::ostringstream os;
    os << R"({"cmd":"ramp","what":")" << what << R"(","to":)" << target
       << R"(,"ms":)" << ms << '}';
    return os.str();
}

std::string MakeFramingCommand(float x, float y, float w, float h) {
    std::ostringstream os;
    os << R"({"cmd":"framing","x":)" << x << R"(,"y":)" << y
       << R"(,"w":)" << w << R"(,"h":)" << h << '}';
    return os.str();
}

std::string MakeFramingOffCommand() {
    return R"({"cmd":"framing","off":true})";
}

bool ParseStats(const std::string& json, StatsInfo& out) {
    if (json.find("actualFps") == std::string::npos &&
        json.find("recording") == std::string::npos) {
        return false;
    }
    out.actualFps = ReadNumber(json, FindKey(json, "actualFps"), 0);
    out.actualBitrateBps = ReadNumber(json, FindKey(json, "actualBitrate"), 0);
    out.droppedFrames = static_cast<int>(ReadNumber(json, FindKey(json, "droppedFrames"), 0));
    out.battery = static_cast<int>(ReadNumber(json, FindKey(json, "battery"), -1));
    out.thermal = ReadString(json, FindKey(json, "thermal"));
    out.recording = ReadBool(json, FindKey(json, "recording"), false);
    out.recordMs = static_cast<int64_t>(ReadNumber(json, FindKey(json, "recordMs"), 0));
    out.recordBytes = static_cast<int64_t>(ReadNumber(json, FindKey(json, "recordBytes"), 0));
    out.storageFreeMb = static_cast<int64_t>(ReadNumber(json, FindKey(json, "storageFreeMb"), -1));
    out.surfaceRotation = static_cast<int>(ReadNumber(json, FindKey(json, "surfaceRotation"), -1));
    out.audioPeak = static_cast<float>(ReadNumber(json, FindKey(json, "audioPeak"), 0.0));
    out.preRollArmedMs =
        static_cast<int64_t>(ReadNumber(json, FindKey(json, "prerollArmedMs"), 0));
    out.preRollFillMs =
        static_cast<int64_t>(ReadNumber(json, FindKey(json, "prerollFillMs"), 0));
    return true;
}

bool ReadAppliedString(const std::string& json, const char* key, std::string& out) {
    const size_t at = FindKey(json, key);
    if (at == std::string::npos) return false;
    const std::string value = ReadString(json, at);
    if (value.empty()) return false;
    out = value;
    return true;
}

bool ReadAppliedNumber(const std::string& json, const char* key, double& out) {
    const size_t at = FindKey(json, key);
    if (at == std::string::npos) return false;
    out = ReadNumber(json, at, 0.0);
    return true;
}

bool ParseAck(const std::string& json, Ack& out) {
    out.ok = ReadBool(json, FindKey(json, "ok"), false);
    out.cmd = ReadString(json, FindKey(json, "cmd"));
    out.error = ReadString(json, FindKey(json, "error"));
    out.appliedJson = ReadSpan(json, FindKey(json, "applied"));
    return !out.cmd.empty();
}

}  // namespace xcam
