#pragma once

// The camera panel. Draws itself from CameraModel and returns the control
// commands the user triggered, as protocol JSON ready to hand to
// NetClient::SendControl -- it never touches the socket itself, so the whole
// panel stays testable and thread-agnostic.

#include "app/camera_model.h"
#include "app/ui/ui_context.h"

#include <string>
#include <vector>

namespace xcam {

class ProPanel {
public:
    // Commands produced this frame, in the order the user caused them.
    struct Output {
        std::vector<std::string> commands;
        bool wantsKeyFrame = false;

        // The panel cannot open a file dialog itself without dragging the
        // window into it, so it asks and the host obliges.
        bool openLutDialog = false;
        bool clearLut = false;

        // The settings sheet, which the panel does not own -- it only has the
        // room for the way in.
        bool openSettings = false;

        // The prompter wants a script. Raised like the LUT dialog, and for the
        // same reason: a modal file dialog inside a draw would block the
        // presenter mid-frame with the lock held.
        bool openPrompterFile = false;

        // Cut to this angle, or -1 for "stay where you are". Raised rather than
        // done here: switching program moves the renderer, the tally and the
        // virtual microphone, and none of those belong inside a draw.
        int cutTo = -1;
    };

    // `model` is edited in place so the panel responds immediately; the phone's
    // ACK later corrects anything it clamped.
    Output Draw(ui::UiContext& ui, CameraModel& model, AppModel& shared, double now);

    // True while the pointer is over a control, so the window knows not to treat
    // the click as a tap-to-focus on the picture.
    bool WantsMouse() const { return wantsMouse_; }

    // True while a value is being typed, so the window can give Escape to the
    // edit rather than treating it as "close".
    bool IsEditing() const { return editing_; }

    void NotePointerActivity(double now) { lastActivity_ = now; }

    // Pro mode: whether every control is on the panel, or only the few anybody
    // uses in a call.
    //
    // The panel had grown to five clusters in five places, and a person opening
    // it for the first time could not tell what half of them were for. Most of
    // them are things you set once for a shoot -- an ISO, a log profile, a
    // lens stop -- not things you touch while talking to somebody. So they are
    // behind one switch, and what is left is what a webcam needs: record, the
    // camera, the microphone, and zoom.
    //
    // Remembered between runs, because somebody who wants the full panel wants
    // it every time.
    bool IsPro() const { return pro_; }
    void SetPro(bool on) { pro_ = on; }

    // Which group of pro controls is showing, so it survives a restart.
    int ProTab() const { return proTab_; }
    void SetProTab(int tab) { proTab_ = tab; }
    float Opacity(double now) const;

private:
    // The tab strip, and whichever group it has selected.
    //
    // Everything pro used to be on screen at once: nine readouts, six chips and
    // the live row, three tiers deep in one corner of a window that was half
    // empty. They are not used together -- somebody setting an exposure is not
    // also choosing a codec -- so only one group is drawn, and the strip says
    // which and offers the others.
    // The rectangle every tab draws into: the width of the bar, the same
    // whichever tab is showing, so switching does not change the panel's shape.
    D2D1_RECT_F TabTray(ui::UiContext& ui) const;

    // Whatever a tab has to say about itself, at the far end of its tray.
    void TrayNote(ui::UiContext& ui, const D2D1_RECT_F& tray, const std::string& text,
                  const D2D1_COLOR_F& colour);

    void DrawProTabs(ui::UiContext& ui, const CameraModel& model);
    void DrawTabContent(ui::UiContext& ui, CameraModel& model, AppModel& shared,
                        Output& out);

    // The cells and chip groups each tab is made of.
    void DrawExposureCells(ui::UiContext& ui, CameraModel& model, Output& out);
    void DrawFormatCells(ui::UiContext& ui, CameraModel& model, AppModel& shared);
    void DrawLookChips(ui::UiContext& ui, CameraModel& model, AppModel& shared,
                       Output& out);
    void DrawRecordChips(ui::UiContext& ui, CameraModel& model, Output& out);
    void DrawColourCells(ui::UiContext& ui, AppModel& shared);
    void DrawPrompterChips(ui::UiContext& ui, AppModel& shared, Output& out);

    // The options a format cell opens, drawn last so they sit over everything.
    void DrawDropdown(ui::UiContext& ui, CameraModel& model, AppModel& shared);
    void DrawStats(ui::UiContext& ui, const CameraModel& model,
                   const AppModel& shared);
    // Grows rightwards from `left`, and does not start at all if it would run
    // past `maxRight` -- it sits among other controls now rather than beside a
    // bar with nothing to its left.
    void DrawFollowFocus(ui::UiContext& ui, CameraModel& model, Output& out,
                         float left, float y0, float height, float maxRight);

    // The zoom. The slider is always there; the lens stops are a shoot's
    // control rather than a call's, so they come with pro.
    void DrawZoom(ui::UiContext& ui, CameraModel& model, Output& out);
    // Takes the shared model too: the grade it also drives lives there, and
    // unlike everything else on the ruler it works with no phone attached.
    void DrawRuler(ui::UiContext& ui, CameraModel& model, AppModel& shared,
                   Output& out);

    // Everything the picture and the sound are sent *to*, as against the pro
    // column, which decides what they look like. They shared one column until
    // it ran off the bottom of a 720p window.
    void DrawDeviceTray(ui::UiContext& ui, CameraModel& model, AppModel& shared,
                        Output& out);

    // One control in a bar. The caller fills these in and DrawBar lays them
    // out, which is what keeps every bar in the panel the same shape.
    struct BarCell {
        int id;
        std::string label;
        bool on;
        bool enabled;
        ui::ChipStyle style;
    };

    // Draws a group of cells as a single bar and returns the id of whichever
    // was clicked, or 0.
    // How wide DrawBar would draw these, so a second group can start after the
    // first rather than at a guessed offset.
    float BarWidth(ui::UiContext& ui, const BarCell* cells, size_t count) const;

    // `withPanel` false draws the cells without their own background, for a
    // group that sits inside a larger one.
    int DrawBar(ui::UiContext& ui, const BarCell* cells, size_t count,
                float x, float y, float height, bool withPanel = true);

    // The one line that says what you are looking at: the mark, the phone's
    // name, how it is connected and what it is sending. Always drawn -- it is
    // the answer to "is this thing working", which is the first question.
    void DrawHeader(ui::UiContext& ui, const CameraModel& model);
    void DrawRecordBanner(ui::UiContext& ui, const CameraModel& model);
    void DrawDisconnected(ui::UiContext& ui, const CameraModel& model);

    // The aperture X, drawn from its own geometry. Shared by the wordmark and
    // the disconnected screen, which want it at wildly different sizes.
    void DrawMark(ui::UiContext& ui, float x, float y, float size,
                  const D2D1_COLOR_F& body);

    // Reads the typed text for the selected control and writes it into the
    // model. False when the text is not a value that control accepts.
    bool CommitTypedValue(CameraModel& model, Output& out);

    // Typing state for the selected control. Empty means the ruler is in
    // charge; anything else means the user is entering a value by hand.
    std::string editBuffer_;
    bool editing_ = false;

    ProControl selected_ = ProControl::None;
    int openDropdown_ = 0;          // which format chip has its options showing
    bool wantsMouse_ = false;
    bool pro_ = false;

    // Where the format chips end, so the recording banner can keep clear of
    // them. Measured rather than assumed: the chips are sized to their text and
    // "2160p" is wider than "720p".
    float formatBarRight_ = 0;

    // Where the header line ends, so nothing is drawn through the phone's name.
    float headerRight_ = 0;

    // The chrome, measured each frame before anything is placed in it. Every
    // control lives between these two lines; nothing is pinned to an edge of
    // the window, which is how text came to be clipped off the right of it.
    float topBarBottom_ = 0;
    float bottomBarTop_ = 0;

    // The tiers of the bottom bar, laid out upwards from the bottom edge so a
    // tier that is not there costs no space.
    float rowLive_ = 0;
    float rowTabs_ = 0;
    float rowContent_ = 0;

    // Which group of pro controls is showing. Remembered between runs: somebody
    // who works in exposure comes back to exposure.
    int proTab_ = 0;

    // Where the open dropdown should be anchored, measured while its cell is
    // drawn and used after everything else is, so the options sit on top.
    D2D1_RECT_F dropdownAnchor_ = D2D1::RectF(0, 0, 0, 0);
    double lastActivity_ = 0;

    // Rebuilding the whole `set` command on every drag frame would restart the
    // pipeline continuously, so format changes are collected and emitted once.
    bool formatDirty_ = false;

    // Dragging a ruler at sixty frames a second would send sixty control
    // commands a second, each one a setRepeatingRequest on the phone. Rate
    // limiting keeps the camera responsive; the value at the end of the drag is
    // always sent, so nothing is lost.
    double lastCommandTime_ = 0;
    double now_ = 0;
};

}  // namespace xcam
