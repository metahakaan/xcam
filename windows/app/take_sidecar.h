#pragma once

// What a take was, written down beside it.
//
// The phone keeps the master and the desktop may keep a proxy; between them
// they hold the picture but not one word about how it was made. An editor
// opening the file a week later can see the resolution and nothing else -- not
// the ISO, not the shutter, not which LUT was on the panel, not whether the log
// profile was engaged, and not which of the two files is the good one.
//
// So each take gets a sidecar: a small JSON file with the same stem, holding
// everything that would otherwise have to be remembered. It is the "which look"
// half of conforming; the EDL below is the "which file" half.

#include "app/camera_model.h"

#include <string>

namespace xcam {

// Writes `<stem>.xcam.json` into `directory`. Returns false and leaves the
// reason in `error` -- a take that recorded fine but could not be described is
// worth a line in the log, not a dialog.
// `shared` carries the half of the look that is not the camera's -- the LUT and
// the matte are the application's, and a sidecar that omitted them would
// describe the exposure of a shot while saying nothing about how it was graded.
bool WriteTakeSidecar(const std::string& directory, const std::string& takeName,
                      const CameraModel& model, const AppModel& shared,
                      double startedAtUnix, const std::string& proxyPath,
                      std::string& error);

// Writes a CMX 3600 edit decision list naming the phone's master, with the
// desktop's proxy as the offline reference when there is one.
//
// An EDL rather than FCPXML: every editor reads one, it is a hundred lines of
// code rather than a schema, and what has to cross is a file name and a
// duration. Anything richer is a look, and the look is in the sidecar.
bool WriteTakeEdl(const std::string& directory, const std::string& takeName,
                  const CameraModel& model, int64_t durationMs, std::string& error);

}  // namespace xcam
