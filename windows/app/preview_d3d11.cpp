#include "app/preview_d3d11.h"

#include <windows.h>
#include <d3d11.h>
#include <d2d1_1.h>
#include <d3dcompiler.h>
#include <dwrite.h>
#include <dxgi1_2.h>

#include <algorithm>
#include <deque>
#include <mutex>

#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

namespace xcam {
namespace {

template <typename T>
void SafeRelease(T*& p) {
    if (p) { p->Release(); p = nullptr; }
}

// A full-screen triangle rather than a quad: one fewer vertex, no seam down the
// diagonal, and it needs no vertex buffer at all -- the positions come from the
// vertex id.
constexpr char kVertexShader[] = R"(
struct VSOut {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

VSOut main(uint id : SV_VertexID) {
    VSOut o;
    o.uv  = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
)";

// BT.709 limited range, which is what the phone's encoder tags its output with.
// Getting this wrong is subtle rather than obvious: the picture still looks
// plausible, just washed out or slightly off in the skin tones.
constexpr char kPixelShader[] = R"(
Texture2D<float>  lumaTex   : register(t0);
Texture2D<float2> chromaTex : register(t1);
Texture3D<float4> lutTex    : register(t2);
SamplerState      samp      : register(s0);

cbuffer Crop : register(b0) {
    float2 uvScale;     // visible size / coded size
    float  lutAmount;   // 0 = bypass, 1 = fully graded
    float  lutSize;     // cube edge length, for the half-texel inset
    float  matteEdge;   // fraction of the height masked at top and bottom, 0 = none
    float  flipX;       // 1 = mirrored left to right
    float  flipY;       // 1 = flipped top to bottom
    float  zebra;       // luma at which the stripes start, 0 = off
    float  peak;        // edge strength that counts as sharp, 0 = off
    float  aids;        // 1 while drawing for a person to look at
    float2 texel;       // one texel of the coded frame, for the edge test

    // The grade. All four are 0 for "leave it alone", so a default install
    // computes nothing and looks exactly as it did before they existed.
    float  gain;        // exposure, in stops
    float  contrast;    // -1 flat, +1 hard
    float  saturation;  // -1 monochrome, +1 heavy
    float  warmth;      // -1 cold, +1 warm

    // The output shape. `crop` is how much of each axis of the source survives
    // -- 1 means all of it -- and `rotate` is 0, 1 or 2 for none, clockwise and
    // anticlockwise.
    float2 crop;
    float  rotate;
    float  pad;
};

float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
    // The matte comes first: masked rows are not sampled at all, so a LUT that
    // lifts black cannot lift the bars with it.
    if (matteEdge > 0.0 && (uv.y < matteEdge || uv.y > 1.0 - matteEdge)) {
        return float4(0.0, 0.0, 0.0, 1.0);
    }

    // Flipped before sampling rather than by drawing the quad backwards: the
    // quad is three vertices generated in the vertex shader, and there is no
    // vertex buffer to reverse.
    float2 t = uv;
    if (flipX > 0.5) t.x = 1.0 - t.x;
    if (flipY > 0.5) t.y = 1.0 - t.y;

    // Rotation before the crop, because it decides which axis is long.
    //
    // A phone held upright still hands over a landscape buffer with the scene
    // lying on its side -- the sensor does not turn with the body. Swapping the
    // axes here stands it up using every pixel there is, which is why this beats
    // cropping a tall slice out of a wide frame and throwing two thirds away.
    if (rotate > 1.5)      t = float2(t.y, 1.0 - t.x);
    else if (rotate > 0.5) t = float2(1.0 - t.y, t.x);

    // Then the crop that makes the source fit the output's shape. Centred: a
    // vertical frame taken off a horizontal sensor is the middle of it.
    t = (t - 0.5) * crop + 0.5;

    float2 src = t * uvScale;

    float  y  = lumaTex.Sample(samp, src);
    float2 uvc = chromaTex.Sample(samp, src);

    y = (y - 0.0627451) * 1.164384;
    float u = uvc.x - 0.5;
    float v = uvc.y - 0.5;

    float3 rgb;
    rgb.r = y + 1.792741 * v;
    rgb.g = y - 0.213249 * u - 0.532909 * v;
    rgb.b = y + 2.112402 * u;

    rgb = saturate(rgb);

    // Kept before the grade.
    //
    // Zebras and peaking measure the picture, not the look. What the phone
    // recorded is this signal; a LUT is a decision about how to show it, and
    // exposure judged after one would be exposure judged against a preference.
    float source = y;

    // Exposure and warmth come before the LUT because they correct the
    // capture, and a look applied to a wrongly exposed frame is a look fighting
    // a mistake. Contrast and saturation come after it, because they are a trim
    // on the look -- which also means toggling the LUT does not change what
    // either of them does.
    if (gain != 0.0) rgb = saturate(rgb * exp2(gain));
    if (warmth != 0.0) {
        rgb.r = saturate(rgb.r * (1.0 + warmth * 0.30));
        rgb.b = saturate(rgb.b * (1.0 - warmth * 0.30));
    }

    if (lutAmount > 0.0) {
        // Sample at texel centres. Without the inset the outer half-texel is
        // clamped, which crushes the darkest and brightest values -- exactly
        // where a log image keeps the detail the LUT exists to bring back.
        float scale = (lutSize - 1.0) / lutSize;
        float offset = 1.0 / (2.0 * lutSize);
        float3 graded = lutTex.Sample(samp, rgb * scale + offset).rgb;
        rgb = lerp(rgb, graded, lutAmount);
    }

    if (contrast != 0.0) {
        // Pivoted on mid grey, so raising contrast opens the ends of the scale
        // rather than simply brightening everything.
        rgb = saturate((rgb - 0.5) * (1.0 + contrast) + 0.5);
    }
    if (saturation != 0.0) {
        // Rec.709 luma, so desaturating leaves the picture at the brightness it
        // already had instead of the average of three channels.
        float grey = dot(rgb, float3(0.2126, 0.7152, 0.0722));
        rgb = saturate(lerp(float3(grey, grey, grey), rgb, 1.0 + saturation));
    }

    // ---- monitoring, never recorded ---------------------------------------
    //
    // `aids` is 0 for anything that will be written to a file. A zebra baked
    // into a recording is a stripe somebody has to explain later.
    if (aids > 0.5) {
        if (zebra > 0.0 && source >= zebra) {
            // Diagonal, six pixels apart, in screen space so the stripes stay
            // the same width whatever the picture is scaled to.
            float stripe = frac((pos.x + pos.y) * 0.1666667);
            if (stripe < 0.5) rgb = float3(0.0, 0.0, 0.0);
        }

        if (peak > 0.0) {
            // A cross of four neighbours rather than a full Sobel: focus
            // peaking only has to find where detail is, and the cheap answer
            // finds the same edges.
            float l = lumaTex.Sample(samp, src - float2(texel.x, 0.0));
            float r = lumaTex.Sample(samp, src + float2(texel.x, 0.0));
            float u = lumaTex.Sample(samp, src - float2(0.0, texel.y));
            float d = lumaTex.Sample(samp, src + float2(0.0, texel.y));
            float edge = abs(r - l) + abs(d - u);
            if (edge > peak) rgb = lerp(rgb, float3(1.0, 0.25, 0.0), 0.85);
        }
    }

    return float4(rgb, 1.0);
}
)";

// How much of the height a cinematic matte masks at each edge.
//
// A matte crops rather than squeezes: the picture keeps its full width and the
// rows outside the target ratio go black, which is what a 16:9 frame delivered
// at 2.35:1 looks like -- 1920x1080 showing 1920x817. Returns 0 when the
// source is already wider than the target, since there would be nothing to mask.
// Measured against the shape being produced rather than the frame that arrived.
// They were the same thing until the output could be vertical; now a 2.35 matte
// on a 9:16 frame has to mask what 9:16 shows, not what the sensor sent.
float MatteEdge(float targetAspect, float outAspect) {
    if (targetAspect <= 0.0f || outAspect <= 0.0f) return 0.0f;
    if (outAspect >= targetAspect) return 0.0f;
    return (1.0f - outAspect / targetAspect) * 0.5f;
}

struct CropConstants {
    float uvScaleX;
    float uvScaleY;
    float lutAmount;
    float lutSize;
    float matteEdge;
    float flipX;
    float flipY;
    float zebra;
    float peak;
    float aids;
    float texelX;
    float texelY;

    // The grade, in the register that used to be padding.
    //
    // A constant buffer is a whole number of 16-byte registers and CreateBuffer
    // refuses anything else. Adding the monitoring fields once took this to 52
    // bytes, the buffer was never created, the shader was handed nothing, and
    // the preview came out as coloured noise -- which is what an unbound
    // constant buffer looks like rather than a crash. There were four floats of
    // slack left over from rounding back up to 64; these are them, so the size
    // has not moved.
    float gain;
    float contrast;
    float saturation;
    float warmth;

    float cropX;
    float cropY;
    float rotate;
    float pad;
};

static_assert(sizeof(CropConstants) % 16 == 0,
              "the shader's constants must fill whole 16-byte registers");

}  // namespace

struct PreviewRenderer::Impl {
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    IDXGISwapChain1* swapChain = nullptr;
    ID3D11RenderTargetView* backBufferView = nullptr;

    ID3D11VertexShader* vertexShader = nullptr;
    ID3D11PixelShader* pixelShader = nullptr;
    ID3D11SamplerState* sampler = nullptr;
    ID3D11Buffer* cropBuffer = nullptr;

    // The cinematic matte's target ratio, 0 when off. Kept as the ratio rather
    // than the mask size because the mask depends on the frame, which changes.
    float matteAspect = 0.0f;

    // Mirroring, which every webcam has and this one did not.
    //
    // Left to right is the one people reach for; top to bottom is for a phone
    // hanging upside down in a clamp, which is how half of them end up mounted.
    // Rotation by ninety degrees is deliberately not here: it changes the shape
    // of the output, and every consumer of it -- the virtual camera's declared
    // size, the recording, the preview's aspect -- would have to renegotiate.
    // That is a portrait mode, not a flip.
    bool flipX = false;
    bool flipY = false;

    // One angle's picture, on the GPU.
    //
    // A cut is a change of which of these the draw reads from. Nothing
    // renegotiates and nothing reconnects: the angle that is not on air keeps
    // arriving and keeps its texture up to date, so switching back is a frame
    // rather than a reconnection -- which is the difference between a vision
    // mixer and closing one application and opening another.
    //
    // The views are onto our own copy, and are rebuilt whenever the incoming
    // texture or subresource changes.
    struct Slot {
        ID3D11ShaderResourceView* lumaView = nullptr;
        ID3D11ShaderResourceView* chromaView = nullptr;
        ID3D11Texture2D* lastTexture = nullptr;
        uint32_t lastSubresource = ~0u;

        // Ours, sampleable, and the destination of the copy out of decoder
        // memory.
        ID3D11Texture2D* sampleTexture = nullptr;
        uint32_t sampleWidth = 0;
        uint32_t sampleHeight = 0;

        uint32_t frameWidth = 0;
        uint32_t frameHeight = 0;
        uint32_t codedWidth = 0;
        uint32_t codedHeight = 0;
        float aspect = 16.0f / 9.0f;
    };

    // A deque rather than a vector: a second angle is added while the first is
    // decoding, and a vector would move the slot a running thread is holding a
    // reference to.
    std::deque<Slot> slots{1};

    // Which one is on air. Read on the UI thread, written on the UI thread --
    // the decode threads only ever touch their own slot.
    size_t program = 0;

    Slot& Live() { return slots[program < slots.size() ? program : 0]; }

    // The grade, if one is loaded. Held as a volume texture so the lookup is a
    // single trilinear sample the GPU does for free.
    ID3D11Texture3D* lutTexture = nullptr;
    ID3D11ShaderResourceView* lutView = nullptr;
    float lutAmount = 0.0f;
    float lutSize = 0.0f;

    // CPU-visible copy, used only when the virtual camera is publishing.
    ID3D11Texture2D* stagingTexture = nullptr;

    // Hardware scaler feeding the virtual camera: decoder size in, a fixed
    // published size out.
    ID3D11VideoDevice* videoDevice = nullptr;
    ID3D11VideoContext* videoContext = nullptr;
    ID3D11VideoProcessor* videoProcessor = nullptr;
    ID3D11VideoProcessorEnumerator* videoEnum = nullptr;
    ID3D11Texture2D* scaledTexture = nullptr;
    ID3D11VideoProcessorOutputView* scaledView = nullptr;

    // Everything one shader-rendered readback needs, at one size.
    //
    // There are two of these because there are two consumers at two different
    // sizes: the virtual camera at its fixed published size, and the graded
    // recording at the capture size. Sharing one set meant each of them tearing
    // the other's down every frame -- the size check would miss, everything
    // would be released and rebuilt, and both would pay for it sixty times a
    // second.
    struct Target {
        ID3D11Texture2D* texture = nullptr;          // BGRA, the rendered picture
        ID3D11RenderTargetView* rtv = nullptr;
        ID3D11VideoProcessor* processor = nullptr;   // BGRA -> NV12
        ID3D11VideoProcessorEnumerator* enumerator = nullptr;
        ID3D11Texture2D* nv12 = nullptr;
        ID3D11VideoProcessorOutputView* nv12View = nullptr;
        ID3D11Texture2D* staging = nullptr;
        uint32_t width = 0, height = 0;
    };

    Target targets[2];

    void ReleaseConverter(Target& target);
    void ReleaseTarget(Target& target);
    uint32_t scalerInWidth = 0, scalerInHeight = 0;
    uint32_t scalerOutWidth = 0, scalerOutHeight = 0;

    void ReleaseScaler();

    // Direct2D shares the swap chain's back buffer, which is why the chain is
    // BGRA and the device was created with BGRA support.
    ID2D1Factory1* d2dFactory = nullptr;
    ID2D1Device* d2dDevice = nullptr;
    ID2D1DeviceContext* d2dContext = nullptr;
    ID2D1Bitmap1* d2dTarget = nullptr;
    IDWriteFactory* dwrite = nullptr;

    // Guards the sampling texture and its views, which the decode thread writes
    // and the UI thread reads. Deliberately narrow: it is never held across
    // Present, because Present waits on the GPU and holding a lock through it
    // would stall the socket reader -- the exact coupling this split exists to
    // remove.
    std::mutex textureLock;

    HWND window = nullptr;
    uint32_t backBufferWidth = 0;
    uint32_t backBufferHeight = 0;

    // Monitoring aids: the zebra threshold and the peaking strength, both 0
    // when off. They belong to the preview alone -- see WriteConstants.
    float zebra = 0.0f;
    float peak = 0.0f;

    // The shape being produced: the target aspect ratio, and whether the source
    // has to be stood up to reach it. 0 means "whatever the frame already is".
    float outAspect = 0.0f;
    int rotate = 0;

    // The grade. Unlike the aids, these reach everything the shader draws --
    // the preview, the virtual camera and the graded recording.
    float gain = 0.0f;
    float contrast = 0.0f;
    float saturation = 0.0f;
    float warmth = 0.0f;

    // Fills the shader's constants. `aids` decides whether the monitoring
    // overlays are drawn, which is the difference between the preview and
    // anything on its way to a file: a zebra baked into a recording is a stripe
    // somebody has to explain later.
    void WriteConstants(bool aids);

    void Flip();
    bool CreateBackBufferView();
    bool CreateD2DTarget();
    void ReleaseD2DTarget();
    void ReleaseViews(Slot& slot);
    void ReleaseSampleTexture();
    void Draw();
};

PreviewRenderer::PreviewRenderer() : impl_(new Impl) {}

PreviewRenderer::~PreviewRenderer() {
    Shutdown();
    delete impl_;
}

void PreviewRenderer::Impl::WriteConstants(bool aids) {
    const Slot& live = Live();
    if (!context || !cropBuffer || live.codedWidth == 0 || live.codedHeight == 0) return;

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(cropBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return;

    auto* crop = static_cast<CropConstants*>(mapped.pData);
    crop->uvScaleX = static_cast<float>(live.frameWidth) / static_cast<float>(live.codedWidth);
    crop->uvScaleY = static_cast<float>(live.frameHeight) / static_cast<float>(live.codedHeight);
    crop->lutAmount = lutView ? lutAmount : 0.0f;
    crop->lutSize = lutSize;
    // The shape the picture is being made into, and how much of the source it
    // takes to fill it.
    const float sourceAspect =
        static_cast<float>(live.frameWidth) / static_cast<float>(live.frameHeight);
    const float rotated = rotate != 0 ? 1.0f / sourceAspect : sourceAspect;
    const float wanted = outAspect > 0.0f ? outAspect : rotated;

    float cropX = 1.0f, cropY = 1.0f;
    if (wanted < rotated) {
        cropX = wanted / rotated;          // the output is narrower: take a column
    } else if (wanted > rotated) {
        cropY = rotated / wanted;          // the output is wider: take a band
    }
    if (rotate != 0) std::swap(cropX, cropY);

    crop->cropX = cropX;
    crop->cropY = cropY;
    crop->rotate = static_cast<float>(rotate);
    crop->pad = 0.0f;

    crop->matteEdge = MatteEdge(matteAspect, wanted);
    crop->flipX = flipX ? 1.0f : 0.0f;
    crop->flipY = flipY ? 1.0f : 0.0f;
    crop->zebra = aids ? zebra : 0.0f;
    crop->peak = aids ? peak : 0.0f;
    crop->aids = aids ? 1.0f : 0.0f;
    crop->texelX = 1.0f / static_cast<float>(live.codedWidth);
    crop->texelY = 1.0f / static_cast<float>(live.codedHeight);

    // Written every frame whether or not they are in use. The mapping is
    // WRITE_DISCARD, so anything left unwritten is whatever was in that memory
    // -- which for a grade would be a picture that changes colour at random.
    crop->gain = gain;
    crop->contrast = contrast;
    crop->saturation = saturation;
    crop->warmth = warmth;

    context->Unmap(cropBuffer, 0);
}

void PreviewRenderer::Impl::ReleaseViews(Slot& slot) {
    SafeRelease(slot.lumaView);
    SafeRelease(slot.chromaView);
    slot.lastTexture = nullptr;
    slot.lastSubresource = ~0u;
}

void PreviewRenderer::Impl::ReleaseScaler() {
    SafeRelease(scaledView);
    SafeRelease(scaledTexture);
    SafeRelease(videoProcessor);
    SafeRelease(videoEnum);
    scalerInWidth = scalerInHeight = 0;
    scalerOutWidth = scalerOutHeight = 0;
}

void PreviewRenderer::Impl::ReleaseConverter(Target& target) {
    SafeRelease(target.nv12View);
    SafeRelease(target.nv12);
    SafeRelease(target.processor);
    SafeRelease(target.enumerator);
    SafeRelease(target.staging);
}

void PreviewRenderer::Impl::ReleaseTarget(Target& target) {
    ReleaseConverter(target);
    SafeRelease(target.rtv);
    SafeRelease(target.texture);
    target.width = target.height = 0;
}

void PreviewRenderer::Impl::ReleaseSampleTexture() {
    for (Slot& slot : slots) {
        ReleaseViews(slot);
        SafeRelease(slot.sampleTexture);
        slot.sampleWidth = slot.sampleHeight = 0;
    }
    SafeRelease(stagingTexture);
}

bool PreviewRenderer::Impl::CreateD2DTarget() {
    ReleaseD2DTarget();
    if (!d2dContext || !swapChain) return false;

    IDXGISurface* surface = nullptr;
    if (FAILED(swapChain->GetBuffer(0, IID_PPV_ARGS(&surface)))) return false;

    const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));

    const HRESULT hr = d2dContext->CreateBitmapFromDxgiSurface(surface, &props, &d2dTarget);
    surface->Release();

    if (SUCCEEDED(hr)) d2dContext->SetTarget(d2dTarget);
    return SUCCEEDED(hr);
}

void PreviewRenderer::Impl::ReleaseD2DTarget() {
    // The order matters: while the context still points at the bitmap, the
    // bitmap keeps the back buffer alive and ResizeBuffers fails outright.
    if (d2dContext) d2dContext->SetTarget(nullptr);
    SafeRelease(d2dTarget);
}

bool PreviewRenderer::Impl::CreateBackBufferView() {
    SafeRelease(backBufferView);

    ID3D11Texture2D* backBuffer = nullptr;
    if (FAILED(swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer)))) return false;

    const HRESULT hr = device->CreateRenderTargetView(backBuffer, nullptr, &backBufferView);
    backBuffer->Release();
    return SUCCEEDED(hr);
}

bool PreviewRenderer::Init(ID3D11Device* device, HWND window) {
    Shutdown();

    impl_->device = device;
    device->AddRef();
    device->GetImmediateContext(&impl_->context);
    impl_->window = window;

    IDXGIDevice* dxgiDevice = nullptr;
    IDXGIAdapter* adapter = nullptr;
    IDXGIFactory2* factory = nullptr;

    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice))) ||
        FAILED(dxgiDevice->GetAdapter(&adapter)) ||
        FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) {
        lastError_ = "could not reach the DXGI factory";
        SafeRelease(adapter);
        SafeRelease(dxgiDevice);
        return false;
    }

    RECT client{};
    GetClientRect(window, &client);
    impl_->backBufferWidth = (std::max)(1L, client.right - client.left);
    impl_->backBufferHeight = (std::max)(1L, client.bottom - client.top);

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = impl_->backBufferWidth;
    desc.Height = impl_->backBufferHeight;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    // FLIP_DISCARD with two buffers is the lowest-latency presentation model
    // DXGI offers for a windowed swap chain; the older BitBlt models add a
    // frame of delay in the desktop compositor.
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

    HRESULT hr = factory->CreateSwapChainForHwnd(device, window, &desc, nullptr, nullptr,
                                                 &impl_->swapChain);
    // Alt+Enter fullscreen would fight the window's own sizing logic.
    factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);

    factory->Release();
    adapter->Release();
    dxgiDevice->Release();

    if (FAILED(hr)) {
        lastError_ = "CreateSwapChainForHwnd failed";
        return false;
    }
    if (!impl_->CreateBackBufferView()) {
        lastError_ = "could not create the back buffer view";
        return false;
    }

    // Direct2D on top of the same device, so the overlay costs one extra draw
    // rather than a second surface and a composite.
    D2D1_FACTORY_OPTIONS factoryOptions{};
    if (SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                    __uuidof(ID2D1Factory1), &factoryOptions,
                                    reinterpret_cast<void**>(&impl_->d2dFactory)))) {
        IDXGIDevice* dxgi = nullptr;
        if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgi)))) {
            if (SUCCEEDED(impl_->d2dFactory->CreateDevice(dxgi, &impl_->d2dDevice))) {
                impl_->d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
                                                      &impl_->d2dContext);
            }
            dxgi->Release();
        }
    }
    if (impl_->d2dContext) {
        impl_->CreateD2DTarget();
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                            reinterpret_cast<IUnknown**>(&impl_->dwrite));
    }

    ID3DBlob* vsBlob = nullptr;
    ID3DBlob* psBlob = nullptr;
    ID3DBlob* errors = nullptr;

    hr = D3DCompile(kVertexShader, sizeof(kVertexShader) - 1, nullptr, nullptr, nullptr,
                    "main", "vs_4_0", 0, 0, &vsBlob, &errors);
    if (FAILED(hr)) {
        lastError_ = errors ? static_cast<const char*>(errors->GetBufferPointer())
                            : "vertex shader compilation failed";
        SafeRelease(errors);
        return false;
    }
    SafeRelease(errors);

    hr = D3DCompile(kPixelShader, sizeof(kPixelShader) - 1, nullptr, nullptr, nullptr,
                    "main", "ps_4_0", 0, 0, &psBlob, &errors);
    if (FAILED(hr)) {
        lastError_ = errors ? static_cast<const char*>(errors->GetBufferPointer())
                            : "pixel shader compilation failed";
        SafeRelease(errors);
        vsBlob->Release();
        return false;
    }
    SafeRelease(errors);

    device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
                               nullptr, &impl_->vertexShader);
    device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(),
                              nullptr, &impl_->pixelShader);
    vsBlob->Release();
    psBlob->Release();

    D3D11_SAMPLER_DESC samplerDesc{};
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    device->CreateSamplerState(&samplerDesc, &impl_->sampler);

    D3D11_BUFFER_DESC cropDesc{};
    cropDesc.ByteWidth = sizeof(CropConstants);
    cropDesc.Usage = D3D11_USAGE_DYNAMIC;
    cropDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cropDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    device->CreateBuffer(&cropDesc, nullptr, &impl_->cropBuffer);

    lastError_.clear();
    return true;
}

void PreviewRenderer::Shutdown() {
    if (!impl_) return;
    SafeRelease(impl_->lutView);
    SafeRelease(impl_->lutTexture);
    for (Impl::Target& target : impl_->targets) impl_->ReleaseTarget(target);
    impl_->ReleaseScaler();
    SafeRelease(impl_->videoContext);
    SafeRelease(impl_->videoDevice);
    impl_->ReleaseD2DTarget();
    SafeRelease(impl_->dwrite);
    SafeRelease(impl_->d2dContext);
    SafeRelease(impl_->d2dDevice);
    SafeRelease(impl_->d2dFactory);
    impl_->ReleaseSampleTexture();
    SafeRelease(impl_->cropBuffer);
    SafeRelease(impl_->sampler);
    SafeRelease(impl_->pixelShader);
    SafeRelease(impl_->vertexShader);
    SafeRelease(impl_->backBufferView);
    SafeRelease(impl_->swapChain);
    SafeRelease(impl_->context);
    SafeRelease(impl_->device);
}

void PreviewRenderer::Resize(uint32_t width, uint32_t height) {
    if (!impl_->swapChain || width == 0 || height == 0) return;
    if (width == impl_->backBufferWidth && height == impl_->backBufferHeight) return;

    impl_->ReleaseD2DTarget();
    SafeRelease(impl_->backBufferView);
    impl_->swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    impl_->backBufferWidth = width;
    impl_->backBufferHeight = height;
    impl_->CreateBackBufferView();
    impl_->CreateD2DTarget();
}

void PreviewRenderer::Impl::Draw() {
    if (!swapChain || !backBufferView) return;

    // Nothing decoded yet. Sampling the empty NV12 planes would put Y, U and V
    // all at zero, which the BT.709 conversion turns into a flat dark green --
    // an alarming thing to show someone whose phone simply has not connected
    // yet. Clear and stop.
    const Slot& live = Live();
    if (!live.lumaView || !live.sampleTexture) {
        const float clear[4] = {0.05f, 0.06f, 0.07f, 1.0f};
        context->ClearRenderTargetView(backBufferView, clear);
        return;
    }

    const float clear[4] = {0.05f, 0.06f, 0.07f, 1.0f};
    context->ClearRenderTargetView(backBufferView, clear);
    context->OMSetRenderTargets(1, &backBufferView, nullptr);

    // Letterbox rather than stretch: a webcam preview that lies about aspect
    // ratio is worse than one with bars.
    const float windowAspect = static_cast<float>(backBufferWidth) /
                               static_cast<float>(backBufferHeight);
    // The shape being produced, not the shape that arrived: in vertical the
    // window should show the tall frame the call is getting, with the room to
    // either side of it left dark.
    const float shown = outAspect > 0.0f
        ? outAspect
        : (rotate != 0 ? 1.0f / live.aspect : live.aspect);

    D3D11_VIEWPORT viewport{};
    if (windowAspect > shown) {
        viewport.Height = static_cast<float>(backBufferHeight);
        viewport.Width = viewport.Height * shown;
        viewport.TopLeftX = (static_cast<float>(backBufferWidth) - viewport.Width) * 0.5f;
        viewport.TopLeftY = 0.0f;
    } else {
        viewport.Width = static_cast<float>(backBufferWidth);
        viewport.Height = viewport.Width / shown;
        viewport.TopLeftX = 0.0f;
        viewport.TopLeftY = (static_cast<float>(backBufferHeight) - viewport.Height) * 0.5f;
    }
    viewport.MaxDepth = 1.0f;
    context->RSSetViewports(1, &viewport);

    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->IASetInputLayout(nullptr);
    context->VSSetShader(vertexShader, nullptr, 0);
    context->PSSetShader(pixelShader, nullptr, 0);
    context->PSSetSamplers(0, 1, &sampler);
    // Drawn for a person, so the monitoring overlays come with it.
    WriteConstants(true);
    context->PSSetConstantBuffers(0, 1, &cropBuffer);

    ID3D11ShaderResourceView* views[] = {live.lumaView, live.chromaView, lutView};
    context->PSSetShaderResources(0, 3, views);

    context->Draw(3, 0);

    // Unbind before presenting: the decoder writes into this same texture for
    // the next frame and D3D refuses to bind it as an output while it is still
    // bound as a shader resource.
    ID3D11ShaderResourceView* none[] = {nullptr, nullptr, nullptr};
    context->PSSetShaderResources(0, 3, none);

    // SyncInterval 0: never wait for vblank. Tearing is a fair trade for not
    // adding up to a frame of latency to a live camera feed.
}

void PreviewRenderer::Impl::Flip() {
    // SyncInterval 0: never wait for vblank. Tearing is a fair trade for not
    // adding up to a frame of latency to a live camera feed.
    if (swapChain) swapChain->Present(0, 0);
}

bool PreviewRenderer::Upload(size_t slotIndex, ID3D11Texture2D* nv12, uint32_t subresource,
                                    uint32_t codedWidth, uint32_t codedHeight,
                                    uint32_t visibleWidth, uint32_t visibleHeight) {
    if (!impl_->swapChain || !impl_->backBufferView || !nv12) return false;

    ID3D11Device* device = impl_->device;
    ID3D11DeviceContext* ctx = impl_->context;

    std::lock_guard<std::mutex> guard(impl_->textureLock);

    // An angle that arrives before anyone made room for it gets room made. The
    // alternative is dropping its frames until the UI thread catches up, which
    // shows as a camera that connects and stays black.
    while (impl_->slots.size() <= slotIndex) impl_->slots.emplace_back();
    Impl::Slot& slot = impl_->slots[slotIndex];

    D3D11_TEXTURE2D_DESC srcDesc{};
    nv12->GetDesc(&srcDesc);

    // DXVA allocates its output textures with BIND_DECODER and, on most drivers,
    // without BIND_SHADER_RESOURCE, so they cannot be sampled directly. One
    // GPU-to-GPU copy into a texture we own sidesteps every driver difference
    // here; at 1080p NV12 that is ~3MB a frame, which does not register next to
    // the decode itself.
    if (!slot.sampleTexture ||
        slot.sampleWidth != srcDesc.Width || slot.sampleHeight != srcDesc.Height) {
        impl_->ReleaseViews(slot);
        SafeRelease(slot.sampleTexture);
        // The readback texture is sized from this one, so it has to go too.
        // Leaving a stale one behind means CopyResource quietly fails and the
        // next readback walks off the end of a smaller mapping.
        SafeRelease(impl_->stagingTexture);

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = srcDesc.Width;
        desc.Height = srcDesc.Height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_NV12;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        // RENDER_TARGET as well as SHADER_RESOURCE: the preview samples this
        // texture, and the video processor that scales it for the virtual
        // camera will not accept an input view over a shader-resource-only
        // surface -- CreateVideoProcessorInputView returns E_INVALIDARG.
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

        if (FAILED(device->CreateTexture2D(&desc, nullptr, &slot.sampleTexture))) {
            lastError_ = "could not create the sampling texture";
            return false;
        }
        slot.sampleWidth = srcDesc.Width;
        slot.sampleHeight = srcDesc.Height;

        D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels = 1;

        // NV12 is not sampleable as one surface; it is read as two planes, luma
        // at full resolution and chroma at half in both axes.
        srv.Format = DXGI_FORMAT_R8_UNORM;
        if (FAILED(device->CreateShaderResourceView(slot.sampleTexture, &srv,
                                                    &slot.lumaView))) {
            lastError_ = "could not create the luma view";
            return false;
        }

        srv.Format = DXGI_FORMAT_R8G8_UNORM;
        if (FAILED(device->CreateShaderResourceView(slot.sampleTexture, &srv,
                                                    &slot.chromaView))) {
            lastError_ = "could not create the chroma view";
            impl_->ReleaseViews(slot);
            return false;
        }
    }

    ctx->CopySubresourceRegion(slot.sampleTexture, 0, 0, 0, 0, nv12, subresource, nullptr);
    slot.lastTexture = nv12;
    slot.lastSubresource = subresource;
    (void)codedWidth;

    if (codedWidth && codedHeight) {
        slot.codedWidth = codedWidth;
        slot.codedHeight = codedHeight;
        slot.frameWidth = visibleWidth;
        slot.frameHeight = visibleHeight;
        slot.aspect = static_cast<float>(visibleWidth) / static_cast<float>(visibleHeight);
    }

    return true;
}

bool PreviewRenderer::Present(ID3D11Texture2D* nv12, uint32_t subresource,
                              uint32_t codedWidth, uint32_t codedHeight,
                              uint32_t visibleWidth, uint32_t visibleHeight) {
    if (!Upload(0, nv12, subresource, codedWidth, codedHeight, visibleWidth, visibleHeight)) {
        return false;
    }
    impl_->Draw();
    impl_->Flip();
    return true;
}

bool PreviewRenderer::UploadFrame(size_t slot, ID3D11Texture2D* nv12, uint32_t subresource,
                                  uint32_t codedWidth, uint32_t codedHeight,
                                  uint32_t visibleWidth, uint32_t visibleHeight) {
    return Upload(slot, nv12, subresource, codedWidth, codedHeight, visibleWidth,
                  visibleHeight);
}

void PreviewRenderer::SetProgram(size_t slot) {
    std::lock_guard<std::mutex> guard(impl_->textureLock);
    while (impl_->slots.size() <= slot) impl_->slots.emplace_back();
    impl_->program = slot;
}

size_t PreviewRenderer::Program() const { return impl_->program; }

bool PreviewRenderer::HasPicture(size_t slot) const {
    std::lock_guard<std::mutex> guard(impl_->textureLock);
    return slot < impl_->slots.size() && impl_->slots[slot].lumaView != nullptr;
}

void PreviewRenderer::PresentLast(const std::function<void()>& drawOverlay) {
    if (!impl_->swapChain || !impl_->backBufferView) return;
    {
        std::lock_guard<std::mutex> guard(impl_->textureLock);
        impl_->Draw();
    }
    if (drawOverlay) drawOverlay();
    impl_->Flip();
}

ID2D1DeviceContext* PreviewRenderer::BeginOverlay() {
    if (!impl_->d2dContext || !impl_->d2dTarget) return nullptr;
    impl_->d2dContext->BeginDraw();
    return impl_->d2dContext;
}

void PreviewRenderer::EndOverlay() {
    if (impl_->d2dContext) impl_->d2dContext->EndDraw();
}

IDWriteFactory* PreviewRenderer::DWrite() const {
    return impl_->dwrite;
}

bool PreviewRenderer::SetLut(const CubeLut& lut) {
    std::lock_guard<std::mutex> guard(impl_->textureLock);
    SafeRelease(impl_->lutView);
    SafeRelease(impl_->lutTexture);
    impl_->lutSize = 0.0f;

    if (!lut.IsValid()) {
        lastError_ = "the LUT is empty or the wrong shape";
        return false;
    }

    D3D11_TEXTURE3D_DESC desc{};
    desc.Width = desc.Height = desc.Depth = static_cast<UINT>(lut.size);
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA data{};
    data.pSysMem = lut.rgba.data();
    data.SysMemPitch = static_cast<UINT>(lut.size * 4 * sizeof(float));
    data.SysMemSlicePitch = static_cast<UINT>(lut.size * lut.size * 4 * sizeof(float));

    if (FAILED(impl_->device->CreateTexture3D(&desc, &data, &impl_->lutTexture))) {
        lastError_ = "could not create the LUT texture";
        return false;
    }
    if (FAILED(impl_->device->CreateShaderResourceView(impl_->lutTexture, nullptr,
                                                       &impl_->lutView))) {
        SafeRelease(impl_->lutTexture);
        lastError_ = "could not create the LUT view";
        return false;
    }

    impl_->lutSize = static_cast<float>(lut.size);
    impl_->lutAmount = 1.0f;
    return true;
}

void PreviewRenderer::ClearLut() {
    std::lock_guard<std::mutex> guard(impl_->textureLock);
    SafeRelease(impl_->lutView);
    SafeRelease(impl_->lutTexture);
    impl_->lutSize = 0.0f;
    impl_->lutAmount = 0.0f;
}

void PreviewRenderer::SetLutAmount(float amount) {
    std::lock_guard<std::mutex> guard(impl_->textureLock);
    impl_->lutAmount = amount < 0.0f ? 0.0f : (amount > 1.0f ? 1.0f : amount);
}

void PreviewRenderer::SetMonitorAids(float zebraLevel, float peakStrength) {
    impl_->zebra = zebraLevel;
    impl_->peak = peakStrength;
}

void PreviewRenderer::SetShape(float outAspect, int rotate) {
    impl_->outAspect = outAspect > 0.0f ? outAspect : 0.0f;
    impl_->rotate = rotate;
}

void PreviewRenderer::SetGrade(float gain, float contrast, float saturation,
                               float warmth) {
    impl_->gain = gain;
    impl_->contrast = contrast;
    impl_->saturation = saturation;
    impl_->warmth = warmth;
}

bool PreviewRenderer::HasGrade() const {
    return impl_->gain != 0.0f || impl_->contrast != 0.0f ||
           impl_->saturation != 0.0f || impl_->warmth != 0.0f;
}

void PreviewRenderer::SetFlip(bool horizontal, bool vertical) {
    impl_->flipX = horizontal;
    impl_->flipY = vertical;
}

bool PreviewRenderer::FlipX() const { return impl_->flipX; }
bool PreviewRenderer::FlipY() const { return impl_->flipY; }

void PreviewRenderer::SetMatte(float targetAspect) {
    impl_->matteAspect = targetAspect > 0.0f ? targetAspect : 0.0f;
}

float PreviewRenderer::MatteEdgeFor(uint32_t width, uint32_t height) const {
    if (height == 0) return 0.0f;
    const float aspect = static_cast<float>(width) / static_cast<float>(height);
    return MatteEdge(impl_->matteAspect, aspect);
}

bool PreviewRenderer::HasLut() const {
    return impl_->lutView != nullptr;
}

bool PreviewRenderer::RenderToNv12(size_t which, uint32_t targetWidth,
                                  uint32_t targetHeight, std::vector<uint8_t>& dst,
                                  uint32_t& stride) {
    std::lock_guard<std::mutex> guard(impl_->textureLock);
    Impl::Slot& live = impl_->Live();
    if (!live.sampleTexture || !live.lumaView || targetWidth == 0 || targetHeight == 0) {
        lastError_ = "no frame to render yet";
        return false;
    }
    Impl::Target& target = impl_->targets[which < 2 ? which : 0];
    // Even sizes only: 4:2:0 chroma has no meaning otherwise, and an encoder
    // handed an odd size produces a stream nothing will decode.
    targetWidth &= ~1u;
    targetHeight &= ~1u;

    // ---- the graded picture, drawn offscreen -------------------------------
    //
    // The same vertex shader, pixel shader and LUT the preview uses, pointed at
    // a texture instead of the window. The alternative -- a second shader that
    // does the same thing -- is two places to change a grade.
    if (!target.texture ||
        target.width != targetWidth || target.height != targetHeight) {
        impl_->ReleaseTarget(target);

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = targetWidth;
        desc.Height = targetHeight;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

        if (FAILED(impl_->device->CreateTexture2D(&desc, nullptr, &target.texture)) ||
            FAILED(impl_->device->CreateRenderTargetView(target.texture, nullptr,
                                                        &target.rtv))) {
            lastError_ = "could not create the render target";
            impl_->ReleaseTarget(target);
            return false;
        }
        target.width = targetWidth;
        target.height = targetHeight;
    }

    const float clear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    impl_->context->ClearRenderTargetView(target.rtv, clear);
    impl_->context->OMSetRenderTargets(1, &target.rtv, nullptr);

    // Fills the frame rather than letterboxing. The preview has a window whose
    // shape it does not control; a recording is exactly the size asked for.
    D3D11_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(targetWidth);
    viewport.Height = static_cast<float>(targetHeight);
    viewport.MaxDepth = 1.0f;
    impl_->context->RSSetViewports(1, &viewport);

    impl_->context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    impl_->context->IASetInputLayout(nullptr);
    impl_->context->VSSetShader(impl_->vertexShader, nullptr, 0);
    impl_->context->PSSetShader(impl_->pixelShader, nullptr, 0);
    impl_->context->PSSetSamplers(0, 1, &impl_->sampler);
    // Drawn for a file. No zebras, no peaking: an overlay written into a
    // recording is a stripe somebody has to explain later.
    impl_->WriteConstants(false);
    impl_->context->PSSetConstantBuffers(0, 1, &impl_->cropBuffer);

    ID3D11ShaderResourceView* views[] = {live.lumaView, live.chromaView, impl_->lutView};
    impl_->context->PSSetShaderResources(0, 3, views);
    impl_->context->Draw(3, 0);

    ID3D11ShaderResourceView* none[] = {nullptr, nullptr, nullptr};
    impl_->context->PSSetShaderResources(0, 3, none);
    ID3D11RenderTargetView* noTarget = nullptr;
    impl_->context->OMSetRenderTargets(1, &noTarget, nullptr);

    // ---- back to NV12, which is what an encoder takes ----------------------

    if (!impl_->videoDevice) {
        if (FAILED(impl_->device->QueryInterface(IID_PPV_ARGS(&impl_->videoDevice))) ||
            FAILED(impl_->context->QueryInterface(IID_PPV_ARGS(&impl_->videoContext)))) {
            lastError_ = "no video processor on this device";
            return false;
        }
    }

    if (!target.processor) {
        D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
        content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        content.InputWidth = targetWidth;
        content.InputHeight = targetHeight;
        content.OutputWidth = targetWidth;
        content.OutputHeight = targetHeight;
        content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

        if (FAILED(impl_->videoDevice->CreateVideoProcessorEnumerator(
                &content, &target.enumerator)) ||
            FAILED(impl_->videoDevice->CreateVideoProcessor(target.enumerator, 0,
                                                           &target.processor))) {
            lastError_ = "could not create the colour converter";
            impl_->ReleaseConverter(target);
            return false;
        }

        D3D11_TEXTURE2D_DESC out{};
        out.Width = targetWidth;
        out.Height = targetHeight;
        out.MipLevels = 1;
        out.ArraySize = 1;
        out.Format = DXGI_FORMAT_NV12;
        out.SampleDesc.Count = 1;
        out.Usage = D3D11_USAGE_DEFAULT;
        out.BindFlags = D3D11_BIND_RENDER_TARGET;

        if (FAILED(impl_->device->CreateTexture2D(&out, nullptr, &target.nv12))) {
            lastError_ = "could not create the NV12 texture";
            impl_->ReleaseConverter(target);
            return false;
        }

        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outView{};
        outView.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        if (FAILED(impl_->videoDevice->CreateVideoProcessorOutputView(
                target.nv12, target.enumerator, &outView, &target.nv12View))) {
            lastError_ = "could not create the output view";
            impl_->ReleaseConverter(target);
            return false;
        }
    }

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inView{};
    inView.FourCC = 0;
    inView.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    inView.Texture2D.MipSlice = 0;

    ID3D11VideoProcessorInputView* input = nullptr;
    if (FAILED(impl_->videoDevice->CreateVideoProcessorInputView(
            target.texture, target.enumerator, &inView, &input))) {
        lastError_ = "could not bind the rendered frame for conversion";
        return false;
    }

    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = input;

    const HRESULT hr = impl_->videoContext->VideoProcessorBlt(
        target.processor, target.nv12View, 0, 1, &stream);
    input->Release();
    if (FAILED(hr)) {
        lastError_ = "colour conversion failed";
        return false;
    }

    // ---- and back to the CPU, where the encoder is -------------------------

    if (!target.staging) {
        D3D11_TEXTURE2D_DESC staging{};
        target.nv12->GetDesc(&staging);
        staging.Usage = D3D11_USAGE_STAGING;
        staging.BindFlags = 0;
        staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        staging.MiscFlags = 0;
        if (FAILED(impl_->device->CreateTexture2D(&staging, nullptr,
                                                  &target.staging))) {
            lastError_ = "could not create the readback texture";
            return false;
        }
    }

    impl_->context->CopyResource(target.staging, target.nv12);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(impl_->context->Map(target.staging, 0, D3D11_MAP_READ, 0, &mapped))) {
        lastError_ = "could not read the frame back";
        return false;
    }

    // Packed on the way out. The encoder wants rows of exactly `width`, and the
    // GPU's stride is its own business.
    const size_t bytes = static_cast<size_t>(targetWidth) * targetHeight * 3 / 2;
    if (dst.size() < bytes) dst.resize(bytes);

    const auto* src = static_cast<const uint8_t*>(mapped.pData);
    for (uint32_t y = 0; y < targetHeight; ++y) {
        std::memcpy(dst.data() + static_cast<size_t>(y) * targetWidth,
                    src + static_cast<size_t>(y) * mapped.RowPitch, targetWidth);
    }
    const uint8_t* srcChroma = src + static_cast<size_t>(mapped.RowPitch) * targetHeight;
    uint8_t* dstChroma = dst.data() + static_cast<size_t>(targetWidth) * targetHeight;
    for (uint32_t y = 0; y < targetHeight / 2; ++y) {
        std::memcpy(dstChroma + static_cast<size_t>(y) * targetWidth,
                    srcChroma + static_cast<size_t>(y) * mapped.RowPitch, targetWidth);
    }

    impl_->context->Unmap(target.staging, 0);
    stride = targetWidth;
    return true;
}

bool PreviewRenderer::GradeToNv12(uint32_t targetWidth, uint32_t targetHeight,
                                  std::vector<uint8_t>& dst, uint32_t& stride) {
    return RenderToNv12(kRecordTarget, targetWidth, targetHeight, dst, stride);
}

bool PreviewRenderer::PublishToNv12(uint32_t targetWidth, uint32_t targetHeight,
                                    std::vector<uint8_t>& dst, uint32_t& stride) {
    return RenderToNv12(kPublishTarget, targetWidth, targetHeight, dst, stride);
}

bool PreviewRenderer::ReadbackScaledNv12(uint32_t targetWidth, uint32_t targetHeight,
                                         std::vector<uint8_t>& dst, uint32_t& stride) {
    std::lock_guard<std::mutex> guard(impl_->textureLock);
    Impl::Slot& live = impl_->Live();
    if (!live.sampleTexture || targetWidth == 0 || targetHeight == 0) {
        lastError_ = "no frame to publish yet";
        return false;
    }

    D3D11_TEXTURE2D_DESC srcDesc{};
    live.sampleTexture->GetDesc(&srcDesc);

    // Source rectangle is the visible area, not the coded one: H.264 rounds
    // 1080 up to 1088, and blitting the padding would publish eight rows of
    // garbage along the bottom.
    const uint32_t visibleWidth = live.frameWidth ? live.frameWidth : srcDesc.Width;
    const uint32_t visibleHeight = live.frameHeight ? live.frameHeight : srcDesc.Height;

    if (!impl_->videoDevice) {
        if (FAILED(impl_->device->QueryInterface(IID_PPV_ARGS(&impl_->videoDevice))) ||
            FAILED(impl_->context->QueryInterface(IID_PPV_ARGS(&impl_->videoContext)))) {
            lastError_ = "no video processor on this device";
            return false;
        }
    }

    if (!impl_->videoProcessor ||
        impl_->scalerInWidth != srcDesc.Width || impl_->scalerInHeight != srcDesc.Height ||
        impl_->scalerOutWidth != targetWidth || impl_->scalerOutHeight != targetHeight) {
        impl_->ReleaseScaler();

        D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
        content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        content.InputWidth = srcDesc.Width;
        content.InputHeight = srcDesc.Height;
        content.OutputWidth = targetWidth;
        content.OutputHeight = targetHeight;
        content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

        if (FAILED(impl_->videoDevice->CreateVideoProcessorEnumerator(&content,
                                                                      &impl_->videoEnum)) ||
            FAILED(impl_->videoDevice->CreateVideoProcessor(impl_->videoEnum, 0,
                                                            &impl_->videoProcessor))) {
            lastError_ = "could not create the video processor";
            impl_->ReleaseScaler();
            return false;
        }

        D3D11_TEXTURE2D_DESC out{};
        out.Width = targetWidth;
        out.Height = targetHeight;
        out.MipLevels = 1;
        out.ArraySize = 1;
        out.Format = DXGI_FORMAT_NV12;
        out.SampleDesc.Count = 1;
        out.Usage = D3D11_USAGE_DEFAULT;
        out.BindFlags = D3D11_BIND_RENDER_TARGET;

        if (FAILED(impl_->device->CreateTexture2D(&out, nullptr, &impl_->scaledTexture))) {
            lastError_ = "could not create the scaled texture";
            impl_->ReleaseScaler();
            return false;
        }

        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outView{};
        outView.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        if (FAILED(impl_->videoDevice->CreateVideoProcessorOutputView(
                impl_->scaledTexture, impl_->videoEnum, &outView, &impl_->scaledView))) {
            lastError_ = "could not create the video processor output view";
            impl_->ReleaseScaler();
            return false;
        }

        // The readback texture is sized from the output, so it goes with it.
        SafeRelease(impl_->stagingTexture);

        impl_->scalerInWidth = srcDesc.Width;
        impl_->scalerInHeight = srcDesc.Height;
        impl_->scalerOutWidth = targetWidth;
        impl_->scalerOutHeight = targetHeight;
    }

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inView{};
    inView.FourCC = 0;
    inView.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    inView.Texture2D.MipSlice = 0;

    ID3D11VideoProcessorInputView* input = nullptr;
    const HRESULT viewHr = impl_->videoDevice->CreateVideoProcessorInputView(
        live.sampleTexture, impl_->videoEnum, &inView, &input);
    if (FAILED(viewHr)) {
        lastError_ = "CreateVideoProcessorInputView failed (hr=0x" +
                     std::to_string(static_cast<uint32_t>(viewHr)) + ")";
        return false;
    }

    const RECT sourceRect{0, 0, static_cast<LONG>(visibleWidth),
                          static_cast<LONG>(visibleHeight)};
    const RECT destRect{0, 0, static_cast<LONG>(targetWidth),
                        static_cast<LONG>(targetHeight)};
    impl_->videoContext->VideoProcessorSetStreamSourceRect(impl_->videoProcessor, 0, TRUE,
                                                           &sourceRect);
    impl_->videoContext->VideoProcessorSetStreamDestRect(impl_->videoProcessor, 0, TRUE,
                                                         &destRect);
    impl_->videoContext->VideoProcessorSetOutputTargetRect(impl_->videoProcessor, TRUE,
                                                           &destRect);

    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = input;

    const HRESULT hr = impl_->videoContext->VideoProcessorBlt(
        impl_->videoProcessor, impl_->scaledView, 0, 1, &stream);
    input->Release();
    if (FAILED(hr)) {
        lastError_ = "VideoProcessorBlt failed";
        return false;
    }

    if (!impl_->stagingTexture) {
        D3D11_TEXTURE2D_DESC staging{};
        impl_->scaledTexture->GetDesc(&staging);
        staging.Usage = D3D11_USAGE_STAGING;
        staging.BindFlags = 0;
        staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        staging.MiscFlags = 0;
        if (FAILED(impl_->device->CreateTexture2D(&staging, nullptr,
                                                  &impl_->stagingTexture))) {
            lastError_ = "could not create the readback texture";
            return false;
        }
    }

    impl_->context->CopyResource(impl_->stagingTexture, impl_->scaledTexture);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(impl_->context->Map(impl_->stagingTexture, 0, D3D11_MAP_READ, 0, &mapped))) {
        lastError_ = "could not map the readback texture";
        return false;
    }

    stride = mapped.RowPitch;

    // NV12 keeps both planes in one mapping: luma rows first, then the
    // half-height interleaved chroma plane.
    const auto* src = static_cast<const uint8_t*>(mapped.pData);
    const size_t lumaBytes = static_cast<size_t>(mapped.RowPitch) * targetHeight;
    const size_t chromaBytes = lumaBytes / 2;

    dst.resize(lumaBytes + chromaBytes);
    std::memcpy(dst.data(), src, lumaBytes + chromaBytes);

    impl_->context->Unmap(impl_->stagingTexture, 0);
    return true;
}

void PreviewRenderer::Repaint() {
    if (!impl_->swapChain || !impl_->backBufferView) return;

    // Redraw from our own copy: the decoder's texture may already have been
    // recycled for a later frame by the time a WM_PAINT arrives.
    if (impl_->Live().lumaView && impl_->Live().sampleTexture) {
        impl_->Draw();
        impl_->Flip();
        return;
    }
    const float clear[4] = {0.05f, 0.06f, 0.07f, 1.0f};
    impl_->context->ClearRenderTargetView(impl_->backBufferView, clear);
    impl_->Flip();
}

}  // namespace xcam
