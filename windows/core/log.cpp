#include "core/log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <share.h>
#include <filesystem>
#include <mutex>
#include <string>

namespace xcam {
namespace {

std::mutex g_lock;
FILE* g_file = nullptr;
std::string g_path;

const char* LevelName(LogLevel level) {
    switch (level) {
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO ";
        case LogLevel::Warn:  return "WARN ";
        case LogLevel::Error: return "ERROR";
    }
    return "?????";
}

// Wall-clock rather than a monotonic counter: the point of these lines is to be
// lined up against the phone's logcat, which timestamps the same way.
std::string Timestamp() {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    char buffer[32];
    sprintf_s(buffer, "%02d-%02d %02d:%02d:%02d.%03d",
              now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);
    return buffer;
}

}  // namespace

void LogOpen(const std::string& appName) {
    std::lock_guard<std::mutex> guard(g_lock);
    if (g_file) return;

    char* localAppData = nullptr;
    size_t length = 0;
    if (_dupenv_s(&localAppData, &length, "LOCALAPPDATA") != 0 || !localAppData) return;

    std::error_code ec;
    const std::filesystem::path directory = std::filesystem::path(localAppData) / appName;
    std::free(localAppData);

    std::filesystem::create_directories(directory, ec);
    if (ec) return;

    // Truncated per run: a session's worth of context is what matters, and
    // older runs are noise once the current one has reproduced the problem.
    //
    // _fsopen rather than fopen so the file stays readable while the app holds
    // it. A log you cannot tail until the program exits is no use for watching
    // a problem happen.
    //
    // And a sibling when the usual name will not open. Anything can be holding
    // it -- another copy of the application, a backup agent, an editor, a file
    // left delete-pending by something that has already gone -- and the old
    // behaviour was to give up without a word, so a whole session ran blind and
    // the first anyone knew of it was a log that stopped at the previous run.
    // Losing the diagnosis is a far worse outcome than an oddly named file.
    for (int attempt = 0; attempt < 10 && !g_file; ++attempt) {
        const std::string name =
            attempt == 0 ? "xcam.log" : "xcam-" + std::to_string(attempt) + ".log";
        const std::filesystem::path path = directory / name;
        g_file = _fsopen(path.string().c_str(), "w", _SH_DENYWR);
        if (g_file) g_path = path.string();
    }
    if (!g_file) return;

}

void LogClose() {
    std::lock_guard<std::mutex> guard(g_lock);
    if (g_file) {
        std::fclose(g_file);
        g_file = nullptr;
    }
}

std::string LogPath() {
    std::lock_guard<std::mutex> guard(g_lock);
    return g_path;
}

namespace {

LONG WINAPI CrashHandler(EXCEPTION_POINTERS* info) {
    const EXCEPTION_RECORD* record = info && info->ExceptionRecord
        ? info->ExceptionRecord : nullptr;

    if (record) {
        // The module and offset are what turn an address into a line number
        // later, via the .pdb sitting next to the binary.
        HMODULE module = nullptr;
        char moduleName[MAX_PATH] = "?";
        uintptr_t offset = 0;

        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               static_cast<LPCSTR>(record->ExceptionAddress), &module)) {
            GetModuleFileNameA(module, moduleName, MAX_PATH);
            offset = reinterpret_cast<uintptr_t>(record->ExceptionAddress) -
                     reinterpret_cast<uintptr_t>(module);
        }

        LogWrite(LogLevel::Error, "CRASH: code 0x%08x at %p (%s+0x%llx)",
                 record->ExceptionCode, record->ExceptionAddress, moduleName,
                 static_cast<unsigned long long>(offset));

        if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
            record->NumberParameters >= 2) {
            LogWrite(LogLevel::Error, "CRASH: %s address 0x%llx",
                     record->ExceptionInformation[0] ? "wrote" : "read",
                     static_cast<unsigned long long>(record->ExceptionInformation[1]));
        }
    }

    LogClose();
    return EXCEPTION_CONTINUE_SEARCH;   // let Windows still produce its own report
}

}  // namespace

void LogInstallCrashHandler() {
    SetUnhandledExceptionFilter(CrashHandler);
}

void LogWrite(LogLevel level, const char* format, ...) {
    char message[1024];

    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    char line[1200];
    sprintf_s(line, "%s %s %s\n", Timestamp().c_str(), LevelName(level), message);

    OutputDebugStringA(line);

    std::lock_guard<std::mutex> guard(g_lock);
    if (!g_file) return;

    std::fputs(line, g_file);
    // Flushed per line rather than buffered. MSVC treats _IOLBF as full
    // buffering, so without this the file stays empty until the buffer fills --
    // and a log you cannot read while the problem is happening is the wrong
    // shape for the job. At a few lines a second the cost does not register.
    std::fflush(g_file);
}

}  // namespace xcam
