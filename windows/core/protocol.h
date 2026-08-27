#pragma once

// Wire format shared with the Android app. docs/PROTOCOL.md is the spec; this
// and android/.../net/Protocol.kt are its two implementations. Keep them in
// lockstep -- tools/dump_stream.py is the third and doubles as a cross-check.

#include <cstdint>
#include <string>
#include <vector>

namespace xcam {

inline constexpr uint16_t kProtocolVersion = 6;
inline constexpr uint16_t kDefaultPort = 27183;

// "XCAM" read back as a little-endian uint32.
inline constexpr uint32_t kPacketMagic = 0x4D414358u;

inline constexpr size_t kHeaderSize = 24;

enum class PacketType : uint8_t {
    Config   = 1,
    KeyFrame = 2,
    Delta    = 3,
    Control  = 4,
    Stats    = 5,
    Audio    = 6,
    Ack      = 7,
    Record   = 8,

    // A chunk of a file being fetched off the phone. Paced by the phone to run
    // only while the link is otherwise idle -- see docs/PROTOCOL.md -- so a
    // transfer never costs the live picture a frame.
    File     = 9,
};

inline constexpr uint8_t kFlagLastFragment = 0x01;

// AUDIO only: this packet is the AudioSpecificConfig rather than a frame.
// Video has a packet type for the same thing because a video CONFIG also means
// "reset your decoder"; audio needs only "here is the codec data".
inline constexpr uint8_t kFlagCodecConfig = 0x02;

// RECORD only: this access unit is a key frame. The streaming path has a packet
// type per kind and needs no flag; recording does, because the receiver is
// muxing rather than decoding and an MP4 must know its sync samples.
inline constexpr uint8_t kFlagKeyFrame = 0x04;

struct PacketHeader {
    uint32_t   magic = kPacketMagic;
    PacketType type  = PacketType::Delta;
    uint8_t    flags = kFlagLastFragment;
    uint16_t   reserved = 0;
    uint32_t   length = 0;
    uint64_t   ptsUs = 0;
    uint32_t   seq = 0;
};

// Parses a 24-byte header. Returns false on a bad magic, which means the stream
// has desynchronised and the only safe move is to drop the connection.
bool ParseHeader(const uint8_t* bytes, PacketHeader& out);

// Serialises into a caller-owned buffer of at least kHeaderSize bytes.
void WriteHeader(const PacketHeader& header, uint8_t* out);

struct Packet {
    PacketHeader header;
    std::vector<uint8_t> payload;

    bool IsVideo() const {
        return header.type == PacketType::KeyFrame || header.type == PacketType::Delta;
    }
    std::string PayloadAsString() const {
        return std::string(reinterpret_cast<const char*>(payload.data()), payload.size());
    }
};

// What the phone reports about itself when a client connects. Parsed out of the
// handshake JSON with a deliberately small hand-rolled reader -- see
// protocol.cpp for why this does not pull in a JSON library.

// One selectable capture mode. A size is not actionable without the frame rate
// it can sustain, so the two always travel together.
struct CaptureMode {
    int width = 0;
    int height = 0;
    int maxFps = 0;
};

struct CameraInfo {
    std::string id;
    std::string facing;
    std::string label;
    int  maxWidth = 0;
    int  maxHeight = 0;
    int  maxFps = 0;
    float zoomMin = 1.0f;
    float zoomMax = 1.0f;
    bool hasTorch = false;
    bool logical = false;

    std::vector<CaptureMode> modes;

    // Manual-control capabilities. Anything the device cannot do must be shown
    // as unavailable rather than offered: the HAL accepts requests it will not
    // honour, so the UI is the only place the distinction can be made.
    bool logProfile = false;        // a flat, gradeable tonemap is available
    bool manualSensor = false;
    bool manualPostProcessing = false;
    int  isoMin = 0, isoMax = 0;
    int64_t exposureMinNs = 0, exposureMaxNs = 0;
    float minFocusDistance = 0.0f;      // dioptres; 0 means fixed focus
    float evMin = 0.0f, evMax = 0.0f, evStep = 0.0f;
    std::vector<std::string> afModes;
    std::vector<std::string> awbModes;

    bool SupportsManualExposure() const { return manualSensor && isoMax > isoMin; }
    bool SupportsManualFocus() const { return minFocusDistance > 0.0f; }
    bool SupportsManualWhiteBalance() const { return manualPostProcessing; }
};

struct DeviceInfo {
    std::string deviceName;
    int androidApi = 0;
    std::vector<CameraInfo> cameras;
    std::vector<std::string> codecs;
    int64_t maxBitrate = 0;

    // Whether the phone can encode a second stream to a local file while it is
    // streaming, and where those files land. The path is reported rather than
    // assumed because `adb pull` needs an absolute one.
    bool recorder = false;
    std::string recordDir;

    // True when this connection joined a session that was already running --
    // the phone lingers for a few seconds after a client vanishes, so a cable
    // pulled out and a reconnection over Wi-Fi land in the same session. A
    // client that answers this with a format command restarts the pipeline and
    // undoes the hand-over it just made.
    bool resumed = false;

    // Sound. `available` means the microphone permission is actually held, not
    // merely that the hardware exists -- a control offered on the strength of
    // the hardware would be offering what the user has already declined.
    bool audioAvailable = false;
    int audioSampleRate = 0;
    int audioChannels = 0;

    std::string rawJson;
};

bool ParseHandshakeJson(const std::string& json, DeviceInfo& out);

// Builds the JSON payload for a CONTROL packet. Field order follows the spec.
std::string MakeSetCommand(const std::string& cameraId, int width, int height,
                           int fps, int bitrate, const std::string& codec);
std::string MakeSimpleCommand(const std::string& cmd);

std::string MakeExposureCommand(bool manual, int iso, int64_t shutterNs);
std::string MakeFocusCommand(const std::string& mode, float distance,
                             float x = 0.5f, float y = 0.5f);
std::string MakeWhiteBalanceCommand(const std::string& mode, int kelvin);
std::string MakePictureProfileCommand(bool log);
std::string MakeZoomCommand(float ratio);
std::string MakeTorchCommand(bool on);
std::string MakeEvCommand(float value);
std::string MakeAudioCommand(bool enabled);

// "start" or "stop"; both take effect immediately and never interrupt the
// stream.
std::string MakeRecordCommand(const std::string& action);

// Changes the recording format, which restarts the pipeline because the
// recorder's surface size is fixed when the capture session is configured.
//
// `enabled` false takes the recording encoder out of the session altogether.
// Having one ready costs a few milliseconds of stream latency even while it is
// idle, so anyone who only wants a webcam should be able to stop paying it.
//
// `toPc` decides where the file is written: here, muxed from RECORD and AUDIO
// packets, or on the phone to be collected afterwards. Neither is better in
// general -- the phone needs nothing from the link, which is what makes it the
// answer over Wi-Fi.
std::string MakeRecordConfigCommand(bool enabled, bool toPc, int width = 0, int height = 0,
                                    int fps = 0, int bitrate = 0,
                                    const std::string& codec = "", int preRollSeconds = 0);

// Parsed ACK. `applied` is authoritative: the phone clamps values it cannot
// deliver -- a shutter longer than the frame interval, most visibly -- and
// reports the result here rather than failing.
struct Ack {
    bool ok = false;
    std::string cmd;
    std::string error;
    std::string appliedJson;
};

bool ParseAck(const std::string& json, Ack& out);

// Reads one string out of a flat `applied` block, undoing the escapes the
// phone's JSON writer emits. org.json escapes every forward slash as \\/, so a
// file path read without this arrives full of backslashes and is useless to
// show or to pass to adb.
bool ReadAppliedString(const std::string& json, const char* key, std::string& out);

// The same for a number. The `applied` block is a handful of scalars, so a full
// parse would cost more than it explains.
bool ReadAppliedNumber(const std::string& json, const char* key, double& out);

// The once-a-second report from the phone. Only the fields the desktop acts on
// are pulled out; the rest of the payload is left in the log.
// One recording sitting on the phone.
struct TakeInfo {
    std::string name;
    int64_t bytes = 0;
    int64_t durationMs = 0;
    int64_t modified = 0;        // unix seconds, phone clock
};

// Reads the `takes` ACK. `dir` comes back as the absolute directory they are
// in, which is the path an `adb pull` needs.
bool ParseTakes(const std::string& appliedJson, std::vector<TakeInfo>& out,
                std::string& dir);

// `action` is "list", "delete", "fetch" or "cancel"; `name` is a bare file name
// for the last three, never a path -- the phone resolves it inside its own
// recordings directory and refuses anything that escapes it.
std::string MakeTakesCommand(const std::string& action, const std::string& name = "");

// Tells the phone whether an application actually has the camera open, so it
// can show a tally light. Not the same as "the desktop is connected": the
// desktop can be running with nobody looking through it.
std::string MakeTallyCommand(bool live);

// Moves a control from where it is to `target`, taking `ms` about it. `what` is
// "focus" (in dioptres) or "zoom" (as a ratio).
//
// An intention, not a stream of positions. The curve runs on the phone, because
// a desktop sending sixty positions a second would put a focus pull at the mercy
// of the link -- one late packet and the move stutters where no easing can hide
// it. Sent this way it cannot stutter: nothing arrives while it happens.
std::string MakeRampCommand(const std::string& what, float target, int ms);

// Frames the picture inside the sensor. All four values are fractions of the
// active array; passing nothing hands the sensor back.
//
// The desktop decides the framing because it has the decoded picture to look
// at; the phone does the cropping because its sensor is larger than the stream,
// so a crop there costs nothing. Cropping a decoded copy here instead would
// throw away resolution to reach the same framing, and pay an encode for it.
std::string MakeFramingCommand(float x, float y, float w, float h);
std::string MakeFramingOffCommand();

struct StatsInfo {
    double actualFps = 0;
    double actualBitrateBps = 0;
    int droppedFrames = 0;
    int battery = -1;
    std::string thermal;

    // The microphone's loudest sample since the last tick, 0..1. Reported every
    // tick whether or not anything is listening, because the whole point is
    // that a dead microphone should look different from a quiet room.
    float audioPeak = 0.0f;

    bool recording = false;
    int64_t recordMs = 0;
    int64_t recordBytes = 0;

    // The pre-roll ring: how much it is armed for, and how much it is actually
    // holding. Reported every tick rather than only when it changes, because
    // the phone can disarm it on its own when it gets hot.
    int64_t preRollArmedMs = 0;
    int64_t preRollFillMs = 0;
    int64_t storageFreeMb = -1;

    // Which way up the phone is being held: 0, 90, 180 or 270, and -1 when it
    // has not said. The sensor does not turn with the body, so a phone held
    // upright still sends a landscape frame with the scene on its side; this is
    // how the desktop knows it can stand that up and use the whole sensor
    // rather than cropping a tall slice out of the middle.
    int surfaceRotation = -1;
};

bool ParseStats(const std::string& json, StatsInfo& out);

}  // namespace xcam
