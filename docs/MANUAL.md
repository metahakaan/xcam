<img src="brand/xcam-mark-512.png" width="72" align="left" hspace="12" vspace="4">

# XCam

Turns an Android phone into a Windows webcam over USB or Wi-Fi, at bitrates a
commercial tool will not give you, and records what the sensor actually gave
rather than what the link could carry. Built for a Xiaomi 17 Pro but nothing in
it is device-specific.

<br clear="left">

```
[Android]  Camera2 -> MediaCodec input Surface (zero-copy hardware encode)
              |  H.264/HEVC Annex-B over TCP, tunnelled through adb forward
              v
[Windows]  xcam-core   depacketise -> Media Foundation hardware decode -> NV12
              |  lock-free shared-memory ring buffer
              v
[Windows]  xcam-dsfilter.dll   DirectShow source filter
              -> Zoom / OBS / Discord / Chrome see "XCam Virtual Camera"
```

The DirectShow filter is loaded **inside** the consuming application's process,
so it cannot share memory with the desktop app by any means other than a named
shared section. That constraint shapes the whole Windows design.

## Status

| Phase | Scope | State |
|-------|-------|-------|
| 1 | Android capture, encode and serve | **done, verified on a Xiaomi 17 Pro** |
| 2 | Windows decoder + preview window | **done, verified on a Xiaomi 17 Pro** |
| 2.5 | Pro mode control panel | **done, verified on a Xiaomi 17 Pro** |
| 3 | Shared memory ring + DirectShow filter | **done, verified with ffmpeg** |
| 4 | Device auto-discovery, settings, reconnect | **done, verified on a Xiaomi 17 Pro** |
| 5a | Dual recording | **done, verified on a Xiaomi 17 Pro** |
| 5b | Sound: capture, recording, wire | **done, verified on a Xiaomi 17 Pro** |
| 5c | Virtual microphone on Windows | **done, verified with ffmpeg** |
| 5d | Wi-Fi transport, adaptive bitrate | **done, verified on a Xiaomi 17 Pro** |

## Building the Android app

```powershell
.\tools\build-android.ps1
```

Add `-Install` to push it to a connected phone and set up the port forward in one
step. The script exists because two things on this machine need working around;
both are explained in its header comment and in `android/gradle.properties`.

## Trying phase 1

1. Enable USB debugging on the phone, plug it in, accept the RSA prompt.
2. Install and start the app, press **Start capture**.
3. On the PC:

```bash
adb forward tcp:27183 tcp:27183
```

4. Watch the live feed:

```bash
python tools/dump_stream.py --play
```

Or record the raw elementary stream and inspect what the encoder actually
produced:

```bash
python tools/dump_stream.py --out capture.h264 --width 1920 --height 1080 --fps 60 --bitrate 80000000
```

`--info` prints the phone's capability handshake — every camera id, its label,
maximum resolution, frame rate and zoom range — and exits.

The script sets up the adb forward itself. That matters more than it sounds:
the phone silently drops its forwards every time it re-enumerates on USB, and
the only symptom is a connection refused on localhost, which points at entirely
the wrong thing.

You can also bring the phone up from the PC without touching it:

```bash
adb shell am start -n com.xcam/.MainActivity --ez autostart true
```

### Measured on a Xiaomi 17 Pro

| Configuration | Result |
|---|---|
| 1280x720 @ 30 fps, 5 Mbit/s | 29.1 fps, 0 dropped frames |
| 1920x1080 @ 30 fps, 40 Mbit/s | 29.3 fps, 0 dropped frames |
| 1920x1080 @ 60 fps, 80 Mbit/s | 58.5 fps at 48 Mbit/s sustained, 0 dropped frames |

All sustained with `thermal: none` on the phone and zero decode errors.

Capture-to-socket queueing — phone encoder, send queue and USB, everything up to
the moment the PC has the frame — measures **20ms median, 36ms p90** at 1080p60,
and stays flat over a run rather than climbing.

## The desktop preview

```powershell
.\windowsuildin\ReleaseÊm-app.exe --width 1920 --height 1080 --fps 60 --bitrate 60000000
```

It finds the phone over adb, sets up the tunnel, starts the app if it is not
already running, and reconnects on its own if the cable is disturbed. The title
bar carries live frame rate, bitrate, latency and gap count.

### Pro mode

The panel floats over the picture and fades out when the mouse sits still, so
the resting state of the window is just the image.

- **Top left** — resolution, frame rate, codec and bitrate. Each opens its
  options inline, built from what the camera actually reports it can sustain:
  a size is never offered without a frame rate it can hold.
- **Bottom** — lens stops (`0.7x` `1x` `2x` `5x` `10x`) and a continuous zoom
  slider. On this phone the lenses live behind one logical camera, so both drive
  the same zoom ratio and cross between ultra-wide, main and tele.
- **Right** — ISO, shutter, white balance, focus and EV. Click one and a
  scrubber opens at the bottom; ISO and shutter travel logarithmically, because
  a linear slider over 50–12750 spends most of its length somewhere useless.
  Anything the hardware cannot do is dimmed rather than offered.
- Click the picture itself to focus there.
- **Type an exact value.** Dragging a ruler is fine for finding a value and
  hopeless for hitting one, so with a row selected just start typing: `400` for
  ISO, `1/120` or `120` for shutter, `2.5` metres or `inf` for focus, `5600` for
  white balance. Enter applies, Escape backs out.
- **LOG** puts the camera on a flat profile built for grading -- a log tone
  curve, desaturated colour, and sharpening and noise reduction turned down,
  since all three bake in decisions a grade cannot undo. Black sits at 0.075 and
  white at 0.92 so neither end clips; middle grey lands near 0.45, in the same
  territory as S-Log3.
- **LOAD LUT** applies a `.cube` to the preview. Clicking it again unloads it.
  The grade is display-only: the stream underneath stays flat, so what the
  virtual camera and any recording receive is still gradeable.

Log without a LUT looks wrong on purpose. That flatness is the detail the
default tone curve throws away, which is the whole reason to shoot it. Measured
on the same scene: blacks lift from 0 to 10, highlights pull in from 210 to 176,
and the spread of the picture drops from 60 to 39 while the midtones stay put.
Switching back restores the first set of numbers exactly.

`--log-profile` and `--lut <path>` apply both at startup.

### HEVC

HEVC is offered only when Windows can actually decode it. The stock HEVC decoder
ships as a Microsoft Store extension that is absent on plenty of installs, and
picking it there produces a silent black picture -- so the panel dims it instead.

```powershell
.\windowsuildin\ReleaseÊm-probe.exe --decoders
```

lists what this machine has. If HEVC shows `(none)`, install **HEVC Video
Extensions** from the Microsoft Store, or stay on H.264 -- at the bitrates USB
allows there is little to gain from HEVC anyway.

The phone's side is unaffected either way: it encodes HEVC perfectly well, and
`xcam-probe --codec hevc` will show frames arriving even on a machine that
cannot decode them.

### Diagnostics

The app writes `%LOCALAPPDATA%\XCamÊm.log`, truncated per run and flushed per
line so it can be read while the app is running. It records connections,
handshakes, stream restarts, every control sent and every ACK returned, plus a
throughput line twice a second. Most of what has gone wrong here only shows up
on real hardware minutes in and leaves nothing behind otherwise.

Shutter is clamped to the frame interval by the phone — an exposure longer than
`1/fps` cannot be delivered, and a camera asked for one halves its frame rate
instead of refusing. The panel shows the clamped value, not the request.

| key | |
|---|---|
| `C` | next camera |
| `T` | torch |
| `L` | log profile on/off |
| `R` | force a key frame |
| `V` | start / stop recording |
| `M` | microphone on/off |
| `Esc` | quit |

Measured at 1920x1080@60: **60 fps, 58 Mbit/s, 19-24ms, no gaps**, for about
11% of one CPU core -- the decode runs on the GPU's video engine and the frame
never leaves VRAM between the decoder and the screen. 3840x2160@30 holds
**31 fps at 26ms**.

Presenting deliberately happens on the UI thread rather than wherever a frame
was decoded. `Present` blocks until the GPU catches up, and anything that blocks
the socket reader backs up into the phone's send queue and grows latency without
bound -- at 4K that reached sixteen seconds. The reader now copies each frame
into renderer memory and goes straight back to reading, and for the same reason
it takes no shared lock: guarding it with the mutex the presenter holds would
reintroduce the same stall through the back door.

`xcam-probe.exe` is the same pipeline without a window. Reach for it when
something is wrong and you want to know whether the problem is upstream of
rendering; `--info` prints the handshake and exits.

### If the preview lags or plays in slow motion

This section is about `tools/dump_stream.py --play`, which shells out to ffplay.
`xcam-app.exe` does not have the problem -- controlling the decode and the
presentation is most of why it exists.


That is the player, not the pipeline. Software-decoding 1080p60 at 50+ Mbit/s
does not run in real time; ffplay then stops draining the pipe, the backlog
pushes through the socket into the phone's send queue, and delay grows without
bound — measured going past four seconds inside fifteen. Because the backlog is
replayed at its original timestamps, it looks like slow motion.

`--play` therefore defaults to hardware decoding (`-hwaccel d3d11va`) and
`-vf setpts=0`, which drops ffplay's timestamp pacing so the newest frame is
shown on arrival. Same stream, same settings:

| | software decode, timestamp pacing | hardware decode, render on arrival |
|---|---|---|
| queueing | 9ms climbing to 4.4s | 20ms, flat |
| throughput | 32 fps, 10 dropped frames | 59.8 fps, none dropped |

`--hwaccel cuda\|dxva2\|none` picks a different decoder, and `--smooth` restores
ffplay's own pacing if you want smoothness over latency.

If the stream dies a few seconds in and the device vanishes from `adb devices`
entirely, that is the USB link resetting, not the app — the app process survives
it. A charge-oriented cable or a front-panel port is the usual cause; a known
good data cable into a rear port fixed it here.

## The virtual camera

```powershell
.	ools
egister-filter.ps1              # once; -Unregister to remove
```

Then start `xcam-app.exe` and pick **XCam Virtual Camera** in Zoom, OBS, Discord,
Chrome or anything else that opens a webcam. It offers NV12, YUY2 and RGB24 at
whatever resolution the app is publishing.

Registration falls back to per-user (`HKCU\Software\Classes`) when it cannot
write the machine-wide keys, so it works without elevation; run it elevated only
if every account on the machine should see the camera. The DLL's path is
recorded in the registry, so do not move or rebuild it elsewhere afterwards
without re-registering.

The `CAM ON` chip in the panel stops and starts publishing.

**After rebuilding the filter, restart whatever had the camera open.** Anything
that has ever enumerated cameras keeps the DLL loaded -- Chrome and NVIDIA
Broadcast do it just by running -- so a rebuilt filter does not take effect in
those processes until they restart. The build renames the old copy aside rather
than failing to link, so the registered path never changes.

The virtual camera publishes a fixed **1920x1080**, deliberately independent of
the capture resolution — the frame is scaled on the GPU by the video processor,
which crops the coded padding away at the same time. A DirectShow pin settles its
format when the application connects and cannot change it afterwards in any way
most applications honour, so a published size that followed capture would black
out an open Zoom call the moment someone switched to 4K, and stay black on the
way back. Capture at whatever resolution suits; the camera stays put.

### How the two halves meet

The filter is a COM DLL, and Windows loads it **into the consuming
application's process** -- Zoom's, OBS's, Chrome's. It shares nothing with
`xcam-app` except a named shared section, and that constraint shapes the whole
Windows design:

```
xcam-app                     |  Zoom / OBS / Chrome
  decode -> D3D11 texture    |
  readback -> NV12           |
  SharedFrameWriter  ------------>  SharedFrameReader
    Local\XCamFrames         |        xcam-dsfilter.dll
    Local\XCamNewFrame       |          -> IMemInputPin::Receive
```

One writer, many readers, no locks. A reader can be an unresponsive
conferencing app that stops collecting for two seconds; a writer that waited for
it would stall the capture pipeline, so the writer never waits and readers detect
a torn frame after the fact and skip it. With no producer running the filter
still delivers frames on schedule -- a camera that stops producing looks hung to
the application, and the person looking at it has no way to know the desktop app
simply is not open.

The filter is written directly against the DirectShow interfaces. The SDK ships
`strmbase.lib` without its headers, so BaseClasses would have to be vendored,
and that is the one thing this project does not do.

## Recording on the phone

The webcam stream and the recording are two separate encodes of the same frames.
The stream is sized for a live link and compressed to arrive on time; the file is
whatever the sensor can give, at a bitrate no USB cable has to carry. One encode
cannot be both, which is why there are two.

Press **RECORD** in the panel (or `V`) and the file starts writing. Nothing
interrupts the stream: the recorder's input surface is part of the capture
session for its whole life, and starting a take only adds it to the repeating
request.

### Where it lands

**On the PC by default**, in `%USERPROFILE%\Videos\XCam`, ready the moment you
stop. The phone sends the recording encoder's frames across the link as they are
produced and the desktop muxes them with the sound it is already receiving.
Nothing is re-encoded on the way — Media Foundation's MP4 sink with its input
type set equal to its output type is a mux and nothing else. Re-encoding here
would undo the entire point of recording at 120 Mbit/s.

That means two streams cross the link at once: the webcam and the recording,
both at full rate. USB carries it comfortably — measured at **156 Mbit/s with
0 gaps** on a single stream, and 4K30 recording alongside a 47 Mbit/s webcam
never dropped a frame. Wi-Fi does not, which is why the other option exists.

**TO PHONE** switches back to the phone writing its own MP4, which needs nothing
from the link at all. Collect those afterwards:

```powershell
tools\pull-recordings.ps1
```

Same destination, `-Remove` to delete each from the phone once the local copy is
verified byte-for-byte, `-NewOnly` to skip what is already here.

Neither is better in general. The PC is where you already are; the phone is what
still works when the link will not carry a second stream.

### What it costs

One sensor feeds both encoders, so it runs at one rate, and the recording size
sets what that rate can be:

| Streaming | Recording | Result |
|---|---|---|
| 1080p60 | 1440p60 HEVC, ~120 Mbit/s | stream stays at 60 |
| 1080p60 | 1080p30 HEVC | stream stays at 60, every second frame recorded |
| 1080p30 | 2160p30 HEVC, ~122 Mbit/s | stream stays at 30 |
| 1080p60 | 2160p30 HEVC | **stream drops to 30** |

The last row is not a policy choice. A capture request carrying a 4K target
cannot complete in 16ms on this sensor, and asking for it anyway does not
produce a slower stream -- it wedges the camera HAL. So the default recording
size is the largest the camera can still deliver at the streaming rate, which on
a Xiaomi 17 Pro at 1080p60 is **1440p60**. Picking 4K explicitly is one press
away and the panel shows the frame rate it costs.

Recording HEVC is worth doing even on a PC with no HEVC decoder: nothing here
ever decodes that file live.

Clicking the size chip past its smallest option turns recording **off** and
takes the second encoder out of the session entirely. Nothing about the phone
being a webcam should cost anything to someone who never records.

### Sound

The phone's microphone is captured once and used twice: muxed into the recording
as an AAC track, and sent to the PC as `AUDIO` packets. One capture and one
encode, because two AudioRecords would be two applications fighting over one
microphone.

Recordings come out as HEVC video plus 48kHz stereo AAC at 192 kbit/s, both
tracks on the same timeline. Getting them onto the same timeline was the whole
difficulty:

- **Which clock the camera uses is measured, not asked.** Camera2 reports it in
  `SENSOR_INFO_TIMESTAMP_SOURCE`, and on this phone it reports the wrong one.
  The two candidates -- `System.nanoTime` and `SystemClock.elapsedRealtimeNanos`
  -- differ by however long the device has been suspended, which here was
  **986508 seconds**, so the first recording with sound had its audio track
  starting eleven days after its video. Comparing a real camera timestamp
  against both clocks settles it beyond any doubt: one is 79ms away, the other
  is a week and a half.
- **Sample zero is placed by the driver, not by guesswork.** `AudioRecord`
  hands over audio captured some tens of milliseconds earlier, and assuming
  otherwise makes sound systematically late by an unknown. `getTimestamp` says
  when a given frame was actually captured. (On this phone the correction turned
  out to be 0ms -- the estimate was already right -- but that is a fact about
  this device, not about the method.)
- Every later timestamp comes from the sample count rather than the clock, so
  the timeline is exactly even and cannot drift.

If the microphone is refused or held by another app, the session carries on
silently. A camera that will not start because of the sound would be a worse
trade than a camera with no sound.

The same sound reaches the PC as **XCam Virtual Microphone**, an audio capture
device alongside the camera. One DLL carries both, so registering once puts both
in every application's list:

```powershell
tools
egister-filter.ps1
```

Measured with ffmpeg reading the device: eight seconds captured in 8.04 seconds
of wall clock -- 0.994x, which is what a live capture device should be -- at
48kHz stereo, mean -25.2 dB.

That pacing was worth getting right. Every buffer claims 20ms of the timeline
whether or not the phone had 20ms of audio to fill it, so a loop that ran as
fast as the shared ring allowed handed downstream more timeline than time had
actually passed: measured at 1.44x, six seconds of audio delivered in four. The
push loop now runs off a real clock and lets the ring decide only what is *in*
the buffers.

**Two Android traps, both of which produce working silence rather than an
error.** They are worth knowing because nothing about them looks like a fault:

- A foreground service must declare `microphone` in the type it passes to
  `startForeground`, not merely in the manifest. Declaring only `camera` gets
  you a perfectly well-formed stream of digital silence -- a microphone that
  connects, runs at the right rate and records nothing.
- HyperOS revokes the microphone permission behind your back when the screen is
  locked. The permission reads as granted, then as denied a moment later, and
  the app logs that it is streaming without sound. Grant it once from the app's
  own dialog with the phone unlocked.

Chasing the first of those went through the desktop decoder twice before the
evidence settled it: `xcam-probe --audio-out` writes the sound off the wire as
ADTS, ffmpeg decoded it as silence too, and that ruled out everything on this
side in one step.

So the phone now measures its own microphone and says what it finds:

```
W XCam/Audio: the microphone has delivered nothing but silence for 3s (peak 0).
              Android does not refuse a microphone it will not give you — check the
              permission, the privacy switch, and whether another app is holding
              the input.
```

A silent recording looks exactly like a broken microphone, and neither the wire
nor the decoder can tell you which end went wrong. Three seconds of samples can.

### Measured

Streaming 1080p60 while recording 1440p60 for 11 seconds, on the same session:

```
phone : 60.8 fps out, 0 dropped frames, thermal none
file  : 1440p, 469 frames in 7.80s = 60.1 fps, 120 Mbit/s HEVC
PC    : 688 frames decoded, 0 gaps
```

With sound, over 11.2 seconds: video 671 frames at 122 Mbit/s HEVC starting at
0.000s, audio 529 frames of 48kHz stereo AAC at 192 kbit/s starting at 0.066s --
the gap being the moment the microphone opened, not a sync error. Over the wire
in the same period: 441 AAC frames, 181 kbit/s, and the two-byte
AudioSpecificConfig that has to precede them.

And at 4K: 226 frames in 7.52s = 30.05 fps, 122 Mbit/s, no dropped frames.

What the recorder costs the live stream, measured the same way each time
(`xcam-probe --seconds 10`, median queueing above best case):

| | median | worst |
|---|---|---|
| recording off | 17–25 ms | 34–46 ms |
| recorder ready, idle | 20–26 ms | 41–52 ms |
| take running | 37–46 ms | 58–76 ms |

A recorder standing by is close to free. A take in progress costs about 15ms,
and that is the honest price of encoding the same frames twice -- the frame rate
itself never moved, and no frames were dropped in any of the three.

## Finding the phone, and remembering it

Start `xcam-app` with no arguments and it works out where the phone is, in the
order of how deliberately each was asked for:

1. `--host 192.168.1.10`, if given
2. a phone on adb, over USB
3. a phone announcing itself on the network
4. the address that worked last time

Announcing is a UDP broadcast twice a second on port 27184 — forty bytes
carrying a name, a port and a protocol version. mDNS is the conventional answer
and Android speaks it through `NsdManager`, but the Windows end of that is a
callback-driven resolver, which is a great deal of machinery to learn one
address. `xcam-probe --discover` lists what it can hear:

```
listening on UDP 27184 for 4 seconds...
  192.168.1.10     Xiaomi 25098PN5AC (protocol v4, port 27183)
```

Losing the phone mid-stream is not an error state. The desktop retries once a
second, and when the phone comes back it handshakes, restarts the decoder and
re-applies everything the panel believes is in force — so a cable knocked out
of its socket costs a second of picture and nothing else.

### What it remembers

Format, camera, ISO, shutter, white balance, focus, EV, log profile, the loaded
LUT, the recording format, the microphone, the virtual camera and the last
Wi-Fi address, in `%LOCALAPPDATA%\XCam\settings.txt`. Written as they change
rather than only on exit, because a crash should not cost an evening of dialling
in, and written only when something actually changed.

It is a flat `key=value` file on purpose. The handshake needs a JSON reader
because the phone decides that format; this file is ours on both ends, and one a
person can read and fix in Notepad is worth more than one that nests. Delete it
to start fresh.

## The settings sheet

Press `,` — everything below lives there, and nothing in it is needed to get a
picture. It exists because the panel is for the shot and these are for the
setup, and mixing the two is how a pro-mode screen turns into a menu.

**Language.** English and Turkish. Picked from `GetUserDefaultUILanguage()` on
first run and remembered after that.

**Connection.** `AUTO` is the order above. `USB` and `WI-FI` pin it, and Wi-Fi
takes a typed address — the one thing the desktop genuinely cannot work out for
itself when the phone is on another subnet or the broadcast is filtered.

**Recordings folder.** A folder picker; empty means `%USERPROFILE%\Videos\XCam`.

**Frame interval.** `1×` records every frame. `1/2`, `1/4` and `1/8` record
every second, fourth or eighth one, which is what makes a timelapse without a
second pipeline: the phone switches from `setRepeatingRequest` to
`setRepeatingBurst` and only the n-th request carries the record surface. The
stream is untouched, so the preview stays at full rate while the file thins out.

**Apply the LUT to recordings.** Off by default, and the only place in this
project that re-encodes. A grade cannot be applied to a stream without decoding
it, so this one takes the graded preview back off the GPU as NV12 and runs it
through a Media Foundation H.264 encoder into a second file,
`XCam_graded_<date>.mp4`. It sits beside the ungraded take rather than replacing
it: the phone's own encode is the good one, and losing it to a grade decided in
a hurry is not recoverable. What this file has instead is the picture as it
looked on screen.

It needs a LUT loaded and recording to the PC, and greys out otherwise.

**Pre-roll.** `OFF`, `5s`, `10s` or `20s`. Press record *after* the moment: the
last few seconds are already encoded and waiting, and the take begins from there.

This costs less here than it would anywhere else, because of what was already
true. The record surface is part of the capture session rather than of a take,
so nothing is reconfigured to keep it fed; the encoder's drain loop already sees
every frame and simply discards it between takes; and the recording format
already asks for a key frame every second, which is the granularity a ring can
be cut to. What is new is a ring of encoded access units, and a flush into the
muxer when the button is finally pressed. Sound is kept too — a take that opens
ten seconds in the past with ten seconds of silence is not the feature anyone
asked for.

**What it is granted, not what is asked for.** The ring lives in the phone's
heap at a bitrate chosen for a file: ten seconds at 120 Mbit/s is 150 MB. The
request is clamped to a quarter of the heap and the ACK reports what fits, so
asking for twenty seconds and being given six is a normal outcome that the panel
shows rather than hides.

**What it costs.** The encoder runs whenever the ring is armed. That is battery
and heat, and it is exactly the cost the recorder was built to avoid paying
while idle — a recorder standing ready is not free. So it is off by default, the
settings sheet says what it costs, and the phone disarms it on its own when the
thermal status reaches severe. The alternative to disarming is a thermally
throttled stream, which is a failure the person on the call sees and did not
choose.

**Local takes only.** The ring is on the phone. Shipping it across the link when
a take starts would stall the live picture for as long as the ring is deep,
which is the one thing a webcam may not do. The control greys out when the
destination is the PC.

While armed and idle, the record button shows how much the ring is actually
holding — the seconds just after arming are the ones where it cannot yet do what
the label promises.

**Cinematic matte.** `OFF`, `2.39`, `2.35` or `1.85`. A matte crops rather than
squeezes: the picture keeps its full width and the rows outside the target ratio
go black, which is what a 2.35:1 delivery looks like on a 16:9 screen.

| Frame | 2.39 | 2.35 | 1.85 |
|---|---|---|---|
| 1920x1080 | 1920x804 | 1920x816 | 1920x1036 |
| 3840x2160 | 3840x1604 | 3840x1632 | 3840x2076 |

The masked rows are always an even number, because 4:2:0 chroma is shared
between row pairs and an odd bar leaves a half-coloured line along the edge of
the mask. That is why 1080p at 2.35 masks 132 rows rather than the 131.5 the
ratio asks for, and lands on 816 visible rows rather than 817 — the side that
shows slightly less than asked for, never more.

The matte is applied where the LUT is, which decides where it appears: the
preview, and a graded recording. It also reaches the virtual camera, painted
into the buffer on the way out rather than through the shader — that path blits
the decoded frame straight to NV12 and never makes a shader pass, and two black
bars are cheaper to write than to render.

What it does not reach is the phone's own recording. That file is the phone's
full-quality encode and never arrives here as pixels; a matte is a delivery
decision, and baking one into the only unrecoverable copy is not a decision to
make on someone's behalf. Crop it in the edit, where the rest of the grade
happens.

A preset carries the matte with the rest of the look.

**Presets.** Four slots. A slot holds the format, the camera, the whole exposure
block, the log profile and the LUT path, so a lighting setup dialled in once can
come back in a click.

## Judging exposure, and writing down what a take was

### Zebras and focus peaking

`Z` cycles the zebras — off, 70%, 95% — and `P` the peaking. Both are in the
settings sheet as well.

70% is where skin sits at a sane exposure and 95% is about to clip. A third
threshold in between would be a number nobody could name.

**Both measure the picture, not the look.** They read the signal before the LUT,
because what the phone recorded is that signal; exposure judged after a grade is
exposure judged against a preference. This matters most in exactly the case the
project was built for: a log image looks flat and grey, and there is no way to
read its exposure by eye at all.

**Neither is ever recorded.** The same shader draws the preview and a graded
recording, so it takes a flag that is on for one and off for the other. A zebra
baked into a recording is a stripe somebody has to explain later.

### Two audio tracks

The phone's microphone and one plugged into this machine, recorded as separate
tracks in the same file. `Sound` in the settings sheet switches it on and picks
the input; both levels are drawn in the stats block, one bar each.

Nobody serious records a shoot on a phone's microphone. Everybody wants it in
the file anyway, because it is the track that carries sync — so it stays, and
the one people will actually use goes beside it.

**Two meters, not one summed.** They are two tracks; a single bar would hide
exactly the case worth seeing, which is one of them dead while the other carries
on.

**The clocks.** The samples are captured here and the file's timeline is the
phone's, so each one has to cross. The offset between the two clocks is measured
rather than assumed: every recorded frame arrives stamped with the phone's time,
and the difference from the moment it lands here is that offset plus whatever
the link took. The *smallest* difference seen recently is the least-delayed
packet, which is the closest to the truth the link offers — so it is a running
minimum, reset every few seconds so it can follow a clock that drifts instead of
latching onto one lucky packet forever.

Within the capture, the endpoint's own timestamps advance the position and this
machine's clock anchors the first one. The two are never subtracted from each
other: a QPC position and a steady_clock reading have different origins, and
their difference is a number with no meaning.

The capture starts when it is switched on rather than when a take begins,
because the file needs the encoder's configuration *before* it opens — a sink
writer takes its streams before it starts and none after, so an interface
switched on halfway through a take cannot join that take.

`xcam-probe --selftest-mic` lists the inputs here and captures one for three
seconds:

```
  [2] Mikrofon (HD Pro Webcam C920)
capturing from Mikrofon (HD Pro Webcam C920) at 48000 Hz, 2 ch for 3 seconds...
  132 AAC frames, 67 kB, peak 0.21 of full scale
  AudioSpecificConfig: 2 bytes
```

It also says when an input hands over digital silence with the endpoint's silent
flag set, which is how Windows delivers a muted or blocked microphone — the same
way Android delivers a denied one, and the same failure this project has already
met twice.

### A sidecar per take

The phone keeps the master and the desktop may keep a proxy. Between them they
hold the picture and not one word about how it was made: an editor opening the
file a week later sees a resolution and nothing else — not the ISO, not the
shutter, not which LUT was on the panel, not whether log was engaged, and not
which of the two files is the good one.

So every take gets `<name>.xcam.json` beside it, holding the format, the camera,
the whole exposure block, the white balance, the focus, and the look — log
profile, LUT path, matte, mirror. Plus one line that says plainly which file is
the master, because that is the only line anybody has to act on.

It also gets a `.edl`. An EDL rather than FCPXML: every editor reads one, it is
a hundred lines of code rather than a schema, and what has to cross is a file
name and a duration. Anything richer is a look, and the look is in the sidecar.

Both land in the recordings folder — which is where the takes browser puts a
file it fetches, so the description and the picture end up together whether the
take was written here or pulled off the phone later.

## The operator

Both of these exist because the person shooting is usually also the person in
front of the camera. Replacing the operator with software is not a garnish in
that situation.

### Follow focus

Two marks and a duration, beside the lens stops: `SET A`, a mark, `SET B`, a
mark, and a time. Set a mark from wherever the lens is now, then ask for the
other one and the phone eases between them. `F1` and `F2` set the marks, `F3`
and `F4` pull to them — a pull is something you do while looking at the subject,
not at a panel.

**The curve runs on the phone.** A desktop sending sixty positions a second
would put a focus pull at the mercy of the link: one late packet over Wi-Fi and
the move stutters in a way no amount of easing hides. What crosses the link is
an intention — go there, take this long — and it cannot stutter, because nothing
arrives while it happens.

The easing is a smoothstep, so the move starts and stops at rest. That is what a
hand on a follow focus does and what a linear ramp conspicuously does not.
Touching focus during a move cancels it: reaching for a control is how a person
says they want it back.

The marks live on the desktop, not the phone. They belong to the shot rather
than to the camera, so they survive a reconnect, a camera switch and a format
change — all of which rebuild the capture request and would lose anything kept
on the other side.

### Auto-framing, in the sensor

`FRAME` in the device tray. The desktop finds the faces; the phone crops its
own sensor to hold them.

That split is the entire point. The sensor is much larger than the stream, so
cropping there costs **nothing** — no rescale, no lost resolution, no second
encode. Every competitor does this by cropping and rescaling its own decoded
copy, which throws away exactly the resolution the crop needed and pays for an
encode to do it.

Detection is the face detector that ships with Windows, so there is no model to
bundle, no runtime to link and nothing to keep up to date. It reads the luma
plane of the frame the virtual camera just published, which is already in hand
and already grey.

**The two coordinate systems are the whole problem.** The detector sees the
stream; the crop is in sensor coordinates. A face at the centre-left of the
stream is not at the centre-left of the sensor once a crop is in force, so each
new framing is *composed* with the one already applied rather than computed
afresh — otherwise the framing walks a little further off every time it looks.

The phone eases towards the framing rather than snapping to it, by a fixed
fraction of the remaining distance each tick: fast while far, slow as it
arrives, never overshooting. A detector that jitters by a few pixels between
frames would otherwise show up as a camera that trembles. That easing is the
difference between an operator and a servo.

Five looks a second, and a command only when the framing has actually moved. The
frame is built for a person rather than a face: headroom above, body below, the
group filling about a third of the width. A face centred in frame is a portrait
of a forehead.

Off by default. A camera that reframes itself while someone is setting up a shot
is fighting them.

## Pulling the cable mid-call

Unplug USB while a call is running and the picture holds rather than dies; the
stream comes back over Wi-Fi in about half a second, and the virtual camera
never stops producing frames — the filter redelivers its last picture while the
link is away, which is what it already did for a phone that was momentarily
late.

Plug the cable back in and it moves back to USB on its own, which is the
difference between 22 Mbit/s and 156.

**What makes it work is that nothing restarts.** The phone keeps its pipeline
alive for five seconds after a client vanishes, and a client that reconnects
within that window is told `"resumed": true` in the handshake. It answers by
doing nothing: no `set` command, no format, no default chosen. A `set` rebuilds
the capture session and both encoders on the phone, which is exactly what a
cable coming loose must not cost.

The phone re-sends the two things its encoders only ever say once — the video
and audio codec configuration — and asks for a key frame, because the frames
arriving on the new socket would otherwise reference pictures that went out over
the dead one.

The cost is a few seconds of camera and encoder after a genuine disconnect,
which is battery nobody asked to spend. The alternative is a full restart every
time a cable moves.

**Measured on the way past:** closing the window with no phone attached took
fifteen seconds and now takes 0.3. The connection attempt was given a
five-second timeout, and quitting waited out whichever attempt was in progress
against a remembered address that was never going to answer. A phone that is
there answers in milliseconds; the timeout only ever governed how long a failure
took, and it is now a second and a half. The same delay could also queue a
hand-over behind a stale address.

## What the panel now says out loud

Three things this project had measured and thrown away, and one it had never
had. They are grouped because they are the same idea: a fault nobody can see is
a fault that ships.

**The microphone is a level, not a word.** A bar in the stats block, peak with a
hold that decays, square-rooted so speech at a sane level does not draw as a
stub. It turns red and says so after three seconds of nothing.

The phone screen shows the same bar in place of the word it used to show. That
word was the bug: it said "sound" whenever capture was configured — which is
exactly what it said both times this project shipped a microphone that produced
nothing, once because HyperOS revoked the permission behind its back and once
because the foreground service type was missing from the `startForeground` call.
Neither raised an error. And "silent" was already taken, meaning "no sound
configured", so the one word that could have been the warning was unavailable.
A bar that does not move cannot be misread that way.

**Tally.** The phone shows `ON AIR` in red when an application actually has the
camera open. That is a different question from whether the desktop is connected:
it can sit there all day with the filter registered and nobody looking through
it. The desktop works it out from the shared section — a consumer reads frames
continuously and stamps the header as it goes, and one that has merely
enumerated the device never reads at all. A second of grace before the light
goes out, so a consumer whose thread was descheduled does not flicker it.

**Mirror.** Left to right, and top to bottom for a phone hanging upside down in
a clamp. Applied in the shader, so it reaches the preview and a graded
recording; the virtual camera mirrors its own buffer, for the same reason it
paints its own matte — that path never makes a shader pass.

Ninety degrees is deliberately not offered. It changes the shape of the output,
and the virtual camera's declared size, the recording and the preview's aspect
would all have to renegotiate. That is a portrait mode, not a flip.

**Start with Windows.** A `HKCU\Run` entry that launches minimised. Per-user,
no elevation, and visible in Task Manager's Startup tab where anyone can switch
it off — anything more powerful would be harder to remove than to install. The
entry is rewritten whenever the switch is touched, because the executable moves
when it is rebuilt somewhere else and a stale entry fails at boot in a way
nobody would connect to this switch.

Minimised rather than hidden: a window with no way back to it is a process
people find in Task Manager and kill.

## Takes on the phone

`.` opens the browser, or the row in the settings sheet. It lists what the phone
has — name, length, size — pulls one with a click, and deletes one after asking.

Until now the only way to these files was `adb pull` and a script. Pre-roll
makes takes cheap and frequent, and a person who records twenty short takes in
an afternoon should not have to read a directory listing to find the one they
meant.

**How a file comes across depends on what is available.** Over USB it is
`adb pull`: a gigabyte in seconds, and it never touches the stream. Over Wi-Fi
there is no adb, so the phone sends the file through the same socket as the
picture — one 256 KB chunk at a time, and **only while that socket is idle**.

That pacing is the whole design. The phone's send queue sheds `DELTA` packets
when it fills, so a transfer that simply enqueued chunks as fast as it could
read them would pay for the file in dropped frames: the live picture would
stutter for as long as the copy took. Offering a chunk only when the queue is
empty means a fetch takes whatever bandwidth is left over and never a frame that
was not. Over Wi-Fi at the ceiling that can be almost nothing, and the transfer
simply takes as long as it takes. Slow is a fair price; a stuttering call is not.

Progress comes from the file itself in both cases — the bytes written locally,
not what the sender claims to have sent.

The name arriving from the phone is treated as a name and never a path. It is a
string from a socket used to open a file, and `../../databases/x` is a perfectly
ordinary file name to a JSON parser; the phone resolves it inside its own
recordings directory and refuses anything that escapes.

## Over Wi-Fi

The phone shows its own address, and the desktop takes it:

```powershell
xcam-app --host 192.168.1.10
```

That path leaves adb out entirely — there need not be a cable attached. Nothing
else about the protocol changes.

Measured on a home network, 1080p30: **22 Mbit/s carried, 0 gaps, 34ms of
queueing**, which is within a few milliseconds of what the cable gives at the
same bitrate. The difference between USB and Wi-Fi is not latency, it is
ceiling.

### Finding that ceiling

Asking Wi-Fi for the bitrate USB is happy with does not produce a softer
picture, it produces a growing queue: the phone sheds frames, latency climbs,
and the stream comes apart while every number still reports the bitrate as
applied. So the desktop watches two signals -- frames the phone dropped, and its
own queueing delay -- and moves the encoder's target between them.

Backing off is sharp and recovering is slow, because guessing high costs a
visible stall and guessing low costs only detail. Asked for 150 Mbit/s at
1440p60 over Wi-Fi, it settles itself:

```
link is behind (13 dropped, 186ms queued); bitrate down to 63 Mb/s
link is calm; bitrate up to 79 Mb/s
link is calm; bitrate up to 98 Mb/s
link is behind (2 dropped, 261ms queued);  bitrate down to 74 Mb/s
link is behind (17 dropped, 118ms queued); bitrate down to 55 Mb/s
```

The panel says so while it is happening -- "link limited to 55 Mb/s" -- because
a stream running at a third of what was asked for explains a soft picture that
would otherwise look like a bad camera. `--fixed-bitrate` turns it off for
anyone who would rather have the drops.

## Installing it

```powershell
.\tools\install.ps1              # install or update
.\tools\install.ps1 -Uninstall   # take it back out
```

**No administrator, and that is a decision rather than a shortcut.** The virtual
camera is a COM server, and a COM server lives under `HKCU\Software\Classes`
just as happily as under the machine-wide hive — `HKCR` is a merged view of the
two, so every application that enumerates cameras finds it either way. Nobody
has to be talked past a UAC prompt to try a webcam, and nothing is installed
that another account inherits.

What it costs: the camera exists for the account that installed it. Anyone who
wants it for every account on the machine can run `register-filter.ps1`
elevated, which is the same registration written to the other hive.

What the installer does that copying the files would not:

- **Puts the binaries somewhere stable**, `%LOCALAPPDATA%\Programs\XCam`. The
  registry records the DLL's path, so a filter registered out of a build tree
  stops working the moment that tree is moved or rebuilt somewhere else — a
  camera that vanishes for no reason anybody could work out.
- **Unregisters the old copy before replacing it.** Overwriting the DLL
  underneath a live registration leaves entries pointing at a file that has
  changed identity.
- **Adds a Start Menu entry and appears in Windows' own installed-programs
  list**, so it can be removed the way everything else is rather than by finding
  this script again.
- **Takes the autostart entry with it** when it goes. Leaving one behind means a
  boot that runs something no longer there.

Settings and recordings are left alone by an uninstall. Someone removing a
version to install another should not lose an afternoon of dialling in.

## A phone that is not there

```powershell
xcam-probe --serve capture.h264 --port 27183 --name "Phantom A" --fps 30
xcam-app --host 127.0.0.1
```

`--serve` speaks the phone's side of the protocol and replays a recorded
elementary stream: handshake, CONFIG, key frames, deltas, STATS once a second,
and an acknowledgement for every control command it is sent. Nothing is acted
on, because there is no camera.

It exists because multicam cannot be built against one phone. It turned out to
be worth more than that: most of this desktop — the decoder, the panel, the
virtual camera, the recorder, the grade — has nothing to do with a camera and
everything to do with a stream arriving, and all of it can now be worked on with
no phone attached at all.

It is also a phone that never runs out of battery, never gets hot, and
reproduces the same failure every time.

The stream is split on start codes: parameter sets become the CONFIG packet and
every picture becomes its own. Anything more careful than that — slice headers,
`first_mb_in_slice` — would be an H.264 parser, and what is being tested is the
transport. A key frame goes out whenever the replay loops, whatever the file
says, or a client joining mid-replay would wait for one that only exists at the
beginning of the recording.

Pacing is against the start time rather than a sleep per frame, so a long replay
does not drift a little later with every picture.

Measured: 187 frames in six seconds, all decoded, no gaps, and the full desktop
connects to it and publishes a virtual camera at 30 fps with 1 ms of latency.

## When an application will not start the camera

The filter runs inside somebody else's process — Discord's, Zoom's, Chrome's —
where there is no console and no way to ask it a question. So it says what it
was asked for, and `xcam-probe` listens:

```powershell
.\xcam-probe.exe --watch-debug --seconds 240
```

Start the application, select XCam, and every negotiation appears, tagged with
the process that made it:

```
pid 24788  Discord.exe  CreateInstance: camera
pid 24788  Discord.exe  GetNumberOfCapabilities -> 12
pid 24788  Discord.exe  [pin 0000017516...] SetFormat: now 960x540 @30
pid 24788  Discord.exe  [pin 0000017516...] offered YUY2  960x540 -> 0x00000000
pid 24788  Discord.exe  [pin 0000017516...] connected at 960x540 @30 fps
```

The channel is `OutputDebugString`, which needs no filesystem access, and that
is the point. There is also a log file, switched on by creating
`%LOCALAPPDATA%\XCam\filter-trace-on` and read at `%LOCALAPPDATA%\XCam\filter.log`
— but a Chromium capture process can reach neither, so for exactly the
applications hardest to diagnose the file stays empty and says nothing about
whether anything happened. That cost an evening: Discord had loaded the filter
and negotiated with it all along, while the log showed a stale entry from
ffmpeg and looked like proof that it never had.

Only one listener can hold the debug channel at a time, so a debugger attached
to the application takes the messages instead.

### What Discord was hitting

The pin used to offer its formats with `IPin::QueryAccept` and treat a refusal
as final. `QueryAccept` is an optimisation, not the contract: a receiving pin
may answer no to everything and still accept a type when it is actually handed
one, and Chromium's capture sink does exactly that. So every application built
on Chromium — Discord among them — was told the camera had no acceptable format
at all, having never been offered one. ffmpeg implements `QueryAccept` properly,
which is why it worked and hid the bug.

Types now go out through `ReceiveConnection`, which is how a source pin is
supposed to propose them.

## How it looks

The identity is written down in [docs/BRAND.md](docs/BRAND.md): the mark, the
palette, the type, and the one rule that keeps the two accents meaningful —
**Signal marks what is in hand, Record marks what cannot be undone.**

The mark is an aperture X. Four bars on the diagonals stopping short of the
centre, so a small opening is left between them: the shape a diaphragm makes
rather than the one a close button makes. The upper-right arm is amber, and it
is the tally light.

It was drawn for 16 pixels. Everything larger is easy; the taskbar is where a
mark either survives or does not.

```powershell
py tools/make-icons.py
```

One set of numbers renders the Windows `.ico`, the Android launcher PNGs and the
brand artwork. The Android adaptive icon and the desktop panel draw the same
geometry themselves — a vector drawable cannot read an SVG, and Direct2D would
rather have two thick lines than a bitmap.

## About "lossless"

Anything sent from the phone to the PC is re-encoded, so a genuinely lossless
live feed is not possible. XCam attacks the problem from two directions:

- Over USB there is no reason to cap the bitrate. At 80–150 Mbit/s the result is
  visually indistinguishable from the source.
- **The recording is a separate encode.** The live stream and the file are two
  encodes of the same frames, so the file is not limited by what the *stream*
  needs to be. It is written on the PC by default and on the phone when the link
  cannot carry both. See below.

## Layout

```
android/                 Gradle project, Kotlin, minSdk 29
  app/src/main/java/com/xcam/
    CaptureService.kt      owns the pipeline, foreground service
    MainActivity.kt        permissions and a start/stop button, nothing more
    camera/                Camera2 session and capability enumeration
    codec/                 MediaCodec surface encoder, the local recorder, audio
    net/                   TCP server and the wire protocol
docs/PROTOCOL.md         wire format -- the source of truth for both sides
docs/BRAND.md            the identity: the mark, the palette, and why
brand/                   the mark, and what the icon generator renders from it
tools/make-icons.py      regenerates every icon from one set of numbers
tools/build-android.ps1  builds the APK with the local workarounds applied
tools/dump_stream.py     reference protocol client, also the phase-1 test rig
tools/pull-recordings.ps1  collects the phone's recordings over adb
windows/core/            protocol, adb, decoders, shared sections, settings,
                         network discovery -- everything both binaries need
windows/app/             the preview window and the pro panel
windows/dsfilter/        the virtual camera and microphone, one COM DLL
```

The phone UI is deliberately almost empty: it is a capture device, and every
setting is driven from the desktop over the control channel described in
`docs/PROTOCOL.md`.
