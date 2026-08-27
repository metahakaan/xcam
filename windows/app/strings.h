#pragma once

// Turkish and English for the desktop panel.
//
// The key is the English text itself rather than an enum. With this many
// strings an enum would be a second thing to keep in step for no gain, and it
// makes every call site unreadable -- `T("Output folder")` says what will be on
// screen; `T(kStrOutputFolder)` sends you to a header to find out. A missing
// translation falls back to the key, so an untranslated string is still a
// correct string.
//
// Android has its own res/values-tr, which is the right mechanism there.

#include <string>

namespace xcam {

enum class Lang { English, Turkish };

class Strings {
public:
    // Defaults to the language Windows is running in: someone on a Turkish
    // system should not have to find a setting to be spoken to in Turkish.
    static void Init();

    static void Set(Lang lang);
    static Lang Current();

    // The name of a language in that language, which is how a language picker
    // has to read -- "Türkçe" is findable by someone who cannot read the rest
    // of the interface.
    static const char* Name(Lang lang);

    static const char* Get(const char* english);
};

// Short because it wraps every visible string in the panel.
inline const char* T(const char* english) { return Strings::Get(english); }

}  // namespace xcam
