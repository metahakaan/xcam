// The one place our GUIDs are defined rather than merely declared.
//
// DEFINE_GUID emits a definition only when INITGUID is in scope, and INITGUID
// also makes every standard GUID in uuids.h a definition -- which collides with
// strmiids.lib the moment anything else includes dshow.h. Keeping INITGUID to
// this file alone gives us our two symbols and leaves the rest to the SDK.

#include <windows.h>
#include <initguid.h>

#include "dsfilter/xcam_guids.h"
