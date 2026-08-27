#pragma once

#include "app/lut.h"

#include <cstdint>
#include <vector>
#include <functional>
#include <string>

struct ID3D11Device;
struct ID3D11Texture2D;
struct ID2D1DeviceContext;
struct IDWriteFactory;
struct HWND__;
typedef HWND__* HWND;

namespace xcam {

// Draws decoder output straight to a window. The decoded frame is already an
// NV12 texture on the GPU, so the only work per frame is binding two shader
// views over it -- luma as R8, chroma as R8G8 -- and converting to RGB in the
// pixel shader. Nothing is ever copied through system memory.
class PreviewRenderer {
public:
    PreviewRenderer();
    ~PreviewRenderer();

    PreviewRenderer(const PreviewRenderer&) = delete;
    PreviewRenderer& operator=(const PreviewRenderer&) = delete;

    bool Init(ID3D11Device* device, HWND window);
    void Shutdown();

    // `visibleWidth/Height` crop away the macroblock padding H.264 adds: a
    // 1080-tall frame is coded as 1088, and sampling that tail draws a band of
    // garbage along the bottom edge.
    bool Present(ID3D11Texture2D* nv12, uint32_t subresource,
                 uint32_t codedWidth, uint32_t codedHeight,
                 uint32_t visibleWidth, uint32_t visibleHeight);

    // Repaints the last frame; used when the window is resized or exposed while
    // the stream is paused. `drawOverlay` runs between the video and the flip.
    void Repaint();

    // Draws the last decoded frame and hands the caller a chance to overlay
    // before presenting. Used when the UI needs to update but no new frame has
    // arrived -- an idle stream still has to respond to the mouse.
    void PresentLast(const std::function<void()>& drawOverlay);

    // Copies a decoded frame into renderer-owned memory and returns immediately.
    //
    // Presenting is deliberately NOT done here. Present blocks while the GPU
    // catches up, and the only thread that has a decoded frame in hand is the
    // one reading the socket -- so presenting from it stalls the reader, the
    // phone's send queue fills, and latency grows without bound. At 1080p the
    // stall is short enough to hide; at 4K it reached sixteen seconds. The copy
    // is a GPU-side blit and costs a fraction of that.
    // `slot` is which angle this frame belongs to. Each one keeps its own
    // texture, so several phones decode into the renderer at once without
    // fighting over a single picture, and the one on program is a choice made
    // at draw time rather than a race at upload time. Slots are made as they
    // are first used.
    bool UploadFrame(size_t slot, ID3D11Texture2D* nv12, uint32_t subresource,
                     uint32_t codedWidth, uint32_t codedHeight,
                     uint32_t visibleWidth, uint32_t visibleHeight);

    // The cut. Which slot the preview draws, the virtual camera publishes and
    // the graded recording encodes -- one call, and everything downstream
    // follows, because all three read whatever is on program.
    void SetProgram(size_t slot);
    size_t Program() const;

    // Whether an angle has ever decoded anything. What tells a camera that is
    // connected but silent from one that is simply not on air yet.
    bool HasPicture(size_t slot) const;

    void Resize(uint32_t width, uint32_t height);

    // Direct2D drawing onto the same back buffer, for the overlay UI. Call
    // BeginOverlay after the video is drawn and EndOverlay before presenting;
    // Present/Repaint do the flip themselves, so the overlay has to land in
    // between.
    ID2D1DeviceContext* BeginOverlay();
    void EndOverlay();

    IDWriteFactory* DWrite() const;

    // Uploads a parsed cube as a volume texture. Passing an invalid LUT clears
    // the grade, which is how the panel turns it off.
    bool SetLut(const CubeLut& lut);
    void ClearLut();

    // 0 bypasses the grade without unloading it, so it can be toggled against
    // the raw image without paying to reload.
    void SetLutAmount(float amount);
    bool HasLut() const;

    // Monitoring aids, drawn over the preview and never into a file.
    //
    // `zebraLevel` is the luma at which the stripes start, 0 for none;
    // `peakStrength` is the edge strength that counts as sharp, 0 for none.
    // Both measure the picture before the LUT: what the phone recorded is that
    // signal, and exposure judged after a grade is exposure judged against a
    // preference.
    void SetMonitorAids(float zebraLevel, float peakStrength);

    // The grade, which is the opposite of the aids: it reaches everything the
    // shader draws, so what a call sees and what a graded take records are what
    // the preview shows. All four are 0 for neutral.
    //
    // `gain` is in stops. The other three run -1 to +1.
    void SetGrade(float gain, float contrast, float saturation, float warmth);
    bool HasGrade() const;

    // The shape the picture is being made into, as a target aspect ratio, and
    // whether the source has to be stood up to reach it.
    //
    // `rotate` is 0, 1 or 2 -- none, clockwise, anticlockwise. A phone held
    // upright still hands over a landscape buffer with the scene on its side,
    // because the sensor does not turn with the body; standing it up here uses
    // every pixel, where cropping a tall slice out of a wide frame throws two
    // thirds of them away.
    //
    // 0 for `outAspect` means "whatever shape the frame already is".
    void SetShape(float outAspect, int rotate);

    // Mirroring. Applied by the shader, so it reaches the preview and anything
    // drawn through it; the virtual camera's readback does its own, for the
    // same reason it paints its own matte.
    void SetFlip(bool horizontal, bool vertical);
    bool FlipX() const;
    bool FlipY() const;

    // The cinematic matte, as a target aspect ratio -- 2.39, 2.35, 1.85 -- or 0
    // for none. Applied by the shader, so it reaches the preview and anything
    // drawn through it, which includes a graded recording.
    void SetMatte(float targetAspect);

    // The same mask as a fraction of the height, for the paths that never go
    // through the shader. The virtual camera is one: it blits the decoded frame
    // straight out, and adding a shader pass there to draw two black bars would
    // cost every frame a round trip it does not otherwise make.
    float MatteEdgeFor(uint32_t width, uint32_t height) const;

    // Pulls the current frame back to system memory as NV12, for publishing to
    // the virtual camera. This stalls until the GPU has finished the frame, so
    // it belongs on the presenting thread and never on the one reading the
    // socket -- the same rule that governs Present.
    // Scales the current frame to a fixed size and pulls it back as NV12, for
    // publishing to the virtual camera.
    //
    // Fixed on purpose. A DirectShow pin negotiates its format once, when the
    // application connects, and cannot change it afterwards in any way most
    // applications honour -- so if the published size followed the capture
    // resolution, changing capture would black out an open Zoom call and never
    // recover. The scale is a hardware video-processor blit, which also crops
    // the coded padding away for free.
    //
    // Stalls until the GPU has finished the frame, so this belongs on the
    // presenting thread and never on the one reading the socket.
    bool ReadbackScaledNv12(uint32_t targetWidth, uint32_t targetHeight,
                            std::vector<uint8_t>& dst, uint32_t& stride);

    // The same frame with the loaded LUT applied, as packed NV12 -- what an
    // encoder takes. Used only for recording a graded picture, which is the one
    // thing in this project that has to re-encode: a grade cannot be applied to
    // a stream without decoding it first.
    //
    // Fills the target rather than letterboxing, and stalls on the GPU like
    // every other readback, so it belongs on the presenting thread.
    bool GradeToNv12(uint32_t targetWidth, uint32_t targetHeight,
                     std::vector<uint8_t>& dst, uint32_t& stride);

    // The frame the virtual camera should publish: the same shader, so what
    // Zoom sees is what the preview shows.
    //
    // This used to be ReadbackScaledNv12, a hardware blit straight off the
    // decoded texture -- which never touched the shader, so a loaded LUT
    // reached the preview and not the call, and the mirror and the matte had to
    // be written a second time as byte-twiddling on the NV12 buffer afterwards.
    // One path means one place where the look is decided.
    bool PublishToNv12(uint32_t targetWidth, uint32_t targetHeight,
                       std::vector<uint8_t>& dst, uint32_t& stride);

    const std::string& LastError() const { return lastError_; }

private:
    // Which set of render resources a readback uses. Two consumers at two sizes
    // -- the virtual camera at its published size, the recording at the capture
    // size -- and one shared set would have them rebuilding each other's every
    // frame.
    static constexpr size_t kPublishTarget = 0;
    static constexpr size_t kRecordTarget = 1;

    // Draws the live slot through the pixel shader at an arbitrary size and
    // pulls it back as packed NV12.
    bool RenderToNv12(size_t which, uint32_t targetWidth, uint32_t targetHeight,
                      std::vector<uint8_t>& dst, uint32_t& stride);

    // Copies the frame into the renderer's own sampleable texture. No drawing,
    // no flip -- callers decide when those happen.
    bool Upload(size_t slot, ID3D11Texture2D* nv12, uint32_t subresource,
                uint32_t codedWidth, uint32_t codedHeight,
                uint32_t visibleWidth, uint32_t visibleHeight);

    struct Impl;
    Impl* impl_;
    std::string lastError_;
};

}  // namespace xcam
