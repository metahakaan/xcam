#include "app/take_sidecar.h"

#include <windows.h>

#include <cstdio>
#include <ctime>

namespace xcam {
namespace {

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                      nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

// The name without its extension, which is what both files are named after.
std::string StemOf(const std::string& name) {
    const size_t dot = name.find_last_of('.');
    return dot == std::string::npos ? name : name.substr(0, dot);
}

// JSON has no opinion about backslashes and neither does Windows, but a reader
// does: a path written raw comes back with its separators eaten.
std::string Escaped(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (char c : text) {
        if (c == '\\' || c == '"') out += '\\';
        out += c;
    }
    return out;
}

FILE* OpenBeside(const std::string& directory, const std::string& stem,
                 const char* extension, std::string& error) {
    const std::string path =
        (directory.empty() ? stem : directory + "\\" + stem) + extension;

    FILE* file = nullptr;
    // Opened wide: the path contains whatever the account name contains, and a
    // narrow fopen goes through the ANSI codepage and fails on exactly the
    // accounts most likely to have one.
    if (_wfopen_s(&file, Widen(path).c_str(), L"wb") != 0 || !file) {
        error = "could not write " + path;
        return nullptr;
    }
    return file;
}

std::string Timecode(int64_t ms, int fps) {
    if (fps <= 0) fps = 25;
    const int64_t frames = ms * fps / 1000;
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%02lld:%02lld:%02lld:%02lld",
                  static_cast<long long>(frames / (3600LL * fps)),
                  static_cast<long long>((frames / (60LL * fps)) % 60),
                  static_cast<long long>((frames / fps) % 60),
                  static_cast<long long>(frames % fps));
    return buffer;
}

}  // namespace

bool WriteTakeSidecar(const std::string& directory, const std::string& takeName,
                      const CameraModel& model, const AppModel& shared,
                      double startedAtUnix, const std::string& proxyPath,
                      std::string& error) {
    FILE* file = OpenBeside(directory, StemOf(takeName), ".xcam.json", error);
    if (!file) return false;

    char started[32] = "";
    const std::time_t when = static_cast<std::time_t>(startedAtUnix);
    std::tm local{};
    localtime_s(&local, &when);
    std::strftime(started, sizeof(started), "%Y-%m-%dT%H:%M:%S", &local);

    const CameraInfo* camera = model.Camera();

    std::fprintf(file, "{\n");
    std::fprintf(file, "  \"take\": \"%s\",\n", Escaped(takeName).c_str());
    std::fprintf(file, "  \"started\": \"%s\",\n", started);

    // Which file is the good one, said plainly. Everything else here is a
    // number; this is the one line somebody actually has to act on.
    std::fprintf(file, "  \"master\": \"%s\",\n",
                 model.recordToPc ? "this file" : "on the phone");
    if (!proxyPath.empty()) {
        std::fprintf(file, "  \"proxy\": \"%s\",\n", Escaped(proxyPath).c_str());
    }

    std::fprintf(file, "  \"format\": { \"width\": %d, \"height\": %d, \"fps\": %d,"
                       " \"codec\": \"%s\", \"bitrate\": %d },\n",
                 model.recordWidth, model.recordHeight, model.recordFps,
                 model.recordCodec.c_str(), model.bitrate);

    std::fprintf(file, "  \"camera\": { \"id\": \"%s\", \"label\": \"%s\","
                       " \"zoom\": %.3f },\n",
                 camera ? Escaped(camera->id).c_str() : "",
                 camera ? Escaped(camera->label).c_str() : "",
                 model.zoom);

    // Shutter as a fraction, because that is how it was chosen and how anyone
    // matching a shot would ask for it again.
    const double shutterSeconds = model.shutterNs / 1e9;
    std::fprintf(file, "  \"exposure\": { \"mode\": \"%s\", \"iso\": %d,"
                       " \"shutter\": \"1/%.0f\", \"ev\": %.2f },\n",
                 model.exposureMode == ExposureMode::Manual ? "manual" : "auto",
                 model.iso, shutterSeconds > 0 ? 1.0 / shutterSeconds : 0.0, model.ev);

    std::fprintf(file, "  \"focus\": { \"mode\": \"%s\", \"dioptres\": %.3f },\n",
                 model.focusMode == FocusMode::Manual ? "manual" : "auto",
                 model.focusDistance);

    std::fprintf(file, "  \"whiteBalance\": { \"mode\": \"%s\", \"kelvin\": %d },\n",
                 Escaped(model.wbMode).c_str(), model.wbKelvin);

    // The look. A file that says which LUT was on the panel is the difference
    // between reproducing a grade and guessing at it.
    std::fprintf(file, "  \"look\": { \"logProfile\": %s, \"lut\": \"%s\","
                       " \"matte\": %.2f, \"mirrored\": %s }\n",
                 model.logProfile ? "true" : "false",
                 Escaped(shared.lutPath).c_str(), shared.matte,
                 model.flipX ? "true" : "false");
    std::fprintf(file, "}\n");

    std::fclose(file);
    error.clear();
    return true;
}

bool WriteTakeEdl(const std::string& directory, const std::string& takeName,
                  const CameraModel& model, int64_t durationMs, std::string& error) {
    FILE* file = OpenBeside(directory, StemOf(takeName), ".edl", error);
    if (!file) return false;

    const int fps = model.recordFps > 0 ? model.recordFps : 25;
    const std::string end = Timecode(durationMs, fps);

    std::fprintf(file, "TITLE: %s\n", StemOf(takeName).c_str());
    std::fprintf(file, "FCM: NON-DROP FRAME\n\n");
    std::fprintf(file, "001  AX       AA/V  C        ");
    std::fprintf(file, "00:00:00:00 %s 00:00:00:00 %s\n", end.c_str(), end.c_str());

    // The name the editor has to relink to, on its own line. Every editor that
    // reads an EDL reads this comment; none of them agree on anything richer.
    std::fprintf(file, "* FROM CLIP NAME: %s\n", takeName.c_str());
    if (!model.recordToPc) {
        std::fprintf(file, "* SOURCE FILE IS ON THE PHONE: %s\n",
                     model.device.recordDir.c_str());
    }

    std::fclose(file);
    error.clear();
    return true;
}

}  // namespace xcam
