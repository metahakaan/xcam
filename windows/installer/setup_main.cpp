// XCam's installer.
//
// One file, one window, one button. It carries the three binaries inside itself
// as resources, so what somebody downloads is a single .exe rather than a zip
// they have to unpack and a script they have to be talked into trusting.
//
// Why this is hand-written rather than an Inno Setup or WiX script:
//
//   * Nothing else in this project needs a tool that is not already installed
//     to build it, and an installer that only some clones can produce is an
//     installer that goes stale.
//   * A generic wizard would be the first thing anybody sees of an application
//     whose whole point is that it does not look generic.
//   * The work an XCam install actually does -- self-register a COM server in
//     HKCU, drop a shortcut, write an uninstall entry -- is shorter in C++ than
//     the PowerShell that was doing it, which had to declare a delegate type and
//     marshal a function pointer by hand to call DllRegisterServer at all.
//
// No administrator, which is the same decision the old script documented: the
// DirectShow filter is a COM server, and HKCU\Software\Classes serves it to
// every application that enumerates cameras just as well as the machine-wide
// hive does. Nobody has to be walked past a UAC prompt to try a webcam.
//
// The same executable uninstalls. It copies itself into the install directory
// and the uninstall entry points back at that copy with --uninstall, so the
// thing that takes XCam out is the thing that put it in.

#include <windows.h>

#include <d2d1.h>
#include <dwrite.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <objbase.h>

#include <string>
#include <vector>

#include "installer/setup_resource.h"

#pragma comment(lib, "d2d1")
#pragma comment(lib, "dwrite")
#pragma comment(lib, "ole32")
#pragma comment(lib, "oleaut32")
#pragma comment(lib, "shell32")

namespace {

// ---- the same palette the application uses ---------------------------------
//
// Not included from ui/theme.h on purpose: that header is about panels floating
// over a picture, and half of its colours are translucent because of it. An
// installer has no picture underneath, so these are the opaque equivalents.

const D2D1_COLOR_F kInk        = D2D1::ColorF(0x0E1114);
const D2D1_COLOR_F kGraphite   = D2D1::ColorF(0x171B20);
const D2D1_COLOR_F kSurface    = D2D1::ColorF(0x1D242C);
const D2D1_COLOR_F kSurfaceHot = D2D1::ColorF(0x232A32);
const D2D1_COLOR_F kText       = D2D1::ColorF(0xF2F5F7);
const D2D1_COLOR_F kTextDim    = D2D1::ColorF(0x8A949E);
const D2D1_COLOR_F kAccent     = D2D1::ColorF(0xFFB020);
const D2D1_COLOR_F kGood       = D2D1::ColorF(0x5BD6A0);
const D2D1_COLOR_F kWarn       = D2D1::ColorF(0xFF6B5B);
const D2D1_COLOR_F kHairline   = D2D1::ColorF(0x2A3138);

constexpr wchar_t kAppName[]    = L"XCam";
constexpr wchar_t kWindowClass[] = L"XCamSetupWindow";

// What the payload is, and what it is called once it is on disk. The order is
// the order it is written in; the filter is last because registering it is the
// step most likely to fail and the least pleasant to half-do.
struct PayloadFile {
    int resourceId;
    const wchar_t* name;
};

const PayloadFile kPayload[] = {
    {IDR_XCAM_APP,      L"xcam-app.exe"},
    {IDR_XCAM_PROBE,    L"xcam-probe.exe"},
    {IDR_XCAM_DSFILTER, L"xcam-dsfilter.dll"},
};

enum class Stage { Ready, Working, Done, Failed };

struct App {
    HWND window = nullptr;

    ID2D1Factory* d2d = nullptr;
    ID2D1HwndRenderTarget* target = nullptr;
    IDWriteFactory* dwrite = nullptr;

    IDWriteTextFormat* title = nullptr;
    IDWriteTextFormat* body = nullptr;
    IDWriteTextFormat* caption = nullptr;
    IDWriteTextFormat* button = nullptr;

    HICON icon = nullptr;

    Stage stage = Stage::Ready;
    bool uninstalling = false;      // this run takes XCam out rather than putting it in
    bool buttonHot = false;
    bool linkHot = false;

    std::wstring detail;            // the line under the heading
    std::wstring installDir;
};

App g_app;

// ---- paths -----------------------------------------------------------------

std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR wide = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, 0, nullptr, &wide))) return {};
    std::wstring out = wide;
    CoTaskMemFree(wide);
    return out;
}

std::wstring InstallDir() {
    const std::wstring base = KnownFolder(FOLDERID_LocalAppData);
    return base.empty() ? std::wstring() : base + L"\\Programs\\" + kAppName;
}

std::wstring SelfPath() {
    wchar_t path[MAX_PATH] = L"";
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return path;
}

// ---- the payload -----------------------------------------------------------

// Gets an in-use file out of the way so a new one can take its name.
//
// The virtual camera is a DirectShow filter, which means anything that has ever
// enumerated cameras has it loaded -- Chrome and NVIDIA Broadcast do it merely
// by running -- and Windows will not overwrite a module in use. It will happily
// *rename* one, though, and what the registry records is the path, so processes
// still holding the old file carry on with the renamed copy until they unload,
// and the new file answers for everyone who comes after.
//
// Without this, updating XCam meant closing every browser first, which is not
// something anybody should be asked to do to update a webcam.
bool MoveAside(const std::wstring& path) {
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return true;

    // Randomised, because a previous cast-off may itself still be loaded and
    // still be holding its name.
    wchar_t suffix[32];
    swprintf_s(suffix, L".%u.old", GetTickCount());
    const std::wstring aside = path + suffix;

    if (!MoveFileExW(path.c_str(), aside.c_str(), MOVEFILE_REPLACE_EXISTING)) return false;

    // Best effort now, certain at the next boot: deleting it outright fails for
    // exactly the reason it had to be renamed in the first place.
    DeleteFileW(aside.c_str());
    MoveFileExW(aside.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    return true;
}

bool WriteResourceTo(int id, const std::wstring& path, std::wstring& error) {
    HRSRC found = FindResourceW(nullptr, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!found) { error = L"this installer is missing part of its payload"; return false; }

    HGLOBAL loaded = LoadResource(nullptr, found);
    const void* bytes = loaded ? LockResource(loaded) : nullptr;
    const DWORD size = SizeofResource(nullptr, found);
    if (!bytes || size == 0) { error = L"this installer's payload is empty"; return false; }

    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

    // Held by something. Rename it out of the way and take its name.
    if (file == INVALID_HANDLE_VALUE && MoveAside(path)) {
        file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    }

    if (file == INVALID_HANDLE_VALUE) {
        // Naming the file is what lets somebody close it rather than guess.
        error = L"could not write " + path + L" -- is it in use?";
        return false;
    }

    DWORD written = 0;
    const bool ok = WriteFile(file, bytes, size, &written, nullptr) && written == size;
    CloseHandle(file);
    if (!ok) { error = L"could not finish writing " + path; return false; }
    return true;
}

// ---- the COM server --------------------------------------------------------

// Calls DllRegisterServer or DllUnregisterServer directly.
//
// Not through regsvr32: it reports one opaque exit code and swallows the
// HRESULT that says what actually went wrong, which is the difference between
// "the camera did not appear" and a number that can be looked up.
bool SelfRegister(const std::wstring& dll, const char* entry, std::wstring& error) {
    HMODULE module = LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!module) { error = L"could not load the virtual camera"; return false; }

    using SelfRegisterFn = HRESULT(STDAPICALLTYPE*)();
    auto call = reinterpret_cast<SelfRegisterFn>(GetProcAddress(module, entry));
    if (!call) {
        FreeLibrary(module);
        error = L"the virtual camera has no registration entry point";
        return false;
    }

    const HRESULT hr = call();
    FreeLibrary(module);

    if (FAILED(hr)) {
        wchar_t buffer[128];
        swprintf_s(buffer, L"registering the virtual camera failed (0x%08lX)",
                   static_cast<unsigned long>(hr));
        error = buffer;
        return false;
    }
    return true;
}

// ---- the Start Menu and the installed-programs list -------------------------

bool CreateShortcut(const std::wstring& target, const std::wstring& linkPath,
                    const std::wstring& workingDir, const std::wstring& description) {
    IShellLinkW* link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&link)))) {
        return false;
    }

    link->SetPath(target.c_str());
    link->SetWorkingDirectory(workingDir.c_str());
    link->SetDescription(description.c_str());
    link->SetIconLocation(target.c_str(), 0);

    IPersistFile* persist = nullptr;
    bool ok = false;
    if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&persist)))) {
        ok = SUCCEEDED(persist->Save(linkPath.c_str(), TRUE));
        persist->Release();
    }
    link->Release();
    return ok;
}

std::wstring StartMenuLink() {
    const std::wstring programs = KnownFolder(FOLDERID_Programs);
    return programs.empty() ? std::wstring() : programs + L"\\" + kAppName + L".lnk";
}

constexpr wchar_t kUninstallKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\XCam";

void SetRegString(HKEY key, const wchar_t* name, const std::wstring& value) {
    RegSetValueExW(key, name, 0, REG_SZ,
                   reinterpret_cast<const BYTE*>(value.c_str()),
                   static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
}

void SetRegDword(HKEY key, const wchar_t* name, DWORD value) {
    RegSetValueExW(key, name, 0, REG_DWORD,
                   reinterpret_cast<const BYTE*>(&value), sizeof(value));
}

void WriteUninstallEntry(const std::wstring& dir, const std::wstring& uninstaller) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kUninstallKey, 0, nullptr, 0,
                        KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return;
    }

    SetRegString(key, L"DisplayName", kAppName);
    SetRegString(key, L"DisplayVersion", L"0.2.2");
    SetRegString(key, L"Publisher", L"Taha Ünivar");
    SetRegString(key, L"InstallLocation", dir);
    SetRegString(key, L"DisplayIcon", dir + L"\\xcam-app.exe");
    SetRegString(key, L"UninstallString", L"\"" + uninstaller + L"\" --uninstall");
    SetRegDword(key, L"NoModify", 1);
    SetRegDword(key, L"NoRepair", 1);

    RegCloseKey(key);
}

// ---- is it running ---------------------------------------------------------

// Answered by trying to open the file rather than by walking the process list:
// the only thing that actually matters is whether these bytes can be replaced,
// and a stale process handle would say yes when the file says no.
bool AppIsRunning(const std::wstring& dir) {
    const std::wstring exe = dir + L"\\xcam-app.exe";
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) return false;

    HANDLE file = CreateFileW(exe.c_str(), GENERIC_WRITE, 0, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return true;
    CloseHandle(file);
    return false;
}

// ---- the two operations ----------------------------------------------------

bool DoInstall(std::wstring& error) {
    const std::wstring dir = InstallDir();
    if (dir.empty()) { error = L"could not find your application data folder"; return false; }

    if (AppIsRunning(dir)) {
        error = L"XCam is open. Close it and try again.";
        return false;
    }

    // An existing installation is unregistered before its files are replaced.
    // The registry records the DLL's path, and overwriting it underneath a live
    // registration leaves entries pointing at a file that has changed identity.
    const std::wstring dll = dir + L"\\xcam-dsfilter.dll";
    if (GetFileAttributesW(dll.c_str()) != INVALID_FILE_ATTRIBUTES) {
        std::wstring ignored;
        SelfRegister(dll, "DllUnregisterServer", ignored);
    }

    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);

    for (const PayloadFile& file : kPayload) {
        if (!WriteResourceTo(file.resourceId, dir + L"\\" + file.name, error)) return false;
    }

    // The uninstaller is this program, kept where it can be found later.
    const std::wstring uninstaller = dir + L"\\xcam-uninstall.exe";
    if (!CopyFileW(SelfPath().c_str(), uninstaller.c_str(), FALSE)) {
        // Reinstalling while the old uninstaller is open is unusual but not
        // impossible, and it fails the same way everything else here does.
        MoveAside(uninstaller);
        if (!CopyFileW(SelfPath().c_str(), uninstaller.c_str(), FALSE)) {
            error = L"could not put the uninstaller in place";
            return false;
        }
    }

    if (!SelfRegister(dll, "DllRegisterServer", error)) return false;

    const std::wstring link = StartMenuLink();
    if (!link.empty()) {
        CreateShortcut(dir + L"\\xcam-app.exe", link, dir, L"Use a phone as a camera");
    }
    WriteUninstallEntry(dir, uninstaller);
    return true;
}

bool DoUninstall(std::wstring& error) {
    const std::wstring dir = InstallDir();
    if (dir.empty()) { error = L"could not find your application data folder"; return false; }

    if (AppIsRunning(dir)) {
        error = L"XCam is open. Close it and try again.";
        return false;
    }

    const std::wstring dll = dir + L"\\xcam-dsfilter.dll";
    if (GetFileAttributesW(dll.c_str()) != INVALID_FILE_ATTRIBUTES) {
        std::wstring ignored;
        SelfRegister(dll, "DllUnregisterServer", ignored);
    }

    const std::wstring link = StartMenuLink();
    if (!link.empty()) DeleteFileW(link.c_str());

    // The autostart entry goes too. Leaving one behind means a boot that tries
    // to run something that is no longer there.
    HKEY run = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                      0, KEY_SET_VALUE, &run) == ERROR_SUCCESS) {
        RegDeleteValueW(run, kAppName);
        RegCloseKey(run);
    }
    RegDeleteKeyW(HKEY_CURRENT_USER, kUninstallKey);

    for (const PayloadFile& file : kPayload) {
        DeleteFileW((dir + L"\\" + file.name).c_str());
    }

    // The cast-offs an update left behind. Each was renamed out of the way
    // because something still had it loaded, and each was asked to go at the
    // next boot -- but the ask is per-file, so one made by an install that has
    // since been superseded may still be sitting here. Deleting the folder is
    // what actually depends on them.
    WIN32_FIND_DATAW found{};
    HANDLE search = FindFirstFileW((dir + LR"(\*.old)").c_str(), &found);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring castOff = dir + L"\\" + found.cFileName;
            if (!DeleteFileW(castOff.c_str())) {
                MoveFileExW(castOff.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
            }
        } while (FindNextFileW(search, &found));
        FindClose(search);
    }

    // Not the directory itself and not this executable: it is running out of
    // that folder. Windows will not delete a running image, so the copy asks to
    // go at the next boot and the folder goes with it.
    MoveFileExW(SelfPath().c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    RemoveDirectoryW(dir.c_str());
    return true;
}

// ---- drawing ---------------------------------------------------------------

const float kWindowWidth = 520.0f;
const float kWindowHeight = 386.0f;

// Three bands: the mark, what is about to happen, and the one control. The
// footer is measured from the bottom so the button and the path sit on the same
// line without either having to know how tall the prose above it turned out.
const float kMargin      = 40.0f;
const float kHeaderBottom = 108.0f;
const float kFooterTop    = kWindowHeight - 92.0f;

D2D1_RECT_F ButtonRect() {
    return D2D1::RectF(kWindowWidth - kMargin - 176.0f, kFooterTop + 23.0f,
                       kWindowWidth - kMargin, kFooterTop + 23.0f + 46.0f);
}

// Left of the button, on the same line, and stopping well short of it.
D2D1_RECT_F FolderRect() {
    return D2D1::RectF(kMargin, kFooterTop + 30.0f,
                       ButtonRect().left - 20.0f, kFooterTop + 62.0f);
}

ID2D1SolidColorBrush* Brush(const D2D1_COLOR_F& colour) {
    static ID2D1SolidColorBrush* brush = nullptr;
    if (!brush && g_app.target) g_app.target->CreateSolidColorBrush(colour, &brush);
    if (brush) brush->SetColor(colour);
    return brush;
}

// Not DrawText: that name belongs to a Windows macro, and taking it here
// turned every call in this file into a syntax error.
void Label(const std::wstring& text, IDWriteTextFormat* format,
              const D2D1_RECT_F& rect, const D2D1_COLOR_F& colour) {
    if (!format || text.empty()) return;
    g_app.target->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), format, rect,
                            Brush(colour), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

std::wstring Heading() {
    if (g_app.uninstalling) {
        switch (g_app.stage) {
            case Stage::Working: return L"Removing…";
            case Stage::Done:    return L"Removed";
            case Stage::Failed:  return L"Could not remove it";
            default:             return L"Remove XCam?";
        }
    }
    switch (g_app.stage) {
        case Stage::Working: return L"Installing…";
        case Stage::Done:    return L"Installed";
        case Stage::Failed:  return L"That did not work";
        // Not "XCam": the wordmark is already saying that three lines above, and
        // a heading that repeats it is a heading that says nothing.
        default:             return L"Ready to install";
    }
}

std::wstring ButtonLabel() {
    if (g_app.stage == Stage::Done) return L"Close";
    if (g_app.stage == Stage::Failed) return L"Close";
    if (g_app.stage == Stage::Working) return g_app.uninstalling ? L"Removing" : L"Installing";
    return g_app.uninstalling ? L"Remove" : L"Install";
}

void Draw() {
    if (!g_app.target) return;

    g_app.target->BeginDraw();
    g_app.target->Clear(kInk);

    // Bands, so the mark and the button each sit on something rather than
    // floating in the middle of one flat rectangle.
    g_app.target->FillRectangle(D2D1::RectF(0, 0, kWindowWidth, kHeaderBottom), Brush(kGraphite));
    g_app.target->FillRectangle(D2D1::RectF(0, kHeaderBottom - 1.0f, kWindowWidth, kHeaderBottom),
                                Brush(kHairline));

    g_app.target->FillRectangle(D2D1::RectF(0, kFooterTop, kWindowWidth, kWindowHeight),
                                Brush(kGraphite));
    g_app.target->FillRectangle(D2D1::RectF(0, kFooterTop, kWindowWidth, kFooterTop + 1.0f),
                                Brush(kHairline));

    Label(L"XCAM", g_app.title,
          D2D1::RectF(kMargin, 28.0f, kWindowWidth - kMargin, 68.0f), kText);
    Label(L"A phone as your camera", g_app.caption,
          D2D1::RectF(kMargin, 66.0f, kWindowWidth - kMargin, 92.0f), kTextDim);

    const D2D1_COLOR_F headingColour =
        g_app.stage == Stage::Failed ? kWarn
      : g_app.stage == Stage::Done   ? kGood
                                     : kText;
    Label(Heading(), g_app.body,
          D2D1::RectF(kMargin, 136.0f, kWindowWidth - kMargin, 166.0f), headingColour);

    // Stops at the footer, so a long message runs out of room rather than out
    // from under the button.
    Label(g_app.detail, g_app.caption,
          D2D1::RectF(kMargin, 174.0f, kWindowWidth - kMargin, kFooterTop - 12.0f), kTextDim);

    // Where it goes, quietly, because somebody occasionally wants to know and
    // nobody wants to be asked. Written the short way: the full path is most of
    // a home directory's name and would not fit beside the button.
    if (g_app.stage == Stage::Ready && !g_app.uninstalling) {
        Label(LR"(%LOCALAPPDATA%\Programs\XCam)", g_app.caption, FolderRect(), kTextDim);
    }

    // The button. It fills with the accent only here: this is the one thing on
    // the window somebody is meant to press, so the rule about accents never
    // filling a control does not have anything to compete with.
    const D2D1_RECT_F button = ButtonRect();
    const bool live = g_app.stage != Stage::Working;
    const D2D1_ROUNDED_RECT rounded = D2D1::RoundedRect(button, 10.0f, 10.0f);

    D2D1_COLOR_F fill = kSurface;
    D2D1_COLOR_F label = kText;
    if (g_app.stage == Stage::Ready) {
        fill = g_app.buttonHot ? D2D1::ColorF(0xFFC24D) : kAccent;
        label = kInk;
    } else if (g_app.buttonHot && live) {
        fill = kSurfaceHot;
    }

    g_app.target->FillRoundedRectangle(rounded, Brush(fill));
    Label(ButtonLabel(), g_app.button, button, live ? label : kTextDim);

    g_app.target->EndDraw();
}

// ---- the window ------------------------------------------------------------

bool PointIn(const D2D1_RECT_F& rect, int x, int y) {
    const float fx = static_cast<float>(x), fy = static_cast<float>(y);
    return fx >= rect.left && fx <= rect.right && fy >= rect.top && fy <= rect.bottom;
}

void RunOperation() {
    g_app.stage = Stage::Working;
    g_app.detail = g_app.uninstalling
        ? L"Unregistering the camera and removing the files."
        : L"Writing the files and registering the camera. A moment.";
    InvalidateRect(g_app.window, nullptr, FALSE);
    UpdateWindow(g_app.window);

    std::wstring error;
    const bool ok = g_app.uninstalling ? DoUninstall(error) : DoInstall(error);

    if (ok) {
        g_app.stage = Stage::Done;
        g_app.detail = g_app.uninstalling
            ? L"XCam is gone. Your settings and recordings were left alone."
            : L"The camera and microphone are in every application's device "
              L"list now — Discord, Zoom, OBS.\n\nInstall the phone app, plug "
              L"the phone in, and press start on it.";
    } else {
        g_app.stage = Stage::Failed;
        g_app.detail = error;
    }
    InvalidateRect(g_app.window, nullptr, FALSE);
}

LRESULT CALLBACK WndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(window, &ps);
            Draw();
            EndPaint(window, &ps);
            return 0;
        }

        case WM_MOUSEMOVE: {
            const bool wasHot = g_app.buttonHot;
            g_app.buttonHot = PointIn(ButtonRect(), LOWORD(lParam), HIWORD(lParam));
            if (wasHot != g_app.buttonHot) {
                SetCursor(LoadCursorW(nullptr, g_app.buttonHot ? IDC_HAND : IDC_ARROW));
                InvalidateRect(window, nullptr, FALSE);
            }
            return 0;
        }

        case WM_SETCURSOR:
            if (LOWORD(lParam) == HTCLIENT) {
                SetCursor(LoadCursorW(nullptr, g_app.buttonHot ? IDC_HAND : IDC_ARROW));
                return TRUE;
            }
            break;

        case WM_LBUTTONUP: {
            if (!PointIn(ButtonRect(), LOWORD(lParam), HIWORD(lParam))) return 0;
            if (g_app.stage == Stage::Working) return 0;

            if (g_app.stage == Stage::Ready) {
                RunOperation();
            } else {
                // Done or Failed: the button closes, and a successful install
                // opens what it just installed on the way out.
                if (g_app.stage == Stage::Done && !g_app.uninstalling) {
                    const std::wstring exe = InstallDir() + L"\\xcam-app.exe";
                    ShellExecuteW(nullptr, L"open", exe.c_str(), nullptr,
                                  InstallDir().c_str(), SW_SHOWNORMAL);
                }
                PostQuitMessage(0);
            }
            return 0;
        }

        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE && g_app.stage != Stage::Working) PostQuitMessage(0);
            if (wParam == VK_RETURN && g_app.stage == Stage::Ready) RunOperation();
            return 0;

        case WM_CLOSE:
            // Not mid-write. Half an installation is worse than none, and the
            // whole thing takes under a second anyway.
            if (g_app.stage == Stage::Working) return 0;
            PostQuitMessage(0);
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

bool CreateGraphics() {
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_app.d2d))) return false;

    RECT client{};
    GetClientRect(g_app.window, &client);
    if (FAILED(g_app.d2d->CreateHwndRenderTarget(
            D2D1::RenderTargetProperties(),
            D2D1::HwndRenderTargetProperties(
                g_app.window,
                D2D1::SizeU(client.right - client.left, client.bottom - client.top)),
            &g_app.target))) {
        return false;
    }

    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(&g_app.dwrite)))) {
        return false;
    }

    // Segoe UI Variable is what Windows 11 draws with; Segoe UI keeps this
    // looking right on 10.
    auto make = [](float size, DWRITE_FONT_WEIGHT weight, IDWriteTextFormat** out,
                   DWRITE_TEXT_ALIGNMENT align, bool wrap) {
        if (FAILED(g_app.dwrite->CreateTextFormat(L"Segoe UI Variable Display", nullptr, weight,
                                                  DWRITE_FONT_STYLE_NORMAL,
                                                  DWRITE_FONT_STRETCH_NORMAL, size, L"en-us",
                                                  out))) {
            g_app.dwrite->CreateTextFormat(L"Segoe UI", nullptr, weight,
                                           DWRITE_FONT_STYLE_NORMAL,
                                           DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", out);
        }
        if (*out) {
            (*out)->SetTextAlignment(align);
            (*out)->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP
                                         : DWRITE_WORD_WRAPPING_NO_WRAP);
            if (!wrap) (*out)->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
    };

    make(26.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, &g_app.title, DWRITE_TEXT_ALIGNMENT_LEADING, false);
    make(19.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, &g_app.body, DWRITE_TEXT_ALIGNMENT_LEADING, false);
    make(13.5f, DWRITE_FONT_WEIGHT_NORMAL, &g_app.caption, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    make(15.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, &g_app.button, DWRITE_TEXT_ALIGNMENT_CENTER, false);

    return true;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    g_app.uninstalling = commandLine && wcsstr(commandLine, L"--uninstall") != nullptr;
    g_app.installDir = InstallDir();
    g_app.detail = g_app.uninstalling
        ? L"The virtual camera will be unregistered and the files removed. Your "
          L"settings and recordings stay where they are."
        : L"Installs the desktop app and the virtual camera for your account "
          L"only, so there is no administrator prompt.\n\nDiscord, Zoom and OBS "
          L"will see XCam as an ordinary webcam.";

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.lpszClassName = kWindowClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_XCAM_SETUP));
    wc.hIconSm = wc.hIcon;
    RegisterClassExW(&wc);

    g_app.icon = wc.hIcon;

    // Sized in pixels at the monitor's scale, so the window is the same size on
    // a 150% display as the numbers in this file say it is.
    const UINT dpi = GetDpiForSystem();
    const int width = MulDiv(static_cast<int>(kWindowWidth), dpi, 96);
    const int height = MulDiv(static_cast<int>(kWindowHeight), dpi, 96);

    RECT frame{0, 0, width, height};
    AdjustWindowRectExForDpi(&frame, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                             FALSE, 0, dpi);

    g_app.window = CreateWindowExW(
        0, kWindowClass, g_app.uninstalling ? L"Remove XCam" : L"Install XCam",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT,
        frame.right - frame.left, frame.bottom - frame.top,
        nullptr, nullptr, instance, nullptr);
    if (!g_app.window) return 1;

    if (!CreateGraphics()) {
        MessageBoxW(nullptr, L"This machine could not start Direct2D, which XCam's "
                             L"installer draws with.", kAppName, MB_ICONERROR);
        return 1;
    }

    // Drawn at the monitor's scale rather than at 96 dpi and stretched.
    g_app.target->SetDpi(static_cast<float>(dpi), static_cast<float>(dpi));

    ShowWindow(g_app.window, SW_SHOW);
    UpdateWindow(g_app.window);

    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    CoUninitialize();
    return 0;
}
