#include "core/adb.h"

#include <windows.h>

#include <cstdlib>
#include <filesystem>
#include <sstream>

namespace xcam {
namespace {

namespace fs = std::filesystem;

std::string EnvVar(const char* name) {
    char* value = nullptr;
    size_t len = 0;
    if (_dupenv_s(&value, &len, name) != 0 || value == nullptr) return {};
    std::string out(value);
    std::free(value);
    return out;
}

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                      nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

// Quotes an argument for CreateProcess. Paths here routinely contain spaces.
std::wstring QuoteArg(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos) return arg;
    std::wstring out = L"\"";
    size_t backslashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') { ++backslashes; continue; }
        if (c == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out += L'"';
        } else {
            out.append(backslashes, L'\\');
            out += c;
        }
        backslashes = 0;
    }
    out.append(backslashes * 2, L'\\');
    out += L'"';
    return out;
}

std::vector<std::string> SplitLines(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream is(text);
    std::string line;
    while (std::getline(is, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
    }
    return lines;
}

}  // namespace

bool RunAdb(const std::string& adbPath, const std::vector<std::string>& args,
            std::string& output, int timeoutMs) {
    output.clear();
    if (adbPath.empty()) return false;

    std::wstring cmdline = QuoteArg(Widen(adbPath));
    for (const auto& arg : args) cmdline += L' ' + QuoteArg(Widen(arg));

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE readPipe = nullptr, writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) return false;
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;      // adb must never flash a console at the user
    si.hStdOutput = writePipe;
    si.hStdError = writePipe;

    PROCESS_INFORMATION pi{};
    std::wstring mutableCmd = cmdline;
    const BOOL ok = CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(writePipe);
    if (!ok) {
        CloseHandle(readPipe);
        return false;
    }

    char buffer[4096];
    DWORD read = 0;
    while (ReadFile(readPipe, buffer, sizeof(buffer), &read, nullptr) && read > 0) {
        output.append(buffer, read);
    }
    CloseHandle(readPipe);

    const DWORD waited = WaitForSingleObject(pi.hProcess, static_cast<DWORD>(timeoutMs));
    DWORD exitCode = 1;
    if (waited == WAIT_OBJECT_0) {
        GetExitCodeProcess(pi.hProcess, &exitCode);
    } else {
        TerminateProcess(pi.hProcess, 1);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return exitCode == 0;
}

std::string FindAdb() {
    std::vector<fs::path> candidates;

    for (const char* var : {"ANDROID_HOME", "ANDROID_SDK_ROOT"}) {
        const std::string root = EnvVar(var);
        if (!root.empty()) candidates.emplace_back(fs::path(root) / "platform-tools" / "adb.exe");
    }
    const std::string localAppData = EnvVar("LOCALAPPDATA");
    if (!localAppData.empty()) {
        candidates.emplace_back(fs::path(localAppData) / "Android" / "Sdk" /
                                "platform-tools" / "adb.exe");
    }

    for (const auto& candidate : candidates) {
        std::error_code ec;
        if (fs::exists(candidate, ec)) return candidate.string();
    }

    // Fall back to PATH.
    wchar_t found[MAX_PATH];
    if (SearchPathW(nullptr, L"adb.exe", nullptr, MAX_PATH, found, nullptr) > 0) {
        const int n = WideCharToMultiByte(CP_UTF8, 0, found, -1, nullptr, 0, nullptr, nullptr);
        std::string out(static_cast<size_t>(n - 1), '\0');
        WideCharToMultiByte(CP_UTF8, 0, found, -1, out.data(), n, nullptr, nullptr);
        return out;
    }
    return {};
}

std::vector<AdbDevice> ListDevices(const std::string& adbPath) {
    std::vector<AdbDevice> devices;
    std::string output;
    if (!RunAdb(adbPath, {"devices", "-l"}, output)) return devices;

    for (const std::string& line : SplitLines(output)) {
        if (line.empty() || line.rfind("List of devices", 0) == 0) continue;
        if (line.rfind("*", 0) == 0) continue;      // daemon start chatter

        std::istringstream is(line);
        AdbDevice device;
        if (!(is >> device.serial >> device.state)) continue;

        std::string token;
        while (is >> token) {
            if (token.rfind("model:", 0) == 0) device.model = token.substr(6);
        }
        devices.push_back(std::move(device));
    }
    return devices;
}

bool EnsureForward(const std::string& adbPath, uint16_t port, const std::string& serial) {
    const std::string spec = "tcp:" + std::to_string(port);

    std::string listing;
    if (RunAdb(adbPath, {"forward", "--list"}, listing)) {
        if (listing.find(spec + " " + spec) != std::string::npos) return true;
    }

    std::vector<std::string> args;
    if (!serial.empty()) { args.push_back("-s"); args.push_back(serial); }
    args.insert(args.end(), {"forward", spec, spec});

    std::string output;
    return RunAdb(adbPath, args, output);
}

bool RemoveForward(const std::string& adbPath, uint16_t port, const std::string& serial) {
    std::vector<std::string> args;
    if (!serial.empty()) { args.push_back("-s"); args.push_back(serial); }
    args.insert(args.end(), {"forward", "--remove", "tcp:" + std::to_string(port)});

    std::string output;
    return RunAdb(adbPath, args, output);
}

bool LaunchApp(const std::string& adbPath, const std::string& serial) {
    std::vector<std::string> args;
    if (!serial.empty()) { args.push_back("-s"); args.push_back(serial); }
    args.insert(args.end(), {"shell", "am", "start", "-n",
                             "com.xcam/.MainActivity", "--ez", "autostart", "true"});

    std::string output;
    return RunAdb(adbPath, args, output);
}

}  // namespace xcam
