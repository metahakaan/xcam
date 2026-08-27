#pragma once

#include <string>
#include <vector>

namespace xcam {

struct AdbDevice {
    std::string serial;
    std::string model;
    std::string state;      // "device", "unauthorized", "offline"

    bool IsUsable() const { return state == "device"; }
};

// Locates adb.exe: ANDROID_HOME, then ANDROID_SDK_ROOT, then the default SDK
// path under %LOCALAPPDATA%, then PATH. Empty if nothing was found.
std::string FindAdb();

std::vector<AdbDevice> ListDevices(const std::string& adbPath);

// Sets up localhost:port -> phone:port, skipping the call if the tunnel already
// exists. Worth doing on every connect: the phone silently discards its
// forwards whenever it re-enumerates on USB, and the only symptom is a refused
// connection on localhost, which points at entirely the wrong thing.
bool EnsureForward(const std::string& adbPath, uint16_t port,
                   const std::string& serial = {});

bool RemoveForward(const std::string& adbPath, uint16_t port,
                   const std::string& serial = {});

// Brings the capture app up without anyone touching the phone. The service is
// unexported, so this goes through the activity, which honours an autostart
// extra for exactly this purpose.
bool LaunchApp(const std::string& adbPath, const std::string& serial = {});

// Runs adb and captures stdout. Exposed because the app needs ad-hoc queries
// (battery, thermal) that do not deserve their own wrappers.
bool RunAdb(const std::string& adbPath, const std::vector<std::string>& args,
            std::string& output, int timeoutMs = 15000);

}  // namespace xcam
