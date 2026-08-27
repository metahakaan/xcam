#include "app/connect_panel.h"

#include "app/strings.h"
#include "app/ui/theme.h"

#include <algorithm>

namespace xcam {

using ui::Align;
using ui::ChipStyle;
using ui::TextStyle;
using ui::UiContext;

namespace {

// Well clear of every other panel's block. The settings sheet learned this the
// hard way: two controls sharing an id are one control as far as hit testing is
// concerned, and clicking a matte option used to change the focus peaking.
enum : int {
    kIdUsb = 1400,
    kIdWifi,
    kIdTypeAddress,
    kIdSkip,
    kIdFoundBase = 1420,       // one per discovered phone
};

}  // namespace

ConnectPanel::Output ConnectPanel::Draw(UiContext& ui, AppModel& shared,
                                        const std::vector<DiscoveredDevice>& found,
                                        bool adbAvailable) {
    Output out;
    if (!open_) return out;

    const float width = ui.Width();
    const float height = ui.Height();

    // Over everything, and dark enough that the empty preview behind it stops
    // competing. This is the only panel in the application that is allowed to
    // block the picture, because when it is up there is no picture.
    ui.Rect(D2D1::RectF(0, 0, width, height), theme::Rgba(0x000000, 0.72f));

    const float cardWidth = (std::min)(560.0f, width - 80.0f);
    const float cardHeight = 316.0f;
    const float x0 = (width - cardWidth) * 0.5f;
    const float y0 = (height - cardHeight) * 0.5f;

    // Opaque, unlike every other panel in the application.
    //
    // Panel() is translucent by design -- it floats over a picture and the
    // picture is the subject. Here there is no picture, and what showed through
    // instead was the "waiting for..." message this card exists to make
    // unnecessary, printed straight across the middle of it.
    const D2D1_RECT_F card = D2D1::RectF(x0, y0, x0 + cardWidth, y0 + cardHeight);
    ui.Rect(card, theme::Rgba(0x11151A, 1.0f), 14.0f);
    ui.Outline(card, theme::kPanelBorder, 14.0f);

    ui.Text(T("How is your phone connected?"),
            D2D1::RectF(x0 + 28.0f, y0 + 26.0f, x0 + cardWidth - 28.0f, y0 + 56.0f),
            TextStyle::ValueLarge, theme::kText);

    // ---- the two ways ------------------------------------------------------

    const float choiceTop = y0 + 74.0f;
    const float choiceHeight = 92.0f;
    const float gap = 14.0f;
    const float choiceWidth = (cardWidth - 56.0f - gap) * 0.5f;

    // Drawn as two panels with their own text rather than as chips: this is the
    // decision the window is asking for, and a pair of chips the size of a
    // resolution button would not read as one.
    auto choice = [&](int id, float left, const char* title, const char* note,
                      bool enabled, bool selected) {
        const D2D1_RECT_F rect =
            D2D1::RectF(left, choiceTop, left + choiceWidth, choiceTop + choiceHeight);

        const bool hot = ui.Hotspot(id, rect, enabled);
        ui.Rect(rect, selected ? theme::kSurfaceOn : theme::kSurface, 11.0f);
        ui.Outline(rect, selected ? theme::kBorderOn : theme::kPanelBorder, 11.0f,
                   selected ? 1.5f : 1.0f);

        const D2D1_COLOR_F titleColour = enabled ? theme::kText : theme::kTextFaint;
        ui.Text(title, D2D1::RectF(rect.left, rect.top + 18.0f, rect.right, rect.top + 46.0f),
                TextStyle::ValueLarge, titleColour, Align::Center);
        ui.Text(note, D2D1::RectF(rect.left + 10.0f, rect.top + 50.0f,
                                  rect.right - 10.0f, rect.top + 74.0f),
                TextStyle::Label, theme::kTextDim, Align::Center);
        return hot;
    };

    if (choice(kIdUsb, x0 + 28.0f, T("USB"),
               adbAvailable ? T("the cable -- fastest, nothing to set up")
                            : T("adb was not found on this machine"),
               adbAvailable, shared.transport == Transport::Usb)) {
        shared.transport = Transport::Usb;
        out.chose = true;
        open_ = false;
    }

    if (choice(kIdWifi, x0 + 28.0f + choiceWidth + gap, T("Wi-Fi"),
               T("no cable -- same network as the phone"),
               true, shared.transport == Transport::WiFi)) {
        shared.transport = Transport::WiFi;
        out.chose = true;
        // Left open when nothing has been heard yet, so the list below can fill
        // in while somebody is still looking at it. Closing on the choice would
        // drop them onto an empty window with no idea whether it was working.
        open_ = !found.empty() || !shared.host.empty();
    }

    // ---- what Wi-Fi has to work with ---------------------------------------

    const float listTop = choiceTop + choiceHeight + 20.0f;
    const D2D1_RECT_F listRect =
        D2D1::RectF(x0 + 28.0f, listTop, x0 + cardWidth - 28.0f, listTop + 96.0f);

    if (found.empty()) {
        // Said plainly, because the reason is almost always one of these two and
        // neither is discoverable by staring at a spinner.
        ui.Text(shared.host.empty()
                    ? T("No phone on the network yet. Open XCam on the phone, allow "
                        "Wi-Fi connections, and press start.")
                    : T("No phone on the network yet -- the saved address will be "
                        "tried anyway."),
                listRect, TextStyle::Label, theme::kTextDim);
    } else {
        ui.Text(T("On the network"),
                D2D1::RectF(listRect.left, listRect.top, listRect.right, listRect.top + 20.0f),
                TextStyle::Label, theme::kTextDim);

        // Three at most. A fourth would need scrolling, and a room with four
        // phones broadcasting is a rig, which is set up from the command line.
        float y = listRect.top + 24.0f;
        const size_t shown = (std::min)(found.size(), static_cast<size_t>(3));
        for (size_t i = 0; i < shown; ++i) {
            const DiscoveredDevice& device = found[i];
            const D2D1_RECT_F row =
                D2D1::RectF(listRect.left, y, listRect.right, y + 26.0f);

            const bool chosen = shared.host == device.address;
            if (ui.Hotspot(kIdFoundBase + static_cast<int>(i), row)) {
                shared.host = device.address;
                shared.transport = Transport::WiFi;
                out.chose = true;
                open_ = false;
            }
            ui.Text(device.name.empty() ? device.address : device.name,
                    D2D1::RectF(row.left + 2.0f, row.top, row.right - 120.0f, row.bottom),
                    TextStyle::Label, chosen ? theme::kAccent : theme::kText);
            ui.Text(device.address,
                    D2D1::RectF(row.left, row.top, row.right - 2.0f, row.bottom),
                    TextStyle::Label, theme::kTextDim, Align::Right);
            y += 26.0f;
        }
    }

    // ---- the way out -------------------------------------------------------

    const float footer = y0 + cardHeight - 30.0f;

    const float typeWidth = (std::max)(ui.SegmentWidth(T("Type an address")), 150.0f);
    if (ui.Segment(kIdTypeAddress, T("Type an address"),
                   D2D1::RectF(x0 + 28.0f, footer - 17.0f, x0 + 28.0f + typeWidth,
                               footer + 17.0f),
                   false)) {
        shared.transport = Transport::WiFi;
        out.openSettings = true;
        out.chose = true;
        open_ = false;
    }

    // Not a cancel: whatever is in the settings already is a real answer, and
    // somebody who opened this by accident should be able to leave it alone.
    const float skipWidth = (std::max)(ui.SegmentWidth(T("Decide for me")), 130.0f);
    if (ui.Segment(kIdSkip, T("Decide for me"),
                   D2D1::RectF(x0 + cardWidth - 28.0f - skipWidth, footer - 17.0f,
                               x0 + cardWidth - 28.0f, footer + 17.0f),
                   false)) {
        shared.transport = Transport::Auto;
        out.chose = true;
        open_ = false;
    }

    return out;
}

}  // namespace xcam
