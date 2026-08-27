#pragma once

// What the application remembers between runs.
//
// Deliberately a flat `key=value` file rather than JSON. The handshake needs a
// JSON reader because the phone decides that format; this file is ours on both
// ends, and a format that a person can read and edit in Notepad is worth more
// here than one that nests. It also cannot half-parse: an unreadable line is
// skipped and the default stands, which is the right failure for settings.
//
// Lives in %LOCALAPPDATA%\XCam\settings.txt.

#include <map>
#include <string>

namespace xcam {

class Settings {
public:
    // Missing file, unreadable file and empty file are all the same thing: use
    // the defaults. None of them is worth refusing to start over.
    void Load();

    // Best effort. Failing to save settings must never take the application
    // down with it, so this reports nothing.
    void Save() const;

    // Where the file lives, for the log and for anyone who wants to delete it.
    static std::string Path();

    std::string GetString(const std::string& key, const std::string& fallback = "") const;
    int GetInt(const std::string& key, int fallback = 0) const;
    float GetFloat(const std::string& key, float fallback = 0.0f) const;
    bool GetBool(const std::string& key, bool fallback = false) const;

    void Set(const std::string& key, const std::string& value);

    // Without this, a string literal picks the bool overload: pointer-to-bool
    // is a standard conversion and beats the user-defined one to std::string,
    // so Set("net.transport", "wifi") silently stored "1".
    void Set(const std::string& key, const char* value);
    void Set(const std::string& key, int value);
    void Set(const std::string& key, float value);
    void Set(const std::string& key, bool value);

    bool Has(const std::string& key) const { return values_.count(key) != 0; }

    // True when anything has changed since the last Save, so a caller can write
    // on a timer without rewriting an unchanged file every time.
    bool Dirty() const { return dirty_; }

private:
    std::map<std::string, std::string> values_;
    mutable bool dirty_ = false;
};

}  // namespace xcam
