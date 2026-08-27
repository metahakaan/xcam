# XCam Wire Protocol v5

Single bidirectional TCP connection. **All integers little-endian.**

The phone listens on `27183` on every interface. Over USB the PC reaches it
through `adb forward tcp:27183 tcp:27183`; over Wi-Fi it connects to the phone's
address directly, and adb is not involved at all. Nothing else differs between
the two — which is why the transport is one line in this document rather than a
section.

---

## 0. Discovery

Over Wi-Fi the client has to learn the phone's address before any of the below
can happen. The phone broadcasts a datagram twice a second to
**UDP 255.255.255.255:27184** for as long as capture is running:

```json
{ "xcam": 1, "name": "Xiaomi 17 Pro", "port": 27183, "version": 4 }
```

`xcam` is the marker a listener checks before parsing anything else, so a stray
datagram on that port costs one string compare. A device that has not been heard
from for five seconds should be dropped rather than offered as a destination that
will not answer.

This is a broadcast rather than mDNS deliberately. mDNS is the conventional
answer and Android speaks it through `NsdManager`, but the client end means a
callback-driven resolver — a great deal of machinery on both sides to learn one
address that fits in forty bytes.

Discovery is only for Wi-Fi. Over USB the client built the tunnel itself and
already knows where the phone is.

---

## 1. Handshake

Sent by the **phone** immediately after accepting a connection.

### Who is allowed to connect

The phone binds its listener to **loopback** unless somebody has switched
"Allow Wi-Fi connections" on in the phone app. This is the whole of the access
control and it is deliberately not a filter: a socket bound to 127.0.0.1 cannot
be reached from another machine as a matter of kernel routing, so there is no
code path on the phone that a mistake could open. USB is unaffected, because
`adb forward` connects to the phone's own loopback.

Before this existed the listener bound to every interface the moment the
service started, with no authentication of any kind. Anyone on the same network
could take the camera, the microphone and the recordings folder -- and because a
new connection displaces the old one, take them *from* whoever was using them.

When Wi-Fi connections are allowed, a client that did **not** arrive over
loopback is answered with a challenge instead of a description:

```json
{"pairing":"required"}
```

It carries nothing else -- not the device name, not the camera list. The client
answers with one CONTROL packet:

```json
{"cmd":"pair","code":"306266"}
```

Six digits, shown in the phone app. On a match the phone sends the real
handshake below and the session proceeds as normal. On a mismatch, or after
fifteen seconds, the phone closes the socket **without saying which it was**:
a wrong code and no code look identical from outside. The comparison is
constant-time, because six digits is a small enough space that a timing side
channel would turn a million guesses into sixty.

A client arriving over loopback is never challenged. Reaching the phone's
loopback already means holding the cable, which is a stronger proof than a code.

```
offset size  field
0      4     magic      "XCAM" (0x58 0x43 0x41 0x4D)
4      2     version    u16, currently 6
6      2     flags      u16, reserved (0)
8      4     jsonLen    u32
12     N     json       UTF-8, jsonLen bytes
```

`json` payload. Every capability field is reported honestly, including when the
device does not support it: a control the hardware will silently ignore must be
shown as unavailable rather than offered and quietly dropped.

```json
{
  "deviceName": "Xiaomi 17 Pro",
  "androidApi": 36,
  "cameras": [
    {
      "id": "0",
      "facing": "back",
      "label": "Main",
      "maxRes": [4096, 3072],
      "maxFps": 60,
      "modes": [ { "size": [1920, 1080], "maxFps": 60 },
                 { "size": [1280, 720],  "maxFps": 60 } ],

      "zoomRange": [0.6, 20.0],
      "hasTorch": true,
      "logical": true,
      "physicalIds": ["3", "4"],

      "manualSensor": true,
      "manualPostProcessing": true,
      "isoRange": [50, 6400],
      "exposureRangeNs": [1000, 500000000],
      "minFocusDistance": 10.0,
      "afModes": ["auto", "continuous", "manual"],
      "awbModes": ["auto", "incandescent", "fluorescent", "daylight", "cloudy", "shade"],
      "evRange": [-4.0, 4.0],
      "evStep": 0.1666
    }
  ],
  "codecs": ["h264", "hevc"],
  "maxBitrate": 200000000,

  "resumed": false,
  "recorder": true,
  "recordDir": "/storage/emulated/0/Android/data/com.xcam/files/recordings",

  "audio": { "available": true, "codec": "aac", "sampleRate": 48000,
             "channels": 2, "bitrate": 192000 },

  "wifiAddress": "192.168.1.10"
}
```

| field | meaning |
|---|---|
| `modes` | the sizes worth offering, each with the frame rate it can sustain. A size without its frame rate is not actionable, so the two always travel together. |
| `manualSensor` | `REQUEST_AVAILABLE_CAPABILITIES` contains `MANUAL_SENSOR`. False means `exposure` with `mode: manual` will be refused. |
| `manualPostProcessing` | contains `MANUAL_POST_PROCESSING`. Required for manual white balance; presets work without it. |
| `isoRange` | `SENSOR_INFO_SENSITIVITY_RANGE`. |
| `exposureRangeNs` | `SENSOR_INFO_EXPOSURE_TIME_RANGE`, nanoseconds. |
| `minFocusDistance` | `LENS_INFO_MINIMUM_FOCUS_DISTANCE` in dioptres. `0` means fixed focus, so manual focus is unavailable. |
| `evStep` | `CONTROL_AE_COMPENSATION_STEP` as a float, so the client can snap to real stops. |
| `resumed` | this connection joined a session that was already running. See below. |
| `recorder` | the phone can write a full-quality file locally while streaming. False means `record` will be refused. |
| `recordDir` | where those files land. Reported rather than assumed, because the client's only way to retrieve one is `adb pull` of an absolute path. |
| `audio.available` | the microphone permission is actually held. False means the phone streams and records silently, and a client should say so rather than offer a control the user has already declined. |
| `wifiAddress` | where a client on the same network can reach this phone. Absent when there is no usable IPv4 address. Reported because it is the one thing the desktop cannot work out for itself. |

The PC MUST NOT send anything before it has fully read the handshake.

---

### Resuming a session

The phone does not tear its pipeline down the moment a client vanishes. It
lingers for five seconds, and a client that connects within that window is
handed `"resumed": true`.

This exists for one case: a USB cable pulled out mid-call. The socket dies
instantly and the same desktop is usually back over Wi-Fi within half a second.
Rebuilding the camera session and both encoders in between would make that a
restart -- seconds of black, a new timestamp base, and every manual camera
setting back to the template -- for a link that was never really gone.

A client that sees `resumed` **must not send `set`**. That command rebuilds the
capture session, which is precisely what the hand-over exists to avoid; sending
it would undo the thing that just worked. There is nothing to configure, because
everything asked for is still running.

What the phone does on a resume is the two things the encoders only say once
plus one they can be asked for again: it re-sends the video and audio codec
configuration and requests a key frame. Without those the new socket would carry
mid-GOP frames referencing pictures that went out over the dead one.

The cost is a few seconds of camera and encoder after a genuine disconnect,
which is battery nobody asked to spend. The alternative is paying a full restart
every time a cable moves.

## 2. Packet framing

Everything after the handshake, in both directions, is a sequence of packets.
Header is **24 bytes**, followed by `len` bytes of payload.

```
offset size  field
0      4     magic   u32  = 0x4D414358  ("XCAM" read as LE u32)
4      1     type    u8   see table
5      1     flags   u8   bit0 = last fragment (always 1 in v1)
                          bit1 = codec config (AUDIO and RECORD)
                          bit2 = key frame (RECORD only)
6      2     _resv   u16  must be 0
8      4     len     u32  payload length in bytes
12     8     ptsUs   u64  presentation timestamp, microseconds, monotonic
20     4     seq     u32  frame counter, wraps; used for loss detection
```

### Packet types

| type | name       | direction   | payload                                        |
|------|------------|-------------|------------------------------------------------|
| 1    | `CONFIG`   | phone → PC  | codec-specific data, Annex-B                   |
| 2    | `KEYFRAME` | phone → PC  | Annex-B NAL units, contains IDR                |
| 3    | `DELTA`    | phone → PC  | Annex-B NAL units, no IDR                      |
| 4    | `CONTROL`  | PC → phone  | UTF-8 JSON                                     |
| 5    | `STATS`    | phone → PC  | UTF-8 JSON                                     |
| 6    | `AUDIO`    | phone → PC  | AAC-LC access units, or the AudioSpecificConfig |
| 7    | `ACK`      | phone → PC  | UTF-8 JSON, response to a CONTROL              |
| 8    | `RECORD`   | phone → PC  | the recording encoder's elementary stream       |
| 9    | `FILE`     | phone → PC  | a chunk of a take being fetched off the phone   |

### CONFIG

- H.264: `SPS` + `PPS`, each prefixed with `00 00 00 01`.
- HEVC: `VPS` + `SPS` + `PPS`, same prefixing.

Sent once when the encoder starts and again after every reconfiguration
(resolution / codec / camera change). The PC MUST reset its decoder on each
`CONFIG` and MUST NOT decode video packets before receiving one.

### KEYFRAME / DELTA

Raw Annex-B elementary stream for exactly one access unit. `seq` increments by
one per encoded frame; a gap means the phone dropped frames under backpressure.

`ptsUs` originates from the camera timestamp, normalised so the first frame of a
**client session** is 0 -- not the first frame of a pipeline run. A `set` that
restarts capture must not restart the clock: a receiver measuring delay against
it would see the clock jump backwards and read the entire session's runtime as
latency.

`seq` does restart at 0 with each encoder session, and a `CONFIG` is the signal
that it has. A receiver must reset its gap detection there, or the counter
returning to zero looks like a burst of lost frames.

### RECORD

The second encoder's output, when the recording is being written on the PC
rather than on the phone. Same Annex-B elementary stream that would otherwise
have gone into a local MP4, at the same resolution and bitrate — the file is not
made smaller to fit the link.

`flags` bit1 marks the codec configuration, which arrives once per take and
must precede everything else. Bit2 marks a key frame, which the client needs
because it is doing the muxing and an MP4 has to know its sync samples.

`ptsUs` shares the origin of `KEYFRAME`/`DELTA` and `AUDIO`, so the client can
mux picture and sound onto one timeline without knowing anything about how
either was captured.

**This is not a second copy of the live stream.** It is a separate encode at a
separate size, and both cross the link at once: 60 Mbit/s of webcam alongside
120 Mbit/s of recording. USB carries that; Wi-Fi does not, which is why the
phone can still write the file itself.

### AUDIO

AAC-LC, 48kHz stereo by default, one access unit per packet with no ADTS header.

The first AUDIO packet of a session carries the **AudioSpecificConfig** and has
`flags` bit1 set. Nothing can be decoded before it. It gets a flag rather than a
packet type of its own because a video `CONFIG` means two things -- here is the
codec data, *and* reset your decoder -- and audio needs only the first.

`ptsUs` shares the video's origin, so the two line up without the receiver
knowing anything about how either was captured. Audio timestamps are derived
from the sample count rather than read off the clock per buffer, so the timeline
is exactly even; and they are taken from whichever clock the camera itself uses
(`SENSOR_INFO_TIMESTAMP_SOURCE`), since the monotonic and boot-time clocks differ
by however long the device has been suspended.

Audio is never shed under backpressure. A dropped frame of it is an audible
click, where a dropped video frame is a moment of staleness nobody notices, and
at a few hundred bytes it costs almost nothing to protect.

---

## 3. CONTROL commands (PC → phone)

One JSON object per packet. `seq` in the header is an opaque request id echoed
back in the matching `ACK`.

```json
{ "cmd": "set", "camera": "0", "width": 1920, "height": 1080,
  "fps": 60, "bitrate": 60000000, "codec": "h264" }
```
Reconfigures the pipeline. Any field may be omitted to keep its current value.
Always answered by a fresh `CONFIG` packet followed by a `KEYFRAME`.

**A `set` rebuilds the capture request from the camera's template defaults**, so
manual exposure, focus, white balance and the picture profile are all discarded.
A client that has set any of them must re-send them after each `CONFIG`, or the
same settings on its own screen will produce a visibly different picture.

A `set` also **ends any recording in progress**, since the recorder's surface
belongs to the capture session being torn down. The file is closed properly, not
truncated, but the take stops there.

| cmd        | fields                              | effect                                  |
|------------|-------------------------------------|-----------------------------------------|
| `set`      | see above                           | reconfigure capture + encoder           |
| `idr`      | —                                   | request an immediate key frame          |
| `bitrate`  | `value` (bps)                       | retarget the encoder without restarting — see below |
| `zoom`     | `ratio` (float)                     | `CONTROL_ZOOM_RATIO`                    |
| `torch`    | `on` (bool)                         | flash LED                               |
| `ev`       | `value` (float, EV steps)           | exposure compensation, auto mode only   |
| `exposure` | `mode`, `iso`, `shutterNs`          | manual or auto exposure — see below     |
| `focus`    | `mode`, `distance`, `x`, `y`        | focus mode, manual distance, tap point  |
| `wb`       | `mode`, `temperature`               | white balance preset or manual Kelvin   |
| `profile`  | `mode`: `standard`\|`log`           | flat, gradeable picture profile         |
| `record`   | `action`, format fields             | local full-quality capture — see below  |
| `audio`    | `enabled` (bool)                    | microphone on or off                    |
| `stop`     | —                                   | tear down the session, keep socket open |

### bitrate

```json
{ "cmd": "bitrate", "value": 45000000 }
```

Retargets the encoder in place. No restart, no `CONFIG`, no key frame: the next
frame is simply smaller.

This exists mainly for closed-loop control. Over USB the chosen rate is carried
without complaint and the right adaptation is none; over Wi-Fi it usually is not,
and the failure mode without adaptation is not a softer picture but a growing
queue — the phone sheds frames, latency climbs, and the stream comes apart while
every number still says the bitrate was applied.

The two signals worth reacting to are `droppedFrames` in `STATS` and the client's
own queueing delay. Both mean the link is behind rather than the encoder.

### exposure

```json
{ "cmd": "exposure", "mode": "manual", "iso": 400, "shutterNs": 8333333 }
{ "cmd": "exposure", "mode": "auto" }
```

Manual mode turns `CONTROL_AE_MODE` off and drives `SENSOR_SENSITIVITY` and
`SENSOR_EXPOSURE_TIME` directly.

**The phone clamps `shutterNs` to the frame interval.** An exposure longer than
`1/fps` cannot be delivered at that frame rate, and a camera asked for one
silently halves its output instead of refusing. The `ACK` therefore reports what
was actually applied, and a client that requested longer should show the clamp
rather than its own value.

Refused with `ok: false` when the camera does not report `manualSensor`.

### focus

```json
{ "cmd": "focus", "mode": "manual", "distance": 4.5 }
{ "cmd": "focus", "mode": "tap", "x": 0.5, "y": 0.5 }
{ "cmd": "focus", "mode": "continuous" }
```

`distance` is in dioptres — 0 is infinity, `minFocusDistance` is as close as the
lens goes. `mode` may be `auto`, `continuous`, `manual` or `tap`; `tap` takes
`x` and `y` normalised to [0,1] over the frame. Omitting `mode` is treated as
`continuous`, which is what v1 clients sent.

### profile

```json
{ "cmd": "profile", "mode": "log" }
```

Switches the camera to a flat picture profile meant to be graded rather than
used as it comes: a Cineon-style tonemap curve that keeps highlight and shadow
detail the default curve discards, a desaturating colour matrix, and sharpening
and noise reduction turned down, since both bake in decisions a grade cannot
undo.

Camera2 has no log mode, so this is assembled from `TONEMAP_MODE_CONTRAST_CURVE`
and `COLOR_CORRECTION_TRANSFORM`. Reported per camera as `logProfile`, and
refused with `ok: false` where the tonemap mode is unavailable.

### record

The stream and the recording are two different things, deliberately. What the PC
receives is compressed for a live link and sized for a webcam; what the phone
writes is the picture the sensor actually produced. Asking one encode to serve
both means either a stream too heavy to arrive on time or a file too compressed
to keep.

So the camera feeds two encoders. The streaming one is unchanged. The second
writes an MP4 on the phone at its own resolution, frame rate, codec and bitrate,
and the file is collected afterwards with `adb pull` — `tools/pull-recordings.ps1`
does this.

```json
{ "cmd": "record", "action": "config", "enabled": true, "target": "pc",
  "width": 3840, "height": 2160, "fps": 30, "bitrate": 120000000,
  "codec": "hevc", "preroll": 0 }
{ "cmd": "record", "action": "config", "enabled": false }
{ "cmd": "record", "action": "start" }
{ "cmd": "record", "action": "stop" }
```

`target` decides where the file is written:

- `pc` — the encoded frames cross the link as `RECORD` packets and the client
  muxes them, together with the `AUDIO` it is already receiving. The file lands
  where the person using it already is, and there is nothing to collect
  afterwards.
- `phone` — the phone writes the MP4 itself, and `tools/pull-recordings.ps1`
  fetches it later. Slower to get at, but it needs nothing from the link, which
  is the difference that matters on Wi-Fi or a cable that will not carry both
  streams at once.

Neither is better in general and the client should be able to say which.

`enabled: false` removes the recording encoder from the capture session
altogether. A recorder standing ready is not free even while idle -- it is a
second output the camera has to configure -- so a client that only wants a
webcam should be able to stop paying for it.

`start` and `stop` take effect immediately and never interrupt the stream: the
recording encoder's input surface is part of the capture session for its whole
life, and starting a take only adds it to the repeating request.

`config` is different — the surface's size is fixed when the session is
configured, so changing the recording format **restarts the pipeline**, exactly
as `set` does, with the same consequence for manual camera settings.

**The recording frame rate must divide the streaming one.** One camera produces
both, so the sensor runs at a single rate and the recorder can only take every
n-th frame, delivered as a repeating burst. A requested rate that does not
divide evenly is snapped to the nearest one that does, and the `ACK` reports
what was applied.

`preroll` is seconds of footage the phone keeps ready so a take can begin before
`start` arrives. It is **always sent**, unlike the other fields, because zero is
a meaningful value here -- it is how the ring is disarmed -- and omitting it
would leave it armed.

The ACK reports what was **granted**, which is not always what was asked for:
the ring lives in the phone's heap at a bitrate chosen for a file, so it is
clamped to a quarter of the heap. Twenty seconds requested can come back as six,
and a client that displays its own request rather than the ACK is lying to
whoever is looking at it.

Arming has two consequences the client should expect. The record target stays in
the repeating request the whole time, so the encoder runs whenever the camera
does; and the phone may disarm the ring on its own when the thermal status
reaches severe. It says so only by reporting `prerollArmedMs: 0` in the next
`STATS`, alongside the `thermal` field that explains it -- there is no
unsolicited ACK, because one would be indistinguishable from a reply.

Pre-roll is refused for `target: "pc"`. The ring is on the phone, and shipping it
across when a take starts would stall the live picture for as long as the ring
is deep.

There is no separate time-lapse command. The desktop's frame-interval setting
is expressed as a record rate: 1/4 at a 60 fps stream is `fps: 15`, and the
burst above is what makes it. Anything that reconfigures the recorder has to
carry the divided rate, or changing the record size silently restores full rate.

**A recording size the sensor cannot drive at the streaming rate lowers the
stream too**, and the `ACK` reports the new rate as `streamFps`. A capture
request carrying a 4K target cannot complete in 16ms; asking for one anyway does
not produce a slower stream, it wedges the camera HAL. The phone therefore
defaults to the largest size that keeps the streaming rate, and larger is a
deliberate choice with a visible cost.

Both outputs are configured with a `streamUseCase` -- `PREVIEW` for the stream,
`VIDEO_RECORD` for the file. Without it a HAL cannot tell two same-sized outputs
apart, and at least one returns buffers the framework never issued and then
stops producing frames on every stream.

Recording HEVC is worth doing even when the PC has no HEVC decoder: nothing on
the PC ever decodes this file live.

```json
{ "ok": true,  "cmd": "record", "applied": { "state": "recording",
  "file": "/storage/.../XCam_20260824-131207.mp4",
  "width": 3840, "height": 2160, "fps": 30, "bitrate": 120000000, "codec": "hevc" } }
{ "ok": true,  "cmd": "record", "applied": { "state": "idle",
  "file": "/storage/.../XCam_20260824-131207.mp4", "durationMs": 42310, "bytes": 634512890 } }
{ "ok": true,  "cmd": "record", "applied": { "state": "off", "enabled": false,
  "streamFps": 60 } }
{ "ok": false, "cmd": "record", "error": "this device cannot run a second encoder" }
```

### audio

```json
{ "cmd": "audio", "enabled": false }
```

Turns the microphone on or off for the session. The `ACK` reports whether sound
is actually running, which is not the same as what was asked for: a refused
permission or a microphone held by another app both come back as
`enabled: false` rather than as an error, because neither is a reason to fail a
video session.

```json
{ "ok": true, "cmd": "audio", "applied": { "enabled": true,
  "sampleRate": 48000, "channels": 2 } }
```

Sound belongs to the client session rather than to a pipeline run: it does not
depend on the camera format, and restarting it on every resolution change would
put an audible gap in the middle of a call.

### wb

```json
{ "cmd": "wb", "mode": "daylight" }
{ "cmd": "wb", "mode": "manual", "temperature": 5600 }
```

Presets come from the camera's `awbModes`. Manual needs `manualPostProcessing`
and drives `COLOR_CORRECTION_GAINS`; the gains are ignored by most HALs unless
`COLOR_CORRECTION_MODE` is set to `TRANSFORM_MATRIX` in the same request, so the
phone always sets both together.

### ACK

```json
{ "ok": true,  "cmd": "set", "applied": { "width": 1920, "height": 1080, "fps": 60 } }
{ "ok": true,  "cmd": "exposure", "applied": { "mode": "manual", "iso": 400, "shutterNs": 16666666 } }
{ "ok": false, "cmd": "exposure", "error": "manual sensor control unavailable" }
```

`applied` always carries the values in force after the command, which are not
necessarily the ones requested — clamping is reported here, not signalled as an
error. Clients should treat `applied` as the truth and update their UI from it.

---

### ramp

```json
{ "cmd": "ramp", "what": "focus", "to": 4.5, "ms": 2000 }
{ "cmd": "ramp", "what": "zoom",  "to": 2.0, "ms": 3000 }
{ "cmd": "ramp", "what": "focus", "cancel": true }
```

Moves a control from where it is to `to`, taking `ms` about it. `focus` is in
dioptres, `zoom` a ratio.

**An intention, not a stream of positions.** The curve runs on the phone. A
client sending sixty positions a second would put a focus pull at the mercy of
the link -- one late packet and the move stutters where no easing can hide it --
and sent this way it cannot stutter, because nothing arrives while it happens.

Eased as a smoothstep, so it starts and stops at rest. One at a time, and any
manual change to the same control cancels it: reaching for a control during a
move is how a person says they want it back. A camera that cannot do it at all
-- a fixed-focus lens -- refuses rather than accepting a move that never happens.

### framing

```json
{ "cmd": "framing", "x": 0.22, "y": 0.15, "w": 0.5, "h": 0.28 }
{ "cmd": "framing", "off": true }
```

Crops the sensor, in fractions of its active array. The client decides the
framing because it has the decoded picture to look at; the phone does the
cropping because its sensor is much larger than the stream, so a crop there
costs nothing at all -- no rescale, no lost resolution, no second encode.

The phone eases towards the rectangle rather than snapping to it, so a detector
that jitters between frames does not become a camera that trembles.

**These are sensor coordinates, not stream coordinates.** A client working from
what it sees must compose each new framing with the one already in force, since
the picture it is looking at is itself the result of that rectangle. Computing
one afresh each time sends the framing walking.

Clamped on arrival as well as by the client: four numbers off a socket become a
rectangle handed to a camera HAL, and a HAL given a rectangle outside its sensor
does not always say so politely.

### tally

```json
{ "cmd": "tally", "live": true }
```

Whether an application on the desktop actually has the camera open. Not the same
as the desktop being connected: it can sit there all day with the filter
registered and nobody consuming it, and until this existed the phone had no way
to tell the difference. Someone in front of a camera is asking whether anyone is
looking, and that is a different question from whether the cable is plugged in.

Sent only when it changes. No ACK: the desktop already knows what it asked for.

The desktop learns it from the shared section rather than from any API -- a
consumer reads frames continuously and stamps `lastReadTick` as it goes, and one
that has merely enumerated the device never reads at all.

### takes

```json
{ "cmd": "takes", "action": "list" }
{ "cmd": "takes", "action": "fetch",  "name": "XCam_20260825-143012.mp4" }
{ "cmd": "takes", "action": "delete", "name": "XCam_20260825-143012.mp4" }
{ "cmd": "takes", "action": "cancel" }
```

`list` answers with the directory and up to two hundred entries, newest first:

```json
{ "ok": true, "cmd": "takes", "applied": {
    "dir": "/storage/emulated/0/Android/data/com.xcam/files/recordings",
    "takes": [ { "name": "XCam_20260825-143012.mp4", "bytes": 634512890,
                 "durationMs": 42310, "modified": 1787654321 } ] } }
```

`name` is a bare file name and never a path. It arrives over a socket and is
used to open a file, so the phone resolves it inside its own recordings
directory and refuses anything that escapes -- `../../databases/x` is a
perfectly ordinary file name to a JSON parser.

`fetch` acknowledges with the name and size, then sends the file as `FILE`
packets: `ptsUs` carries the byte offset, and an empty final packet with
`FLAG_LAST_FRAGMENT` ends it. An empty packet rather than a flag on the last
data chunk, so the receiver learns the file is complete without knowing its
length in advance.

**The phone only sends a chunk while its send queue is empty.** The queue sheds
`DELTA` packets when it fills, so a transfer that enqueued chunks at will would
pay for the file in dropped frames. A fetch therefore takes whatever bandwidth
the stream is not using, and over Wi-Fi at the ceiling that can be almost
nothing. One fetch at a time; `cancel` stops it.

A client with adb should not use this at all. `adb pull` against the reported
`dir` is faster by an order of magnitude and costs the stream nothing.

## 4. STATS (phone → PC)

Emitted once per second, best effort.

```json
{ "actualFps": 59.8, "actualBitrate": 58900000, "encQueueMs": 12,
  "droppedFrames": 0, "battery": 84, "thermal": "light", "surfaceRotation": 0,
  "recording": true, "recordMs": 42310, "recordBytes": 634512890,
  "storageFreeMb": 41234,
  "prerollArmedMs": 6000, "prerollFillMs": 5840,
  "audioPeak": 0.31 }
```

The recording fields are how a client learns a take is still running and how
much of the disk it has left. A recording that fills the phone's storage stops
where it stopped, so the free space belongs in the same tick as the byte count.

`audioPeak` is the loudest sample since the last tick, 0..1, and reading it on
the phone resets it -- so there is exactly one reader. It is reported every tick
whether or not anything is listening, because the entire point is that a dead
microphone should look different from a quiet room. This project has twice
shipped a microphone that produced nothing: once because HyperOS revoked the
permission behind its back, once because `FOREGROUND_SERVICE_TYPE_MICROPHONE`
was missing from the `startForeground` call. Neither raised an error anywhere.
Both would have been obvious in the first second as a meter that never moved.

`prerollArmedMs` is the ring's length as the phone currently has it, and
`prerollFillMs` how much of that it is actually holding. The two differ for the
first few seconds after arming, and a client that shows the request instead of
the fill promises a reach the phone does not yet have. A drop of `armedMs` to
zero that the client did not ask for is a thermal disarm, and `thermal` in the
same object says why.

`thermal` mirrors `PowerManager.getCurrentThermalStatus()`:
`none|light|moderate|severe|critical|emergency|shutdown`.

---

## 5. Backpressure rules

The phone maintains a bounded send queue. When it is full:

1. Drop `DELTA` packets, oldest first.
2. **Never** drop `CONFIG` or `KEYFRAME`.
3. Increment `droppedFrames` in the next `STATS`.

The PC detects loss via `seq` gaps and SHOULD send `{"cmd":"idr"}` when a gap is
observed, so the picture recovers within one frame instead of waiting for the
next scheduled key frame.

---

## 6. Shutdown

Either side may close the socket. The phone tears down camera and encoder on
disconnect and returns to listening for a new connection.
