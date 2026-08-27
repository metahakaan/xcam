#pragma once

// What the phone has on it.
//
// The recordings written locally are the full-quality ones -- the reason the
// link never carries the master -- and until now the only way to reach them was
// `adb pull` and a script. Pre-roll makes takes cheap and frequent, which makes
// a script the wrong interface: a person who records twenty short takes in an
// afternoon should not have to read a directory listing to find the one they
// meant.
//
// An overlay like the settings sheet, and for the same reason: a second window
// would need its own render target, message loop and theme.

#include "app/camera_model.h"
#include "app/ui/ui_context.h"

#include <string>
#include <vector>

namespace xcam {

class TakesPanel {
public:
    struct Output {
        std::vector<std::string> commands;

        // Raised rather than acted on, so a fetch decides its destination on the
        // thread that owns the model instead of inside the draw.
        std::string fetch;
    };

    bool IsOpen() const { return open_; }
    void Open() { open_ = true; refresh_ = true; }
    void Close() { open_ = false; confirmDelete_.clear(); }
    void Toggle() { if (open_) Close(); else Open(); }

    Output Draw(ui::UiContext& ui, CameraModel& model);

private:
    bool open_ = false;

    // Set when the sheet opens, cleared once the listing has been asked for.
    // Opening is the only moment we know the person wants this to be current.
    bool refresh_ = false;

    float scroll_ = 0.0f;

    // Deleting a take is the one thing here that cannot be undone, so it asks
    // once. Holds the name awaiting confirmation, empty otherwise.
    std::string confirmDelete_;
};

}  // namespace xcam
