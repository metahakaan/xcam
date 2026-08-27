#include "app/takes_panel.h"

#include "app/strings.h"
#include "app/ui/theme.h"
#include "core/protocol.h"

#include <algorithm>
#include <cstdio>

namespace xcam {

using namespace ui;

namespace {

enum : int {
    kIdClose = 1200,
    kIdRefresh = 1201,
    kIdConfirmYes = 1202,
    kIdConfirmNo = 1203,
    kIdCancelFetch = 1204,

    // Two ids per row, far enough apart that a long listing cannot collide with
    // anything above.
    kIdRowBase = 1300,
    kIdDeleteBase = 1600,
};

constexpr float kRowHeight = 52.0f;

std::string HumanBytes(int64_t bytes) {
    char buffer[32];
    if (bytes >= 1'000'000'000) {
        std::snprintf(buffer, sizeof(buffer), "%.1f GB", bytes / 1e9);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%lld MB",
                      static_cast<long long>(bytes / 1'000'000));
    }
    return buffer;
}

std::string HumanDuration(int64_t ms) {
    if (ms <= 0) return "--:--";
    const int64_t total = ms / 1000;
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%lld:%02lld",
                  static_cast<long long>(total / 60), static_cast<long long>(total % 60));
    return buffer;
}

// "XCam_20260825-143012.mp4" as something a person reads. The name is already
// the timestamp, so this is a reformat rather than a lookup -- and a file whose
// name does not fit the pattern is shown as it is rather than guessed at.
std::string HumanName(const std::string& name) {
    const size_t dash = name.find('-');
    if (name.size() < 24 || name.rfind("XCam", 0) != 0 || dash == std::string::npos) {
        return name;
    }
    const std::string date = name.substr(5, 8);
    const std::string time = name.substr(14, 6);
    if (date.size() != 8 || time.size() != 6) return name;

    return date.substr(6, 2) + "." + date.substr(4, 2) + "  " +
           time.substr(0, 2) + ":" + time.substr(2, 2) + ":" + time.substr(4, 2);
}

}  // namespace

TakesPanel::Output TakesPanel::Draw(UiContext& ui, CameraModel& model) {
    Output out;
    if (!open_) return out;

    if ((refresh_ || model.takesStale) && model.connected && !model.takesPending) {
        out.commands.push_back(MakeTakesCommand("list"));
        model.takesPending = true;
        model.takesStale = false;
        refresh_ = false;
    }

    const float width = (std::min)(560.0f, ui.Width() - 2 * theme::kPad);
    const float x0 = (ui.Width() - width) * 0.5f;
    const float height = (std::min)(ui.Height() - 2 * theme::kPad, 640.0f);
    const float top = (ui.Height() - height) * 0.5f;

    ui.Panel(D2D1::RectF(x0, top, x0 + width, top + height), 14.0f, theme::kPanelRaised);

    float y = top + 20.0f;
    ui.Text(T("Takes on the phone"),
            D2D1::RectF(x0 + 22.0f, y, x0 + width - 200.0f, y + 30.0f),
            TextStyle::Value, theme::kText, Align::Left);

    const D2D1_RECT_F refreshRect =
        D2D1::RectF(x0 + width - 178.0f, y - 2.0f, x0 + width - 100.0f, y + 30.0f);
    ui.Panel(refreshRect, 8.0f);
    if (ui.Segment(kIdRefresh, T("Refresh"), refreshRect, false, model.connected)) {
        refresh_ = true;
    }

    const D2D1_RECT_F closeRect =
        D2D1::RectF(x0 + width - 90.0f, y - 2.0f, x0 + width - 18.0f, y + 30.0f);
    ui.Panel(closeRect, 8.0f);
    if (ui.Segment(kIdClose, T("Close"), closeRect, false)) Close();
    y += 44.0f;

    // ---- what is going on, if anything ------------------------------------

    if (model.Fetching()) {
        const D2D1_RECT_F rect = D2D1::RectF(x0 + 14.0f, y, x0 + width - 14.0f, y + 46.0f);
        ui.Panel(rect, 9.0f);

        const double share = model.fetchBytes > 0
            ? static_cast<double>(model.fetchReceived) / static_cast<double>(model.fetchBytes)
            : 0.0;
        const float clamped = static_cast<float>((std::min)(1.0, (std::max)(0.0, share)));

        // The bar is the progress. A percentage alone tells you where it is;
        // watching the bar tells you whether it is moving, which over Wi-Fi with
        // a stream running is the question actually being asked.
        ui.Rect(D2D1::RectF(rect.left + 12.0f, rect.bottom - 16.0f,
                            rect.left + 12.0f + (rect.right - rect.left - 24.0f) * clamped,
                            rect.bottom - 12.0f),
                theme::kAccent, 2.0f);

        char label[160];
        std::snprintf(label, sizeof(label), "%s  %s / %s", HumanName(model.fetchName).c_str(),
                      HumanBytes(model.fetchReceived).c_str(),
                      HumanBytes(model.fetchBytes).c_str());
        ui.Text(label, D2D1::RectF(rect.left + 12.0f, rect.top + 6.0f,
                                   rect.right - 100.0f, rect.top + 28.0f),
                TextStyle::Chip, theme::kText, Align::Left);

        const D2D1_RECT_F cancelRect =
            D2D1::RectF(rect.right - 92.0f, rect.top + 6.0f, rect.right - 12.0f,
                        rect.top + 32.0f);
        if (ui.Segment(kIdCancelFetch, T("Stop"), cancelRect, false)) {
            out.commands.push_back(MakeTakesCommand("cancel"));
        }
        y += 54.0f;
    }

    if (!confirmDelete_.empty()) {
        const D2D1_RECT_F rect = D2D1::RectF(x0 + 14.0f, y, x0 + width - 14.0f, y + 46.0f);
        ui.Panel(rect, 9.0f);

        char label[200];
        std::snprintf(label, sizeof(label), T("Delete %s? It cannot be undone"),
                      HumanName(confirmDelete_).c_str());
        ui.Text(label, D2D1::RectF(rect.left + 12.0f, rect.top, rect.right - 180.0f,
                                   rect.bottom),
                TextStyle::Chip, theme::kText, Align::Left);

        const D2D1_RECT_F noRect = D2D1::RectF(rect.right - 172.0f, rect.top + 8.0f,
                                               rect.right - 96.0f, rect.bottom - 8.0f);
        const D2D1_RECT_F yesRect = D2D1::RectF(rect.right - 88.0f, rect.top + 8.0f,
                                                rect.right - 12.0f, rect.bottom - 8.0f);
        if (ui.Segment(kIdConfirmNo, T("Keep"), noRect, false)) confirmDelete_.clear();
        if (ui.Segment(kIdConfirmYes, T("Delete"), yesRect, false, true,
                       ChipStyle::Record)) {
            out.commands.push_back(MakeTakesCommand("delete", confirmDelete_));
            confirmDelete_.clear();
        }
        y += 54.0f;
    }

    // ---- the list ----------------------------------------------------------

    const float listTop = y;
    const float listBottom = top + height - 46.0f;
    const float visible = listBottom - listTop;

    if (model.takes.empty()) {
        const char* message =
            !model.connected      ? "No phone connected"
            : model.takesPending  ? "Asking the phone…"
            : !model.CanRecord()  ? "This phone cannot record"
                                  : "Nothing recorded on the phone yet";
        ui.Text(T(message),
                D2D1::RectF(x0 + 22.0f, listTop + 20.0f, x0 + width - 22.0f, listTop + 44.0f),
                TextStyle::Chip, theme::kTextDim, Align::Left);
    } else {
        const float content = model.takes.size() * kRowHeight;
        const float maxScroll = (std::max)(0.0f, content - visible);

        // The wheel only moves the list while the pointer is over it, so a
        // scroll aimed at the sheet does not also drag whatever is behind it.
        const Input& input = ui.GetInput();
        if (input.mouseX >= x0 && input.mouseX <= x0 + width &&
            input.mouseY >= listTop && input.mouseY <= listBottom) {
            scroll_ -= input.wheel * kRowHeight;
        }
        scroll_ = (std::max)(0.0f, (std::min)(maxScroll, scroll_));

        for (size_t i = 0; i < model.takes.size(); ++i) {
            const float rowTop = listTop + i * kRowHeight - scroll_;
            // Clipped by not drawing: the rows are cheap, and a scissor
            // rectangle for a list this short would be machinery for its own
            // sake. A partly visible row at either edge is simply skipped.
            if (rowTop < listTop - kRowHeight || rowTop > listBottom) continue;

            const TakeInfo& take = model.takes[i];
            const D2D1_RECT_F row =
                D2D1::RectF(x0 + 14.0f, rowTop + 3.0f, x0 + width - 14.0f,
                            rowTop + kRowHeight - 3.0f);

            const int rowId = kIdRowBase + static_cast<int>(i);
            const bool busy = model.Fetching();
            const bool clicked = ui.Hotspot(rowId, row, !busy);
            if (!busy && ui.IsHot(rowId)) ui.Rect(row, theme::kHover, 8.0f);

            ui.Text(HumanName(take.name),
                    D2D1::RectF(row.left + 12.0f, row.top + 4.0f, row.right - 150.0f,
                                row.top + 24.0f),
                    TextStyle::Chip, theme::kText, Align::Left);

            char detail[96];
            std::snprintf(detail, sizeof(detail), "%s   %s",
                          HumanDuration(take.durationMs).c_str(),
                          HumanBytes(take.bytes).c_str());
            ui.Text(detail,
                    D2D1::RectF(row.left + 12.0f, row.top + 24.0f, row.right - 150.0f,
                                row.bottom - 2.0f),
                    TextStyle::Stat, theme::kTextDim, Align::Left);

            const D2D1_RECT_F deleteRect =
                D2D1::RectF(row.right - 96.0f, row.top + 10.0f, row.right - 12.0f,
                            row.bottom - 10.0f);
            if (ui.Segment(kIdDeleteBase + static_cast<int>(i), T("Delete"), deleteRect,
                           false, !busy)) {
                confirmDelete_ = take.name;
            } else if (clicked) {
                out.fetch = take.name;
            }
        }
    }

    // ---- the footer --------------------------------------------------------

    const std::string footer =
        !model.takesError.empty() ? model.takesError
        : model.takesDir.empty()  ? std::string()
                                  : model.takesDir;
    ui.Text(footer,
            D2D1::RectF(x0 + 22.0f, top + height - 34.0f, x0 + width - 22.0f,
                        top + height - 14.0f),
            TextStyle::Stat,
            model.takesError.empty() ? theme::kTextFaint : theme::kWarn, Align::Left);

    return out;
}

}  // namespace xcam
