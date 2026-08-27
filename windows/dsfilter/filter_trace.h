#pragma once

// A record of what applications actually ask this device for.
//
// The filter runs inside somebody else's process -- Discord's, Zoom's,
// Chrome's -- where there is no console, no debugger and no way to ask it a
// question. When one of them says "could not start the camera", the only thing
// that can settle why is what it asked for and what it was told, and that has
// to be written down at the time.
//
// Appends to %LOCALAPPDATA%\XCam\filter.log, tagged with the process that made
// the call, so several applications can be compared side by side. Appends
// rather than truncates for the same reason: the interesting run is usually the
// one before the one you are watching.
//
// Off unless the file %LOCALAPPDATA%\XCam\filter-trace-on exists. Writing a
// line per negotiation into every process that ever enumerates a camera is not
// something to leave on by default.

namespace xcam::trace {

// Cheap and safe from any thread. Does nothing at all when tracing is off.
void Write(const char* format, ...);

// Whether the switch file is present. Checked once.
bool Enabled();

}  // namespace xcam::trace
