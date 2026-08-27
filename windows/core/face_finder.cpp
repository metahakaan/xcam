#include "core/face_finder.h"

// Before the projection headers, which is what C++/WinRT asks for when a
// classic COM interface has to be declared alongside it.
#include <unknwn.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.FaceAnalysis.h>

#include <cstring>

// Declared here rather than included.
//
// The interface that hands out the bytes behind a memory buffer lives in an
// interop header that pulls in the whole WRL surface; this is the whole of it,
// and the uuid is its contract.
struct __declspec(uuid("5b0d3235-4dba-4d44-865e-8f1d0e4fd04d")) __declspec(novtable)
IMemoryBufferByteAccess : ::IUnknown {
    virtual HRESULT __stdcall GetBuffer(uint8_t** value, uint32_t* capacity) = 0;
};

namespace xcam {
namespace {

using namespace winrt::Windows::Graphics::Imaging;
using namespace winrt::Windows::Media::FaceAnalysis;

// A WinRT type cannot be allocated with new -- it deletes the operator, on
// purpose -- so the one long-lived object goes in a plain struct that can be.
struct Held {
    FaceDetector detector{nullptr};
};

Held* AsHeld(void* p) { return static_cast<Held*>(p); }

}  // namespace

FaceFinder::FaceFinder() = default;

FaceFinder::~FaceFinder() { Close(); }

bool FaceFinder::Open() {
    Close();
    try {
        // Apartment-threaded initialisation is the caller's business; this only
        // needs the runtime to be up, which everything else in this process has
        // already seen to.
        FaceDetector detector = FaceDetector::CreateAsync().get();
        if (!detector) {
            lastError_ = "the face detector is not available on this system";
            return false;
        }

        // Grey8 is what it wants and NV12's luma plane already is. Asking for a
        // format it does not support would cost a conversion per frame to reach
        // exactly the same pixels.
        if (!FaceDetector::IsBitmapPixelFormatSupported(BitmapPixelFormat::Gray8)) {
            lastError_ = "the face detector will not take an 8-bit grey frame";
            return false;
        }

        detector_ = new Held{std::move(detector)};
        lastError_.clear();
        return true;
    } catch (const winrt::hresult_error& error) {
        lastError_ = winrt::to_string(error.message());
        return false;
    } catch (...) {
        lastError_ = "the face detector could not be created";
        return false;
    }
}

void FaceFinder::Close() {
    if (!detector_) return;
    delete AsHeld(detector_);
    detector_ = nullptr;
}

bool FaceFinder::Detect(const uint8_t* nv12, uint32_t stride, uint32_t width,
                        uint32_t height, std::vector<FaceBox>& out) {
    out.clear();
    if (!detector_ || !nv12 || width == 0 || height == 0) return false;

    try {
        SoftwareBitmap grey(BitmapPixelFormat::Gray8, static_cast<int32_t>(width),
                            static_cast<int32_t>(height));
        {
            BitmapBuffer buffer = grey.LockBuffer(BitmapBufferAccessMode::Write);
            auto reference = buffer.CreateReference();

            uint8_t* data = nullptr;
            uint32_t capacity = 0;
            reference.as<IMemoryBufferByteAccess>()->GetBuffer(&data, &capacity);
            if (!data) return false;

            const BitmapPlaneDescription plane = buffer.GetPlaneDescription(0);
            for (uint32_t row = 0; row < height; ++row) {
                std::memcpy(data + plane.StartIndex + plane.Stride * row,
                            nv12 + static_cast<size_t>(stride) * row, width);
            }
        }

        const auto faces = AsHeld(detector_)->detector.DetectFacesAsync(grey).get();
        const float fw = static_cast<float>(width);
        const float fh = static_cast<float>(height);
        for (const auto& face : faces) {
            const BitmapBounds box = face.FaceBox();
            out.push_back(FaceBox{box.X / fw, box.Y / fh, box.Width / fw, box.Height / fh});
        }
        return true;
    } catch (const winrt::hresult_error& error) {
        lastError_ = winrt::to_string(error.message());
        return false;
    } catch (...) {
        lastError_ = "face detection failed";
        return false;
    }
}

}  // namespace xcam
