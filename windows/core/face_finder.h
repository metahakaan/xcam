#pragma once

// Where the people are in the picture.
//
// Uses the face detector that ships with Windows -- Windows.Media.FaceAnalysis
// -- rather than a model of our own. That is the whole reason auto-framing is
// affordable here: no weights to bundle, no runtime to link, nothing to keep up
// to date, and it takes the NV12 this project already has in hand.
//
// It is not a tracker and makes no promises between calls. Smoothing what it
// returns is the caller's job, and doing that badly is the difference between a
// camera operator and a servo.

#include <cstdint>
#include <string>
#include <vector>

namespace xcam {

// A face, in fractions of the frame.
struct FaceBox {
    float x = 0, y = 0, w = 0, h = 0;
};

class FaceFinder {
public:
    FaceFinder();
    ~FaceFinder();

    FaceFinder(const FaceFinder&) = delete;
    FaceFinder& operator=(const FaceFinder&) = delete;

    // Fails on a machine without the component, which is a real configuration
    // rather than an error -- the caller turns the feature off and says so.
    bool Open();
    void Close();
    bool IsOpen() const { return detector_ != nullptr; }

    // `nv12` is a packed frame: `stride` bytes per luma row, chroma following.
    // Only the luma is read; the detector wants grey and this saves a
    // conversion of a plane it would throw away.
    bool Detect(const uint8_t* nv12, uint32_t stride, uint32_t width, uint32_t height,
                std::vector<FaceBox>& out);

    const std::string& LastError() const { return lastError_; }

private:
    void* detector_ = nullptr;        // FaceDetector, held type-erased
    std::string lastError_;
};

}  // namespace xcam
