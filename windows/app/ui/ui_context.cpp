#include "app/ui/ui_context.h"

#include <d2d1_1.h>
#include <dwrite.h>

#include <algorithm>
#include <cmath>
#include <unordered_map>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

namespace xcam::ui {
namespace {

template <typename T>
void SafeRelease(T*& p) {
    if (p) { p->Release(); p = nullptr; }
}

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                      nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

uint32_t ColourKey(const D2D1_COLOR_F& c) {
    auto q = [](float v) { return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return (q(c.a) << 24) | (q(c.r) << 16) | (q(c.g) << 8) | q(c.b);
}

}  // namespace

struct UiContext::Impl {
    ID2D1DeviceContext* target = nullptr;
    IDWriteFactory* dwrite = nullptr;

    IDWriteTextFormat* label = nullptr;
    IDWriteTextFormat* value = nullptr;
    IDWriteTextFormat* valueLarge = nullptr;
    IDWriteTextFormat* chip = nullptr;
    IDWriteTextFormat* stat = nullptr;

    // Brushes are cached by colour: a frame asks for the same dozen shades over
    // and over, and creating one per draw call shows up immediately in a
    // 60 fps loop.
    std::unordered_map<uint32_t, ID2D1SolidColorBrush*> brushes;

    // The one wrapped block on screen -- the prompter's script -- kept between
    // frames.
    //
    // Laying text out is not drawing it: DirectWrite has to break every line of
    // the whole script to answer for its height, and a script is a file somebody
    // loaded, so that was a full pass over kilobytes of text sixty times a
    // second while nothing about it had changed. Only three things can change
    // the answer, and all three are the key.
    IDWriteTextFormat* blockFormat = nullptr;
    IDWriteTextLayout* blockLayout = nullptr;
    std::string blockText;
    float blockWidth = 0.0f;
    float blockSize = 0.0f;
    float blockHeight = 0.0f;

    void ReleaseBlock() {
        if (blockLayout) { blockLayout->Release(); blockLayout = nullptr; }
        if (blockFormat) { blockFormat->Release(); blockFormat = nullptr; }
        blockText.clear();
        blockWidth = blockSize = blockHeight = 0.0f;
    }
};

UiContext::UiContext() : impl_(new Impl) {}

UiContext::~UiContext() {
    Shutdown();
    delete impl_;
}

bool UiContext::Init(ID2D1DeviceContext* target, IDWriteFactory* dwrite) {
    Shutdown();
    impl_->target = target;
    impl_->dwrite = dwrite;

    // Segoe UI Variable ships with Windows 11 and is what the rest of the system
    // draws with; falling back to Segoe UI keeps this working on 10.
    auto make = [&](float size, DWRITE_FONT_WEIGHT weight, IDWriteTextFormat** out) {
        HRESULT hr = dwrite->CreateTextFormat(L"Segoe UI Variable Display", nullptr, weight,
                                              DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                              size, L"en-us", out);
        if (FAILED(hr)) {
            dwrite->CreateTextFormat(L"Segoe UI", nullptr, weight,
                                     DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                     size, L"en-us", out);
        }
        if (*out) {
            (*out)->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            (*out)->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }
    };

    make(theme::kSizeLabel, DWRITE_FONT_WEIGHT_SEMI_BOLD, &impl_->label);
    make(theme::kSizeValue, DWRITE_FONT_WEIGHT_SEMI_BOLD, &impl_->value);
    make(theme::kSizeValue * 1.6f, DWRITE_FONT_WEIGHT_LIGHT, &impl_->valueLarge);
    make(theme::kSizeChip, DWRITE_FONT_WEIGHT_MEDIUM, &impl_->chip);
    make(theme::kSizeStat, DWRITE_FONT_WEIGHT_NORMAL, &impl_->stat);

    return impl_->label != nullptr;
}

void UiContext::Shutdown() {
    if (!impl_) return;
    for (auto& [key, brush] : impl_->brushes) SafeRelease(brush);
    impl_->brushes.clear();
    SafeRelease(impl_->label);
    SafeRelease(impl_->value);
    SafeRelease(impl_->valueLarge);
    SafeRelease(impl_->chip);
    SafeRelease(impl_->stat);
    impl_->ReleaseBlock();
    impl_->target = nullptr;
    impl_->dwrite = nullptr;
}

void UiContext::BeginFrame(const Input& input, float width, float height, float opacity) {
    input_ = input;
    width_ = width;
    height_ = height;
    opacity_ = std::clamp(opacity, 0.0f, 1.0f);
    hotThisFrame_ = false;
    hot_ = 0;
}

void UiContext::EndFrame() {
    // A widget only stops being active when the button comes up, so a drag that
    // wanders off the control keeps working -- which is what makes sliders feel
    // right rather than slipping out from under the cursor.
    if (!input_.mouseDown) active_ = 0;
}

ID2D1SolidColorBrush* UiContext::Brush(const D2D1_COLOR_F& colour) {
    D2D1_COLOR_F faded = colour;
    faded.a *= opacity_;

    const uint32_t key = ColourKey(faded);
    auto it = impl_->brushes.find(key);
    if (it != impl_->brushes.end()) return it->second;

    ID2D1SolidColorBrush* brush = nullptr;
    if (FAILED(impl_->target->CreateSolidColorBrush(faded, &brush))) return nullptr;
    impl_->brushes.emplace(key, brush);
    return brush;
}

IDWriteTextFormat* UiContext::Format(TextStyle style) const {
    switch (style) {
        case TextStyle::Label:      return impl_->label;
        case TextStyle::Value:      return impl_->value;
        case TextStyle::ValueLarge: return impl_->valueLarge;
        case TextStyle::Chip:       return impl_->chip;
        case TextStyle::Stat:       return impl_->stat;
    }
    return impl_->value;
}

bool UiContext::Hit(const D2D1_RECT_F& rect) const {
    return input_.mouseX >= rect.left && input_.mouseX <= rect.right &&
           input_.mouseY >= rect.top && input_.mouseY <= rect.bottom;
}

// ---- primitives ------------------------------------------------------------

void UiContext::Panel(const D2D1_RECT_F& rect, float radius, const D2D1_COLOR_F& fill) {
    const D2D1_ROUNDED_RECT rounded = D2D1::RoundedRect(rect, radius, radius);
    if (auto* brush = Brush(fill)) impl_->target->FillRoundedRectangle(rounded, brush);
    if (auto* border = Brush(theme::kPanelBorder)) {
        impl_->target->DrawRoundedRectangle(rounded, border, 1.0f);
    }
}

void UiContext::PushClip(const D2D1_RECT_F& rect) {
    impl_->target->PushAxisAlignedClip(rect, D2D1_ANTIALIAS_MODE_ALIASED);
    clip_ = rect;
    clipped_ = true;
}

void UiContext::PopClip() {
    impl_->target->PopAxisAlignedClip();
    clipped_ = false;
}

void UiContext::Rect(const D2D1_RECT_F& rect, const D2D1_COLOR_F& colour, float radius) {
    auto* brush = Brush(colour);
    if (!brush) return;
    if (radius > 0.0f) {
        impl_->target->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush);
    } else {
        impl_->target->FillRectangle(rect, brush);
    }
}

void UiContext::Outline(const D2D1_RECT_F& rect, const D2D1_COLOR_F& colour,
                        float radius, float thickness) {
    auto* brush = Brush(colour);
    if (!brush) return;
    // Inset by half the stroke so the edge lands inside the rectangle rather
    // than straddling it, which is what makes adjacent controls look unevenly
    // sized.
    const float inset = thickness * 0.5f;
    const D2D1_RECT_F inner = D2D1::RectF(rect.left + inset, rect.top + inset,
                                          rect.right - inset, rect.bottom - inset);
    if (radius > 0.0f) {
        impl_->target->DrawRoundedRectangle(D2D1::RoundedRect(inner, radius, radius),
                                            brush, thickness);
    } else {
        impl_->target->DrawRectangle(inner, brush, thickness);
    }
}

void UiContext::Line(float x1, float y1, float x2, float y2, const D2D1_COLOR_F& colour,
                     float thickness) {
    if (auto* brush = Brush(colour)) {
        impl_->target->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), brush, thickness);
    }
}

void UiContext::Text(const std::string& text, const D2D1_RECT_F& rect, TextStyle style,
                     const D2D1_COLOR_F& colour, Align align) {
    IDWriteTextFormat* format = Format(style);
    if (!format) return;

    format->SetTextAlignment(align == Align::Left   ? DWRITE_TEXT_ALIGNMENT_LEADING
                           : align == Align::Center ? DWRITE_TEXT_ALIGNMENT_CENTER
                                                    : DWRITE_TEXT_ALIGNMENT_TRAILING);

    const std::wstring wide = Widen(text);
    if (auto* brush = Brush(colour)) {
        impl_->target->DrawText(wide.c_str(), static_cast<UINT32>(wide.size()), format, rect,
                                brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
}

float UiContext::TextBlock(const std::string& text, const D2D1_RECT_F& rect,
                           float size, const D2D1_COLOR_F& colour, float scrollY,
                           bool mirrored) {
    if (!impl_->target || !impl_->dwrite || text.empty()) return 0.0f;

    const float width = rect.right - rect.left;
    if (width <= 1.0f) return 0.0f;

    // Rebuilt only when the text, the column or the point size has moved.
    // Scrolling is not one of those: it is a draw offset, and the layout it is
    // drawn from is the same layout it was drawn from last frame.
    if (!impl_->blockLayout || impl_->blockText != text ||
        impl_->blockWidth != width || impl_->blockSize != size) {
        impl_->ReleaseBlock();

        // Its own format, built here rather than borrowed. The five cached
        // formats are all NO_WRAP and centred in their box, which is right for a
        // chip and useless for a script -- and turning wrapping on in one of
        // them would turn it on for every label on the panel, since they are
        // shared.
        IDWriteTextFormat* format = nullptr;
        if (FAILED(impl_->dwrite->CreateTextFormat(
                L"Segoe UI Variable Display", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"en-us",
                &format))) {
            impl_->dwrite->CreateTextFormat(L"Segoe UI", nullptr,
                                            DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                            DWRITE_FONT_STYLE_NORMAL,
                                            DWRITE_FONT_STRETCH_NORMAL, size, L"en-us",
                                            &format);
        }
        if (!format) return 0.0f;

        format->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);

        const std::wstring wide = Widen(text);

        // A very tall box, so the layout reports the height of the whole script
        // rather than the height of what fits. That number is the scroll extent.
        IDWriteTextLayout* layout = nullptr;
        if (FAILED(impl_->dwrite->CreateTextLayout(wide.c_str(),
                                                   static_cast<UINT32>(wide.size()),
                                                   format, width, 1'000'000.0f, &layout))) {
            format->Release();
            return 0.0f;
        }

        DWRITE_TEXT_METRICS metrics{};
        layout->GetMetrics(&metrics);

        impl_->blockFormat = format;
        impl_->blockLayout = layout;
        impl_->blockText = text;
        impl_->blockWidth = width;
        impl_->blockSize = size;
        impl_->blockHeight = metrics.height;
    }

    IDWriteTextLayout* layout = impl_->blockLayout;

    if (auto* brush = Brush(colour)) {
        impl_->target->PushAxisAlignedClip(rect, D2D1_ANTIALIAS_MODE_ALIASED);

        // Mirrored for a beam splitter, which reverses everything in front of
        // it -- so text meant to be read through one has to be drawn backwards.
        D2D1_MATRIX_3X2_F previous;
        impl_->target->GetTransform(&previous);
        if (mirrored) {
            const float centre = (rect.left + rect.right) * 0.5f;
            impl_->target->SetTransform(
                D2D1::Matrix3x2F::Scale(-1.0f, 1.0f, D2D1::Point2F(centre, 0.0f)) * previous);
        }

        impl_->target->DrawTextLayout(D2D1::Point2F(rect.left, rect.top - scrollY), layout,
                                      brush, D2D1_DRAW_TEXT_OPTIONS_NONE);

        if (mirrored) impl_->target->SetTransform(previous);
        impl_->target->PopAxisAlignedClip();
    }

    // Neither is released: they belong to the cache now, and Shutdown frees
    // them with everything else.
    return impl_->blockHeight;
}

float UiContext::MeasureText(const std::string& text, TextStyle style) const {
    IDWriteTextFormat* format = Format(style);
    if (!format) return 0.0f;

    const std::wstring wide = Widen(text);
    IDWriteTextLayout* layout = nullptr;
    if (FAILED(impl_->dwrite->CreateTextLayout(wide.c_str(), static_cast<UINT32>(wide.size()),
                                               format, 4096.0f, 64.0f, &layout))) {
        return 0.0f;
    }
    DWRITE_TEXT_METRICS metrics{};
    layout->GetMetrics(&metrics);
    layout->Release();
    return metrics.width;
}

// ---- widgets ---------------------------------------------------------------

bool UiContext::Hotspot(int id, const D2D1_RECT_F& rect, bool enabled) {
    if (!enabled || !Hit(rect)) return false;

    // Outside the clip is out of sight, and out of reach with it.
    if (clipped_ && (input_.mouseX < clip_.left || input_.mouseX > clip_.right ||
                     input_.mouseY < clip_.top || input_.mouseY > clip_.bottom)) {
        return false;
    }

    hot_ = id;
    hotThisFrame_ = true;
    if (input_.mousePressed) active_ = id;
    return input_.mouseReleased && active_ == id;
}

namespace {

// Room a toggle needs beside its label: the lamp, its margins, and the gap
// before the text starts.
constexpr float kLedInset = 15.0f;
constexpr float kLedSize = 7.0f;
constexpr float kLedTextGap = 27.0f;

}  // namespace

float UiContext::ChipWidth(const std::string& label, ChipStyle style) const {
    const float text = MeasureText(label, TextStyle::Chip);
    return style == ChipStyle::Value ? text + 26.0f : text + kLedTextGap + 14.0f;
}

bool UiContext::Chip(int id, const std::string& label, const D2D1_RECT_F& rect,
                     bool selected, bool enabled, ChipStyle style) {
    bool clicked = false;

    if (enabled && Hit(rect)) {
        hot_ = id;
        hotThisFrame_ = true;
        if (input_.mousePressed) active_ = id;
        if (input_.mouseReleased && active_ == id) clicked = true;
    }

    const bool hovered = enabled && hot_ == id;
    const bool held = enabled && active_ == id;

    // The surface never becomes the accent, however "on" the control is. Five
    // lit toggles filled with amber is a wall of colour that says nothing, and
    // the accent is supposed to mean "this is the one thing in hand".
    const D2D1_COLOR_F fill = !enabled ? theme::kPanel
                            : held     ? theme::kSurfaceOn
                            : hovered  ? theme::kSurfaceHot
                            : selected ? theme::kSurfaceOn
                                       : theme::kSurface;
    Rect(rect, fill, theme::kChipRadius);

    // A Value chip shows what is chosen with its edge. A Toggle shows on and
    // off with its lamp, so lighting its edge as well would say the same thing
    // twice and leave a row of toggles looking like a row of selections.
    const bool edge = selected && enabled && style == ChipStyle::Value;
    Outline(rect, edge ? theme::kBorderOn : theme::kPanelBorder,
            theme::kChipRadius, edge ? 1.4f : 1.0f);

    if (style == ChipStyle::Value) {
        const D2D1_COLOR_F colour = !enabled ? theme::kTextFaint
                                  : selected ? theme::kText
                                             : theme::kTextDim;
        Text(label, rect, TextStyle::Chip, colour, Align::Center);
        return clicked;
    }

    const D2D1_COLOR_F lamp =
        !enabled ? theme::kLedDisabled
        : !selected ? theme::kLedOff
        : style == ChipStyle::Record ? theme::kRecord
                                     : theme::kAccent;

    const float cx = rect.left + kLedInset;
    const float cy = (rect.top + rect.bottom) * 0.5f;
    Rect(D2D1::RectF(cx - kLedSize * 0.5f, cy - kLedSize * 0.5f,
                     cx + kLedSize * 0.5f, cy + kLedSize * 0.5f),
         lamp, kLedSize * 0.5f);

    const D2D1_COLOR_F colour = !enabled ? theme::kTextFaint
                              : selected ? theme::kText
                                         : theme::kTextDim;
    Text(label, D2D1::RectF(rect.left + kLedTextGap, rect.top,
                            rect.right - 10.0f, rect.bottom),
         TextStyle::Chip, colour, Align::Left);
    return clicked;
}

bool UiContext::Segment(int id, const std::string& label, const D2D1_RECT_F& rect,
                        bool selected, bool enabled, ChipStyle style) {
    bool clicked = false;

    if (enabled && Hit(rect)) {
        hot_ = id;
        hotThisFrame_ = true;
        if (input_.mousePressed) active_ = id;
        if (input_.mouseReleased && active_ == id) clicked = true;
    }

    const bool hovered = enabled && hot_ == id;

    // No border of its own. A cell belongs to a group, and the group already
    // has an edge; giving every cell one as well is what turns a control
    // surface into a row of scattered buttons.
    const D2D1_RECT_F inner = D2D1::RectF(rect.left + 3.0f, rect.top + 3.0f,
                                          rect.right - 3.0f, rect.bottom - 3.0f);
    if (selected && enabled) {
        Rect(inner, theme::kSurfaceOn, 6.0f);
    } else if (hovered) {
        Rect(inner, theme::kHover, 6.0f);
    }

    const D2D1_COLOR_F colour = !enabled ? theme::kTextFaint
                              : selected ? theme::kText
                                         : theme::kTextDim;

    if (style == ChipStyle::Value) {
        Text(label, rect, TextStyle::Chip, colour, Align::Center);

        // A short rule under the chosen cell. It is the one mark that says
        // "this one", and it is small enough that a row of cells still reads as
        // one control rather than as a set of lit buttons.
        if (selected && enabled) {
            const float inset = 12.0f;
            const float y = rect.bottom - 7.0f;
            Rect(D2D1::RectF(rect.left + inset, y, rect.right - inset, y + 2.0f),
                 theme::kAccent, 1.0f);
        }
        return clicked;
    }

    // A toggle carries a lamp instead, because a row of them has to be
    // readable without reading any of the words.
    const D2D1_COLOR_F lamp =
        !enabled ? theme::kLedDisabled
        : !selected ? theme::kLedOff
        : style == ChipStyle::Record ? theme::kRecord
                                     : theme::kAccent;

    const float size = 7.0f;
    const float cx = rect.left + 16.0f;
    const float cy = (rect.top + rect.bottom) * 0.5f;
    Rect(D2D1::RectF(cx - size * 0.5f, cy - size * 0.5f,
                     cx + size * 0.5f, cy + size * 0.5f),
         lamp, size * 0.5f);

    Text(label, D2D1::RectF(rect.left + 28.0f, rect.top, rect.right - 10.0f, rect.bottom),
         TextStyle::Chip, colour, Align::Left);
    return clicked;
}

float UiContext::SegmentWidth(const std::string& label, ChipStyle style) const {
    const float text = MeasureText(label, TextStyle::Chip);
    return style == ChipStyle::Value ? text + 30.0f : text + 44.0f;
}

void UiContext::Divider(float x, float top, float bottom) {
    // Inset from the group's edges, so the rules read as separations between
    // cells rather than as a grid drawn over them.
    Line(x, top + 8.0f, x, bottom - 8.0f, theme::kPanelBorder, 1.0f);
}

bool UiContext::Readout(int id, const std::string& label, const std::string& value,
                        const D2D1_RECT_F& rect, bool open, bool enabled, bool chosen) {
    bool clicked = false;

    if (enabled && Hit(rect)) {
        hot_ = id;
        hotThisFrame_ = true;
        if (input_.mousePressed) active_ = id;
        if (input_.mouseReleased && active_ == id) clicked = true;
    }

    const bool hovered = enabled && hot_ == id;
    if (open) {
        Rect(D2D1::RectF(rect.left + 3.0f, rect.top + 3.0f,
                         rect.right - 3.0f, rect.bottom - 3.0f),
             theme::kSurfaceOn, 6.0f);
    } else if (hovered) {
        Rect(D2D1::RectF(rect.left + 3.0f, rect.top + 3.0f,
                         rect.right - 3.0f, rect.bottom - 3.0f),
             theme::kHover, 6.0f);
    }

    // Label above value, which is how the pro column reads and how a camera
    // labels a dial. The pairing is what makes it a readout rather than two
    // words.
    const D2D1_COLOR_F valueColour = !enabled ? theme::kTextFaint
                                   : open     ? theme::kAccent
                                   : chosen   ? theme::kText
                                              : theme::kTextDim;

    Text(label, D2D1::RectF(rect.left + 13.0f, rect.top + 6.0f,
                            rect.right - 13.0f, rect.top + 21.0f),
         TextStyle::Label, enabled ? theme::kTextDim : theme::kTextFaint, Align::Left);
    Text(value, D2D1::RectF(rect.left + 13.0f, rect.top + 20.0f,
                            rect.right - 13.0f, rect.bottom - 5.0f),
         TextStyle::Value, valueColour, Align::Left);
    return clicked;
}

float UiContext::ReadoutWidth(const std::string& label, const std::string& value) const {
    return (std::max)(MeasureText(label, TextStyle::Label),
                      MeasureText(value, TextStyle::Value)) + 30.0f;
}

bool UiContext::Slider(int id, const D2D1_RECT_F& rect, float& value, float min, float max) {
    const float trackY = (rect.top + rect.bottom) * 0.5f;
    const float knobRadius = 7.0f;
    const float left = rect.left + knobRadius;
    const float right = rect.right - knobRadius;
    const float span = (std::max)(right - left, 1.0f);

    // Grabbing anywhere on the row works, not just on the knob -- a 14px target
    // is not something anyone should have to hit.
    if (Hit(rect)) {
        hot_ = id;
        hotThisFrame_ = true;
        if (input_.mousePressed) active_ = id;
    }

    bool dragging = false;
    if (active_ == id) {
        const float t = std::clamp((input_.mouseX - left) / span, 0.0f, 1.0f);
        value = min + t * (max - min);
        dragging = true;
    }

    const float t = std::clamp((value - min) / (max - min == 0 ? 1.0f : max - min), 0.0f, 1.0f);
    const float knobX = left + t * span;

    Rect(D2D1::RectF(left, trackY - 2.0f, right, trackY + 2.0f), theme::kTrack, 2.0f);
    Rect(D2D1::RectF(left, trackY - 2.0f, knobX, trackY + 2.0f), theme::kAccent, 2.0f);

    const D2D1_ELLIPSE knob = D2D1::Ellipse(D2D1::Point2F(knobX, trackY),
                                            knobRadius, knobRadius);
    if (auto* brush = Brush(hot_ == id || dragging ? theme::kAccent : theme::kText)) {
        impl_->target->FillEllipse(knob, brush);
    }
    return dragging;
}

bool UiContext::Ruler(int id, const D2D1_RECT_F& rect, float& t, int tickCount,
                      const std::string& readout, bool editing, const std::string& hint) {
    if (!editing && Hit(rect)) {
        hot_ = id;
        hotThisFrame_ = true;
        if (input_.mousePressed) active_ = id;
    }

    const float inset = 24.0f;
    const float left = rect.left + inset;
    const float right = rect.right - inset;
    const float span = (std::max)(right - left, 1.0f);

    bool dragging = false;
    if (!editing && active_ == id) {
        t = std::clamp((input_.mouseX - left) / span, 0.0f, 1.0f);
        dragging = true;
    }
    // The wheel is the fine adjustment the drag cannot give: a pixel of travel
    // covers far too much of a range that spans two orders of magnitude.
    if (!editing && hot_ == id && input_.wheel != 0.0f) {
        t = std::clamp(t + input_.wheel * 0.01f, 0.0f, 1.0f);
        dragging = true;
    }

    const float baseline = rect.bottom - 16.0f;

    // Ticks taller at the ends and centre, the way a lens barrel is marked.
    for (int i = 0; i <= tickCount; ++i) {
        const float x = left + span * (static_cast<float>(i) / tickCount);
        const bool major = (i % 5 == 0);
        const float height = major ? 12.0f : 6.0f;
        Line(x, baseline - height, x, baseline,
             major ? theme::kTextDim : theme::kTextFaint, 1.0f);
    }

    const float cursorX = left + span * std::clamp(t, 0.0f, 1.0f);
    Line(cursorX, baseline - 18.0f, cursorX, baseline + 4.0f, theme::kAccent, 2.0f);

    const D2D1_RECT_F readoutRect =
        D2D1::RectF(rect.left, rect.top, rect.right, baseline - 20.0f);

    if (editing) {
        // A blinking caret is the only thing that says "this is a field" without
        // drawing a box that would fight the rest of the panel.
        const bool caretOn = fmod(static_cast<double>(GetTickCount64()), 1000.0) < 600.0;
        Text(readout + (caretOn ? "|" : " "), readoutRect,
             TextStyle::ValueLarge, theme::kAccent, Align::Center);
    } else {
        Text(readout, readoutRect, TextStyle::ValueLarge, theme::kText, Align::Center);
    }

    if (!hint.empty()) {
        Text(hint, D2D1::RectF(rect.left, rect.top + 2.0f, rect.right - 12.0f, rect.top + 20.0f),
             TextStyle::Label, theme::kTextFaint, Align::Right);
    }

    return dragging;
}

}  // namespace xcam::ui
