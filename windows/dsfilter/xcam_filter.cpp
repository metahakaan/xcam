// XCam Virtual Camera -- a DirectShow push source.
//
// Written directly against the DirectShow interfaces rather than on the old
// BaseClasses library. The Windows SDK ships strmbase.lib but not its headers,
// so using it would mean vendoring twenty-year-old sources into a project whose
// one hard requirement is that it builds from a clean checkout with nothing but
// MSVC and the SDK. The interfaces themselves are all in strmif.h, and what
// BaseClasses would have provided is mostly the COM boilerplate below.
//
// This DLL is loaded into the consuming application's process -- Zoom's, OBS's,
// Chrome's. It owns no capture pipeline of its own; it reads frames out of the
// shared section xcam-app publishes and pushes them downstream.

#include <windows.h>
#include <dshow.h>
#include <ks.h>
#include <ksmedia.h>

// Note the absence of initguid.h: our GUIDs are defined in xcam_guids.cpp, and
// defining them here would also turn every standard GUID in uuids.h into a
// definition that collides with strmiids.lib.
#include "core/shared_frames.h"
#include "dsfilter/filter_trace.h"
#include "dsfilter/xcam_audio_filter.h"
#include "dsfilter/xcam_guids.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "strmiids.lib")

using namespace xcam;

namespace {

HMODULE g_module = nullptr;
std::atomic<long> g_lockCount{0};

// What we publish when no producer is running. A consumer may enumerate media
// types long before xcam-app is started, and refusing to describe ourselves
// then would keep the device out of its list entirely.
constexpr uint32_t kDefaultWidth = 1920;
constexpr uint32_t kDefaultHeight = 1080;
constexpr uint32_t kDefaultFps = 30;

constexpr REFERENCE_TIME kOneSecond = 10'000'000;

// The formats we offer, most preferred first. NV12 is what the producer already
// has, so it costs nothing; the other two exist because plenty of applications
// will not accept anything else.
enum class OutFormat { Nv12, Yuy2, Rgb24 };

struct FormatInfo {
    OutFormat format;
    const GUID* subtype;
    WORD bitCount;
    DWORD compression;
};

const FormatInfo kFormats[] = {
    {OutFormat::Nv12,  &MEDIASUBTYPE_NV12,  12, MAKEFOURCC('N', 'V', '1', '2')},
    {OutFormat::Yuy2,  &MEDIASUBTYPE_YUY2,  16, MAKEFOURCC('Y', 'U', 'Y', '2')},
    {OutFormat::Rgb24, &MEDIASUBTYPE_RGB24, 24, BI_RGB},
};

size_t ImageBytes(OutFormat format, uint32_t width, uint32_t height) {
    switch (format) {
        case OutFormat::Nv12:  return static_cast<size_t>(width) * height * 3 / 2;
        case OutFormat::Yuy2:  return static_cast<size_t>(width) * height * 2;
        case OutFormat::Rgb24: return static_cast<size_t>(width) * height * 3;
    }
    return 0;
}

void FreeMediaType(AM_MEDIA_TYPE& mt) {
    if (mt.cbFormat != 0) {
        CoTaskMemFree(mt.pbFormat);
        mt.cbFormat = 0;
        mt.pbFormat = nullptr;
    }
    if (mt.pUnk) {
        mt.pUnk->Release();
        mt.pUnk = nullptr;
    }
}

void DeleteMediaType(AM_MEDIA_TYPE* mt) {
    if (!mt) return;
    FreeMediaType(*mt);
    CoTaskMemFree(mt);
}

// Builds a VIDEOINFOHEADER media type. Callers own the result.
AM_MEDIA_TYPE* CreateMediaType(const FormatInfo& info, uint32_t width, uint32_t height,
                               uint32_t fps) {
    auto* mt = static_cast<AM_MEDIA_TYPE*>(CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE)));
    if (!mt) return nullptr;
    std::memset(mt, 0, sizeof(AM_MEDIA_TYPE));

    auto* vih = static_cast<VIDEOINFOHEADER*>(CoTaskMemAlloc(sizeof(VIDEOINFOHEADER)));
    if (!vih) {
        CoTaskMemFree(mt);
        return nullptr;
    }
    std::memset(vih, 0, sizeof(VIDEOINFOHEADER));

    vih->AvgTimePerFrame = kOneSecond / (fps ? fps : kDefaultFps);
    vih->dwBitRate = static_cast<DWORD>(ImageBytes(info.format, width, height) * 8 * fps);

    vih->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    vih->bmiHeader.biWidth = static_cast<LONG>(width);
    // Positive for every format.
    //
    // The sign only means anything for uncompressed RGB, where it chooses
    // bottom-up or top-down. For a FOURCC format the picture is top-down by
    // definition and the height is conventionally positive -- and a negative
    // one is not merely unconventional, it is a number applications compare
    // against. ffmpeg tests `biHeight != requested_height` with no abs(), so a
    // -720 here made every size request silently fail to match and the device
    // connected at whatever it happened to offer first.
    vih->bmiHeader.biHeight = static_cast<LONG>(height);
    vih->bmiHeader.biPlanes = 1;
    vih->bmiHeader.biBitCount = info.bitCount;
    vih->bmiHeader.biCompression = info.compression;
    vih->bmiHeader.biSizeImage = static_cast<DWORD>(ImageBytes(info.format, width, height));

    mt->majortype = MEDIATYPE_Video;
    mt->subtype = *info.subtype;
    mt->formattype = FORMAT_VideoInfo;
    mt->bFixedSizeSamples = TRUE;
    mt->bTemporalCompression = FALSE;
    mt->lSampleSize = vih->bmiHeader.biSizeImage;
    mt->cbFormat = sizeof(VIDEOINFOHEADER);
    mt->pbFormat = reinterpret_cast<BYTE*>(vih);
    return mt;
}

// For the trace only. A raw GUID in a log is a lookup for whoever reads it.
const char* FormatName(const GUID& subtype) {
    if (subtype == MEDIASUBTYPE_NV12)  return "NV12 ";
    if (subtype == MEDIASUBTYPE_YUY2)  return "YUY2 ";
    if (subtype == MEDIASUBTYPE_RGB24) return "RGB24";
    return "?????";
}

const FormatInfo* FindFormat(const GUID& subtype) {
    for (const FormatInfo& info : kFormats) {
        if (*info.subtype == subtype) return &info;
    }
    return nullptr;
}

// The sizes this device offers.
//
// It offered exactly one -- 1920x1080 -- and that is why Discord said "could
// not start the camera". Chromium reads the capability list, picks a resolution
// against the constraints the page asked for, and gives up when nothing in the
// list can serve it; a device with a single entry serves almost nothing. Most
// video calls ask for 720p or smaller.
//
// The producer still publishes one size. The filter scales, which costs a
// downsample on the CPU only when something negotiates a size below native.
struct SizeInfo {
    uint32_t width;
    uint32_t height;
};

// Native first, so an application that simply takes the first offer gets the
// picture unscaled.
//
// 16:9 leads, because that is what a call wants and what an application laying
// out around a webcam expects. The vertical and square sizes follow because the
// desktop can now produce them, and a size the producer publishes that this pin
// will not offer is a picture nothing can see without being squashed to fit --
// the rescale here does each axis independently.
constexpr SizeInfo kSizes[] = {
    {1920, 1080},
    {1280, 720},
    {960, 540},
    {640, 360},
    {1080, 1920},
    {720, 1280},
    {1080, 1080},
    {720, 720},
};

const SizeInfo* FindSize(uint32_t width, uint32_t height) {
    for (const SizeInfo& size : kSizes) {
        if (size.width == width && size.height == height) return &size;
    }
    return nullptr;
}

// Bilinear NV12 rescale.
//
// Nearest neighbour is visibly ragged on faces at 1.5x reduction, which is
// exactly the ratio 1080 to 720 asks for, and this only runs when someone
// negotiated a smaller size than the producer publishes.
void ScaleNv12(const uint8_t* src, uint32_t srcStride, uint32_t srcW, uint32_t srcH,
               uint8_t* dst, uint32_t dstW, uint32_t dstH) {
    if (srcW == 0 || srcH == 0 || dstW == 0 || dstH == 0) return;

    const float xRatio = static_cast<float>(srcW) / static_cast<float>(dstW);
    const float yRatio = static_cast<float>(srcH) / static_cast<float>(dstH);

    // ---- luma --------------------------------------------------------------
    for (uint32_t y = 0; y < dstH; ++y) {
        const float sy = (y + 0.5f) * yRatio - 0.5f;
        const int y0 = (std::max)(0, static_cast<int>(sy));
        const int y1 = (std::min)(static_cast<int>(srcH) - 1, y0 + 1);
        const float fy = sy - static_cast<float>(y0);

        const uint8_t* row0 = src + static_cast<size_t>(y0) * srcStride;
        const uint8_t* row1 = src + static_cast<size_t>(y1) * srcStride;
        uint8_t* out = dst + static_cast<size_t>(y) * dstW;

        for (uint32_t x = 0; x < dstW; ++x) {
            const float sx = (x + 0.5f) * xRatio - 0.5f;
            const int x0 = (std::max)(0, static_cast<int>(sx));
            const int x1 = (std::min)(static_cast<int>(srcW) - 1, x0 + 1);
            const float fx = sx - static_cast<float>(x0);

            const float top = row0[x0] + (row0[x1] - row0[x0]) * fx;
            const float bottom = row1[x0] + (row1[x1] - row1[x0]) * fx;
            out[x] = static_cast<uint8_t>(top + (bottom - top) * fy + 0.5f);
        }
    }

    // ---- chroma, half resolution in both directions ------------------------
    const uint8_t* srcChroma = src + static_cast<size_t>(srcStride) * srcH;
    uint8_t* dstChroma = dst + static_cast<size_t>(dstW) * dstH;
    const uint32_t srcCW = srcW / 2, srcCH = srcH / 2;
    const uint32_t dstCW = dstW / 2, dstCH = dstH / 2;
    if (srcCW == 0 || srcCH == 0) return;

    const float cxRatio = static_cast<float>(srcCW) / static_cast<float>(dstCW);
    const float cyRatio = static_cast<float>(srcCH) / static_cast<float>(dstCH);

    for (uint32_t y = 0; y < dstCH; ++y) {
        const float sy = (y + 0.5f) * cyRatio - 0.5f;
        const int y0 = (std::max)(0, static_cast<int>(sy));
        const int y1 = (std::min)(static_cast<int>(srcCH) - 1, y0 + 1);
        const float fy = sy - static_cast<float>(y0);

        const uint8_t* row0 = srcChroma + static_cast<size_t>(y0) * srcStride;
        const uint8_t* row1 = srcChroma + static_cast<size_t>(y1) * srcStride;
        uint8_t* out = dstChroma + static_cast<size_t>(y) * dstCW * 2;

        for (uint32_t x = 0; x < dstCW; ++x) {
            const float sx = (x + 0.5f) * cxRatio - 0.5f;
            const int x0 = (std::max)(0, static_cast<int>(sx));
            const int x1 = (std::min)(static_cast<int>(srcCW) - 1, x0 + 1);
            const float fx = sx - static_cast<float>(x0);

            // U and V are interleaved, so each sample is two bytes apart.
            for (int plane = 0; plane < 2; ++plane) {
                const float top = row0[x0 * 2 + plane] +
                    (row0[x1 * 2 + plane] - row0[x0 * 2 + plane]) * fx;
                const float bottom = row1[x0 * 2 + plane] +
                    (row1[x1 * 2 + plane] - row1[x0 * 2 + plane]) * fx;
                out[x * 2 + plane] =
                    static_cast<uint8_t>(top + (bottom - top) * fy + 0.5f);
            }
        }
    }
}

// ---- pixel conversion ------------------------------------------------------
//
// The producer publishes NV12. Everything else is derived here, inside the
// consuming application's process, which is why only the formats applications
// actually ask for are supported.

void Nv12ToYuy2(const uint8_t* src, uint32_t srcStride, uint8_t* dst,
                uint32_t width, uint32_t height) {
    const uint8_t* chroma = src + static_cast<size_t>(srcStride) * height;

    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* lumaRow = src + static_cast<size_t>(y) * srcStride;
        const uint8_t* chromaRow = chroma + static_cast<size_t>(y / 2) * srcStride;
        uint8_t* out = dst + static_cast<size_t>(y) * width * 2;

        for (uint32_t x = 0; x < width; x += 2) {
            out[0] = lumaRow[x];
            out[1] = chromaRow[x];          // U
            out[2] = lumaRow[x + 1];
            out[3] = chromaRow[x + 1];      // V
            out += 4;
        }
    }
}

void Nv12ToRgb24(const uint8_t* src, uint32_t srcStride, uint8_t* dst,
                 uint32_t width, uint32_t height) {
    const uint8_t* chroma = src + static_cast<size_t>(srcStride) * height;

    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* lumaRow = src + static_cast<size_t>(y) * srcStride;
        const uint8_t* chromaRow = chroma + static_cast<size_t>(y / 2) * srcStride;

        // RGB24 in a DIB is bottom-up, so the last source row goes first.
        uint8_t* out = dst + static_cast<size_t>(height - 1 - y) * width * 3;

        for (uint32_t x = 0; x < width; ++x) {
            // BT.709 limited range, matching what the phone's encoder tags.
            const int c = static_cast<int>(lumaRow[x]) - 16;
            const int u = static_cast<int>(chromaRow[(x & ~1u)]) - 128;
            const int v = static_cast<int>(chromaRow[(x & ~1u) + 1]) - 128;

            const int r = (298 * c + 459 * v + 128) >> 8;
            const int g = (298 * c - 55 * u - 136 * v + 128) >> 8;
            const int b = (298 * c + 541 * u + 128) >> 8;

            out[0] = static_cast<uint8_t>(std::clamp(b, 0, 255));
            out[1] = static_cast<uint8_t>(std::clamp(g, 0, 255));
            out[2] = static_cast<uint8_t>(std::clamp(r, 0, 255));
            out += 3;
        }
    }
}

// The picture shown when there is nothing to show.
//
// It used to be a flat dark fill, which is indistinguishable from a camera that
// is broken -- and "broken" is the wrong conclusion, since the usual reason is
// simply that the phone is not connected yet. So it draws the mark instead: an
// aperture X, amber arm at the upper right, on the panel's own ground. Anyone
// who sees it in a video call knows which application is responsible and that
// it is waiting rather than dead.
//
// Geometry is the brand's, on a 1024 grid scaled to the frame: bars 146 wide,
// arms reaching from 120 out of the centre to 336 short of the corners. Drawn
// as luma and chroma because every output format here is one conversion away
// from NV12, and one drawing is easier to keep right than three.
void DrawSlateNv12(std::vector<uint8_t>& nv12, uint32_t width, uint32_t height) {
    const size_t lumaBytes = static_cast<size_t>(width) * height;
    nv12.assign(lumaBytes * 3 / 2, 0);

    uint8_t* luma = nv12.data();
    uint8_t* chroma = nv12.data() + lumaBytes;

    // Ink for the ground, Signal for the tally arm, Text for the other three,
    // converted to Rec.601 the same way the rest of the pipeline does.
    constexpr uint8_t kGroundY = 18;
    constexpr uint8_t kMarkY = 226;
    constexpr uint8_t kTallyY = 173, kTallyU = 53, kTallyV = 173;

    std::memset(luma, kGroundY, lumaBytes);
    std::memset(chroma, 128, lumaBytes / 2);

    // Normalised coordinates, +-1 across the shorter side, so the mark stays
    // square and centred whatever the frame's shape.
    const double half = (std::min)(width, height) * 0.5 * 0.62;
    if (half < 4.0) return;

    const double cx = width * 0.5;
    const double cy = height * 0.5;
    const double barHalf = 73.0 / 512.0;      // bar width 146 on the 1024 grid
    const double inner = 120.0 / 512.0;       // arms stop short of the centre
    const double outer = 336.0 * 1.41421356 / 512.0;
    const double root2 = 1.41421356;

    // Which of the four arms a point falls in, or -1. Arm 0 is the upper right,
    // the one that carries the tally colour.
    auto armAt = [&](double u, double v) -> int {
        const double dA = (u - v) / root2, tA = (u + v) / root2;   // "\"
        const double dB = (u + v) / root2, tB = (u - v) / root2;   // "/"

        // A rounded cap is a disc at the arm's end, which is what keeps the
        // diagonal from looking chipped at small sizes.
        auto inArm = [&](double d, double t) {
            if (std::fabs(d) <= barHalf && t >= inner && t <= outer) return true;
            const double ex = t - outer;
            return std::sqrt(d * d + ex * ex) <= barHalf && t > inner;
        };

        if (inArm(dB, tB)) return 0;          // upper right
        if (inArm(dB, -tB)) return 1;         // lower left
        if (inArm(dA, tA)) return 2;          // lower right
        if (inArm(dA, -tA)) return 3;         // upper left
        return -1;
    };

    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            // Four samples per pixel. A hard diagonal edge on a flat ground is
            // exactly where aliasing shows, and this is drawn once per size.
            int hits = 0, tally = 0;
            for (int sy = 0; sy < 2; ++sy) {
                for (int sx = 0; sx < 2; ++sx) {
                    const double u = (x + 0.25 + sx * 0.5 - cx) / half;
                    const double v = (y + 0.25 + sy * 0.5 - cy) / half;
                    const int arm = armAt(u, v);
                    if (arm < 0) continue;
                    ++hits;
                    if (arm == 0) ++tally;
                }
            }
            if (hits == 0) continue;

            const double coverage = hits / 4.0;
            const bool amber = tally * 2 > hits;
            const uint8_t markY = amber ? kTallyY : kMarkY;

            luma[static_cast<size_t>(y) * width + x] = static_cast<uint8_t>(
                kGroundY + (markY - kGroundY) * coverage);

            if (amber && coverage > 0.5 && (x % 2) == 0 && (y % 2) == 0) {
                const size_t c = static_cast<size_t>(y / 2) * width + (x / 2) * 2;
                chroma[c] = kTallyU;
                chroma[c + 1] = kTallyV;
            }
        }
    }
}

// The slate in whatever format was negotiated. Drawn once per size by the
// caller and only converted here, since a placeholder is on screen for as long
// as the phone is away and redrawing it thirty times a second would be work
// done to produce the same picture.
void FillPlaceholder(OutFormat format, uint8_t* dst, uint32_t width, uint32_t height,
                     const std::vector<uint8_t>& slate) {
    switch (format) {
        case OutFormat::Nv12:
            std::memcpy(dst, slate.data(), slate.size());
            break;
        case OutFormat::Yuy2:
            Nv12ToYuy2(slate.data(), width, dst, width, height);
            break;
        case OutFormat::Rgb24:
            Nv12ToRgb24(slate.data(), width, dst, width, height);
            break;
    }
}

}  // namespace

// ---------------------------------------------------------------------------

class XCamFilter;

class XCamPin final : public IPin, public IAMStreamConfig, public IKsPropertySet {
public:
    explicit XCamPin(XCamFilter* filter);
    ~XCamPin();

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // IPin
    STDMETHODIMP Connect(IPin* receive, const AM_MEDIA_TYPE* mt) override;
    STDMETHODIMP ReceiveConnection(IPin*, const AM_MEDIA_TYPE*) override { return E_UNEXPECTED; }
    STDMETHODIMP Disconnect() override;
    STDMETHODIMP ConnectedTo(IPin** pin) override;
    STDMETHODIMP ConnectionMediaType(AM_MEDIA_TYPE* mt) override;
    STDMETHODIMP QueryPinInfo(PIN_INFO* info) override;
    STDMETHODIMP QueryDirection(PIN_DIRECTION* dir) override;
    STDMETHODIMP QueryId(LPWSTR* id) override;
    STDMETHODIMP QueryAccept(const AM_MEDIA_TYPE* mt) override;
    STDMETHODIMP EnumMediaTypes(IEnumMediaTypes** en) override;
    STDMETHODIMP QueryInternalConnections(IPin**, ULONG*) override { return E_NOTIMPL; }
    STDMETHODIMP EndOfStream() override { return S_OK; }
    STDMETHODIMP BeginFlush() override { return S_OK; }
    STDMETHODIMP EndFlush() override { return S_OK; }
    STDMETHODIMP NewSegment(REFERENCE_TIME, REFERENCE_TIME, double) override { return S_OK; }

    // IAMStreamConfig -- what applications use to ask for a resolution.
    STDMETHODIMP SetFormat(AM_MEDIA_TYPE* mt) override;
    STDMETHODIMP GetFormat(AM_MEDIA_TYPE** mt) override;
    STDMETHODIMP GetNumberOfCapabilities(int* count, int* size) override;
    STDMETHODIMP GetStreamCaps(int index, AM_MEDIA_TYPE** mt, BYTE* scc) override;

    // IKsPropertySet -- how a filter declares itself a capture pin, which is
    // what makes applications treat it as a camera rather than a generic source.
    STDMETHODIMP Set(REFGUID set, DWORD id, void* instance, DWORD instanceLength,
                     void* data, DWORD dataLength) override;
    STDMETHODIMP Get(REFGUID set, DWORD id, void* instance, DWORD instanceLength,
                     void* data, DWORD dataLength, DWORD* returned) override;
    STDMETHODIMP QuerySupported(REFGUID set, DWORD id, DWORD* support) override;

    HRESULT Active();
    HRESULT Inactive();

    uint32_t Width() const { return width_; }
    uint32_t Height() const { return height_; }

private:
    static DWORD WINAPI ThreadEntry(void* self);
    void PushLoop();

    XCamFilter* filter_;                 // weak; the filter owns this pin
    IPin* connected_ = nullptr;
    IMemInputPin* memInput_ = nullptr;
    IMemAllocator* allocator_ = nullptr;

    AM_MEDIA_TYPE currentType_{};
    OutFormat format_ = OutFormat::Nv12;
    uint32_t width_ = kDefaultWidth;
    uint32_t height_ = kDefaultHeight;
    uint32_t fps_ = kDefaultFps;

    HANDLE thread_ = nullptr;
    HANDLE stopEvent_ = nullptr;
    std::atomic<bool> running_{false};

    SharedFrameReader reader_;
    std::vector<uint8_t> scratch_;

    // Packed NV12 at the negotiated size, when that is smaller than what the
    // producer publishes. Only allocated if something actually asks for a
    // smaller picture.
    std::vector<uint8_t> resized_;

    // The "no signal" picture, in NV12 at the negotiated size. Rebuilt only
    // when that size changes.
    std::vector<uint8_t> slate_;
    uint32_t slateWidth_ = 0, slateHeight_ = 0;

    // The last frame actually delivered, in output format. A camera holds its
    // last picture when the source is momentarily late; it does not flash black,
    // which is what happens if a placeholder goes out instead.
    std::vector<uint8_t> lastFrame_;

    std::atomic<long> refCount_{1};
};

class XCamFilter final : public IBaseFilter {
public:
    XCamFilter();
    ~XCamFilter();

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // IPersist
    STDMETHODIMP GetClassID(CLSID* clsid) override;

    // IMediaFilter
    STDMETHODIMP Stop() override;
    STDMETHODIMP Pause() override;
    STDMETHODIMP Run(REFERENCE_TIME start) override;
    STDMETHODIMP GetState(DWORD timeout, FILTER_STATE* state) override;
    STDMETHODIMP SetSyncSource(IReferenceClock* clock) override;
    STDMETHODIMP GetSyncSource(IReferenceClock** clock) override;

    // IBaseFilter
    STDMETHODIMP EnumPins(IEnumPins** en) override;
    STDMETHODIMP FindPin(LPCWSTR id, IPin** pin) override;
    STDMETHODIMP QueryFilterInfo(FILTER_INFO* info) override;
    STDMETHODIMP JoinFilterGraph(IFilterGraph* graph, LPCWSTR name) override;
    STDMETHODIMP QueryVendorInfo(LPWSTR* vendor) override;

    XCamPin* Pin() { return pin_; }
    IReferenceClock* Clock() const { return clock_; }
    FILTER_STATE State() const { return state_; }
    IFilterGraph* Graph() const { return graph_; }
    REFERENCE_TIME StartTime() const { return startTime_; }

private:
    XCamPin* pin_;
    IFilterGraph* graph_ = nullptr;      // weak, per the DirectShow contract
    IReferenceClock* clock_ = nullptr;
    FILTER_STATE state_ = State_Stopped;
    REFERENCE_TIME startTime_ = 0;
    WCHAR name_[MAX_FILTER_NAME] = XCAM_FILTER_NAME;
    std::atomic<long> refCount_{1};
};

// ---- enumerators -----------------------------------------------------------

class PinEnumerator final : public IEnumPins {
public:
    explicit PinEnumerator(IPin* pin) : pin_(pin) { pin_->AddRef(); }
    ~PinEnumerator() { pin_->Release(); }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IEnumPins) {
            *ppv = static_cast<IEnumPins*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++refCount_; }
    STDMETHODIMP_(ULONG) Release() override {
        const long n = --refCount_;
        if (n == 0) delete this;
        return n;
    }

    STDMETHODIMP Next(ULONG count, IPin** pins, ULONG* fetched) override {
        if (!pins) return E_POINTER;
        ULONG produced = 0;
        if (count > 0 && index_ == 0) {
            pin_->AddRef();
            pins[0] = pin_;
            ++index_;
            produced = 1;
        }
        if (fetched) *fetched = produced;
        return produced == count ? S_OK : S_FALSE;
    }
    STDMETHODIMP Skip(ULONG count) override {
        index_ += count;
        return index_ > 1 ? S_FALSE : S_OK;
    }
    STDMETHODIMP Reset() override { index_ = 0; return S_OK; }
    STDMETHODIMP Clone(IEnumPins** en) override {
        if (!en) return E_POINTER;
        auto* copy = new PinEnumerator(pin_);
        copy->index_ = index_;
        *en = copy;
        return S_OK;
    }

private:
    IPin* pin_;
    ULONG index_ = 0;
    std::atomic<long> refCount_{1};
};

class MediaTypeEnumerator final : public IEnumMediaTypes {
public:
    explicit MediaTypeEnumerator(uint32_t fps) : fps_(fps) {}

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IEnumMediaTypes) {
            *ppv = static_cast<IEnumMediaTypes*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++refCount_; }
    STDMETHODIMP_(ULONG) Release() override {
        const long n = --refCount_;
        if (n == 0) delete this;
        return n;
    }

    STDMETHODIMP Next(ULONG count, AM_MEDIA_TYPE** types, ULONG* fetched) override {
        if (!types) return E_POINTER;
        constexpr ULONG kTotal = ARRAYSIZE(kFormats) * ARRAYSIZE(kSizes);
        ULONG produced = 0;
        while (produced < count && index_ < kTotal) {
            AM_MEDIA_TYPE* mt = CreateMediaType(kFormats[index_ / ARRAYSIZE(kSizes)],
                                                kSizes[index_ % ARRAYSIZE(kSizes)].width,
                                                kSizes[index_ % ARRAYSIZE(kSizes)].height,
                                                fps_);
            if (!mt) break;
            types[produced++] = mt;
            ++index_;
        }
        if (fetched) *fetched = produced;
        return produced == count ? S_OK : S_FALSE;
    }
    STDMETHODIMP Skip(ULONG count) override {
        index_ += count;
        return index_ > ARRAYSIZE(kFormats) * ARRAYSIZE(kSizes) ? S_FALSE : S_OK;
    }
    STDMETHODIMP Reset() override { index_ = 0; return S_OK; }
    STDMETHODIMP Clone(IEnumMediaTypes** en) override {
        if (!en) return E_POINTER;
        auto* copy = new MediaTypeEnumerator(fps_);
        copy->index_ = index_;
        *en = copy;
        return S_OK;
    }

private:
    uint32_t fps_;
    ULONG index_ = 0;
    std::atomic<long> refCount_{1};
};

// ---- pin -------------------------------------------------------------------

XCamPin::XCamPin(XCamFilter* filter) : filter_(filter) {
    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    // The frame rate comes from the producer; the size does not. What this pin
    // offers is the fixed ladder in kSizes, and it scales to whatever gets
    // negotiated -- so a camera list shows every size this device can serve
    // rather than only the one being published right now.
    if (reader_.Attach() && reader_.ProducerAlive()) {
        uint32_t num = 30, den = 1;
        reader_.FrameRate(num, den);
        fps_ = den ? num / den : 30;
    }
}

XCamPin::~XCamPin() {
    Inactive();
    FreeMediaType(currentType_);
    if (stopEvent_) CloseHandle(stopEvent_);
}

STDMETHODIMP XCamPin::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;

    if (riid == IID_IUnknown || riid == IID_IPin) {
        *ppv = static_cast<IPin*>(this);
    } else if (riid == IID_IAMStreamConfig) {
        *ppv = static_cast<IAMStreamConfig*>(this);
    } else if (riid == IID_IKsPropertySet) {
        *ppv = static_cast<IKsPropertySet*>(this);
    } else {
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}

STDMETHODIMP_(ULONG) XCamPin::AddRef() { return ++refCount_; }

STDMETHODIMP_(ULONG) XCamPin::Release() {
    const long n = --refCount_;
    if (n == 0) delete this;
    return n;
}

STDMETHODIMP XCamPin::QueryAccept(const AM_MEDIA_TYPE* mt) {
    if (!mt) return E_POINTER;
    if (mt->majortype != MEDIATYPE_Video) {
        trace::Write("QueryAccept: refused, not video");
        return S_FALSE;
    }
    if (mt->formattype != FORMAT_VideoInfo || !mt->pbFormat) return S_FALSE;
    if (mt->cbFormat < sizeof(VIDEOINFOHEADER)) return S_FALSE;
    if (!FindFormat(mt->subtype)) return S_FALSE;

    const auto* vih = reinterpret_cast<const VIDEOINFOHEADER*>(mt->pbFormat);
    const uint32_t width = static_cast<uint32_t>(vih->bmiHeader.biWidth);
    const uint32_t height = static_cast<uint32_t>(std::abs(vih->bmiHeader.biHeight));
    // A size of zero means "you choose", which several applications send.
    if (width == 0 && height == 0) return S_OK;

    const bool ok = FindSize(width, height) != nullptr;
    trace::Write("[pin %p] QueryAccept: %ux%u -> %s", this, width, height, ok ? "yes" : "NO");
    return ok ? S_OK : S_FALSE;
}

STDMETHODIMP XCamPin::EnumMediaTypes(IEnumMediaTypes** en) {
    trace::Write("EnumMediaTypes");
    if (!en) return E_POINTER;
    *en = new MediaTypeEnumerator(fps_);
    return S_OK;
}

STDMETHODIMP XCamPin::Connect(IPin* receive, const AM_MEDIA_TYPE* mt) {
    if (!receive) return E_POINTER;
    trace::Write("[pin %p] Connect: %s type offered, currently %ux%u", this,
                 mt ? "a" : "no", width_, height_);
    if (connected_) return VFW_E_ALREADY_CONNECTED;
    if (filter_->State() != State_Stopped) return VFW_E_NOT_STOPPED;

    // Everything worth offering, best first.
    std::vector<AM_MEDIA_TYPE*> candidates;

    if (mt && mt->majortype != GUID_NULL && QueryAccept(mt) == S_OK) {
        const auto* vih = reinterpret_cast<const VIDEOINFOHEADER*>(mt->pbFormat);
        uint32_t w = static_cast<uint32_t>(vih->bmiHeader.biWidth);
        uint32_t h = static_cast<uint32_t>(std::abs(vih->bmiHeader.biHeight));
        if (w == 0 || h == 0) { w = width_; h = height_; }
        if (AM_MEDIA_TYPE* only = CreateMediaType(*FindFormat(mt->subtype), w, h, fps_)) {
            candidates.push_back(only);
        }
    } else {
        // Whatever SetFormat last asked for comes first.
        //
        // Applications ask for a resolution through IAMStreamConfig and then
        // let the graph connect with no media type of its own, so a pin that
        // simply offers its own list in its own order silently ignores the
        // request -- which is what made every capture come out at 1080p no
        // matter what was asked for.
        const FormatInfo* preferred = nullptr;
        for (const FormatInfo& info : kFormats) {
            if (info.format == format_) preferred = &info;
        }
        if (preferred) {
            if (AM_MEDIA_TYPE* first = CreateMediaType(*preferred, width_, height_, fps_)) {
                candidates.push_back(first);
            }
        }

        // Then everything else, native size first.
        for (const FormatInfo& info : kFormats) {
            for (const SizeInfo& size : kSizes) {
                if (preferred && &info == preferred &&
                    size.width == width_ && size.height == height_) {
                    continue;      // already at the head of the list
                }
                if (AM_MEDIA_TYPE* candidate =
                        CreateMediaType(info, size.width, size.height, fps_)) {
                    candidates.push_back(candidate);
                }
            }
        }
    }

    // Offered through ReceiveConnection, not QueryAccept.
    //
    // QueryAccept is an optimisation, not the contract: a receiving pin is free
    // to answer S_FALSE to everything and still accept a type when it is
    // actually handed one, and Chromium's capture sink does exactly that. This
    // pin used to treat a QueryAccept refusal as final, so every application
    // built on Chromium -- Discord among them -- was told the camera had no
    // acceptable format at all, having never been offered one.
    //
    // A failed ReceiveConnection leaves the receiver unconnected, so the next
    // candidate can go straight out after it.
    AM_MEDIA_TYPE* chosen = nullptr;
    for (AM_MEDIA_TYPE* candidate : candidates) {
        if (chosen) {
            DeleteMediaType(candidate);
            continue;
        }
        const HRESULT hr = receive->ReceiveConnection(static_cast<IPin*>(this), candidate);
        const auto* vih = reinterpret_cast<const VIDEOINFOHEADER*>(candidate->pbFormat);
        trace::Write("[pin %p] offered %s %ldx%ld -> 0x%08lX", this,
                     FormatName(candidate->subtype), vih->bmiHeader.biWidth,
                     vih->bmiHeader.biHeight, static_cast<unsigned long>(hr));
        if (SUCCEEDED(hr)) {
            chosen = candidate;
            continue;
        }
        DeleteMediaType(candidate);
    }

    if (!chosen) {
        trace::Write("[pin %p] Connect: nothing downstream would accept", this);
        return VFW_E_NO_ACCEPTABLE_TYPES;
    }

    HRESULT hr = receive->QueryInterface(IID_IMemInputPin,
                                         reinterpret_cast<void**>(&memInput_));
    if (FAILED(hr)) {
        receive->Disconnect();
        DeleteMediaType(chosen);
        return hr;
    }

    FreeMediaType(currentType_);
    currentType_ = *chosen;
    CoTaskMemFree(chosen);          // the contents moved into currentType_

    const FormatInfo* info = FindFormat(currentType_.subtype);
    format_ = info ? info->format : OutFormat::Nv12;

    const auto* vih = reinterpret_cast<VIDEOINFOHEADER*>(currentType_.pbFormat);
    width_ = static_cast<uint32_t>(vih->bmiHeader.biWidth);
    height_ = static_cast<uint32_t>(std::abs(vih->bmiHeader.biHeight));
    if (vih->AvgTimePerFrame > 0) {
        fps_ = static_cast<uint32_t>(kOneSecond / vih->AvgTimePerFrame);
    }

    // Negotiate buffers. The downstream filter usually supplies the allocator;
    // asking for one more buffer than it wants avoids a stall each time we are
    // filling one while it still holds the other.
    hr = memInput_->GetAllocator(&allocator_);
    if (FAILED(hr) || !allocator_) {
        hr = CoCreateInstance(CLSID_MemoryAllocator, nullptr, CLSCTX_INPROC_SERVER,
                              IID_IMemAllocator, reinterpret_cast<void**>(&allocator_));
        if (FAILED(hr)) {
            memInput_->Release();
            memInput_ = nullptr;
            receive->Disconnect();
            return hr;
        }
    }

    ALLOCATOR_PROPERTIES request{};
    request.cBuffers = 3;
    request.cbBuffer = static_cast<long>(ImageBytes(format_, width_, height_));
    request.cbAlign = 1;

    ALLOCATOR_PROPERTIES actual{};
    hr = allocator_->SetProperties(&request, &actual);
    if (SUCCEEDED(hr)) hr = memInput_->NotifyAllocator(allocator_, FALSE);
    if (FAILED(hr)) {
        allocator_->Release();
        allocator_ = nullptr;
        memInput_->Release();
        memInput_ = nullptr;
        receive->Disconnect();
        return hr;
    }

    connected_ = receive;
    connected_->AddRef();
    trace::Write("[pin %p] connected at %ux%u @%u fps", this, width_, height_, fps_);
    return S_OK;
}

STDMETHODIMP XCamPin::Disconnect() {
    if (filter_->State() != State_Stopped) return VFW_E_NOT_STOPPED;
    Inactive();

    if (allocator_) { allocator_->Release(); allocator_ = nullptr; }
    if (memInput_) { memInput_->Release(); memInput_ = nullptr; }
    if (connected_) { connected_->Release(); connected_ = nullptr; }
    return S_OK;
}

STDMETHODIMP XCamPin::ConnectedTo(IPin** pin) {
    if (!pin) return E_POINTER;
    *pin = connected_;
    if (!connected_) return VFW_E_NOT_CONNECTED;
    connected_->AddRef();
    return S_OK;
}

STDMETHODIMP XCamPin::ConnectionMediaType(AM_MEDIA_TYPE* mt) {
    if (!mt) return E_POINTER;
    if (!connected_) {
        std::memset(mt, 0, sizeof(*mt));
        return VFW_E_NOT_CONNECTED;
    }
    *mt = currentType_;
    if (mt->cbFormat) {
        mt->pbFormat = static_cast<BYTE*>(CoTaskMemAlloc(mt->cbFormat));
        if (!mt->pbFormat) return E_OUTOFMEMORY;
        std::memcpy(mt->pbFormat, currentType_.pbFormat, mt->cbFormat);
    }
    if (mt->pUnk) mt->pUnk->AddRef();
    return S_OK;
}

STDMETHODIMP XCamPin::QueryPinInfo(PIN_INFO* info) {
    if (!info) return E_POINTER;
    info->pFilter = filter_;
    filter_->AddRef();
    info->dir = PINDIR_OUTPUT;
    wcscpy_s(info->achName, L"Capture");
    return S_OK;
}

STDMETHODIMP XCamPin::QueryDirection(PIN_DIRECTION* dir) {
    if (!dir) return E_POINTER;
    *dir = PINDIR_OUTPUT;
    return S_OK;
}

STDMETHODIMP XCamPin::QueryId(LPWSTR* id) {
    if (!id) return E_POINTER;
    const wchar_t name[] = L"Capture";
    *id = static_cast<LPWSTR>(CoTaskMemAlloc(sizeof(name)));
    if (!*id) return E_OUTOFMEMORY;
    std::memcpy(*id, name, sizeof(name));
    return S_OK;
}

// ---- IAMStreamConfig -------------------------------------------------------

STDMETHODIMP XCamPin::SetFormat(AM_MEDIA_TYPE* mt) {
    if (!mt) return E_POINTER;
    if (QueryAccept(mt) != S_OK) {
        trace::Write("SetFormat: refused");
        return E_INVALIDARG;
    }
    if (connected_) return VFW_E_ALREADY_CONNECTED;

    const auto* vih = reinterpret_cast<VIDEOINFOHEADER*>(mt->pbFormat);
    const uint32_t w = static_cast<uint32_t>(vih->bmiHeader.biWidth);
    const uint32_t h = static_cast<uint32_t>(std::abs(vih->bmiHeader.biHeight));
    if (w > 0 && h > 0 && FindSize(w, h)) {
        width_ = w;
        height_ = h;
    }
    if (vih->AvgTimePerFrame > 0) {
        fps_ = static_cast<uint32_t>(kOneSecond / vih->AvgTimePerFrame);
    }
    const FormatInfo* info = FindFormat(mt->subtype);
    format_ = info ? info->format : OutFormat::Nv12;
    trace::Write("[pin %p] SetFormat: now %ux%u @%u", this, width_, height_, fps_);
    return S_OK;
}

STDMETHODIMP XCamPin::GetFormat(AM_MEDIA_TYPE** mt) {
    if (!mt) return E_POINTER;
    const FormatInfo* info = FindFormat(currentType_.subtype);
    *mt = CreateMediaType(info ? *info : kFormats[0], width_, height_, fps_);
    return *mt ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP XCamPin::GetNumberOfCapabilities(int* count, int* size) {
    if (!count || !size) return E_POINTER;
    // Every pixel format at every size. An application reads this list and
    // picks; one entry in it means almost nothing can be served.
    *count = ARRAYSIZE(kFormats) * ARRAYSIZE(kSizes);
    *size = sizeof(VIDEO_STREAM_CONFIG_CAPS);
    trace::Write("GetNumberOfCapabilities -> %d", *count);
    return S_OK;
}

STDMETHODIMP XCamPin::GetStreamCaps(int index, AM_MEDIA_TYPE** mt, BYTE* scc) {
    if (!mt || !scc) return E_POINTER;
    if (index < 0 || index >= ARRAYSIZE(kFormats) * ARRAYSIZE(kSizes)) return S_FALSE;

    // Size varies fastest, so the first few entries are the native size in
    // every pixel format -- what an application taking the first workable
    // offer should get.
    const FormatInfo& info = kFormats[index / ARRAYSIZE(kSizes)];
    const SizeInfo& size = kSizes[index % ARRAYSIZE(kSizes)];

    *mt = CreateMediaType(info, size.width, size.height, fps_);
    if (!*mt) return E_OUTOFMEMORY;

    auto* caps = reinterpret_cast<VIDEO_STREAM_CONFIG_CAPS*>(scc);
    std::memset(caps, 0, sizeof(*caps));
    caps->guid = FORMAT_VideoInfo;
    caps->VideoStandard = AnalogVideo_None;
    caps->InputSize.cx = static_cast<LONG>(kSizes[0].width);
    caps->InputSize.cy = static_cast<LONG>(kSizes[0].height);
    caps->MinCroppingSize = caps->MaxCroppingSize = caps->InputSize;
    caps->MinOutputSize.cx = static_cast<LONG>(size.width);
    caps->MinOutputSize.cy = static_cast<LONG>(size.height);
    caps->MaxOutputSize = caps->MinOutputSize;
    caps->CropGranularityX = caps->CropGranularityY = 1;
    caps->OutputGranularityX = caps->OutputGranularityY = 1;
    caps->MinFrameInterval = kOneSecond / 60;
    caps->MaxFrameInterval = kOneSecond / 1;
    caps->MinBitsPerSecond = caps->MaxBitsPerSecond =
        static_cast<LONG>(ImageBytes(info.format, size.width, size.height) * 8 * fps_);
    return S_OK;
}

// ---- IKsPropertySet --------------------------------------------------------

STDMETHODIMP XCamPin::Set(REFGUID, DWORD, void*, DWORD, void*, DWORD) {
    return E_NOTIMPL;
}

STDMETHODIMP XCamPin::Get(REFGUID set, DWORD id, void*, DWORD, void* data,
                          DWORD dataLength, DWORD* returned) {
    if (set != AMPROPSETID_Pin) return E_PROP_SET_UNSUPPORTED;
    if (id != AMPROPERTY_PIN_CATEGORY) return E_PROP_ID_UNSUPPORTED;
    if (data && dataLength < sizeof(GUID)) return E_UNEXPECTED;

    if (returned) *returned = sizeof(GUID);
    if (!data) return S_OK;

    // Declaring PIN_CATEGORY_CAPTURE is what makes applications list this as a
    // camera. Without it the filter registers but never shows up as a device.
    *static_cast<GUID*>(data) = PIN_CATEGORY_CAPTURE;
    return S_OK;
}

STDMETHODIMP XCamPin::QuerySupported(REFGUID set, DWORD id, DWORD* support) {
    if (set != AMPROPSETID_Pin) return E_PROP_SET_UNSUPPORTED;
    if (id != AMPROPERTY_PIN_CATEGORY) return E_PROP_ID_UNSUPPORTED;
    if (support) *support = KSPROPERTY_SUPPORT_GET;
    return S_OK;
}

// ---- streaming -------------------------------------------------------------

HRESULT XCamPin::Active() {
    trace::Write("Active: %s", connected_ ? "starting" : "NOT CONNECTED");
    if (!connected_ || running_.load()) return S_OK;
    if (!allocator_) {
        trace::Write("Active: no allocator");
        return VFW_E_NO_ALLOCATOR;
    }

    HRESULT hr = allocator_->Commit();
    if (FAILED(hr)) return hr;

    ResetEvent(stopEvent_);
    running_ = true;
    thread_ = CreateThread(nullptr, 0, ThreadEntry, this, 0, nullptr);
    if (!thread_) {
        running_ = false;
        return E_FAIL;
    }
    return S_OK;
}

HRESULT XCamPin::Inactive() {
    if (!running_.exchange(false)) return S_OK;

    SetEvent(stopEvent_);
    if (thread_) {
        WaitForSingleObject(thread_, 3000);
        CloseHandle(thread_);
        thread_ = nullptr;
    }
    if (allocator_) allocator_->Decommit();
    return S_OK;
}

DWORD WINAPI XCamPin::ThreadEntry(void* self) {
    // The graph may deliver samples on this thread into filters that expect COM
    // to be initialised.
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    static_cast<XCamPin*>(self)->PushLoop();
    CoUninitialize();
    return 0;
}

void XCamPin::PushLoop() {
    const REFERENCE_TIME frameDuration = kOneSecond / (fps_ ? fps_ : kDefaultFps);
    REFERENCE_TIME nextStart = 0;

    // The producer's stride is its own business; we always hand downstream a
    // tightly packed buffer, since that is what a media type describes.
    const size_t outBytes = ImageBytes(format_, width_, height_);

    while (running_.load()) {
        if (WaitForSingleObject(stopEvent_, 0) == WAIT_OBJECT_0) break;

        IMediaSample* sample = nullptr;
        HRESULT hr = allocator_->GetBuffer(&sample, nullptr, nullptr, 0);
        if (FAILED(hr) || !sample) {
            Sleep(5);
            continue;
        }

        BYTE* dst = nullptr;
        if (FAILED(sample->GetPointer(&dst)) || !dst) {
            sample->Release();
            continue;
        }

        bool haveFrame = false;
        uint64_t ptsUs = 0;

        if (!reader_.IsAttached()) reader_.Attach();

        // Every iteration, not only when a frame arrives. This is the tally, and
        // what it has to mean is "an application has the camera open" -- which is
        // true from the moment the graph runs, before and between frames. Tying
        // it to frames would make the light go out whenever the phone did.
        reader_.NoteRead();

        // The producer publishes one size; this pin may have negotiated a
        // smaller one. Whether they match decides only whether a rescale
        // happens, not whether there is a picture.
        const bool producerLive = reader_.IsAttached() && reader_.ProducerAlive() &&
                                  reader_.Width() > 0 && reader_.Height() > 0;

        if (producerLive) {
            const uint32_t srcW = reader_.Width();
            const uint32_t srcH = reader_.Height();
            const uint32_t stride = reader_.Stride();
            const size_t needed = static_cast<size_t>(stride) * srcH * 3 / 2;
            if (scratch_.size() < needed) scratch_.resize(needed);

            // Wait about one frame. Longer only delays the decision to reuse the
            // previous picture, which is what we do anyway when nothing new has
            // arrived.
            if (reader_.ReadLatest(scratch_.data(), scratch_.size(), ptsUs,
                                   static_cast<uint32_t>(frameDuration / 10000) + 4)) {
                const uint8_t* plane = scratch_.data();
                uint32_t planeStride = stride;

                if (srcW != width_ || srcH != height_) {
                    // Rescale into a packed NV12 buffer first, so the format
                    // conversions below stay exactly as they were and only ever
                    // deal with one size.
                    const size_t scaled = static_cast<size_t>(width_) * height_ * 3 / 2;
                    if (resized_.size() < scaled) resized_.resize(scaled);
                    ScaleNv12(scratch_.data(), stride, srcW, srcH,
                              resized_.data(), width_, height_);
                    plane = resized_.data();
                    planeStride = width_;
                }

                switch (format_) {
                    case OutFormat::Nv12: {
                        // Repack to the packed stride the media type promises.
                        for (uint32_t y = 0; y < height_; ++y) {
                            std::memcpy(dst + static_cast<size_t>(y) * width_,
                                        plane + static_cast<size_t>(y) * planeStride,
                                        width_);
                        }
                        const uint8_t* srcChroma =
                            plane + static_cast<size_t>(planeStride) * height_;
                        BYTE* dstChroma = dst + static_cast<size_t>(width_) * height_;
                        for (uint32_t y = 0; y < height_ / 2; ++y) {
                            std::memcpy(dstChroma + static_cast<size_t>(y) * width_,
                                        srcChroma + static_cast<size_t>(y) * planeStride,
                                        width_);
                        }
                        break;
                    }
                    case OutFormat::Yuy2:
                        Nv12ToYuy2(plane, planeStride, dst, width_, height_);
                        break;
                    case OutFormat::Rgb24:
                        Nv12ToRgb24(plane, planeStride, dst, width_, height_);
                        break;
                }
                haveFrame = true;

                // Keep a copy so a late frame later can be covered by this one.
                if (lastFrame_.size() != outBytes) lastFrame_.resize(outBytes);
                std::memcpy(lastFrame_.data(), dst, outBytes);
            }
        }

        if (!haveFrame) {
            if (producerLive && lastFrame_.size() == outBytes) {
                // Source is alive but late -- redeliver what it last sent. This
                // is the common case during motion, when frames grow and the
                // publisher slips past a frame interval, and it is the whole
                // reason not to reach for the placeholder here.
                std::memcpy(dst, lastFrame_.data(), outBytes);
            } else {
                // Genuinely nothing to show. Keep the cadence anyway: a camera
                // that stops delivering samples looks hung to the application.
                if (slateWidth_ != width_ || slateHeight_ != height_) {
                    DrawSlateNv12(slate_, width_, height_);
                    slateWidth_ = width_;
                    slateHeight_ = height_;
                }
                FillPlaceholder(format_, dst, width_, height_, slate_);
                lastFrame_.clear();
                Sleep(static_cast<DWORD>(frameDuration / 10000));
            }
        }

        sample->SetActualDataLength(static_cast<long>(outBytes));
        sample->SetSyncPoint(TRUE);
        sample->SetDiscontinuity(nextStart == 0);

        REFERENCE_TIME start = nextStart;
        REFERENCE_TIME end = start + frameDuration;
        sample->SetTime(&start, &end);
        nextStart = end;

        hr = memInput_->Receive(sample);
        sample->Release();

        if (FAILED(hr)) break;
    }
}

// ---- filter ----------------------------------------------------------------

XCamFilter::XCamFilter() {
    pin_ = new XCamPin(this);
    ++g_lockCount;
}

XCamFilter::~XCamFilter() {
    if (pin_) {
        pin_->Release();
        pin_ = nullptr;
    }
    if (clock_) clock_->Release();
    --g_lockCount;
}

STDMETHODIMP XCamFilter::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;

    if (riid == IID_IUnknown || riid == IID_IPersist || riid == IID_IMediaFilter ||
        riid == IID_IBaseFilter) {
        *ppv = static_cast<IBaseFilter*>(this);
    } else {
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}

STDMETHODIMP_(ULONG) XCamFilter::AddRef() { return ++refCount_; }

STDMETHODIMP_(ULONG) XCamFilter::Release() {
    const long n = --refCount_;
    if (n == 0) delete this;
    return n;
}

STDMETHODIMP XCamFilter::GetClassID(CLSID* clsid) {
    if (!clsid) return E_POINTER;
    *clsid = CLSID_XCamVirtualCamera;
    return S_OK;
}

STDMETHODIMP XCamFilter::Stop() {
    trace::Write("Stop (state %d)", static_cast<int>(state_));
    if (state_ == State_Stopped) return S_OK;
    pin_->Inactive();
    state_ = State_Stopped;
    return S_OK;
}

STDMETHODIMP XCamFilter::Pause() {
    trace::Write("Pause (state %d)", static_cast<int>(state_));
    if (state_ == State_Stopped) {
        // A source filter starts producing on the transition into Paused: the
        // graph expects a first sample to be available before Run.
        const HRESULT hr = pin_->Active();
        if (FAILED(hr)) return hr;
    }
    state_ = State_Paused;
    return S_OK;
}

STDMETHODIMP XCamFilter::Run(REFERENCE_TIME start) {
    trace::Write("Run (state %d)", static_cast<int>(state_));
    if (state_ == State_Stopped) {
        const HRESULT hr = Pause();
        if (FAILED(hr)) return hr;
    }
    startTime_ = start;
    state_ = State_Running;
    return S_OK;
}

STDMETHODIMP XCamFilter::GetState(DWORD, FILTER_STATE* state) {
    if (!state) return E_POINTER;
    *state = state_;
    return S_OK;
}

STDMETHODIMP XCamFilter::SetSyncSource(IReferenceClock* clock) {
    if (clock) clock->AddRef();
    if (clock_) clock_->Release();
    clock_ = clock;
    return S_OK;
}

STDMETHODIMP XCamFilter::GetSyncSource(IReferenceClock** clock) {
    if (!clock) return E_POINTER;
    *clock = clock_;
    if (clock_) clock_->AddRef();
    return S_OK;
}

STDMETHODIMP XCamFilter::EnumPins(IEnumPins** en) {
    if (!en) return E_POINTER;
    *en = new PinEnumerator(static_cast<IPin*>(pin_));
    return S_OK;
}

STDMETHODIMP XCamFilter::FindPin(LPCWSTR id, IPin** pin) {
    if (!pin) return E_POINTER;
    if (id && wcscmp(id, L"Capture") == 0) {
        *pin = static_cast<IPin*>(pin_);
        pin_->AddRef();
        return S_OK;
    }
    *pin = nullptr;
    return VFW_E_NOT_FOUND;
}

STDMETHODIMP XCamFilter::QueryFilterInfo(FILTER_INFO* info) {
    if (!info) return E_POINTER;
    wcscpy_s(info->achName, name_);
    info->pGraph = graph_;
    if (graph_) graph_->AddRef();
    return S_OK;
}

STDMETHODIMP XCamFilter::JoinFilterGraph(IFilterGraph* graph, LPCWSTR name) {
    // Held weakly by contract: the graph owns the filter, and AddRef'ing it here
    // would make a cycle neither side ever breaks.
    graph_ = graph;
    if (name) wcscpy_s(name_, name);
    return S_OK;
}

STDMETHODIMP XCamFilter::QueryVendorInfo(LPWSTR* vendor) {
    if (!vendor) return E_POINTER;
    const wchar_t text[] = L"XCam";
    *vendor = static_cast<LPWSTR>(CoTaskMemAlloc(sizeof(text)));
    if (!*vendor) return E_OUTOFMEMORY;
    std::memcpy(*vendor, text, sizeof(text));
    return S_OK;
}

// ---- class factory ---------------------------------------------------------

class ClassFactory final : public IClassFactory {
public:
    enum class Kind { Camera, Microphone };

    explicit ClassFactory(Kind kind) : kind_(kind) {}

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IClassFactory) {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++refCount_; }
    STDMETHODIMP_(ULONG) Release() override {
        const long n = --refCount_;
        if (n == 0) delete this;
        return n;
    }

    STDMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;

        trace::Write("CreateInstance: %s",
                     kind_ == Kind::Microphone ? "microphone" : "camera");

        if (kind_ == Kind::Microphone) return audiofilter::CreateInstance(riid, ppv);

        auto* filter = new XCamFilter();
        const HRESULT hr = filter->QueryInterface(riid, ppv);
        filter->Release();
        return hr;
    }

    STDMETHODIMP LockServer(BOOL lock) override {
        if (lock) ++g_lockCount; else --g_lockCount;
        return S_OK;
    }

private:
    Kind kind_;
    std::atomic<long> refCount_{1};
};

// ---- registration ----------------------------------------------------------

namespace {

bool WriteRegistryString(HKEY root, const wchar_t* subkey, const wchar_t* name,
                         const wchar_t* value) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(root, subkey, 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) !=
        ERROR_SUCCESS) {
        return false;
    }
    const LONG result = RegSetValueExW(
        key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value),
        static_cast<DWORD>((wcslen(value) + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}

void DeleteRegistryTree(HKEY root, const wchar_t* subkey) {
    RegDeleteTreeW(root, subkey);
}

}  // namespace

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;

    // The first thing this DLL is ever asked. Traced because its absence is a
    // finding in itself: an application that lists the device but never gets
    // here has decided against it from the registry alone.
    trace::Write("DllGetClassObject");

    // One DLL, two devices. Someone who picks the camera in an application
    // almost always wants the microphone from the same dialog, and two DLLs
    // would mean two registrations to keep in step and two paths to get wrong.
    if (clsid == CLSID_XCamVirtualMicrophone) {
        auto* factory = new ClassFactory(ClassFactory::Kind::Microphone);
        const HRESULT hr = factory->QueryInterface(riid, ppv);
        factory->Release();
        return hr;
    }
    if (clsid != CLSID_XCamVirtualCamera) return CLASS_E_CLASSNOTAVAILABLE;

    auto* factory = new ClassFactory(ClassFactory::Kind::Camera);
    const HRESULT hr = factory->QueryInterface(riid, ppv);
    factory->Release();
    return hr;
}

STDAPI DllCanUnloadNow() {
    // Both devices live here, so the DLL leaves only when neither is in use.
    if (audiofilter::InUse()) return S_FALSE;
    return g_lockCount.load() == 0 ? S_OK : S_FALSE;
}

STDAPI DllRegisterServer() {
    wchar_t path[MAX_PATH] = L"";
    if (!GetModuleFileNameW(g_module, path, MAX_PATH)) return E_FAIL;

    // The COM registration proper.
    std::wstring clsidKey = L"CLSID\\" XCAM_CLSID_STRING;
    if (!WriteRegistryString(HKEY_CLASSES_ROOT, clsidKey.c_str(), nullptr,
                             XCAM_FILTER_NAME)) {
        // Per-user rather than per-machine when not elevated, which is the
        // common case for something a person installs for themselves.
        clsidKey = L"Software\\Classes\\CLSID\\" XCAM_CLSID_STRING;
        if (!WriteRegistryString(HKEY_CURRENT_USER, clsidKey.c_str(), nullptr,
                                 XCAM_FILTER_NAME)) {
            return E_ACCESSDENIED;
        }
        const std::wstring inproc = clsidKey + L"\\InprocServer32";
        WriteRegistryString(HKEY_CURRENT_USER, inproc.c_str(), nullptr, path);
        WriteRegistryString(HKEY_CURRENT_USER, inproc.c_str(), L"ThreadingModel", L"Both");
    } else {
        const std::wstring inproc = clsidKey + L"\\InprocServer32";
        WriteRegistryString(HKEY_CLASSES_ROOT, inproc.c_str(), nullptr, path);
        WriteRegistryString(HKEY_CLASSES_ROOT, inproc.c_str(), L"ThreadingModel", L"Both");
    }

    // And the device-category registration, which is what puts us in the list
    // applications enumerate.
    //
    // IFilterMapper2 writes under HKCR, which is machine-wide and needs
    // elevation. That is a poor trade for something a person installs for
    // themselves, so when it is refused we write the same entries under
    // HKCU\Software\Classes by hand. HKCR is a merged view of the machine and
    // user hives, so the device enumerator finds them either way.
    IFilterMapper2* mapper = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FilterMapper2, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IFilterMapper2, reinterpret_cast<void**>(&mapper));
    if (SUCCEEDED(hr)) {
        REGPINTYPES pinTypes[ARRAYSIZE(kFormats)]{};
        for (int i = 0; i < ARRAYSIZE(kFormats); ++i) {
            pinTypes[i].clsMajorType = &MEDIATYPE_Video;
            pinTypes[i].clsMinorType = kFormats[i].subtype;
        }

        REGFILTERPINS pins{};
        pins.strName = const_cast<LPWSTR>(L"Capture");
        pins.bRendered = FALSE;
        pins.bOutput = TRUE;
        pins.bZero = FALSE;
        pins.bMany = FALSE;
        pins.clsConnectsToFilter = &CLSID_NULL;
        pins.strConnectsToPin = nullptr;
        pins.nMediaTypes = ARRAYSIZE(pinTypes);
        pins.lpMediaType = pinTypes;

        REGFILTER2 filter{};
        filter.dwVersion = 1;
        filter.dwMerit = MERIT_DO_NOT_USE + 1;   // selectable, never auto-connected
        filter.cPins = 1;
        filter.rgPins = &pins;

        IMoniker* moniker = nullptr;
        hr = mapper->RegisterFilter(CLSID_XCamVirtualCamera, XCAM_FILTER_NAME, &moniker,
                                    &CLSID_VideoInputDeviceCategory, nullptr, &filter);
        if (moniker) moniker->Release();
        mapper->Release();

        if (SUCCEEDED(hr)) return audiofilter::Register(path);
    }

    // Per-user fallback. FriendlyName and CLSID are what the enumerator needs to
    // build a moniker; FilterData only guides intelligent connect, which nothing
    // does for a capture device a person picks by name.
    const std::wstring instance =
        L"Software\\Classes\\CLSID\\{860BB310-5D01-11D0-BD3B-00A0C911CE86}"
        L"\\Instance\\" XCAM_CLSID_STRING;

    if (!WriteRegistryString(HKEY_CURRENT_USER, instance.c_str(), L"FriendlyName",
                             XCAM_FILTER_NAME) ||
        !WriteRegistryString(HKEY_CURRENT_USER, instance.c_str(), L"CLSID",
                             XCAM_CLSID_STRING)) {
        return E_ACCESSDENIED;
    }
    return audiofilter::Register(path);
}

STDAPI DllUnregisterServer() {
    IFilterMapper2* mapper = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FilterMapper2, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_IFilterMapper2, reinterpret_cast<void**>(&mapper)))) {
        mapper->UnregisterFilter(&CLSID_VideoInputDeviceCategory, nullptr,
                                 CLSID_XCamVirtualCamera);
        mapper->Release();
    }

    DeleteRegistryTree(HKEY_CLASSES_ROOT, L"CLSID\\" XCAM_CLSID_STRING);
    DeleteRegistryTree(HKEY_CURRENT_USER, L"Software\\Classes\\CLSID\\" XCAM_CLSID_STRING);
    return audiofilter::Unregister();
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void*) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = instance;
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}
