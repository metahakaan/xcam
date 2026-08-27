#pragma once

// The microphone half of the filter DLL.
//
// One DLL carries both devices because they are two halves of one thing: a
// person who picks "XCam Virtual Camera" in an application almost always wants
// "XCam Virtual Microphone" in the same dialog, and two DLLs would mean two
// registrations to keep in step and two paths to get wrong.
//
// Kept behind this small interface so the camera's translation unit stays about
// the camera. It owns the DLL entry points and calls in here for the audio
// CLSID.

#include <windows.h>

namespace xcam::audiofilter {

// Creates the microphone filter, for DllGetClassObject.
HRESULT CreateInstance(REFIID riid, void** ppv);

// Whether anything still holds a microphone object, for DllCanUnloadNow.
bool InUse();

// Registration, called from DllRegisterServer/DllUnregisterServer with the
// path already resolved. Both follow the same machine-wide-then-per-user
// fallback as the camera.
HRESULT Register(const wchar_t* modulePath);
HRESULT Unregister();

}  // namespace xcam::audiofilter
