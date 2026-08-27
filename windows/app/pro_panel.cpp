#include "app/pro_panel.h"
#include "app/strings.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cctype>

namespace xcam {
namespace {

using namespace xcam::ui;

// Widget ids. Immediate mode needs them stable across frames but not globally
// meaningful, so a block per region keeps collisions impossible by inspection.
enum Id : int {
    kIdFormatBase = 100,
    kIdDropdownBase = 200,
    kIdLensBase = 300,
    kIdZoomSlider = 350,
    kIdProBase = 400,
    kIdRuler = 500,
    kIdTorch = 510,
    kIdAutoToggle = 520,
    kIdLogToggle = 530,
    kIdLutButton = 531,
    kIdVirtualCam = 532,
    kIdRecord = 533,
    kIdRecordFormat = 534,
    kIdMic = 535,
    kIdRecordTarget = 536,
    kIdFocusSetA = 540,
    kIdFocusGoA = 541,
    kIdFocusSetB = 542,
    kIdFocusGoB = 543,
    kIdRampTime = 544,
    kIdAutoFrame = 545,
    kIdSettings = 537,

    // One per angle, so a rig of any size still has one id each.
    kIdAngleBase = 560,

    // The switch between the short panel and all of it.
    kIdPro = 559,

    // One per group of pro controls.
    kIdTabBase = 600,

    // One per cell of the colour tab.
    kIdColourBase = 700,

    // One per control on the prompter tab.
    kIdPrompterBase = 720,
};

// The groups the pro tier can be showing. Only one at a time -- see
// ProPanel::DrawProTabs.
//
// Appended rather than inserted: the open tab is remembered between runs as an
// int, and putting a new one in the middle would reopen somebody on a page they
// did not leave.
enum ProTab {
    kTabExposure = 0, kTabFormat, kTabLook, kTabRecord, kTabColour, kTabPrompter,
    kTabCount
};

std::string Format(const char* pattern, ...) {
    char buffer[128];
    va_list args;
    va_start(args, pattern);
    vsnprintf(buffer, sizeof(buffer), pattern, args);
    va_end(args);
    return buffer;
}

// Perceptually even travel for a range that spans orders of magnitude. ISO and
// shutter both do, and a linear slider over them spends most of its length in
// values nobody wants.
float ToLog(float value, float min, float max) {
    if (min <= 0 || max <= min) return 0.0f;
    return std::log(value / min) / std::log(max / min);
}
float FromLog(float t, float min, float max) {
    if (min <= 0 || max <= min) return min;
    return min * std::pow(max / min, std::clamp(t, 0.0f, 1.0f));
}

const char* kWbLabels[] = {"AWB", "TUNG", "FLUO", "SUN", "CLOUD", "SHADE"};
const char* kWbModes[]  = {"auto", "incandescent", "fluorescent", "daylight", "cloudy", "shade"};

// How much room the wordmark takes at the top left, so the format chips start
// clear of it.
constexpr float kWordmarkWidth = 96.0f;

}  // namespace

float ProPanel::Opacity(double now) const {
    const double idle = now - lastActivity_;
    if (idle < theme::kIdleFadeAfterSeconds) return 1.0f;

    const double fading = idle - theme::kIdleFadeAfterSeconds;
    if (fading >= theme::kFadeDuration) return 0.0f;
    return static_cast<float>(1.0 - fading / theme::kFadeDuration);
}

ProPanel::Output ProPanel::Draw(UiContext& ui, CameraModel& model, AppModel& shared,
                                double now) {
    Output out;
    wantsMouse_ = false;
    now_ = now;

    // Fully faded out means nothing to draw and nothing to click; the picture is
    // the whole interface at that point.
    if (ui.Opacity() <= 0.01f) {
        selected_ = ProControl::None;
        openDropdown_ = 0;
        return out;
    }

    const ui::Input& input = ui.GetInput();
    if (selected_ != ProControl::None) {
        for (char c : input.typed) {
            if (std::isdigit(static_cast<unsigned char>(c)) || c == '.' || c == '/' ||
                c == '-' || c == '+' || c == 'i' || c == 'n' || c == 'f') {
                editing_ = true;
                if (editBuffer_.size() < 12) editBuffer_ += c;
            }
        }
        if (input.backspace && !editBuffer_.empty()) editBuffer_.pop_back();
        if (input.cancel) { editing_ = false; editBuffer_.clear(); }
        if (input.commit && editing_) {
            CommitTypedValue(model, out);
            editing_ = false;
            editBuffer_.clear();
        }
    } else {
        editing_ = false;
        editBuffer_.clear();
    }

    // ---- the chrome ---------------------------------------------------------
    //
    // Two solid bars rather than shapes floating over the picture.
    //
    // Text on an unknown background is a gamble this panel kept losing. The
    // readouts were dim grey drawn straight onto whatever the camera happened
    // to be pointed at, and against a white wall they were simply not there.
    // A bar underneath makes the contrast a property of the design rather than
    // of the shot -- which is the one rule every camera and broadcast interface
    // follows and this one did not.
    //
    // It also puts everything within one eye movement. Pinned to the four
    // corners of a 1920-wide window, the exposure readout and the record button
    // were fifteen hundred pixels apart, and nothing on screen suggested they
    // belonged to the same instrument.
    //
    // The bars still fade with the rest of the panel, so the default state of
    // the window is the picture and nothing else.
    const float rowH = 34.0f;

    // Laid out upwards from the bottom edge, so a tier that is not there costs
    // no space rather than leaving a gap where it would have been.
    float y = ui.Height() - theme::kPad - rowH;
    rowLive_ = y;
    if (pro_) {
        y -= theme::kGap + 46.0f;
        rowContent_ = y;
        y -= 24.0f;
        rowTabs_ = y;
    }
    bottomBarTop_ = y - theme::kPad;
    topBarBottom_ = 54.0f;

    ui.Rect(D2D1::RectF(0, 0, ui.Width(), topBarBottom_), theme::kPanel);
    ui.Line(0, topBarBottom_, ui.Width(), topBarBottom_, theme::kPanelBorder, 1.0f);

    ui.Rect(D2D1::RectF(0, bottomBarTop_, ui.Width(), ui.Height()), theme::kPanel);
    ui.Line(0, bottomBarTop_, ui.Width(), bottomBarTop_, theme::kPanelBorder, 1.0f);

    // Always: what you are looking at, how it is doing, and whether it is
    // recording. None of these are controls, and all of them answer a question
    // somebody has while a call is running.
    DrawHeader(ui, model);
    DrawStats(ui, model, shared);
    DrawRecordBanner(ui, model);
    DrawDisconnected(ui, model);

    // A shoot's controls rather than a call's. See ProPanel::IsPro.
    if (pro_) {
        DrawProTabs(ui, model);
        DrawTabContent(ui, model, shared, out);
        DrawRuler(ui, model, shared, out);
    }

    DrawZoom(ui, model, out);
    DrawDeviceTray(ui, model, shared, out);

    // Last, so it lands over the tier it belongs to rather than under it.
    if (pro_ && openDropdown_ != 0) DrawDropdown(ui, model, shared);

    if (formatDirty_ && !ui.GetInput().mouseDown) {
        out.commands.push_back(MakeSetCommand(model.Camera() ? model.Camera()->id : "",
                                              model.width, model.height, model.fps,
                                              model.bitrate, model.codec));
        // The record rate is a fraction of the stream rate, so a new stream
        // rate is a new record rate. Without this, a 1/4 time-lapse set at 60
        // stays at 15 after dropping the stream to 30 -- half, not a quarter.
        if (model.recordEnabled && model.recordInterval > 1) {
            out.commands.push_back(MakeRecordConfigCommand(
                model.recordEnabled, model.recordToPc, model.recordWantWidth,
                model.recordWantHeight, model.recordRate(), 0, model.recordCodec,
            model.preRoll));
        }
        formatDirty_ = false;
    }

    wantsMouse_ = ui.WantsMouse();
    return out;
}

// ---- top left: capture format ----------------------------------------------

D2D1_RECT_F ProPanel::TabTray(UiContext& ui) const {
    // One rectangle, the width of the bar, whichever tab is showing.
    //
    // Sized to its contents, each tab was a different small box in the corner
    // of a window that was mostly empty, and switching tabs made the panel
    // change shape underneath the pointer. A constant tray is what makes them
    // read as one thing with four pages rather than four things.
    const D2D1_RECT_F rect = D2D1::RectF(theme::kPad, rowContent_,
                                         ui.Width() - theme::kPad, rowContent_ + 46.0f);
    ui.Panel(rect, 10.0f);
    return rect;
}

// Whatever a tab has to say about itself, at the far end of its tray -- the
// same place on every one of them.
void ProPanel::TrayNote(UiContext& ui, const D2D1_RECT_F& tray, const std::string& text,
                        const D2D1_COLOR_F& colour) {
    const float w = ui.MeasureText(text, TextStyle::Stat);
    const float mid = (tray.top + tray.bottom) * 0.5f;
    ui.Text(text,
            D2D1::RectF(tray.right - theme::kGap * 2 - w, mid - 8.0f,
                        tray.right - theme::kGap * 2, mid + 8.0f),
            TextStyle::Stat, colour, Align::Left);
}

void ProPanel::DrawProTabs(UiContext& ui, const CameraModel& model) {
    // Text with a rule under the one in hand, not four more boxes. The panel
    // already has enough boxes; a tab is a place, and places are named rather
    // than framed.
    struct Tab {
        int index;
        std::string label;
        bool available;
    };
    const std::vector<Tab> tabs = {
        {kTabExposure, T("EXPOSURE"), true},
        {kTabFormat, T("FORMAT"), true},
        {kTabLook, T("LOOK"), true},
        {kTabRecord, T("RECORDING"), model.CanRecord()},
        {kTabColour, T("COLOUR"), true},
        {kTabPrompter, T("PROMPTER"), true},
    };

    float x = theme::kPad;
    for (const Tab& tab : tabs) {
        if (!tab.available) {
            if (proTab_ == tab.index) proTab_ = kTabExposure;
            continue;
        }
        const float w = ui.MeasureText(tab.label, TextStyle::Label) + 22.0f;
        const D2D1_RECT_F rect = D2D1::RectF(x, rowTabs_, x + w, rowTabs_ + 22.0f);
        const int id = kIdTabBase + tab.index;

        const bool current = proTab_ == tab.index;
        if (ui.Hotspot(id, rect, true)) {
            proTab_ = tab.index;
            // A ruler or a list belonging to the group being left would
            // otherwise stay open over the one being arrived at.
            selected_ = ProControl::None;
            openDropdown_ = 0;
        }
        const bool hovered = ui.IsHot(id);

        ui.Text(tab.label,
                D2D1::RectF(rect.left + 11.0f, rect.top + 3.0f, rect.right - 11.0f,
                            rect.bottom - 5.0f),
                TextStyle::Label,
                current ? theme::kText : (hovered ? theme::kTextDim : theme::kTextFaint),
                Align::Left);

        if (current) {
            ui.Rect(D2D1::RectF(rect.left + 11.0f, rect.bottom - 2.0f,
                                rect.right - 11.0f, rect.bottom),
                    theme::kAccent, 1.0f);
        }
        x += w;
    }
}

void ProPanel::DrawTabContent(UiContext& ui, CameraModel& model, AppModel& shared,
                              Output& out) {
    switch (proTab_) {
        case kTabFormat: DrawFormatCells(ui, model, shared); break;
        case kTabLook:   DrawLookChips(ui, model, shared, out); break;
        case kTabRecord: DrawRecordChips(ui, model, out); break;
        case kTabColour: DrawColourCells(ui, shared); break;
        case kTabPrompter: DrawPrompterChips(ui, shared, out); break;
        default:         DrawExposureCells(ui, model, out); break;
    }
}

// ---- the exposure tab ------------------------------------------------------

void ProPanel::DrawExposureCells(UiContext& ui, CameraModel& model, Output& out) {
    const CameraInfo* camera = model.Camera();
    if (!camera) return;

    struct Cell {
        std::string label;
        std::string value;
        ProControl control;
        bool enabled;
        bool chosen;          // somebody set this, as against the camera deciding
    };

    const std::vector<Cell> cells = {
        {"ISO",
         model.exposureMode == ExposureMode::Manual ? Format("%d", model.iso) : T("AUTO"),
         ProControl::Iso, model.CanManualExposure(),
         model.exposureMode == ExposureMode::Manual},
        {T("SHUTTER"),
         model.exposureMode == ExposureMode::Manual ? FormatShutter(model.shutterNs)
                                                    : T("AUTO"),
         ProControl::Shutter, model.CanManualExposure(),
         model.exposureMode == ExposureMode::Manual},
        {T("WB"),
         model.wbMode == "manual" ? Format("%dK", model.wbKelvin)
                                  : (model.wbMode == "auto" ? T("AUTO") : model.wbMode),
         ProControl::WhiteBalance, model.CanManualWhiteBalance(), model.wbMode != "auto"},
        {T("FOCUS"),
         model.focusMode == FocusMode::Manual ? FormatFocus(model.focusDistance) : "AF",
         ProControl::Focus, model.CanManualFocus(),
         model.focusMode == FocusMode::Manual},
        {"EV", Format("%+.1f", model.ev), ProControl::Ev,
         model.exposureMode == ExposureMode::Auto, model.ev != 0.0f},
    };

    const D2D1_RECT_F tray = TabTray(ui);
    const float y0 = tray.top;
    const float height = tray.bottom - tray.top;

    // Every cell the same width, and the widest one sets it.
    //
    // Measured per cell, the group came out ragged -- ISO narrow, ENSTANTANE
    // wide -- and a row of dials that are not the same size does not read as a
    // row of dials. The tray behind them spans the bar, so every tab is the
    // same rectangle and switching between them does not change the shape of
    // the panel.
    float cellWidth = 112.0f;
    for (const Cell& cell : cells) {
        cellWidth = (std::max)(cellWidth, ui.ReadoutWidth(cell.label, cell.value));
    }

    const float x0 = tray.left;
    float x = x0;
    for (size_t i = 0; i < cells.size(); ++i) {
        const Cell& cell = cells[i];
        const D2D1_RECT_F rect = D2D1::RectF(x, y0, x + cellWidth, y0 + height);
        if (i > 0) ui.Divider(x, y0, y0 + height);

        const bool active = selected_ == cell.control;
        if (ui.Readout(kIdProBase + static_cast<int>(i), cell.label, cell.value, rect,
                       active, cell.enabled, cell.chosen)) {
            // Selecting a cell also takes its parameter off auto -- the reason
            // anyone opens it is to set a value.
            selected_ = active ? ProControl::None : cell.control;
            if (!active) {
                if (cell.control == ProControl::Iso ||
                    cell.control == ProControl::Shutter) {
                    if (model.exposureMode == ExposureMode::Auto) {
                        model.exposureMode = ExposureMode::Manual;
                        out.commands.push_back(
                            MakeExposureCommand(true, model.iso, model.shutterNs));
                    }
                } else if (cell.control == ProControl::Focus &&
                           model.focusMode == FocusMode::Continuous) {
                    model.focusMode = FocusMode::Manual;
                    out.commands.push_back(MakeFocusCommand("manual", model.focusDistance));
                }
            }
        }
        x += cellWidth;
    }
    ui.Divider(x, y0, y0 + height);

    // AUTO returns everything to the camera's own judgement in one action,
    // which is the escape hatch a pro mode needs most. At the far end of the
    // tray, where every tab keeps whatever it has to say about itself.
    const bool anythingManual = model.exposureMode == ExposureMode::Manual ||
                                model.focusMode == FocusMode::Manual ||
                                model.wbMode != "auto" || model.ev != 0.0f;
    const float autoWidth = (std::max)(ui.SegmentWidth(T("RESET AUTO")), 128.0f);
    const D2D1_RECT_F autoRect = D2D1::RectF(tray.right - theme::kGap - autoWidth,
                                             y0 + 7.0f, tray.right - theme::kGap,
                                             y0 + height - 7.0f);
    if (ui.Segment(kIdAutoToggle, T("RESET AUTO"), autoRect, false, anythingManual)) {
        model.exposureMode = ExposureMode::Auto;
        model.focusMode = FocusMode::Continuous;
        model.wbMode = "auto";
        model.ev = 0.0f;
        selected_ = ProControl::None;
        out.commands.push_back(MakeExposureCommand(false, 0, 0));
        out.commands.push_back(MakeFocusCommand("continuous", 0));
        out.commands.push_back(MakeWhiteBalanceCommand("auto", 0));
        out.commands.push_back(MakeEvCommand(0.0f));
    }

    // The follow focus belongs with focus, which is where somebody looking for
    // it would go. It used to sit beside the lens stops, which is where it was
    // put when the lens stops were the only bar at the bottom of the window.
    // In the space the cells leave, and never into the reset button.
    DrawFollowFocus(ui, model, out, x + theme::kGap * 2, y0 + 7.0f, height - 14.0f,
                    autoRect.left - theme::kGap * 2);
}

// ---- the prompter tab ------------------------------------------------------

void ProPanel::DrawPrompterChips(UiContext& ui, AppModel& shared, Output& out) {
    const D2D1_RECT_F tray = TabTray(ui);
    const float rowH = 34.0f;
    const float y = tray.top + (tray.bottom - tray.top - rowH) * 0.5f;

    // A script's name is the useful label, the way a LUT's is. Trimmed for the
    // same reason: files get called long things.
    std::string script = shared.prompterPath;
    const size_t slash = script.find_last_of("\\/");
    if (slash != std::string::npos) script = script.substr(slash + 1);
    if (script.size() > 16) script = script.substr(0, 15) + "...";

    std::vector<BarCell> chips;
    chips.push_back({kIdPrompterBase, script.empty() ? T("LOAD A SCRIPT") : script,
                     shared.HasScript(), true, ChipStyle::Value});
    chips.push_back({kIdPrompterBase + 1, shared.prompterOn ? T("HIDE") : T("SHOW"),
                     shared.prompterOn, shared.HasScript(), ChipStyle::Toggle});
    // Running is the state that matters while you are talking, so it carries the
    // recording colour: nothing else on this row is time-critical.
    chips.push_back({kIdPrompterBase + 2,
                     shared.prompterRunning ? T("PAUSE") : T("PLAY"),
                     shared.prompterRunning, shared.HasScript() && shared.prompterOn,
                     ChipStyle::Record});
    chips.push_back({kIdPrompterBase + 3, T("TOP"), false, shared.HasScript(),
                     ChipStyle::Value});
    chips.push_back({kIdPrompterBase + 4, Format("%.0f/s", shared.prompterSpeed), false,
                     shared.HasScript(), ChipStyle::Value});
    chips.push_back({kIdPrompterBase + 5, Format("%.0fpt", shared.prompterSize), false,
                     shared.HasScript(), ChipStyle::Value});
    chips.push_back({kIdPrompterBase + 6, T("MIRROR"), shared.prompterMirror,
                     shared.HasScript(), ChipStyle::Toggle});

    switch (DrawBar(ui, chips.data(), chips.size(), tray.left + 4.0f, y, rowH, false)) {
        case kIdPrompterBase:
            out.openPrompterFile = true;
            break;
        case kIdPrompterBase + 1:
            shared.prompterOn = !shared.prompterOn;
            if (!shared.prompterOn) shared.prompterRunning = false;
            break;
        case kIdPrompterBase + 2:
            shared.prompterRunning = !shared.prompterRunning;
            break;
        case kIdPrompterBase + 3:
            shared.prompterOffset = 0.0f;
            break;
        case kIdPrompterBase + 4: {
            // A cycle rather than a ruler: nobody dials a reading speed in, they
            // try one, find it too fast, and try the next.
            static const float kSpeeds[] = {20.0f, 30.0f, 40.0f, 55.0f, 75.0f, 100.0f};
            size_t next = 0;
            for (size_t i = 0; i < 6; ++i) {
                if (kSpeeds[i] == shared.prompterSpeed) { next = (i + 1) % 6; break; }
            }
            shared.prompterSpeed = kSpeeds[next];
            break;
        }
        case kIdPrompterBase + 5: {
            static const float kSizes[] = {24.0f, 34.0f, 46.0f, 60.0f, 80.0f};
            size_t next = 0;
            for (size_t i = 0; i < 5; ++i) {
                if (kSizes[i] == shared.prompterSize) { next = (i + 1) % 5; break; }
            }
            shared.prompterSize = kSizes[next];
            break;
        }
        case kIdPrompterBase + 6:
            shared.prompterMirror = !shared.prompterMirror;
            break;
        default:
            break;
    }

    if (!shared.HasScript()) {
        TrayNote(ui, tray, T("a plain text file, one you can read aloud"),
                 theme::kTextDim);
    } else {
        TrayNote(ui, tray, T("only you see this -- not the call, not the recording"),
                 theme::kTextDim);
    }
}

// ---- the colour tab --------------------------------------------------------

void ProPanel::DrawColourCells(UiContext& ui, AppModel& shared) {
    // Cells and the ruler, exactly like exposure. A grade is a set of values you
    // dial, which is what the ruler is for -- and it brings the typed entry and
    // the caret with it, neither of which a row of sliders would have had.
    struct Cell {
        std::string label;
        std::string value;
        ProControl control;
        bool enabled;
        bool chosen;
    };

    const std::vector<Cell> cells = {
        {T("BRIGHT"), Format("%+.2f", shared.gain), ProControl::Gain, true,
         shared.gain != 0.0f},
        {T("CONTRAST"), Format("%+.2f", shared.contrast), ProControl::Contrast, true,
         shared.contrast != 0.0f},
        {T("SATURATION"), Format("%+.2f", shared.saturation), ProControl::Saturation, true,
         shared.saturation != 0.0f},
        {T("WARMTH"), Format("%+.2f", shared.warmth), ProControl::Warmth, true,
         shared.warmth != 0.0f},
        // Only worth a cell when there is a LUT for it to weaken.
        {"LUT", Format("%.0f%%", shared.lutAmount * 100.0f), ProControl::LutAmount,
         !shared.lutName.empty(), shared.lutAmount < 1.0f},
    };

    const D2D1_RECT_F tray = TabTray(ui);
    const float y0 = tray.top;
    const float height = tray.bottom - tray.top;

    float cellWidth = 112.0f;
    for (const Cell& cell : cells) {
        cellWidth = (std::max)(cellWidth, ui.ReadoutWidth(cell.label, cell.value));
    }

    float x = tray.left;
    for (size_t i = 0; i < cells.size(); ++i) {
        const Cell& cell = cells[i];
        const D2D1_RECT_F rect = D2D1::RectF(x, y0, x + cellWidth, y0 + height);
        if (i > 0) ui.Divider(x, y0, y0 + height);

        const bool active = selected_ == cell.control;
        if (ui.Readout(kIdColourBase + static_cast<int>(i), cell.label, cell.value, rect,
                       active, cell.enabled, cell.chosen)) {
            selected_ = active ? ProControl::None : cell.control;
        }
        x += cellWidth;
    }
    ui.Divider(x, y0, y0 + height);

    // Back to what the camera sent. The LUT is left alone: it is a file
    // somebody loaded, not a number they nudged.
    const float resetWidth = (std::max)(ui.SegmentWidth(T("RESET COLOUR")), 128.0f);
    const D2D1_RECT_F resetRect = D2D1::RectF(tray.right - theme::kGap - resetWidth,
                                              y0 + 7.0f, tray.right - theme::kGap,
                                              y0 + height - 7.0f);
    if (ui.Segment(kIdColourBase + 90, T("RESET COLOUR"), resetRect, false,
                   !shared.GradeIsNeutral() || shared.lutAmount < 1.0f)) {
        shared.gain = shared.contrast = shared.saturation = shared.warmth = 0.0f;
        shared.lutAmount = 1.0f;
        selected_ = ProControl::None;
    }

    if (shared.GradeIsNeutral() && shared.lutName.empty()) {
        TrayNote(ui, tray, T("the picture is as the camera sent it"), theme::kTextDim);
    } else {
        // Worth saying once: this is the difference from every other grade
        // control anybody has used with a phone.
        TrayNote(ui, tray, T("this reaches the call and the recording"), theme::kTextDim);
    }
}

// ---- the format tab --------------------------------------------------------

void ProPanel::DrawFormatCells(UiContext& ui, CameraModel& model, AppModel& shared) {
    struct Cell {
        std::string label;
        std::string value;
        int dropdown;
    };
    // Size, rate, codec and bitrate are what the phone sends. Shape is what
    // leaves this machine, and it is here rather than under the look because
    // changing it changes the size every consumer negotiated.
    const std::vector<Cell> cells = {
        {T("SIZE"), Format("%dp", model.height), 1},
        {T("RATE"), Format("%d", model.fps), 2},
        {T("CODEC"), model.codec == "hevc" ? "HEVC" : "H.264", 3},
        {T("BITRATE"), Format("%d", model.bitrate / 1'000'000), 4},
        {T("SHAPE"), shared.shape == Shape::Vertical ? T("VERTICAL")
                   : shared.shape == Shape::Square   ? T("SQUARE")
                                                     : T("WIDE"), 5},
    };

    const D2D1_RECT_F tray = TabTray(ui);
    const float y0 = tray.top;
    const float height = tray.bottom - tray.top;

    float cellWidth = 112.0f;
    for (const Cell& cell : cells) {
        cellWidth = (std::max)(cellWidth, ui.ReadoutWidth(cell.label, cell.value));
    }

    float x = tray.left;
    for (size_t i = 0; i < cells.size(); ++i) {
        const D2D1_RECT_F rect = D2D1::RectF(x, y0, x + cellWidth, y0 + height);
        if (i > 0) ui.Divider(x, y0, y0 + height);

        const bool open = openDropdown_ == cells[i].dropdown;
        if (open) dropdownAnchor_ = rect;

        if (ui.Readout(kIdFormatBase + static_cast<int>(i), cells[i].label,
                       cells[i].value, rect, open)) {
            openDropdown_ = open ? 0 : cells[i].dropdown;
            if (openDropdown_ != 0) dropdownAnchor_ = rect;
        }
        x += cellWidth;
    }
    ui.Divider(x, y0, y0 + height);
    formatBarRight_ = x;

    // Two different warnings, because the two do different damage.
    if (shared.shape != Shape::Wide) {
        TrayNote(ui, tray, T("reconnect the camera in the app that is using it"),
                 theme::kWarn);
    } else {
        TrayNote(ui, tray, T("changing these restarts the stream"), theme::kTextDim);
    }
}

void ProPanel::DrawDropdown(UiContext& ui, CameraModel& model, AppModel& shared) {
    struct Option {
        std::string label;
        bool selected;
        bool enabled;
    };
    std::vector<Option> options;

    switch (openDropdown_) {
        case 1:
            for (const CaptureMode& mode : model.PreferredModes()) {
                options.push_back({Format("%dp", mode.height),
                                   mode.height == model.height, true});
            }
            break;
        case 2:
            for (int rate : model.FrameRatesForCurrentSize()) {
                options.push_back({Format("%d", rate), rate == model.fps, true});
            }
            break;
        case 3:
            for (const char* name : {"h264", "hevc"}) {
                const std::string id = name;
                options.push_back({id == "hevc" ? "HEVC" : "H.264",
                                   model.codec == id, shared.CanDecode(id)});
            }
            break;
        case 5:
            for (Shape shape : {Shape::Wide, Shape::Vertical, Shape::Square}) {
                options.push_back({shape == Shape::Vertical ? T("VERTICAL")
                                   : shape == Shape::Square ? T("SQUARE")
                                                            : T("WIDE"),
                                   shared.shape == shape, true});
            }
            break;
        default:
            for (int mbps : {20, 40, 60, 100, 150}) {
                options.push_back({Format("%d", mbps),
                                   model.bitrate / 1'000'000 == mbps, true});
            }
            break;
    }

    const float optionHeight = 34.0f;
    float optionWidth = 0;
    std::vector<float> optionWidths;
    for (const Option& option : options) {
        const float w = (std::max)(ui.SegmentWidth(option.label), 62.0f);
        optionWidths.push_back(w);
        optionWidth += w;
    }

    // Above the cell, not below it. The cells are at the bottom of the window
    // now; a list opening downwards would open off-screen.
    const float oy = dropdownAnchor_.top - 8.0f - optionHeight;
    float ox = (std::min)(dropdownAnchor_.left, ui.Width() - theme::kPad - optionWidth);
    ui.Panel(D2D1::RectF(ox, oy, ox + optionWidth, oy + optionHeight), 9.0f);

    for (size_t i = 0; i < options.size(); ++i) {
        const D2D1_RECT_F rect = D2D1::RectF(ox, oy, ox + optionWidths[i], oy + optionHeight);
        if (i > 0) ui.Divider(ox, oy, oy + optionHeight);

        const bool clicked = ui.Segment(kIdDropdownBase + static_cast<int>(i),
                                        options[i].label, rect,
                                        options[i].selected, options[i].enabled);
        ox += optionWidths[i];
        if (!clicked) continue;

        switch (openDropdown_) {
            case 1: {
                const auto modes = model.PreferredModes();
                if (i < modes.size()) {
                    model.width = modes[i].width;
                    model.height = modes[i].height;
                    model.fps = (std::min)(model.fps, modes[i].maxFps);
                }
                break;
            }
            case 2: {
                const auto rates = model.FrameRatesForCurrentSize();
                if (i < rates.size()) model.fps = rates[i];
                break;
            }
            case 3:
                model.codec = (i == 1) ? "hevc" : "h264";
                break;
            case 5: {
                static const Shape kShapes[] = {Shape::Wide, Shape::Vertical,
                                                Shape::Square};
                if (i < 3) shared.shape = kShapes[i];
                // The phone is not told: the shape is made here, out of what it
                // already sends. Nothing to restart.
                openDropdown_ = 0;
                return;
            }
            default: {
                static const int kRates[] = {20, 40, 60, 100, 150};
                if (i < 5) model.bitrate = kRates[i] * 1'000'000;
                break;
            }
        }
        formatDirty_ = true;
        openDropdown_ = 0;
    }
}

// ---- top right: live stats -------------------------------------------------

void ProPanel::DrawStats(UiContext& ui, const CameraModel& model,
                         const AppModel& shared) {
    // The status only appears here while connected. Disconnected, it is the
    // only thing on screen and DrawDisconnected gives it the middle.
    if (!model.connected) return;

    const float x1 = ui.Width() - theme::kPad;
    const float mid = topBarBottom_ * 0.5f;

    // Right to left, so nothing is pinned to an edge it can be clipped by. The
    // old block was measured from a fixed 210-pixel width and a long device
    // name ran straight off the window.
    float x = x1;

    auto rightOf = [&](const std::string& text, TextStyle style,
                       const D2D1_COLOR_F& colour) {
        const float w = ui.MeasureText(text, style);
        ui.Text(text, D2D1::RectF(x - w, mid - 9.0f, x, mid + 9.0f), style, colour,
                Align::Left);
        x -= w + 16.0f;
    };

    const std::string line = Format("%.0f fps   %.0f Mb/s   %.0f ms",
                                    model.statFps, model.statMbps, model.statLatencyMs);

    // Gaps are the one number worth shouting about: it means frames were shed
    // and the picture is not what the camera saw.
    rightOf(line, TextStyle::Stat,
            model.statGaps > 0 ? theme::kWarn : theme::kText);

    // The meters sit beside the numbers rather than under them.
    //
    // Named, because there are two. An unlabelled pair of bars in a corner is a
    // puzzle: they look like one thing measured twice rather than the phone and
    // the room. This project has twice shipped a microphone that was silently
    // dead, and both times a meter that never moved would have said so in the
    // first second.
    auto meter = [&](const std::string& name, float peak, float hold, bool dead,
                     const D2D1_COLOR_F& live) {
        const float barWidth = 56.0f;
        const float right = x;
        const float left = right - barWidth;

        ui.Rect(D2D1::RectF(left, mid - 3.0f, right, mid + 3.0f), theme::kLedOff, 3.0f);

        // Square-rooted, not linear. Speech at a sane recording level sits
        // around a tenth of full scale, and a linear meter draws that as a stub
        // that looks broken.
        const float shown = std::sqrt((std::min)(1.0f, (std::max)(0.0f, peak)));
        const float held = std::sqrt((std::min)(1.0f, (std::max)(0.0f, hold)));
        const D2D1_COLOR_F colour = dead ? theme::kWarn
                                  : shown > 0.97f ? theme::kRecord   // clipping
                                                  : live;
        if (shown > 0.0f) {
            ui.Rect(D2D1::RectF(left, mid - 3.0f, left + barWidth * shown, mid + 3.0f),
                    colour, 3.0f);
        }
        if (held > 0.01f) {
            const float hx = left + barWidth * held;
            ui.Rect(D2D1::RectF(hx - 1.5f, mid - 3.0f, hx + 1.5f, mid + 3.0f),
                    theme::kText, 1.0f);
        }
        x = left - 8.0f;

        const float w = ui.MeasureText(name, TextStyle::Stat);
        ui.Text(name, D2D1::RectF(x - w, mid - 9.0f, x, mid + 9.0f), TextStyle::Stat,
                dead ? theme::kWarn : theme::kTextDim, Align::Left);
        x -= w + 16.0f;
    };

    if (shared.deskMic) {
        meter(shared.deskMicName.empty() ? T("this PC") : shared.deskMicName,
              shared.deskMicPeak, shared.deskMicHold, false, theme::kAccent);
    }
    if (model.CanUseMic() && model.micEnabled) {
        // Silence is only a fault when something is listening, and only after
        // long enough that a pause between words cannot be mistaken for it.
        meter(T("phone"), model.audioPeak, model.audioHold, model.micSilentMs >= 3000,
              theme::kGood);
    }

    // Everything that is wrong, in the middle, where there is room for a
    // sentence. These used to stack downwards on the right and push the meters
    // off the bottom of the block.
    std::vector<std::pair<std::string, D2D1_COLOR_F>> notes;
    if (model.statGaps > 0) {
        notes.push_back({Format("%d %s", model.statGaps, T("dropped")), theme::kWarn});
    }
    if (model.bitrateLimited && model.activeBitrate > 0) {
        notes.push_back({Format("%s %d Mb/s", T("link limited to"),
                                model.activeBitrate / 1000000), theme::kWarn});
    }
    if (model.storageFreeMb >= 0 && (model.recording || model.storageFreeMb < 5000)) {
        const bool tight = model.storageFreeMb < 5000;
        notes.push_back({Format("%lld GB %s", static_cast<long long>(model.storageFreeMb / 1000),
                                T("free")),
                         tight ? theme::kWarn : theme::kTextDim});
    }

    float nx = headerRight_ + 28.0f;
    for (const auto& note : notes) {
        const float w = ui.MeasureText(note.first, TextStyle::Stat);
        if (nx + w > x - 12.0f) break;      // no room; the log still has it
        ui.Text(note.first, D2D1::RectF(nx, mid - 9.0f, nx + w, mid + 9.0f),
                TextStyle::Stat, note.second, Align::Left);
        nx += w + 18.0f;
    }
}

// ---- bottom: lens stops and zoom -------------------------------------------

// The follow focus.
//
// Two marks and a duration. Set a mark from wherever the lens is now, then ask
// for the other one and the phone eases between them -- the curve runs there, so
// a late packet cannot turn a pull into a stutter.
//
// Drawn beside the lens stops because that is what it is: the other half of the
// lens. Hidden when the window is too narrow to hold both, and when focus is on
// automatic, where a mark would mean nothing.
// Grows rightwards from `left` and refuses to start if it would pass
// `maxRight`. It used to grow leftwards from an anchor, which was fine while
// the anchor was the lens bar and there was nothing to its left -- put in a row
// of cells it drew itself straight over two of them.
void ProPanel::DrawFollowFocus(UiContext& ui, CameraModel& model, Output& out,
                               float left, float y0, float height, float maxRight) {
    if (model.focusMode != FocusMode::Manual) return;

    const CameraInfo* camera = model.Camera();
    if (!camera || camera->minFocusDistance <= 0.0f) return;

    constexpr float kCell = 54.0f;
    const float width = kCell * 5;
    if (left + width > maxRight) return;      // no room; the keys still work

    ui.Panel(D2D1::RectF(left, y0, left + width, y0 + height), 9.0f);

    struct Cell {
        int id;
        std::string label;
        bool selected;
        bool enabled;
    };

    // A mark that has not been set cannot be gone to, and saying so by greying
    // the arrow is cheaper than an error nobody reads.
    const Cell cells[] = {
        {kIdFocusSetA, "SET A", false, true},
        {kIdFocusGoA, model.focusA >= 0.0f ? Format("%.1f", model.focusA) : "A",
         model.rampingFocus, model.focusA >= 0.0f},
        {kIdFocusSetB, "SET B", false, true},
        {kIdFocusGoB, model.focusB >= 0.0f ? Format("%.1f", model.focusB) : "B",
         model.rampingFocus, model.focusB >= 0.0f},
        {kIdRampTime, Format("%.0fs", model.rampMs / 1000.0f), false, true},
    };

    for (int i = 0; i < 5; ++i) {
        const D2D1_RECT_F rect =
            D2D1::RectF(left + kCell * i, y0, left + kCell * (i + 1), y0 + height);
        if (i > 0) ui.Divider(rect.left, y0, y0 + height);

        if (!ui.Segment(cells[i].id, cells[i].label, rect, cells[i].selected,
                        cells[i].enabled)) {
            continue;
        }
        switch (cells[i].id) {
            case kIdFocusSetA: model.focusA = model.focusDistance; break;
            case kIdFocusSetB: model.focusB = model.focusDistance; break;
            case kIdFocusGoA:
            case kIdFocusGoB: {
                const float target = cells[i].id == kIdFocusGoA ? model.focusA : model.focusB;
                out.commands.push_back(MakeRampCommand("focus", target, model.rampMs));
                // The panel's own idea of where focus is has to move with it, or
                // the ruler snaps back the moment anything else touches it.
                model.focusDistance = target;
                break;
            }
            case kIdRampTime: {
                // One, two, three, five. Longer than five is a move nobody makes
                // with a button, and shorter than one is a cut.
                const int steps[] = {1000, 2000, 3000, 5000};
                int next = 0;
                for (int s = 0; s < 4; ++s) {
                    if (steps[s] == model.rampMs) next = (s + 1) % 4;
                }
                model.rampMs = steps[next];
                break;
            }
            default: break;
        }
    }
}

void ProPanel::DrawZoom(UiContext& ui, CameraModel& model, Output& out) {
    const CameraInfo* camera = model.Camera();
    if (!camera) return;

    const float height = theme::kChipHeight;

    // The right-hand end of the row that is always there. Centred at the bottom
    // of the window it belonged to nothing; here it is plainly one of the
    // controls, and it is the one image control anybody reaches for mid-call.
    const float y0 = rowLive_ + (34.0f - height) * 0.5f;

    // The lens stops, and the follow focus beside them. Both belong to setting
    // a shot up rather than to being in a call, so they come with pro; the
    // slider below stays either way, because zoom is the one image control
    // anybody reaches for mid-call.
    if (pro_) {
        // One segmented control, the way a phone camera shows its lenses. Six
        // separate buttons in a row is the same information arranged as clutter.
        const auto stops = model.LensStops();
        const float cellWidth = 52.0f;
        const float width = cellWidth * stops.size();
        const float x0 = ui.Width() - theme::kPad - width;

        ui.Panel(D2D1::RectF(x0, y0, x0 + width, y0 + height), 9.0f);

        float x = x0;
        for (size_t i = 0; i < stops.size(); ++i) {
            const D2D1_RECT_F rect = D2D1::RectF(x, y0, x + cellWidth, y0 + height);
            if (i > 0) ui.Divider(x, y0, y0 + height);

            // A stop counts as current when the zoom is within a few percent, so
            // the mark does not flicker off the moment the slider is nudged.
            const bool current = std::fabs(model.zoom - stops[i]) < stops[i] * 0.05f;
            if (ui.Segment(kIdLensBase + static_cast<int>(i), Format("%.2gx", stops[i]),
                           rect, current)) {
                model.zoom = stops[i];
                out.commands.push_back(MakeZoomCommand(model.zoom));
            }
            x += cellWidth;
        }
    }

    // Beside the stops when they are there, in their place when they are not.
    // Either way it ends where the window ends, which is where the eye already
    // is after reading the row.
    const float valueWidth = 44.0f;
    const float sliderWidth = 180.0f;
    const float right = pro_ ? ui.Width() - theme::kPad -
                                   52.0f * static_cast<float>(model.LensStops().size()) -
                                   theme::kGap
                             : ui.Width() - theme::kPad - valueWidth;
    const D2D1_RECT_F sliderRect = D2D1::RectF(right - sliderWidth, y0,
                                               right, y0 + height);

    // Zoom travels logarithmically too: half a lens range should not sit inside
    // the first tenth of the slider.
    float t = ToLog(model.zoom, camera->zoomMin, camera->zoomMax);
    if (ui.Slider(kIdZoomSlider, sliderRect, t, 0.0f, 1.0f)) {
        model.zoom = FromLog(t, camera->zoomMin, camera->zoomMax);
        out.commands.push_back(MakeZoomCommand(model.zoom));
    }

    // The number the slider is at. Without pro there are no lens stops above it
    // to read the zoom off, and a slider with no value on it is a control you
    // have to experiment with to understand.
    if (!pro_) {
        ui.Text(Format("%.2gx", model.zoom),
                D2D1::RectF(sliderRect.right + 8.0f, sliderRect.top,
                            ui.Width() - theme::kPad, sliderRect.bottom),
                TextStyle::Chip, theme::kTextDim, Align::Left);
    }
}

// ---- top left: the wordmark -------------------------------------------------

void ProPanel::DrawMark(UiContext& ui, float x, float y, float size,
                        const D2D1_COLOR_F& body) {
    // Drawn rather than loaded. Two thick diagonal bars are exactly what the
    // aperture X is made of, so a bitmap would buy nothing and cost a second
    // asset path, a second failure mode, and a second thing to keep in step
    // with brand/xcam-mark.svg.
    //
    // The fractions are the mark's own geometry on its 1024 grid: bars 146
    // wide, outer corners at 176, arms ending 120 from the centre.
    const float bar = size * 0.1426f;
    const float outer = size * 0.1719f;
    const float inner = size * 0.4171f;

    const float lo = x + outer, hi = x + size - outer;
    const float loY = y + outer, hiY = y + size - outer;
    const float il = x + inner, ir = x + size - inner;
    const float it = y + inner, ib = y + size - inner;

    ui.Line(lo, loY, il, it, body, bar);         // top left
    ui.Line(lo, hiY, il, ib, body, bar);         // bottom left
    ui.Line(hi, hiY, ir, ib, body, bar);         // bottom right
    ui.Line(hi, loY, ir, it, theme::kAccent, bar);   // the tally
}

void ProPanel::DrawHeader(UiContext& ui, const CameraModel& model) {
    const float size = 18.0f;
    const float mid = topBarBottom_ * 0.5f;
    DrawMark(ui, theme::kPad, mid - size * 0.5f, size, theme::kTextDim);

    // One line, in the order somebody would ask it: which phone, over what,
    // sending what. It used to take three places on screen -- the phone's name
    // was nowhere at all, the transport was a line under the frame rate, and
    // the format was a row of chips that also happened to be controls.
    std::string line = "XCAM";
    if (model.connected) {
        if (!model.device.deviceName.empty()) line += "   " + model.device.deviceName;
        if (!model.transportLabel.empty()) line += "   ·   " + model.transportLabel;

        // The format is left off in pro mode: the cells that set it are in the
        // bottom bar, and a line that also stated it was a repetition.
        if (!pro_) {
            line += "   ·   " + Format("%dp%d", model.height, model.fps) + "  " +
                    (model.codec == "hevc" ? "HEVC" : "H.264");
        }
    }

    const float x0 = theme::kPad + size + 10.0f;
    ui.Text(line, D2D1::RectF(x0, mid - 9.0f, x0 + 700.0f, mid + 9.0f),
            TextStyle::Chip, theme::kText, Align::Left);

    headerRight_ = x0 + ui.MeasureText(line, TextStyle::Chip);
    formatBarRight_ = headerRight_;
}

// ---- top centre: the recording banner ---------------------------------------

void ProPanel::DrawRecordBanner(UiContext& ui, const CameraModel& model) {
    if (!model.recording) return;

    // A recording used to be a line of small text among the frame rate and the
    // bitrate. It is the one state where not noticing costs something that
    // cannot be recovered, so it no longer competes for attention.
    const std::string label = FormatDuration(model.recordMs) + "   " +
                              FormatBytes(model.recordBytes) + "   " +
                              (model.recordToPc ? "PC" : "PHONE");

    const float width = ui.MeasureText(label, TextStyle::Chip) + 60.0f;
    const float height = 30.0f;
    const float x0 = (ui.Width() - width) * 0.5f;

    // Centred on the top row when it fits, one row down when it does not. On a
    // narrow window the format chips and the stats block reach into the middle,
    // and a banner drawn over either of them would be worse than a banner
    // drawn slightly lower.
    const float statsLeft = ui.Width() - theme::kPad - 210.0f;
    const bool fits = x0 > formatBarRight_ + theme::kGap &&
                      x0 + width < statsLeft - theme::kGap;
    const float y0 = fits ? theme::kPad
                          : theme::kPad + theme::kChipHeight + theme::kGap;

    ui.Panel(D2D1::RectF(x0, y0, x0 + width, y0 + height), height * 0.5f,
             theme::kRecordSoft);

    // One pulse a second: the cadence of a tally light, and of every recording
    // indicator anyone has ever seen.
    const double phase = now_ - std::floor(now_);
    const float alpha = static_cast<float>(
        0.4 + 0.6 * (phase < 0.5 ? 1.0 - phase * 2.0 : (phase - 0.5) * 2.0));
    const float dot = 9.0f;
    const float cx = x0 + 19.0f;
    const float cy = y0 + height * 0.5f;
    ui.Rect(D2D1::RectF(cx - dot * 0.5f, cy - dot * 0.5f,
                        cx + dot * 0.5f, cy + dot * 0.5f),
            theme::Rgba(0xFF3B30, alpha), dot * 0.5f);

    ui.Text(label, D2D1::RectF(x0 + 34.0f, y0, x0 + width - 12.0f, y0 + height),
            TextStyle::Chip, theme::kText, Align::Left);
}

// ---- centre: nothing connected ----------------------------------------------

void ProPanel::DrawDisconnected(UiContext& ui, const CameraModel& model) {
    if (model.connected) return;

    // The window is empty at this point and the only thing worth saying is
    // where we are looking. Saying it in the corner, in the space reserved for
    // a frame rate, is the wrong size for the only message on screen.
    const float size = 56.0f;
    const float cy = ui.Height() * 0.5f;
    DrawMark(ui, (ui.Width() - size) * 0.5f, cy - size - 20.0f, size, theme::kTextDim);

    ui.Text(model.status, D2D1::RectF(0, cy + 10.0f, ui.Width(), cy + 42.0f),
            TextStyle::Value, theme::kTextDim, Align::Center);
}

// ---- bars -------------------------------------------------------------------

float ProPanel::BarWidth(UiContext& ui, const BarCell* cells, size_t count) const {
    float width = 0;
    for (size_t i = 0; i < count; ++i) {
        width += (std::max)(ui.SegmentWidth(cells[i].label, cells[i].style), 78.0f);
    }
    return width;
}

int ProPanel::DrawBar(UiContext& ui, const BarCell* cells, size_t count,
                      float x, float y, float height, bool withPanel) {
    if (count == 0) return 0;

    float width = 0;
    std::vector<float> widths;
    for (size_t i = 0; i < count; ++i) {
        const float w = (std::max)(ui.SegmentWidth(cells[i].label, cells[i].style), 78.0f);
        widths.push_back(w);
        width += w;
    }

    if (withPanel) ui.Panel(D2D1::RectF(x, y, x + width, y + height), 9.0f);

    int clicked = 0;
    float cx = x;
    for (size_t i = 0; i < count; ++i) {
        const D2D1_RECT_F rect = D2D1::RectF(cx, y, cx + widths[i], y + height);
        if (i > 0) ui.Divider(cx, y, y + height);
        if (ui.Segment(cells[i].id, cells[i].label, rect, cells[i].on,
                       cells[i].enabled, cells[i].style)) {
            clicked = cells[i].id;
        }
        cx += widths[i];
    }
    return clicked;
}

// ---- bottom left: devices and output ----------------------------------------

void ProPanel::DrawLookChips(UiContext& ui, CameraModel& model, AppModel& shared,
                             Output& out) {
    const CameraInfo* camera = model.Camera();
    if (!camera) return;

    const D2D1_RECT_F tray = TabTray(ui);
    const float rowH = 34.0f;
    const float y = tray.top + (tray.bottom - tray.top - rowH) * 0.5f;

    std::vector<BarCell> look;
    look.push_back({kIdLogToggle, "LOG", model.logProfile, model.CanLogProfile(),
                    ChipStyle::Toggle});
    // The lamp says whether a grade is loaded, so the label can be its name
    // rather than an instruction. Trimmed: a LUT is often called something like
    // "FCMP FULL Contrast v3.cube", and the whole of that in a chip pushed
    // every other control off the row.
    std::string lut = shared.lutName;
    if (lut.size() > 14) lut = lut.substr(0, 13) + "...";
    look.push_back({kIdLutButton, lut.empty() ? "LUT" : lut, !shared.lutName.empty(),
                    true, ChipStyle::Toggle});
    // The camera operator, such as it is. Off unless asked for: a camera that
    // reframes itself while someone is setting up a shot is fighting them.
    look.push_back({kIdAutoFrame, T("FRAME"), model.autoFrame, true, ChipStyle::Toggle});
    if (camera->hasTorch) {
        look.push_back({kIdTorch, T("TORCH"), model.torch, true, ChipStyle::Toggle});
    }

    switch (DrawBar(ui, look.data(), look.size(), tray.left + 4.0f, y, rowH, false)) {
        case kIdLogToggle:
            // Log capture and the grade that makes it viewable sit together,
            // because turning one on without the other is what makes people
            // think log is broken.
            model.logProfile = !model.logProfile;
            out.commands.push_back(MakePictureProfileCommand(model.logProfile));
            break;
        case kIdLutButton:
            // Clicking a loaded LUT unloads it; there is no second button to
            // hunt for.
            if (shared.lutName.empty()) out.openLutDialog = true;
            else out.clearLut = true;
            break;
        case kIdAutoFrame:
            // The command follows from the presenter, which is where the
            // detector runs and where the crop is composed.
            model.autoFrame = !model.autoFrame;
            break;
        case kIdTorch:
            model.torch = !model.torch;
            out.commands.push_back(MakeTorchCommand(model.torch));
            break;
        default:
            break;
    }
}

void ProPanel::DrawRecordChips(UiContext& ui, CameraModel& model, Output& out) {
    const D2D1_RECT_F tray = TabTray(ui);
    const float rowH = 34.0f;
    const float y = tray.top + (tray.bottom - tray.top - rowH) * 0.5f;

    // The chip shows what was *asked* for.
    //
    // It used to show what came back, and that made it look broken: clicking it
    // changed the request while the label kept reporting the phone's unchanged
    // answer, so the control appeared stuck at whatever the last clamp had been.
    // A control has to move when it is pressed. What actually came back is on
    // the line beside it, when the two differ.
    const std::string sizeLabel =
        !model.recordEnabled         ? T("OFF")
        : model.recordWantHeight > 0 ? Format("%dp", model.recordWantHeight)
                                     : T("MAX");

    std::vector<BarCell> chips = {
        {kIdRecordFormat, sizeLabel, false, !model.recording, ChipStyle::Value},
    };
    if (model.recordEnabled) {
        chips.push_back({kIdRecordTarget, model.recordToPc ? T("TO PC") : T("TO PHONE"),
                         false, !model.recording, ChipStyle::Value});
    }

    const int clicked = DrawBar(ui, chips.data(), chips.size(), tray.left + 4.0f, y,
                                rowH, false);

    // A recording smaller than the stream is worth saying out loud. One sensor
    // drives both, so a large stream leaves little for the file -- which is the
    // wrong way round, since the file is the work and the stream is the
    // convenience. Nobody would guess that from a chip reading 720p.
    const int wanted = model.recordWantHeight > 0 ? model.recordWantHeight : model.height;
    if (model.recordEnabled && model.recordHeight > 0 && wanted > 0 &&
        model.recordHeight < wanted) {
        TrayNote(ui, tray, Format("%s %dp", T("the phone can only give the recording"),
                                  model.recordHeight), theme::kWarn);
    } else if (model.preRollGranted > 0) {
        TrayNote(ui, tray, Format("%s %ds", T("pre-roll"), model.preRollGranted),
                 theme::kTextDim);
    }

    if (clicked == kIdRecordFormat) {
        // Cycles the sizes the stream offers, then "the camera at its best",
        // then off. Off is part of the cycle rather than a separate control
        // because a recorder standing ready is not free -- it costs a few
        // milliseconds of latency, and at 4K the frame rate -- and someone who
        // only wants a webcam should be able to stop paying for it.
        const auto modes = model.PreferredModes();
        size_t next = 0;
        for (size_t i = 0; i < modes.size(); ++i) {
            if (modes[i].height == model.recordWantHeight) { next = i + 1; break; }
        }
        // Every branch moves the *request*. Two of them used to move the answer
        // instead -- left behind when the two were split apart -- so coming back
        // from off cleared a field nobody was reading while the request stayed
        // wherever the cycle had last stopped, and the recording came back at
        // that size no matter what the chip said.
        if (!model.recordEnabled) {
            // Back on at the camera's best, rather than wherever it was turned
            // off. Somebody switching recording back on wants a recording.
            model.recordEnabled = true;
            model.recordWantWidth = model.recordWantHeight = 0;
        } else if (model.recordWantWidth == 0) {
            model.recordWantWidth = modes.empty() ? 0 : modes[0].width;
            model.recordWantHeight = modes.empty() ? 0 : modes[0].height;
        } else if (next >= modes.size()) {
            model.recordEnabled = false;
        } else {
            model.recordWantWidth = modes[next].width;
            model.recordWantHeight = modes[next].height;
        }
        out.commands.push_back(MakeRecordConfigCommand(
            model.recordEnabled, model.recordToPc, model.recordWantWidth,
            model.recordWantHeight, model.recordRate(), 0, model.recordCodec,
            model.preRoll));
    } else if (clicked == kIdRecordTarget) {
        // Over Wi-Fi the answer has to be the phone, and someone who has just
        // switched transports needs to change this in one press.
        model.recordToPc = !model.recordToPc;
        out.commands.push_back(MakeRecordConfigCommand(
            model.recordEnabled, model.recordToPc, model.recordWantWidth,
            model.recordWantHeight, model.recordRate(), 0, model.recordCodec,
            model.preRoll));
    }
}

// ---- the row that is always there ------------------------------------------

void ProPanel::DrawDeviceTray(UiContext& ui, CameraModel& model, AppModel& shared,
                              Output& out) {
    const CameraInfo* camera = model.Camera();
    if (!camera) return;

    const float rowHeight = 34.0f;
    float x = theme::kPad;

    // The cut comes first, and it is never behind a tab.
    //
    // Losing a take because the record button was hidden, or missing a cut
    // because the camera row was, are the two mistakes this panel must not
    // allow. A rig of one hides the group entirely rather than showing a single
    // button that does nothing.
    if (shared.rig.size() > 1) {
        std::vector<BarCell> cut;
        for (size_t i = 0; i < shared.rig.size(); ++i) {
            const AngleSummary& each = shared.rig[i];
            // Cutting to an angle that has never decoded a frame would cut to
            // black, so it is shown and refused rather than hidden -- somebody
            // needs to see that the second phone is there and not ready.
            cut.push_back({kIdAngleBase + static_cast<int>(i), each.label,
                           i == shared.program, each.hasPicture, ChipStyle::Toggle});
        }
        const int clicked = DrawBar(ui, cut.data(), cut.size(), x, rowLive_, rowHeight);
        if (clicked >= kIdAngleBase) out.cutTo = clicked - kIdAngleBase;
        x += BarWidth(ui, cut.data(), cut.size()) + theme::kGap * 2;
    }

    // Armed and not recording, the button says how far back it can reach -- and
    // while the ring is still filling it says how far it has got, because the
    // seconds after arming are the ones where it cannot yet do what the label
    // promises.
    std::string recLabel;
    if (model.recording) {
        recLabel = FormatDuration(model.recordMs);
    } else if (model.preRollGranted > 0) {
        const int64_t armedMs = model.preRollGranted * 1000LL;
        const int64_t fill = model.preRollFillMs < armedMs ? model.preRollFillMs : armedMs;
        recLabel = Format("< %llds", static_cast<long long>(fill / 1000));
    } else {
        recLabel = T("RECORD");
    }

    std::vector<BarCell> live;
    if (model.CanRecord()) {
        live.push_back({kIdRecord, recLabel, model.recording, model.recordEnabled,
                        ChipStyle::Record});
    }
    // "WEBCAM", not "CAM". The old label could as easily have meant "choose a
    // camera", and the one thing this button must be unambiguous about is
    // whether other applications can see you.
    live.push_back({kIdVirtualCam, T("WEBCAM"), shared.virtualCamera, true,
                    ChipStyle::Toggle});
    if (model.CanUseMic()) {
        live.push_back({kIdMic, T("MIC"), model.micEnabled, true, ChipStyle::Toggle});
    }
    live.push_back({kIdPro, T("PRO"), pro_, true, ChipStyle::Toggle});
    // Everything chosen once and expected to survive lives behind this rather
    // than on the panel, which is for what changes while you are working.
    live.push_back({kIdSettings, T("Settings"), false, true, ChipStyle::Value});

    switch (DrawBar(ui, live.data(), live.size(), x, rowLive_, rowHeight)) {
        case kIdRecord:
            out.commands.push_back(MakeRecordCommand(model.recording ? "stop" : "start"));
            break;
        case kIdVirtualCam:
            shared.virtualCamera = !shared.virtualCamera;
            break;
        case kIdMic:
            model.micEnabled = !model.micEnabled;
            out.commands.push_back(MakeAudioCommand(model.micEnabled));
            break;
        case kIdPro:
            pro_ = !pro_;
            // A control left selected under a tier that is no longer drawn
            // would keep the keyboard and the ruler to itself.
            if (!pro_) { selected_ = ProControl::None; openDropdown_ = 0; }
            break;
        case kIdSettings:
            out.openSettings = true;
            break;
        default:
            break;
    }
}

// ---- typed values ----------------------------------------------------------

bool ProPanel::CommitTypedValue(CameraModel& model, Output& out) {
    const CameraInfo* camera = model.Camera();
    if (!camera || editBuffer_.empty()) return false;

    const std::string& text = editBuffer_;

    switch (selected_) {
        case ProControl::Iso: {
            const int value = std::atoi(text.c_str());
            if (value <= 0) return false;
            model.iso = std::clamp(value, camera->isoMin, camera->isoMax);
            model.exposureMode = ExposureMode::Manual;
            out.commands.push_back(MakeExposureCommand(true, model.iso, model.shutterNs));
            return true;
        }

        case ProControl::Shutter: {
            // Cameras are spoken to in fractions, so accept the three forms
            // anyone would actually type: "1/120", a bare "120" meaning 1/120,
            // and "0.5" or "2s" for the long end.
            double seconds = 0;
            const size_t slash = text.find('/');
            if (slash != std::string::npos) {
                const double numerator = std::atof(text.substr(0, slash).c_str());
                const double denominator = std::atof(text.substr(slash + 1).c_str());
                if (denominator <= 0) return false;
                seconds = (numerator == 0 ? 1.0 : numerator) / denominator;
            } else {
                const double value = std::atof(text.c_str());
                if (value <= 0) return false;
                seconds = (value >= 1.0 && text.find('.') == std::string::npos)
                    ? 1.0 / value       // "120" reads as 1/120, the way a dial is marked
                    : value;            // "0.5" reads as half a second
            }

            const int64_t ns = static_cast<int64_t>(seconds * 1e9);
            model.shutterNs = std::clamp(ns, camera->exposureMinNs, camera->exposureMaxNs);
            model.exposureMode = ExposureMode::Manual;
            out.commands.push_back(MakeExposureCommand(true, model.iso, model.shutterNs));
            return true;
        }

        case ProControl::Focus: {
            // Typed as a distance, which is how anyone thinks about focus, and
            // converted to the dioptres Camera2 wants.
            if (text.find('i') != std::string::npos) {
                model.focusDistance = 0.0f;         // "inf"
            } else {
                const double metres = std::atof(text.c_str());
                if (metres <= 0) return false;
                model.focusDistance = std::clamp(static_cast<float>(1.0 / metres),
                                                 0.0f, camera->minFocusDistance);
            }
            model.focusMode = FocusMode::Manual;
            out.commands.push_back(MakeFocusCommand("manual", model.focusDistance));
            return true;
        }

        case ProControl::WhiteBalance: {
            const int kelvin = std::atoi(text.c_str());
            if (kelvin < 1000) return false;
            model.wbKelvin = std::clamp(kelvin, 2000, 10000);
            model.wbMode = "manual";
            out.commands.push_back(MakeWhiteBalanceCommand("manual", model.wbKelvin));
            return true;
        }

        case ProControl::Ev: {
            model.ev = std::clamp(static_cast<float>(std::atof(text.c_str())),
                                  camera->evMin, camera->evMax);
            out.commands.push_back(MakeEvCommand(model.ev));
            return true;
        }

        default:
            return false;
    }
}

// ---- bottom: the scrubber for whichever row is selected ---------------------

void ProPanel::DrawRuler(UiContext& ui, CameraModel& model, AppModel& shared,
                         Output& out) {
    if (selected_ == ProControl::None) return;

    // The grade happens on this machine, so its controls work whether or not a
    // phone is attached. Everything else asks the camera what its range is.
    const bool cameraless = selected_ == ProControl::Gain ||
                            selected_ == ProControl::Contrast ||
                            selected_ == ProControl::Saturation ||
                            selected_ == ProControl::Warmth ||
                            selected_ == ProControl::LutAmount;

    const CameraInfo* camera = model.Camera();
    if (!camera && !cameraless) return;

    // Directly above the bar, over the picture, like the dropdowns. It used to
    // be measured from the bottom of the window against a column that no longer
    // exists, which put it through the middle of the tiers.
    const float width = (std::min)(560.0f, ui.Width() - 2 * theme::kPad);
    const float x0 = theme::kPad;
    const float y1 = bottomBarTop_ - theme::kGap;
    const float y0 = y1 - theme::kRulerHeight;

    const D2D1_RECT_F rect = D2D1::RectF(x0, y0, x0 + width, y1);
    ui.Panel(rect);

    // White balance is a set of named presets, not a continuum, so it gets chips
    // instead of a ruler -- a scale of light sources would read as nonsense.
    if (selected_ == ProControl::WhiteBalance && camera) {
        const size_t count = sizeof(kWbModes) / sizeof(kWbModes[0]);
        const float groupWidth = width - 2 * theme::kPad;
        const float cellWidth = groupWidth / count;
        const float cellHeight = 36.0f;
        const float cellY = y0 + (theme::kRulerHeight - cellHeight) * 0.5f;
        float x = x0 + theme::kPad;

        ui.Panel(D2D1::RectF(x, cellY, x + groupWidth, cellY + cellHeight), 9.0f);

        for (size_t i = 0; i < count; ++i) {
            const bool available =
                std::find(camera->awbModes.begin(), camera->awbModes.end(), kWbModes[i]) !=
                camera->awbModes.end();
            const D2D1_RECT_F cell = D2D1::RectF(x, cellY, x + cellWidth, cellY + cellHeight);
            if (i > 0) ui.Divider(x, cellY, cellY + cellHeight);

            if (ui.Segment(kIdRuler + static_cast<int>(i), kWbLabels[i], cell,
                           model.wbMode == kWbModes[i], available)) {
                model.wbMode = kWbModes[i];
                out.commands.push_back(MakeWhiteBalanceCommand(model.wbMode, model.wbKelvin));
            }
            x += cellWidth;
        }
        return;
    }

    float t = 0.0f;
    std::string readout;
    int ticks = 20;

    switch (selected_) {
        case ProControl::Iso:
            t = ToLog(static_cast<float>(model.iso),
                      static_cast<float>(camera->isoMin), static_cast<float>(camera->isoMax));
            readout = Format("ISO %d", model.iso);
            break;
        case ProControl::Shutter:
            t = ToLog(static_cast<float>(model.shutterNs),
                      static_cast<float>(camera->exposureMinNs),
                      static_cast<float>(camera->exposureMaxNs));
            readout = model.shutterClamped
                ? FormatShutter(model.shutterNs) + "  (capped by " + Format("%d fps)", model.fps)
                : FormatShutter(model.shutterNs);
            break;
        case ProControl::Focus:
            t = camera->minFocusDistance > 0 ? model.focusDistance / camera->minFocusDistance : 0;
            readout = FormatFocus(model.focusDistance);
            ticks = 16;
            break;
        case ProControl::Ev:
            t = camera->evMax > camera->evMin
                ? (model.ev - camera->evMin) / (camera->evMax - camera->evMin) : 0.5f;
            readout = Format("%+.1f EV", model.ev);
            ticks = static_cast<int>((camera->evMax - camera->evMin) * 3.0f);
            break;

        // The grade runs on a plain -1..1, so the ruler maps it straight: half
        // way along is neutral, which is where somebody expects to find it.
        case ProControl::Gain:
            t = (shared.gain + 2.0f) / 4.0f;
            readout = Format("%+.2f", shared.gain);
            break;
        case ProControl::Contrast:
            t = (shared.contrast + 1.0f) * 0.5f;
            readout = Format("%+.2f", shared.contrast);
            break;
        case ProControl::Saturation:
            t = (shared.saturation + 1.0f) * 0.5f;
            readout = Format("%+.2f", shared.saturation);
            break;
        case ProControl::Warmth:
            t = (shared.warmth + 1.0f) * 0.5f;
            readout = Format("%+.2f", shared.warmth);
            break;
        case ProControl::LutAmount:
            t = shared.lutAmount;
            readout = Format("LUT %.0f%%", shared.lutAmount * 100.0f);
            ticks = 10;
            break;

        default: return;
    }

    const std::string shown = editing_ ? editBuffer_ : readout;
    const std::string hint = editing_ ? "ENTER TO APPLY" : "TYPE A VALUE";

    const bool dragging = ui.Ruler(kIdRuler, rect, t, ticks, shown, editing_, hint);
    const bool released = ui.GetInput().mouseReleased;

    // Throttle while dragging, but never drop the final value: a slider that
    // leaves the camera one step behind where it was let go is worse than one
    // that updates a little less smoothly.
    const bool emit = dragging && (released || now_ - lastCommandTime_ >= 0.05);
    if (!dragging) return;

    editing_ = false;
    editBuffer_.clear();

    // The model follows the drag every frame so the readout stays live; only
    // the command to the phone is throttled.
    std::string command;
    switch (selected_) {
        case ProControl::Iso:
            model.iso = static_cast<int>(FromLog(t, static_cast<float>(camera->isoMin),
                                                 static_cast<float>(camera->isoMax)));
            command = MakeExposureCommand(true, model.iso, model.shutterNs);
            break;

        case ProControl::Shutter:
            model.shutterNs = static_cast<int64_t>(
                FromLog(t, static_cast<float>(camera->exposureMinNs),
                        static_cast<float>(camera->exposureMaxNs)));
            command = MakeExposureCommand(true, model.iso, model.shutterNs);
            break;

        case ProControl::Focus:
            model.focusDistance = t * camera->minFocusDistance;
            command = MakeFocusCommand("manual", model.focusDistance);
            break;

        case ProControl::Ev:
            model.ev = camera->evMin + t * (camera->evMax - camera->evMin);
            command = MakeEvCommand(model.ev);
            break;

        // No command: the grade is applied here, by the shader, and the phone
        // is not told about it. That is the whole point -- what it sends stays
        // gradeable.
        case ProControl::Gain:       shared.gain = t * 4.0f - 2.0f; break;
        case ProControl::Contrast:   shared.contrast = t * 2.0f - 1.0f; break;
        case ProControl::Saturation: shared.saturation = t * 2.0f - 1.0f; break;
        case ProControl::Warmth:     shared.warmth = t * 2.0f - 1.0f; break;
        case ProControl::LutAmount:  shared.lutAmount = t; break;

        default:
            return;
    }

    if (emit && !command.empty()) {
        out.commands.push_back(std::move(command));
        lastCommandTime_ = now_;
    }
}

}  // namespace xcam
