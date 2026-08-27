#pragma once

// The settings sheet.
//
// Everything here is a choice someone makes once and expects to survive: where
// recordings go, how the desktop should look for the phone, which language to
// be spoken to in. None of it belongs on the panel over the picture, which is
// for the things that change while you are working.
//
// Drawn as an overlay by ProPanel rather than as a window of its own. A second
// window would need its own render target, its own message loop and its own
// theme, for a page of rows.

#include "app/camera_model.h"
#include "app/ui/ui_context.h"

#include <string>
#include <vector>

namespace xcam {

class Settings;

class SettingsPanel {
public:
    struct Output {
        std::vector<std::string> commands;

        // Raised rather than acted on: a modal folder dialog inside the draw
        // would block the presenter mid-frame with the lock held.
        bool chooseFolder = false;

        // The connection was changed, so the worker should drop what it has and
        // look again.
        bool reconnect = false;

        // The takes browser was asked for. Raised rather than opened here so
        // one sheet does not reach into another mid-draw.
        bool openTakes = false;

        // The Run key was toggled. Written outside the draw, like everything
        // else here that touches something outside this process.
        bool autostartChanged = false;

        // The input was changed, so the capture has to be reopened on it.
        bool restartDeskMic = false;
    };

    bool IsOpen() const { return open_; }
    void Open() { open_ = true; }
    void Close();
    void Toggle() { if (open_) Close(); else Open(); }

    // True while a text field has the keyboard, so the window gives Escape to
    // the field rather than treating it as "close the sheet".
    bool IsEditing() const { return editing_ != Field::None; }

    Output Draw(ui::UiContext& ui, CameraModel& model, AppModel& shared,
                Settings& settings);

private:
    enum class Field { None, Host, PairCode, PresetName };

    void DrawSection(ui::UiContext& ui, const char* title, float& y);
    bool DrawRow(ui::UiContext& ui, int id, const char* label, const std::string& value,
                 float& y, bool enabled = true);

    void CommitField(CameraModel& model, AppModel& shared, Settings& settings,
                     Output& out);

    bool open_ = false;
    Field editing_ = Field::None;
    std::string editBuffer_;
    int pendingPresetSlot_ = -1;

    float x0_ = 0, width_ = 0;

    // How tall the content came out last frame, and how far it is scrolled.
    //
    // Measured rather than computed: the sheet grows a row whenever something is
    // added to it, and a height worked out by hand is a height that goes stale
    // the first time it does. Immediate mode has no layout pass to ask, so the
    // previous frame's answer is used -- wrong once, on the first frame, and
    // right from then on.
    float contentHeight_ = 0;
    float scroll_ = 0;
};

}  // namespace xcam
