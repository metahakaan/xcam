#include "app/lut.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace xcam {
namespace {

// The largest cube worth accepting. 65 is the biggest size in common use and
// already 1.1M entries; anything past that is more likely a malformed file than
// a real LUT.
constexpr int kMaxSize = 65;

// std::ifstream given a narrow path interprets it in the ANSI code page, not
// UTF-8, so anything outside it simply fails to open. This project's own
// directory is called "Yeni klasör (2)", which is exactly the case that breaks.
std::filesystem::path WidenPath(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                                      nullptr, 0);
    std::wstring wide(static_cast<size_t>(n), wchar_t{});
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                        wide.data(), n);
    return std::filesystem::path(wide);
}

std::string Trim(const std::string& s) {
    const size_t begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return {};
    const size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(begin, end - begin + 1);
}

}  // namespace

bool LoadCubeLut(const std::string& path, CubeLut& out, std::string& error) {
    std::ifstream file(WidenPath(path));
    if (!file) {
        error = "could not open " + path;
        return false;
    }

    out = CubeLut{};
    std::vector<float> triples;
    triples.reserve(33 * 33 * 33 * 3);

    std::string line;
    int lineNumber = 0;

    while (std::getline(file, line)) {
        ++lineNumber;
        line = Trim(line);
        if (line.empty() || line[0] == '#') continue;

        std::istringstream in(line);
        std::string keyword;
        in >> keyword;

        if (keyword == "TITLE") {
            std::getline(in, out.title);
            out.title = Trim(out.title);
            if (out.title.size() >= 2 && out.title.front() == '"' && out.title.back() == '"') {
                out.title = out.title.substr(1, out.title.size() - 2);
            }
            continue;
        }
        if (keyword == "LUT_3D_SIZE") {
            in >> out.size;
            if (out.size < 2 || out.size > kMaxSize) {
                error = "unsupported LUT_3D_SIZE " + std::to_string(out.size);
                return false;
            }
            continue;
        }
        if (keyword == "LUT_1D_SIZE") {
            error = "this is a 1D LUT; XCam applies 3D cubes only";
            return false;
        }
        if (keyword == "DOMAIN_MIN") {
            in >> out.domainMin[0] >> out.domainMin[1] >> out.domainMin[2];
            continue;
        }
        if (keyword == "DOMAIN_MAX") {
            in >> out.domainMax[0] >> out.domainMax[1] >> out.domainMax[2];
            continue;
        }

        // Anything else on a line of its own should be three floats.
        float r = 0, g = 0, b = 0;
        std::istringstream values(line);
        if (!(values >> r >> g >> b)) {
            error = "unexpected content on line " + std::to_string(lineNumber);
            return false;
        }
        triples.push_back(r);
        triples.push_back(g);
        triples.push_back(b);
    }

    if (out.size < 2) {
        error = "no LUT_3D_SIZE found";
        return false;
    }

    const size_t expected = static_cast<size_t>(out.size) * out.size * out.size * 3;
    if (triples.size() != expected) {
        error = "expected " + std::to_string(expected / 3) + " entries, found " +
                std::to_string(triples.size() / 3);
        return false;
    }

    // .cube stores red fastest, which is already the order a D3D volume texture
    // wants, so this is a straight widen to RGBA.
    out.rgba.resize(expected / 3 * 4);
    for (size_t i = 0, j = 0; i < triples.size(); i += 3, j += 4) {
        out.rgba[j + 0] = triples[i + 0];
        out.rgba[j + 1] = triples[i + 1];
        out.rgba[j + 2] = triples[i + 2];
        out.rgba[j + 3] = 1.0f;
    }
    return true;
}

}  // namespace xcam
