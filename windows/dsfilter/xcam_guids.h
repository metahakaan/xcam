#pragma once

// Fixed identity for the virtual camera.
//
// These must never change once anything has registered them: they are what
// lives in HKCR\CLSID and what every application that has ever picked "XCam
// Virtual Camera" remembers it by. Regenerating them would orphan the old
// registration and leave a dead device in every app's camera list.

#include <guiddef.h>

// {8CACBF3B-D1C2-488A-B8DB-3A6CB3E9905A}
DEFINE_GUID(CLSID_XCamVirtualCamera,
            0x8cacbf3b, 0xd1c2, 0x488a, 0xb8, 0xdb, 0x3a, 0x6c, 0xb3, 0xe9, 0x90, 0x5a);

// {D52CDAA8-132E-4362-8859-93DC7317552C}
DEFINE_GUID(CLSID_XCamOutputPin,
            0xd52cdaa8, 0x132e, 0x4362, 0x88, 0x59, 0x93, 0xdc, 0x73, 0x17, 0x55, 0x2c);

#define XCAM_FILTER_NAME L"XCam Virtual Camera"
#define XCAM_CLSID_STRING L"{8CACBF3B-D1C2-488A-B8DB-3A6CB3E9905A}"

// {B4E7A1D6-3F52-4C88-9A0E-7D61C2F84B13}
DEFINE_GUID(CLSID_XCamVirtualMicrophone,
            0xb4e7a1d6, 0x3f52, 0x4c88, 0x9a, 0x0e, 0x7d, 0x61, 0xc2, 0xf8, 0x4b, 0x13);

// {5C1F9E24-8A73-4D06-B2E5-1F48C7A960DD}
DEFINE_GUID(CLSID_XCamAudioOutputPin,
            0x5c1f9e24, 0x8a73, 0x4d06, 0xb2, 0xe5, 0x1f, 0x48, 0xc7, 0xa9, 0x60, 0xdd);

#define XCAM_AUDIO_FILTER_NAME L"XCam Virtual Microphone"
#define XCAM_AUDIO_CLSID_STRING L"{B4E7A1D6-3F52-4C88-9A0E-7D61C2F84B13}"
