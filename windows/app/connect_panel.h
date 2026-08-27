#pragma once

// The first thing the window shows: how is the phone attached?
//
// It used to answer that question by itself. The transport was a setting three
// taps into the settings sheet, it defaulted to Auto, and Auto reaches for the
// cable first -- so somebody who wanted Wi-Fi had to plug in over USB anyway to
// find out where the sheet was, which is precisely backwards.
//
// So it is asked, once, in front of everything else, and the answer is kept.
// Over the empty preview rather than in a dialog: there is no picture yet, the
// window is doing nothing else, and this is the one thing standing between
// somebody and their camera working.

#include <string>
#include <vector>

#include "app/app_model.h"
#include "app/ui/ui_context.h"
#include "core/discovery.h"

namespace xcam {

class ConnectPanel {
public:
    struct Output {
        // A transport was chosen. The worker drops whatever it was attempting
        // and starts again on the one that was picked.
        bool chose = false;

        // Raised by "Type an address": the settings sheet owns text entry, and
        // duplicating a caret here to avoid opening it would be two carets.
        bool openSettings = false;
    };

    // Shown until a transport has been chosen, and again whenever somebody asks
    // for it back from the settings sheet.
    bool IsOpen() const { return open_; }
    void Open() { open_ = true; }
    void Close() { open_ = false; }

    // `found` is whatever the beacon has heard from lately. An empty list is
    // not a failure and is not presented as one: a phone that has not been
    // started yet is the ordinary case on a first run.
    Output Draw(ui::UiContext& ui, AppModel& shared,
                const std::vector<DiscoveredDevice>& found, bool adbAvailable);

private:
    bool open_ = true;
};

}  // namespace xcam
