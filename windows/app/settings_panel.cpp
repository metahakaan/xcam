#include "app/settings_panel.h"

#include "core/wasapi_capture.h"

#include "app/strings.h"
#include "core/protocol.h"
#include "core/settings.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace xcam {
namespace {

using namespace xcam::ui;

// Ids for this sheet, well clear of the panel's own blocks.
enum Id : int {
    kIdClose = 900,
    kIdLangBase = 910,
    kIdTransportBase = 920,
    kIdHost = 930,
    kIdPairCode = 931,
    kIdFolder = 940,
    kIdFolderReset = 941,
    kIdIntervalBase = 950,
    kIdGraded = 960,

    // The look block, moved out of the way and spaced.
    //
    // These overlapped: kIdGraded and kIdZebraBase were both 960, so the first
    // zebra level shared an id with "apply the look to recordings"; matte slots
    // 1 to 3 sat on peaking slots 1 to 3; and kIdFlipX sat on matte slot 3.
    // Two controls with one id are one control as far as the hit test is
    // concerned, so clicking a matte option could set a peaking level.
    kIdZebraBase = 1100,
    kIdPeakBase = 1110,
    kIdMatteBase = 1120,
    kIdFlipX = 1130,
    kIdFlipY = 1131,
    kIdPreRollBase = 990,
    kIdBrowseTakes = 999,
    kIdAutostart = 1000,
    kIdDeskMic = 1001,
    kIdDeskMicDevice = 1002,
    kIdPresetBase = 970,
    kIdPresetSaveBase = 980,
};

constexpr float kRowHeight = 46.0f;
constexpr float kSectionGap = 18.0f;

std::string Format(const char* pattern, int value) {
    char buffer[128];
    std::snprintf(buffer, sizeof(buffer), pattern, value);
    return buffer;
}

// The last part of a path, which is what a row has room for. The full path is
// what the person chose, and they know where they put it.
std::string LeafOf(const std::string& path, const char* fallback) {
    if (path.empty()) return fallback;
    const size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

const int kIntervals[] = {1, 2, 4, 8};

// The ratios cinema actually delivers in, and off. 2.39 is modern scope and
// 2.35 the older number for it -- close, but not the same picture: at 1920 wide
// they leave 803 and 817 rows, fourteen apart, which matters to anyone matching
// an existing timeline. 1.85 is flat, and on a 16:9 screen it masks little
// enough to read as a hint rather than a statement.
const int kPreRolls[] = {0, 5, 10, 20};

// Where skin sits at a sane exposure, and where the picture is about to clip.
// A third threshold in between would be a number nobody could name.
const float kZebras[] = {0.0f, 0.70f, 0.95f};
const char* kZebraLabels[] = {"OFF", "70%", "95%"};

// Edge strength that counts as sharp. Lower catches more, including grain.
const float kPeaks[] = {0.0f, 0.10f, 0.05f};
const char* kPeakLabels[] = {"OFF", "LOW", "HIGH"};

const float kMattes[] = {0.0f, 2.39f, 2.35f, 1.85f};
const char* kMatteLabels[] = {"OFF", "2.39", "2.35", "1.85"};
const char* kIntervalLabels[] = {
    "every frame", "every 2nd frame", "every 4th frame", "every 8th frame",
};

}  // namespace

void SettingsPanel::Close() {
    open_ = false;
    editing_ = Field::None;
    editBuffer_.clear();
    pendingPresetSlot_ = -1;
}

void SettingsPanel::DrawSection(UiContext& ui, const char* title, float& y) {
    y += kSectionGap;
    ui.Text(T(title), D2D1::RectF(x0_ + 22.0f, y, x0_ + width_ - 22.0f, y + 18.0f),
            TextStyle::Label, theme::kAccent, Align::Left);
    y += 24.0f;
}

// A label on the left and something on the right, which is the shape every row
// in this sheet takes. Returns the rect the caller fills with its control.
bool SettingsPanel::DrawRow(UiContext& ui, int id, const char* label,
                            const std::string& value, float& y, bool enabled) {
    const D2D1_RECT_F rect =
        D2D1::RectF(x0_ + 14.0f, y, x0_ + width_ - 14.0f, y + kRowHeight - 6.0f);

    const bool clicked = ui.Hotspot(id, rect, enabled);
    if (enabled && ui.IsHot(id)) ui.Rect(rect, theme::kHover, 8.0f);

    ui.Text(T(label), D2D1::RectF(rect.left + 12.0f, rect.top, rect.right - 12.0f, rect.bottom),
            TextStyle::Chip, enabled ? theme::kText : theme::kTextFaint, Align::Left);
    ui.Text(value, D2D1::RectF(rect.left + 12.0f, rect.top, rect.right - 12.0f, rect.bottom),
            TextStyle::Chip, enabled ? theme::kTextDim : theme::kTextFaint, Align::Right);

    y += kRowHeight;
    return clicked;
}

SettingsPanel::Output SettingsPanel::Draw(UiContext& ui, CameraModel& model,
                                          AppModel& shared,
                                          Settings& settings) {
    Output out;
    if (!open_) return out;

    // ---- typing ------------------------------------------------------------

    const Input& input = ui.GetInput();
    if (editing_ != Field::None) {
        for (char c : input.typed) {
            if (std::isprint(static_cast<unsigned char>(c)) && editBuffer_.size() < 64) {
                editBuffer_ += c;
            }
        }
        if (input.backspace && !editBuffer_.empty()) editBuffer_.pop_back();
        if (input.cancel) {
            editing_ = Field::None;
            editBuffer_.clear();
            pendingPresetSlot_ = -1;
        }
        if (input.commit) CommitField(model, shared, settings, out);
    }

    // ---- the sheet ---------------------------------------------------------

    width_ = (std::min)(520.0f, ui.Width() - 2 * theme::kPad);
    x0_ = (ui.Width() - width_) * 0.5f;

    // Tall enough for its contents and no taller, up to the window. A sheet with
    // empty space below the last row reads as unfinished; one that runs off the
    // bottom of the window draws over the panel underneath, which is what this
    // did once a Look section and a pre-roll row were added to it.
    const float chrome = 64.0f;      // title row, and the padding under the last row
    const float wanted = contentHeight_ > 0 ? contentHeight_ + chrome : 640.0f;
    const float height = (std::min)(ui.Height() - 2 * theme::kPad, wanted);
    const float top = (ui.Height() - height) * 0.5f;

    ui.Panel(D2D1::RectF(x0_, top, x0_ + width_, top + height), 14.0f, theme::kPanelRaised);

    float y = top + 20.0f;

    ui.Text(T("Settings"), D2D1::RectF(x0_ + 22.0f, y, x0_ + width_ - 100.0f, y + 30.0f),
            TextStyle::Value, theme::kText, Align::Left);

    const D2D1_RECT_F closeRect =
        D2D1::RectF(x0_ + width_ - 90.0f, y - 2.0f, x0_ + width_ - 18.0f, y + 30.0f);
    ui.Panel(closeRect, 8.0f);
    if (ui.Segment(kIdClose, T("Close"), closeRect, false)) Close();
    y += 40.0f;

    // Everything below scrolls as one. The wheel only moves it while the pointer
    // is over the sheet, so a scroll aimed elsewhere does not drag it.
    const float bodyTop = y;
    const float bodyBottom = top + height - 12.0f;
    const float overflow = (std::max)(0.0f, contentHeight_ - (bodyBottom - bodyTop));
    if (overflow > 0.0f) {
        if (input.mouseX >= x0_ && input.mouseX <= x0_ + width_ &&
            input.mouseY >= top && input.mouseY <= top + height) {
            scroll_ -= input.wheel * kRowHeight;
        }
        scroll_ = (std::max)(0.0f, (std::min)(overflow, scroll_));
    } else {
        scroll_ = 0.0f;
    }

    ui.PushClip(D2D1::RectF(x0_, bodyTop, x0_ + width_, bodyBottom));
    y -= scroll_;

    // ---- language ----------------------------------------------------------

    DrawSection(ui, "Application", y);
    if (DrawRow(ui, kIdAutostart, "Start with Windows",
                T(shared.autostart ? "On" : "Off"), y)) {
        shared.autostart = !shared.autostart;
        out.autostartChanged = true;
    }

    {
        const float cellWidth = (width_ - 28.0f) / 2.0f;
        const D2D1_RECT_F group =
            D2D1::RectF(x0_ + 14.0f, y, x0_ + 14.0f + cellWidth * 2, y + 34.0f);
        ui.Panel(group, 9.0f);

        const Lang langs[] = {Lang::English, Lang::Turkish};
        for (int i = 0; i < 2; ++i) {
            const D2D1_RECT_F cell = D2D1::RectF(x0_ + 14.0f + cellWidth * i, y,
                                                 x0_ + 14.0f + cellWidth * (i + 1), y + 34.0f);
            if (i > 0) ui.Divider(cell.left, y, y + 34.0f);
            if (ui.Segment(kIdLangBase + i, Strings::Name(langs[i]), cell,
                           Strings::Current() == langs[i])) {
                Strings::Set(langs[i]);
                settings.Set("ui.language", langs[i] == Lang::Turkish ? "tr" : "en");
            }
        }
        y += 40.0f;
    }

    // ---- connection --------------------------------------------------------

    DrawSection(ui, "Connection", y);
    {
        const char* labels[] = {"Automatic", "USB only", "Wi-Fi only"};
        const Transport modes[] = {Transport::Auto, Transport::Usb, Transport::WiFi};
        const float cellWidth = (width_ - 28.0f) / 3.0f;
        const D2D1_RECT_F group =
            D2D1::RectF(x0_ + 14.0f, y, x0_ + 14.0f + cellWidth * 3, y + 34.0f);
        ui.Panel(group, 9.0f);

        for (int i = 0; i < 3; ++i) {
            const D2D1_RECT_F cell = D2D1::RectF(x0_ + 14.0f + cellWidth * i, y,
                                                 x0_ + 14.0f + cellWidth * (i + 1), y + 34.0f);
            if (i > 0) ui.Divider(cell.left, y, y + 34.0f);
            if (ui.Segment(kIdTransportBase + i, T(labels[i]), cell,
                           shared.transport == modes[i])) {
                shared.transport = modes[i];
                out.reconnect = true;
            }
        }
        y += 40.0f;
    }

    // The address only matters when Wi-Fi is on the table at all.
    {
        const bool relevant = shared.transport != Transport::Usb;
        const std::string shown = editing_ == Field::Host
            ? editBuffer_ + "_"
            : (shared.host.empty() ? std::string(T("Type an address, Enter to save"))
                                  : shared.host);
        if (DrawRow(ui, kIdHost, "Phone address", shown, y, relevant) && relevant) {
            editing_ = Field::Host;
            editBuffer_ = shared.host;
        }
    }

    // The code the phone shows once somebody has allowed Wi-Fi connections on
    // it. Only over Wi-Fi: a phone on the cable never asks, so a row demanding
    // one there would be a question with no answer.
    {
        const bool relevant = shared.transport != Transport::Usb;
        const std::string shown = editing_ == Field::PairCode
            ? editBuffer_ + "_"
            : (shared.pairCode.empty()
                   ? std::string(T("Only if the phone asks for one"))
                   : shared.pairCode);
        if (DrawRow(ui, kIdPairCode, "Pairing code", shown, y, relevant) && relevant) {
            editing_ = Field::PairCode;
            editBuffer_ = shared.pairCode;
        }
    }

    // ---- look --------------------------------------------------------------

    DrawSection(ui, "Look", y);
    {
        const float cellWidth = (width_ - 28.0f) / 4.0f;
        ui.Panel(D2D1::RectF(x0_ + 14.0f, y, x0_ + 14.0f + cellWidth * 4, y + 34.0f), 9.0f);
        for (int i = 0; i < 4; ++i) {
            const D2D1_RECT_F cell = D2D1::RectF(x0_ + 14.0f + cellWidth * i, y,
                                                 x0_ + 14.0f + cellWidth * (i + 1), y + 34.0f);
            if (i > 0) ui.Divider(cell.left, y, y + 34.0f);
            if (ui.Segment(kIdMatteBase + i, kMatteLabels[i], cell,
                           shared.matte == kMattes[i], true)) {
                shared.matte = kMattes[i];
            }
        }
        y += 40.0f;

        // What it costs in pixels, since that is the number someone matching a
        // timeline in an editor actually needs.
        char detail[96] = "";
        if (shared.matte > 0.0f && model.height > 0) {
            const int visible = static_cast<int>(model.width / shared.matte) & ~1;
            snprintf(detail, sizeof(detail), "%dx%d, %d %s", model.width, visible,
                     (model.height - visible) / 2, T("rows masked top and bottom"));
        }
        ui.Text(shared.matte > 0.0f ? detail : T("No matte"),
                D2D1::RectF(x0_ + 16.0f, y, x0_ + width_ - 16.0f, y + 18.0f),
                TextStyle::Stat, theme::kTextDim, Align::Left);
        y += 26.0f;
    }

    {
        // Zebras and peaking, together: they are the two things you look at
        // rather than record, and separating them would suggest otherwise.
        const float cellWidth = (width_ - 28.0f) / 6.0f;
        ui.Panel(D2D1::RectF(x0_ + 14.0f, y, x0_ + 14.0f + cellWidth * 6, y + 34.0f), 9.0f);
        for (int i = 0; i < 6; ++i) {
            const D2D1_RECT_F cell = D2D1::RectF(x0_ + 14.0f + cellWidth * i, y,
                                                 x0_ + 14.0f + cellWidth * (i + 1), y + 34.0f);
            if (i > 0) ui.Divider(cell.left, y, y + 34.0f);
            const bool isZebra = i < 3;
            const int slot = isZebra ? i : i - 3;
            const bool on = isZebra ? shared.zebra == kZebras[slot]
                                    : shared.peaking == kPeaks[slot];
            if (ui.Segment((isZebra ? kIdZebraBase : kIdPeakBase) + slot,
                           isZebra ? kZebraLabels[slot] : kPeakLabels[slot], cell, on)) {
                if (isZebra) shared.zebra = kZebras[slot];
                else shared.peaking = kPeaks[slot];
            }
        }
        y += 40.0f;
        ui.Text(T("Zebras and focus peaking, on the preview only"),
                D2D1::RectF(x0_ + 16.0f, y, x0_ + width_ - 16.0f, y + 18.0f),
                TextStyle::Stat, theme::kTextDim, Align::Left);
        y += 26.0f;
    }

    {
        // Mirroring. Ninety degrees is not offered: it changes the shape of the
        // output, and the virtual camera's declared size, the recording and the
        // preview's aspect would all have to renegotiate. That is a portrait
        // mode, not a flip.
        const float cellWidth = (width_ - 28.0f) / 2.0f;
        ui.Panel(D2D1::RectF(x0_ + 14.0f, y, x0_ + 14.0f + cellWidth * 2, y + 34.0f), 9.0f);

        const D2D1_RECT_F left =
            D2D1::RectF(x0_ + 14.0f, y, x0_ + 14.0f + cellWidth, y + 34.0f);
        const D2D1_RECT_F right =
            D2D1::RectF(x0_ + 14.0f + cellWidth, y, x0_ + 14.0f + cellWidth * 2, y + 34.0f);
        ui.Divider(right.left, y, y + 34.0f);

        if (ui.Segment(kIdFlipX, T("Mirror"), left, model.flipX, true, ChipStyle::Toggle)) {
            model.flipX = !model.flipX;
        }
        if (ui.Segment(kIdFlipY, T("Upside down"), right, model.flipY, true,
                       ChipStyle::Toggle)) {
            model.flipY = !model.flipY;
        }
        y += 40.0f;
    }

    // ---- sound -------------------------------------------------------------

    DrawSection(ui, "Sound", y);
    if (DrawRow(ui, kIdDeskMic, "Microphone on this PC",
                T(shared.deskMic ? "On" : "Off"), y)) {
        shared.deskMic = !shared.deskMic;
    }
    if (DrawRow(ui, kIdDeskMicDevice, "Input",
                shared.deskMicName.empty() ? T("Default") : shared.deskMicName, y,
                shared.deskMic)) {
        // Cycles rather than opening a list: most machines have two or three
        // capture endpoints, and a list would be a second panel for a choice
        // that fits on one row.
        const auto inputs = WasapiCapture::List();
        if (!inputs.empty()) {
            size_t next = 0;
            for (size_t i = 0; i < inputs.size(); ++i) {
                if (inputs[i].id == shared.deskMicId) next = (i + 1) % inputs.size();
            }
            shared.deskMicId = inputs[next].id;
            shared.deskMicName = inputs[next].name;
            out.restartDeskMic = true;
        }
    }
    ui.Text(T("Recorded as a second track beside the phone's"),
            D2D1::RectF(x0_ + 16.0f, y, x0_ + width_ - 16.0f, y + 18.0f),
            TextStyle::Stat, theme::kTextFaint, Align::Left);
    y += 24.0f;

    // ---- recordings --------------------------------------------------------

    DrawSection(ui, "Recordings", y);
    if (DrawRow(ui, kIdBrowseTakes, "Takes on the phone", T("Browse"), y,
                model.connected)) {
        out.openTakes = true;
    }
    if (DrawRow(ui, kIdFolder, "Recordings folder",
                LeafOf(shared.recordFolder, T("Choose…")), y)) {
        out.chooseFolder = true;
    }

    {
        const float cellWidth = (width_ - 28.0f) / 4.0f;
        ui.Panel(D2D1::RectF(x0_ + 14.0f, y, x0_ + 14.0f + cellWidth * 4, y + 34.0f), 9.0f);
        for (int i = 0; i < 4; ++i) {
            const D2D1_RECT_F cell = D2D1::RectF(x0_ + 14.0f + cellWidth * i, y,
                                                 x0_ + 14.0f + cellWidth * (i + 1), y + 34.0f);
            if (i > 0) ui.Divider(cell.left, y, y + 34.0f);
            const std::string label = i == 0 ? "1×" : Format("1/%d", kIntervals[i]);
            if (ui.Segment(kIdIntervalBase + i, label, cell,
                           model.recordInterval == kIntervals[i], !model.recording)) {
                model.recordInterval = kIntervals[i];
                out.commands.push_back(MakeRecordConfigCommand(
                    model.recordEnabled, model.recordToPc, model.recordWantWidth,
                    model.recordWantHeight, model.recordRate(), 0, model.recordCodec,
            model.preRoll));
            }
        }
        y += 6.0f;
        ui.Text(T(kIntervalLabels[
                      std::find(std::begin(kIntervals), std::end(kIntervals),
                                model.recordInterval) - std::begin(kIntervals)]),
                D2D1::RectF(x0_ + 16.0f, y + 30.0f, x0_ + width_ - 16.0f, y + 50.0f),
                TextStyle::Stat, theme::kTextDim, Align::Left);
        y += 56.0f;
    }

    {
        // Local takes only: the ring is on the phone, and shipping it across
        // when a take starts would stall the live picture for as long as the
        // ring is deep.
        const bool possible = !model.recordToPc && model.recordEnabled;
        const float cellWidth = (width_ - 28.0f) / 4.0f;
        ui.Panel(D2D1::RectF(x0_ + 14.0f, y, x0_ + 14.0f + cellWidth * 4, y + 34.0f), 9.0f);
        for (int i = 0; i < 4; ++i) {
            const D2D1_RECT_F cell = D2D1::RectF(x0_ + 14.0f + cellWidth * i, y,
                                                 x0_ + 14.0f + cellWidth * (i + 1), y + 34.0f);
            if (i > 0) ui.Divider(cell.left, y, y + 34.0f);
            const std::string label = i == 0 ? T("OFF") : Format("%ds", kPreRolls[i]);
            if (ui.Segment(kIdPreRollBase + i, label, cell,
                           model.preRoll == kPreRolls[i], possible)) {
                model.preRoll = kPreRolls[i];
                out.commands.push_back(MakeRecordConfigCommand(
                    model.recordEnabled, model.recordToPc, model.recordWantWidth,
                    model.recordWantHeight, model.recordRate(), 0, model.recordCodec,
                    model.preRoll));
            }
        }
        y += 40.0f;

        // What the phone granted, not what was asked for, and what that costs.
        char detail[160] = "";
        if (!possible) {
            snprintf(detail, sizeof(detail), "%s",
                     T("Takes on the phone only"));
        } else if (model.preRollGranted > 0) {
            // One format string rather than glued fragments: a sentence
            // assembled from pieces reads badly in English and cannot be
            // translated at all.
            snprintf(detail, sizeof(detail),
                     T("Takes begin %ds before you press record"),
                     model.preRollGranted);
        } else if (model.preRollGranted < 0) {
            snprintf(detail, sizeof(detail), "%s", T("This phone is too old for it"));
        } else if (model.preRoll > 0) {
            snprintf(detail, sizeof(detail), "%s", T("Not enough memory on the phone"));
        } else {
            snprintf(detail, sizeof(detail), "%s", T("The take starts when you press it"));
        }
        ui.Text(detail, D2D1::RectF(x0_ + 16.0f, y, x0_ + width_ - 16.0f, y + 18.0f),
                TextStyle::Stat, theme::kTextDim, Align::Left);
        y += 20.0f;

        // The cost, on its own line and only while it is being paid.
        if (model.preRollGranted > 0) {
            ui.Text(T("The encoder runs the whole time this is armed"),
                    D2D1::RectF(x0_ + 16.0f, y, x0_ + width_ - 16.0f, y + 18.0f),
                    TextStyle::Stat, theme::kTextFaint, Align::Left);
            y += 18.0f;
        }
        y += 6.0f;
    }

    {
        const D2D1_RECT_F rect =
            D2D1::RectF(x0_ + 14.0f, y, x0_ + width_ - 14.0f, y + 34.0f);
        ui.Panel(rect, 9.0f);
        // A LUT or a colour setting -- either is a look worth keeping, and the
        // toggle used to be dead unless a .cube had been loaded.
        const bool possible = model.recordToPc &&
                              (!shared.lutPath.empty() || !shared.GradeIsNeutral());
        if (ui.Segment(kIdGraded, T("Apply the look to recordings"), rect,
                       shared.recordGraded, possible, ChipStyle::Toggle)) {
            shared.recordGraded = !shared.recordGraded;
        }
        y += 38.0f;
        ui.Text(T("A second file, re-encoded here from the stream"),
                D2D1::RectF(x0_ + 30.0f, y, x0_ + width_ - 16.0f, y + 18.0f),
                TextStyle::Stat, theme::kTextFaint, Align::Left);
        y += 22.0f;
    }

    // ---- presets -----------------------------------------------------------

    DrawSection(ui, "Presets", y);
    {
        const float cellWidth = (width_ - 28.0f) / kPresetSlots;
        ui.Panel(D2D1::RectF(x0_ + 14.0f, y, x0_ + 14.0f + cellWidth * kPresetSlots,
                             y + 38.0f), 9.0f);

        for (size_t i = 0; i < kPresetSlots; ++i) {
            const D2D1_RECT_F cell =
                D2D1::RectF(x0_ + 14.0f + cellWidth * i, y,
                            x0_ + 14.0f + cellWidth * (i + 1), y + 38.0f);
            if (i > 0) ui.Divider(cell.left, y, y + 38.0f);

            const Preset& preset = shared.presets[i];
            const std::string label =
                (editing_ == Field::PresetName && pendingPresetSlot_ == static_cast<int>(i))
                    ? editBuffer_ + "_"
                    : (preset.Empty() ? std::string(T("Empty")) : preset.name);

            if (ui.Segment(kIdPresetBase + static_cast<int>(i), label, cell,
                           false, true)) {
                if (preset.Empty()) {
                    // An empty slot asks for a name rather than silently
                    // taking one: a preset called "Preset 3" is a preset
                    // nobody can find again.
                    editing_ = Field::PresetName;
                    pendingPresetSlot_ = static_cast<int>(i);
                    editBuffer_.clear();
                } else {
                    model.exposureMode = preset.exposureManual ? ExposureMode::Manual
                                                               : ExposureMode::Auto;
                    model.iso = preset.iso;
                    model.shutterNs = preset.shutterNs;
                    model.focusMode = preset.focusManual ? FocusMode::Manual
                                                         : FocusMode::Continuous;
                    model.focusDistance = preset.focusDistance;
                    model.wbMode = preset.wbMode;
                    model.wbKelvin = preset.wbKelvin;
                    model.ev = preset.ev;
                    model.logProfile = preset.logProfile;
                    model.zoom = preset.zoom;
                    shared.matte = preset.matte;

                    if (model.exposureMode == ExposureMode::Manual) {
                        out.commands.push_back(
                            MakeExposureCommand(true, model.iso, model.shutterNs));
                    } else {
                        out.commands.push_back(MakeExposureCommand(false, 0, 0));
                        out.commands.push_back(MakeEvCommand(model.ev));
                    }
                    if (model.focusMode == FocusMode::Manual) {
                        out.commands.push_back(
                            MakeFocusCommand("manual", model.focusDistance));
                    } else {
                        out.commands.push_back(MakeFocusCommand("continuous", 0));
                    }
                    out.commands.push_back(
                        MakeWhiteBalanceCommand(model.wbMode, model.wbKelvin));
                    out.commands.push_back(MakePictureProfileCommand(model.logProfile));
                    out.commands.push_back(MakeZoomCommand(model.zoom));
                }
            }
        }
        y += 44.0f;

        ui.Text(shared.presets[0].Empty() ? T("Nothing saved yet")
                                         : T("Click a slot to load, right-click to replace"),
                D2D1::RectF(x0_ + 16.0f, y, x0_ + width_ - 16.0f, y + 18.0f),
                TextStyle::Stat, theme::kTextFaint, Align::Left);
        y += 22.0f;
    }

    ui.PopClip();

    // What the next frame sizes the sheet with. Measured from where the layout
    // actually ended, so adding a row anywhere above needs no arithmetic here.
    contentHeight_ = (y + scroll_) - bodyTop;

    return out;
}

void SettingsPanel::CommitField(CameraModel& model, AppModel& shared,
                                Settings& settings, Output& out) {
    if (editing_ == Field::Host) {
        shared.host = editBuffer_;
        settings.Set("net.host", shared.host);
        out.reconnect = true;
    } else if (editing_ == Field::PairCode) {
        // Digits only, and no more than six of them. The phone will refuse
        // anything else anyway; refusing it here means the stored setting is
        // always something that could work.
        std::string digits;
        for (char c : editBuffer_) {
            if (c >= '0' && c <= '9' && digits.size() < 6) digits.push_back(c);
        }
        shared.pairCode = digits;
        settings.Set("net.pairCode", shared.pairCode);
        out.reconnect = true;
    } else if (editing_ == Field::PresetName && pendingPresetSlot_ >= 0 &&
               pendingPresetSlot_ < static_cast<int>(kPresetSlots) && !editBuffer_.empty()) {
        Preset& preset = shared.presets[pendingPresetSlot_];
        preset.name = editBuffer_;
        preset.exposureManual = model.exposureMode == ExposureMode::Manual;
        preset.iso = model.iso;
        preset.shutterNs = model.shutterNs;
        preset.focusManual = model.focusMode == FocusMode::Manual;
        preset.focusDistance = model.focusDistance;
        preset.wbMode = model.wbMode;
        preset.wbKelvin = model.wbKelvin;
        preset.ev = model.ev;
        preset.logProfile = model.logProfile;
        preset.zoom = model.zoom;
        preset.lutPath = shared.lutPath;
        preset.matte = shared.matte;
    }

    editing_ = Field::None;
    editBuffer_.clear();
    pendingPresetSlot_ = -1;
}

}  // namespace xcam
