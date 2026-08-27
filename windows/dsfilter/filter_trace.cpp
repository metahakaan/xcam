#include "dsfilter/filter_trace.h"

#include <windows.h>
#include <shlobj.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>

namespace xcam::trace {
namespace {

std::mutex g_lock;

std::string XCamDirectory() {
    PWSTR wide = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &wide))) return {};
    char narrow[MAX_PATH * 2] = "";
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, narrow, sizeof(narrow), nullptr, nullptr);
    CoTaskMemFree(wide);
    return std::string(narrow) + "\\XCam";
}

std::string ProcessName() {
    wchar_t path[MAX_PATH] = L"";
    GetModuleFileNameW(nullptr, path, MAX_PATH);

    const wchar_t* leaf = wcsrchr(path, L'\\');
    leaf = leaf ? leaf + 1 : path;

    char narrow[MAX_PATH] = "";
    WideCharToMultiByte(CP_UTF8, 0, leaf, -1, narrow, sizeof(narrow), nullptr, nullptr);
    return narrow;
}

}  // namespace

bool Enabled() {
    // Resolved once. This is called from the hot path of every negotiation, and
    // in the common case the answer is no.
    static const bool enabled = [] {
        const std::string dir = XCamDirectory();
        if (dir.empty()) return false;
        const std::string flag = dir + "\\filter-trace-on";
        return GetFileAttributesA(flag.c_str()) != INVALID_FILE_ATTRIBUTES;
    }();
    return enabled;
}

void Write(const char* format, ...) {
    static const std::string path = XCamDirectory() + "\\filter.log";
    static const std::string process = ProcessName();

    char message[512];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    SYSTEMTIME now{};
    GetLocalTime(&now);

    // The debug channel first, and ungated.
    //
    // This filter is loaded into whatever process wants a camera, and some of
    // those are sandboxed: a Chromium capture process may be unable to read the
    // flag file that turns tracing on, let alone write the log, so a file-only
    // trace reports nothing about exactly the applications hardest to diagnose
    // -- which is not the same as nothing having happened. OutputDebugString
    // needs no filesystem access at all. It costs one call into the kernel, goes
    // nowhere unless something is listening, and nothing here is on a per-frame
    // path. `xcam-probe --watch-debug` listens.
    {
        char line[640];
        snprintf(line, sizeof(line), "[XCam] %-18s %s\n", process.c_str(), message);
        OutputDebugStringA(line);
    }

    if (!Enabled()) return;

    std::lock_guard<std::mutex> guard(g_lock);
    FILE* file = nullptr;
    // Shared append: several applications may hold this device at once, and one
    // of them locking the others out of the log would hide exactly the case
    // worth seeing.
    if (fopen_s(&file, path.c_str(), "a") != 0 || !file) return;
    std::fprintf(file, "%02d:%02d:%02d.%03d  %-18s %s\n",
                 now.wHour, now.wMinute, now.wSecond, now.wMilliseconds,
                 process.c_str(), message);
    std::fclose(file);
}

}  // namespace xcam::trace
