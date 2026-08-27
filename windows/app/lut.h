#pragma once

// Cube LUT loading.
//
// A log profile is only half the job: the picture it produces is flat by design
// and looks wrong until something puts contrast and colour back. Loading a .cube
// here means the preview -- and later the virtual camera -- shows the graded
// image while the stream underneath stays gradeable.

#include <cstdint>
#include <string>
#include <vector>

namespace xcam {

// A parsed 3D LUT, ready to become a D3D11 volume texture. Values are RGBA
// float, in the order D3D wants: x fastest, then y, then z.
struct CubeLut {
    int size = 0;                       // per-axis, typically 17, 33 or 65
    std::vector<float> rgba;            // size^3 * 4
    std::string title;
    float domainMin[3] = {0, 0, 0};
    float domainMax[3] = {1, 1, 1};

    bool IsValid() const {
        return size >= 2 && rgba.size() == static_cast<size_t>(size) * size * size * 4;
    }
};

// Reads an Adobe .cube file. 1D LUTs are rejected rather than approximated:
// silently applying a curve where a cube was expected would be worse than
// saying no.
bool LoadCubeLut(const std::string& path, CubeLut& out, std::string& error);

}  // namespace xcam
