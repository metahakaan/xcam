#pragma once

// A diagnostic log for the desktop side.
//
// This exists because the failures worth chasing here are the ones that only
// show up on real hardware, minutes in, and leave nothing behind: a USB reset, a
// pipeline restart, a decoder refusing a stream. Reproducing those to watch them
// is expensive, so the app records what it did as it goes.
//
// Writes to %LOCALAPPDATA%\XCam\xcam.log, truncated at startup, and mirrors to
// the debugger. Safe to call from any thread.

#include <string>

namespace xcam {

enum class LogLevel { Debug, Info, Warn, Error };

// Opens the log file. Called once at startup; safe to skip, in which case
// logging silently does nothing rather than failing the app.
void LogOpen(const std::string& appName = "XCam");
void LogClose();

// Full path of the current log file, for the UI to show and for the collector
// script to pick up. Empty when logging is not open.
std::string LogPath();

void LogWrite(LogLevel level, const char* format, ...);

// Records the exception and the faulting address to the log before the process
// dies. A crash that leaves nothing behind is the hardest kind to chase on
// someone else's machine, and this costs one call at startup.
void LogInstallCrashHandler();

#define XCAM_LOG_DEBUG(...) ::xcam::LogWrite(::xcam::LogLevel::Debug, __VA_ARGS__)
#define XCAM_LOG_INFO(...)  ::xcam::LogWrite(::xcam::LogLevel::Info,  __VA_ARGS__)
#define XCAM_LOG_WARN(...)  ::xcam::LogWrite(::xcam::LogLevel::Warn,  __VA_ARGS__)
#define XCAM_LOG_ERROR(...) ::xcam::LogWrite(::xcam::LogLevel::Error, __VA_ARGS__)

}  // namespace xcam
