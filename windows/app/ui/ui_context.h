#pragma once

// Immediate-mode UI over Direct2D.
//
// Immediate mode suits this panel: what it shows depends entirely on the camera
// that happens to be connected, and rebuilding a retained widget tree every time
// a phone is plugged in would cost more than it saves. Widgets are identified by
// a caller-supplied id, and the context tracks which one the pointer is over
// (`hot`) and which one the mouse went down on (`active`) -- enough state to
// make dragging work without any widget owning anything.

#include "app/ui/theme.h"

#include <cstdint>
#include <string>

struct ID2D1DeviceContext;
struct IDWriteFactory;
struct IDWriteTextFormat;
struct ID2D1SolidColorBrush;

namespace xcam::ui {

enum class TextStyle { Label, Value, Chip, Stat, ValueLarge };
enum class Align { Left, Center, Right };

// What a chip is for, which decides how its state is shown.
//
//  Value  -- a setting with several options: a resolution, a lens stop, a white
//            balance preset. Selected means "this is the one", and the chip
//            says so with its border and the weight of its text.
//  Toggle -- something that is on or off. Carries a small indicator light,
//            because a row of these needs to be readable at a glance without
//            reading any of the words.
//  Record -- a toggle whose light is the recording colour.
enum class ChipStyle { Value, Toggle, Record };

struct Input {
    float mouseX = 0, mouseY = 0;
    bool mouseDown = false;         // held right now
    bool mousePressed = false;      // went down this frame
    bool mouseReleased = false;     // came up this frame
    float wheel = 0;

    // Typed characters since the last frame, plus the editing keys that do not
    // arrive as characters. Dragging a ruler is fine for finding a value and
    // hopeless for hitting an exact one, so anything on the pro column can be
    // typed instead.
    std::string typed;
    bool backspace = false;
    bool commit = false;            // Enter
    bool cancel = false;            // Escape

    // Cleared by the context at the end of each frame; the window only ever
    // sets them.
    void EndFrame() {
        mousePressed = false;
        mouseReleased = false;
        wheel = 0;
        typed.clear();
        backspace = false;
        commit = false;
        cancel = false;
    }
};

class UiContext {
public:
    UiContext();
    ~UiContext();

    UiContext(const UiContext&) = delete;
    UiContext& operator=(const UiContext&) = delete;

    bool Init(ID2D1DeviceContext* target, IDWriteFactory* dwrite);
    void Shutdown();

    void BeginFrame(const Input& input, float width, float height, float opacity);
    void EndFrame();

    // ---- drawing primitives ------------------------------------------------

    void Panel(const D2D1_RECT_F& rect, float radius = theme::kPanelRadius,
               const D2D1_COLOR_F& fill = theme::kPanel);
    void Rect(const D2D1_RECT_F& rect, const D2D1_COLOR_F& colour, float radius = 0.0f);

    // The outline of a rounded rectangle. What lets a control show state with
    // its edge instead of by filling itself with the accent.
    void Outline(const D2D1_RECT_F& rect, const D2D1_COLOR_F& colour,
                 float radius = 0.0f, float thickness = 1.0f);
    void Line(float x1, float y1, float x2, float y2, const D2D1_COLOR_F& colour,
              float thickness = 1.0f);
    void Text(const std::string& text, const D2D1_RECT_F& rect, TextStyle style,
              const D2D1_COLOR_F& colour, Align align = Align::Left);

    // Wrapped, scrolling, multi-line text -- a script rather than a label.
    //
    // Returns the height of the whole block, which is what a caller scrolling
    // through it needs to know when it has reached the end. `scrollY` is how
    // far down the block the top of `rect` sits.
    //
    // Builds its own format and layout instead of using the cached five: those
    // are all NO_WRAP and vertically centred, and turning wrapping on in one
    // would turn it on for every label on the panel.
    float TextBlock(const std::string& text, const D2D1_RECT_F& rect, float size,
                    const D2D1_COLOR_F& colour, float scrollY, bool mirrored);

    float MeasureText(const std::string& text, TextStyle style) const;

    // ---- widgets -----------------------------------------------------------

    // An interactive region that draws nothing. Lets a caller compose its own
    // visuals -- the pro column's rows are drawn as text and highlights, not as
    // buttons -- while still taking part in hot/active tracking.
    // Confines drawing and hit testing to `rect` until PopClip.
    //
    // Both, not just drawing: a row scrolled out of a sheet is still under the
    // pointer as far as an immediate-mode hotspot is concerned, and a button
    // nobody can see should not be a button anybody can press.
    void PushClip(const D2D1_RECT_F& rect);
    void PopClip();

    bool Hotspot(int id, const D2D1_RECT_F& rect, bool enabled = true);

    // A control. Returns true on the frame it is clicked. `selected` means
    // "chosen" for a Value chip and "on" for a Toggle.
    bool Chip(int id, const std::string& label, const D2D1_RECT_F& rect,
              bool selected, bool enabled = true,
              ChipStyle style = ChipStyle::Value);

    // Width a chip needs for its label, including whatever its style adds.
    float ChipWidth(const std::string& label, ChipStyle style = ChipStyle::Value) const;

    // ---- groups ------------------------------------------------------------
    //
    // A row of separate buttons reads as scattered; the same controls inside
    // one container, divided by hairlines, read as an instrument. Everything
    // below draws a *cell*: the caller draws the container with Panel first and
    // the dividers between cells, and the cells draw only their own state.

    bool Segment(int id, const std::string& label, const D2D1_RECT_F& rect,
                 bool selected, bool enabled = true,
                 ChipStyle style = ChipStyle::Value);
    float SegmentWidth(const std::string& label, ChipStyle style = ChipStyle::Value) const;

    // The hairline between two cells.
    void Divider(float x, float top, float bottom);

    // A cell that shows a named value -- FPS over "60", the way the pro column
    // and a camera's own dials read. `open` is for a cell whose options are
    // showing.
    //
    // `chosen` is for a value somebody set as against one the camera is
    // deciding: a manual ISO reads at full strength, an automatic one reads
    // dim. Without it a row of readouts gives equal weight to what is being
    // controlled and what is merely being reported.
    bool Readout(int id, const std::string& label, const std::string& value,
                 const D2D1_RECT_F& rect, bool open, bool enabled = true,
                 bool chosen = true);
    float ReadoutWidth(const std::string& label, const std::string& value) const;

    // Horizontal track with a knob. Writes through `value` and returns true
    // while the user is dragging it.
    bool Slider(int id, const D2D1_RECT_F& rect, float& value, float min, float max);

    // The wide scrubber a camera puts at the bottom of the screen for whichever
    // parameter is selected. `t` is normalised [0,1]; ticks are drawn along it
    // so the travel reads as a scale rather than a bare bar.
    //
    // When `editing` is set the readout is replaced by the text being typed,
    // with a caret, and dragging is disabled so a stray movement cannot discard
    // a half-entered value.
    bool Ruler(int id, const D2D1_RECT_F& rect, float& t, int tickCount,
               const std::string& readout, bool editing = false,
               const std::string& hint = {});

    bool IsHot(int id) const { return hot_ == id; }
    bool IsActive(int id) const { return active_ == id; }
    bool WantsMouse() const { return active_ != 0 || hotThisFrame_; }

    const Input& GetInput() const { return input_; }
    float Width() const { return width_; }
    float Height() const { return height_; }
    float Opacity() const { return opacity_; }

private:
    bool Hit(const D2D1_RECT_F& rect) const;
    ID2D1SolidColorBrush* Brush(const D2D1_COLOR_F& colour);
    IDWriteTextFormat* Format(TextStyle style) const;

    struct Impl;
    Impl* impl_ = nullptr;

    Input input_;
    int hot_ = 0;
    int active_ = 0;
    bool hotThisFrame_ = false;
    float width_ = 0, height_ = 0;

    // The clip in force, if any. Hit testing consults it so that a control
    // scrolled out of view cannot be pressed.
    D2D1_RECT_F clip_{};
    bool clipped_ = false;
    float opacity_ = 1.0f;
};

}  // namespace xcam::ui
