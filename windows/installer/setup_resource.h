#pragma once

// Ids for the installer's own resources. Kept apart from the application's
// (app/xcam_resource.h) because the two executables have no resources in
// common: this one carries the other one inside it.

#define IDI_XCAM_SETUP     1

// The payload. RCDATA, written to disk byte for byte.
#define IDR_XCAM_APP       100
#define IDR_XCAM_PROBE     101
#define IDR_XCAM_DSFILTER  102
