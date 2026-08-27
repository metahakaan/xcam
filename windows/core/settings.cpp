#include "core/settings.h"

#include <windows.h>
#include <shlobj.h>

#include <cstdio>
#include <cstdlib>

namespace xcam {
namespace {

std::string LocalAppDataDir() {
    PWSTR wide = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &wide))) return {};

    char narrow[MAX_PATH * 2] = "";
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, narrow, sizeof(narrow), nullptr, nullptr);
    CoTaskMemFree(wide);
    return narrow;
}

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                      nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::string Trim(const std::string& s) {
    size_t begin = 0;
    size_t end = s.size();
    while (begin < end && (s[begin] == ' ' || s[begin] == '\t')) ++begin;
    while (end > begin && (s[end - 1] == ' ' || s[end - 1] == '\t' ||
                           s[end - 1] == '\r' || s[end - 1] == '\n')) --end;
    return s.substr(begin, end - begin);
}

}  // namespace

std::string Settings::Path() {
    const std::string base = LocalAppDataDir();
    if (base.empty()) return {};
    return base + "\\XCam\\settings.txt";
}

void Settings::Load() {
    values_.clear();
    dirty_ = false;

    const std::string path = Path();
    if (path.empty()) return;

    // The path can contain anything the user's account name contains, so it has
    // to be opened wide: a narrow fopen would go through the ANSI codepage and
    // fail on exactly the accounts most likely to have one.
    FILE* file = nullptr;
    if (_wfopen_s(&file, Widen(path).c_str(), L"rb") != 0 || !file) return;

    char line[1024];
    while (std::fgets(line, sizeof(line), file)) {
        const std::string text = Trim(line);
        if (text.empty() || text[0] == '#') continue;

        const size_t equals = text.find('=');
        if (equals == std::string::npos) continue;

        const std::string key = Trim(text.substr(0, equals));
        if (!key.empty()) values_[key] = Trim(text.substr(equals + 1));
    }
    std::fclose(file);
}

void Settings::Save() const {
    const std::string path = Path();
    if (path.empty()) return;

    const size_t slash = path.find_last_of('\\');
    if (slash != std::string::npos) {
        SHCreateDirectoryExW(nullptr, Widen(path.substr(0, slash)).c_str(), nullptr);
    }

    FILE* file = nullptr;
    if (_wfopen_s(&file, Widen(path).c_str(), L"wb") != 0 || !file) return;

    std::fprintf(file, "# XCam settings. Delete this file to start fresh.\n");
    for (const auto& [key, value] : values_) {
        std::fprintf(file, "%s=%s\n", key.c_str(), value.c_str());
    }
    std::fclose(file);
    dirty_ = false;
}

std::string Settings::GetString(const std::string& key, const std::string& fallback) const {
    const auto it = values_.find(key);
    return it == values_.end() ? fallback : it->second;
}

int Settings::GetInt(const std::string& key, int fallback) const {
    const auto it = values_.find(key);
    if (it == values_.end() || it->second.empty()) return fallback;
    return std::atoi(it->second.c_str());
}

float Settings::GetFloat(const std::string& key, float fallback) const {
    const auto it = values_.find(key);
    if (it == values_.end() || it->second.empty()) return fallback;
    return static_cast<float>(std::atof(it->second.c_str()));
}

bool Settings::GetBool(const std::string& key, bool fallback) const {
    const auto it = values_.find(key);
    if (it == values_.end()) return fallback;
    return it->second == "1" || it->second == "true" || it->second == "yes";
}

void Settings::Set(const std::string& key, const std::string& value) {
    auto& slot = values_[key];
    if (slot != value) {
        slot = value;
        dirty_ = true;
    }
}

void Settings::Set(const std::string& key, const char* value) {
    Set(key, std::string(value ? value : ""));
}

void Settings::Set(const std::string& key, int value) {
    Set(key, std::to_string(value));
}

void Settings::Set(const std::string& key, float value) {
    char buffer[32];
    // Six digits is more than any of these values means; the alternative prints
    // 0.30000001 for a number the user typed as 0.3.
    std::snprintf(buffer, sizeof(buffer), "%.6g", value);
    Set(key, std::string(buffer));
}

void Settings::Set(const std::string& key, bool value) {
    Set(key, std::string(value ? "1" : "0"));
}

}  // namespace xcam
