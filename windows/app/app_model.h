#pragma once

// What belongs to the application rather than to a camera.
//
// These two used to be one struct, which was fine while there was one phone.
// A second angle makes the difference load-bearing: two cameras have two
// exposures, two formats, two recordings and two sets of statistics, but there
// is only ever one recordings folder, one virtual camera, one desk microphone
// and one set of presets. Held together, adding a camera would either duplicate
// the settings -- so changing the folder on angle B leaves angle A writing
// somewhere else -- or leave sixty places quietly writing one camera's state
// through whichever model came to hand.
//
// The rule for deciding where a field goes: if plugging in a second phone
// should give it its own copy, it is a camera's. If the answer would be the
// same whichever angle is on air, it is the application's.

#include <cstdint>
#include <string>
#include <vector>
#include <algorithm>

namespace xcam {

// The shape of what leaves this machine.
//
// Not the shape of what arrives: the phone goes on sending the sensor's native
// 16:9 whatever this says, and the crop or the rotation happens here. That is
// deliberate -- the published size has always been independent of the capture
// size, because a DirectShow pin settles its format when a consumer connects
// and cannot renegotiate afterwards, and that seam is exactly what a vertical
// mode needs.
enum class Shape { Wide, Vertical, Square };

// The aspect ratio each one asks for, and the size it publishes at.
inline float AspectOf(Shape shape) {
    return shape == Shape::Vertical ? 9.0f / 16.0f
         : shape == Shape::Square   ? 1.0f
                                    : 16.0f / 9.0f;
}

inline void SizeOf(Shape shape, uint32_t& width, uint32_t& height) {
    if (shape == Shape::Vertical)    { width = 1080; height = 1920; }
    else if (shape == Shape::Square) { width = 1080; height = 1080; }
    else                             { width = 1920; height = 1080; }
}

// How the desktop should look for the phone. Automatic is right for one person
// with one cable; the other two exist because someone who works over Wi-Fi does
// not want a cable plugged in for charging to silently take over, and someone
// on USB does not want to wait on a network search.
enum class Transport { Auto, Usb, WiFi };

// A named snapshot of everything that decides how the picture looks. Format is
// deliberately not in it: a preset is a look, and applying one should not
// restart the stream.
struct Preset {
    std::string name;

    bool exposureManual = false;
    int iso = 400;
    int64_t shutterNs = 16'666'666;
    bool focusManual = false;
    float focusDistance = 0.0f;
    std::string wbMode = "auto";
    int wbKelvin = 5600;
    float ev = 0.0f;
    bool logProfile = false;
    float zoom = 1.0f;
    std::string lutPath;
    float matte = 0.0f;

    bool Empty() const { return name.empty(); }
};

// Four is enough to hold a lit setup, a daylight one, a log one and a spare,
// and few enough to pick from without reading.
inline constexpr size_t kPresetSlots = 4;

// One angle, as far as the panel needs to know.
//
// The panel draws one camera and chooses which; it has no business reaching
// into another angle's exposure. So the rig reaches it as a list of these --
// enough to draw a button and know whether cutting to it would show anything.
struct AngleSummary {
    std::string label;         // the phone's name, or "CAM 2" before it says
    bool connected = false;
    bool hasPicture = false;   // has ever decoded a frame, so a cut lands on one
};

struct AppModel {
    // ---- which angle is on air ---------------------------------------------
    //
    // The index into the rig's cameras that the virtual camera publishes and
    // the panel's controls address. With one camera it is always 0, and every
    // path that reads it behaves exactly as it did before there was a second.
    size_t program = 0;

    // Refilled each frame from the rig. A vector rather than a count because
    // the button has to say which phone it cuts to, and "CAM 2" is a worse
    // label than the name the phone gave.
    std::vector<AngleSummary> rig;

    // ---- the virtual camera ------------------------------------------------

    // Whether frames are being published to the virtual camera. On by default:
    // being a webcam is what the application is for.
    bool virtualCamera = true;

    // Fixed, and deliberately independent of the capture resolution. A
    // DirectShow pin settles its format when the application connects and
    // cannot change it afterwards in any way most applications honour, so a
    // published size that followed capture would black out an open call the
    // moment someone switched to 4K -- and stay black on the way back.
    //
    // It is also what makes a cut possible. Two angles at different capture
    // sizes both arrive at this one published size, so switching between them
    // is a change of texture rather than a renegotiation nothing downstream
    // would survive.
    uint32_t virtualCameraWidth = 1920;
    uint32_t virtualCameraHeight = 1080;

    // Wide for a call, vertical for the places that only take vertical.
    //
    // Changing it re-opens the shared section at a new size, and a consumer
    // already connected negotiated the old one -- so its picture squashes until
    // it reconnects. There is no way round that: it is what a DirectShow pin
    // is. The control says so.
    Shape shape = Shape::Wide;

    // What the phone last said about which way up it is: 0, 90, 180 or 270.
    // -1 means it has never said, which is every phone until the app on it is
    // new enough.
    int surfaceRotation = -1;

    // Turning the frame upright uses the whole sensor, where cropping a tall
    // slice out of a wide one throws two thirds of it away -- so it is only
    // worth doing when the phone is actually being held upright. Off means
    // crop, whatever the phone reports.
    bool autoRotate = true;

    // What the renderer should be told: 0 none, 1 clockwise, 2 anticlockwise.
    int RotationForShape() const {
        if (shape != Shape::Vertical || !autoRotate) return 0;
        if (surfaceRotation == 90) return 1;
        if (surfaceRotation == 270) return 2;
        return 0;
    }

    // True while an application is actually consuming the virtual camera, which
    // is not the same as the desktop being connected. Sent to the phone so it
    // can show a tally light -- to the phone on program, which is the whole
    // point of a tally: the one with the light on is the one being seen.
    bool tally = false;

    // ---- where things are found and put ------------------------------------

    Transport transport = Transport::Auto;

    // The address to use when Wi-Fi is chosen. One of them, still: a rig of
    // several phones over Wi-Fi wants one of these per angle, and that is a
    // discovery problem rather than a settings one.
    std::string host;

    // The six digits the phone shows when it is allowing Wi-Fi connections.
    //
    // Kept, not asked for each time: a phone paired once should stay paired.
    // Never sent over USB -- the phone does not ask there, because reaching its
    // loopback already means holding the cable.
    std::string pairCode;

    // Empty means the default, %USERPROFILE%\Videos\XCam.
    std::string recordFolder;

    // Whether XCam starts with Windows. Read from the registry at startup
    // rather than from the settings file: the registry is where the fact
    // actually lives, and a file that disagreed with it would be the one people
    // believed.
    bool autostart = false;

    // ---- the look ----------------------------------------------------------

    // The cinematic matte, as a target aspect ratio: 2.39 and 2.35 are scope,
    // 1.85 is flat, 0 is off. A framing choice rather than a format one -- the
    // frame stays 16:9 and the rows outside the ratio go black, which is what a
    // 2.35:1 delivery looks like on a 16:9 screen.
    //
    // Shared across angles on purpose: a cut that changed the shape of the
    // frame is not a cut anybody wants.
    float matte = 0.0f;

    // Loaded grade, applied to the preview only -- the stream stays flat.
    // The name is what the button shows; the path is what a later run needs to
    // load the same grade again without asking.
    std::string lutName;
    std::string lutPath;
    float lutAmount = 1.0f;

    // Monitoring aids. `zebra` is the luma the stripes start at -- 0.70 is where
    // skin sits at a sane exposure, 0.95 is about to clip -- and `peaking` is
    // the edge strength that counts as sharp. Zero is off for both.
    //
    // Preview only, always. An overlay written into a recording is a stripe
    // somebody has to explain later.
    float zebra = 0.0f;
    float peaking = 0.0f;

    // The grade this side applies, on top of whatever the LUT does.
    //
    // Unlike the aids above, these reach everything: the preview, the virtual
    // camera and a graded recording all go through the same shader, so a call
    // sees what the operator sees. The phone's own full-quality file stays flat
    // -- a look baked into the master cannot be taken out again.
    //
    // All four are 0 for neutral. `gain` is in stops; the rest run -1 to +1.
    float gain = 0.0f;
    float contrast = 0.0f;
    float saturation = 0.0f;
    float warmth = 0.0f;

    bool GradeIsNeutral() const {
        return gain == 0.0f && contrast == 0.0f && saturation == 0.0f && warmth == 0.0f;
    }

    // Bake the loaded LUT into the recording. Only possible on the PC side, and
    // only by decoding and re-encoding here, which is why it is a choice rather
    // than the default. It records what is published, so with several angles it
    // records the cut.
    bool recordGraded = false;

    Preset presets[kPresetSlots];

    // ---- this machine ------------------------------------------------------

    // The microphone plugged into this machine, recorded as a second track
    // beside the phone's. Nobody serious records a shoot on a phone's
    // microphone; everybody wants it in the file anyway, because it is the
    // track that carries sync.
    //
    // One of them, however many cameras there are: it is a microphone in a
    // room, not a property of an angle.
    bool deskMic = false;
    std::string deskMicId;
    std::string deskMicName;
    float deskMicPeak = 0.0f;
    float deskMicHold = 0.0f;

    // Codecs this PC can actually decode. The phone advertising HEVC says
    // nothing about whether Windows can play it back: the stock HEVC decoder
    // ships as a Store extension that is absent on plenty of machines, and
    // picking it there produces a silent black picture. The panel dims what
    // cannot work rather than letting someone choose it and wonder.
    // ---- the teleprompter --------------------------------------------------
    //
    // Preview only, and that is not a rule anybody has to enforce: the prompter
    // is drawn in the Direct2D overlay, which lands on the back buffer after
    // the video, while the virtual camera and the recording read the decoded
    // texture. Nothing that reaches a call or a file can see it.
    bool prompterOn = false;
    std::string prompterPath;
    std::string prompterText;

    // Points per second. Slow enough to read aloud is somewhere around 40.
    float prompterSpeed = 40.0f;
    float prompterSize = 34.0f;

    // For a beam splitter, which reverses everything in front of it.
    bool prompterMirror = false;

    // How much of the window's width the column takes. A line of text wider
    // than about sixty characters is one the eye loses its place in.
    float prompterWidth = 0.55f;

    // Where the script has scrolled to, and whether it is moving. Runtime, not
    // a setting -- coming back to a script should come back to the top.
    float prompterOffset = 0.0f;
    bool prompterRunning = false;
    float prompterExtent = 0.0f;      // the whole block's height, measured

    bool HasScript() const { return !prompterText.empty(); }

    std::vector<std::string> decodableCodecs;

    bool CanDecode(const std::string& name) const {
        return std::find(decodableCodecs.begin(), decodableCodecs.end(), name) !=
               decodableCodecs.end();
    }
};

}  // namespace xcam
