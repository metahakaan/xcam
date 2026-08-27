# XCam — identity

The written source for everything the product looks like. The desktop panel had
a considered look for a long time before this file existed, which meant nothing
else in the project could follow it. Now `windows/app/ui/theme.h`,
`android/.../res/values/colors.xml` and the icon generator all carry the same
numbers, and this says why.

---

## What it is

**A phone already has the best sensor in the house. XCam makes it available to
the desktop without giving up what makes it good.**

That sentence decides more than it looks like. It is why the recording is a
separate encode rather than a copy of the stream, why the panel exposes ISO and
shutter rather than a "quality" slider, and why every number in the README is a
measurement rather than a claim.

The name is not a free choice any more. It is the DirectShow device name people
have already selected inside applications, the `com.xcam` package id, and the
`Local\XCam*` shared sections. Changing it would orphan every registration on
every machine that has ever run it.

---

## The mark

An **aperture X**: four bars on the diagonals, stopping short of the centre so a
small opening is left between them. The shape a diaphragm makes, not the one a
close button makes — and that gap is the whole difference.

The upper-right arm is Signal amber. It is the tally light: the one place in the
identity where a colour means *this is live*.

![the mark](../brand/xcam-mark-512.png)

**Geometry**, on a 1024 grid, fixed once because three renderers have to agree:

| | |
|---|---|
| Bar width | 146, round caps |
| Outer corners | 176 and 848 |
| Arm ends | 120 from the centre |
| Tile corner radius | 22% of the tile |

`brand/xcam-mark.svg` is the readable source. `tools/make-icons.py` draws the
same numbers with Pillow and emits the Windows `.ico` and the Android PNGs; the
Android vector drawables carry them a third time, because a vector drawable
cannot read an SVG.

It was drawn for **16 pixels**. Everything above that is easy; the taskbar and
the tab strip are where a mark either survives or does not, which is why the
bars are heavy, the angles are 45°, and there is no detail that disappears
first.

**Do not**: recolour the arms, add a third colour, put the mark on a light
ground without its tile, or stretch it. If it needs to be smaller, make it
smaller.

---

## Wordmark

`XCAM`, uppercase, tracked out — the way a camera body is labelled rather than
the way an app is branded. The mark sits to its left at the same height as the
capitals.

---

## Colour

Dark glass and one accent. Lifted from the panel rather than invented for this
document, and now named so both platforms can say the same thing.

| Token | Hex | What it is for |
|---|---|---|
| **Ink** | `#0E1114` | the ground everything sits on |
| **Graphite** | `#171B20` | raised surfaces: cards, panels |
| **Signal** | `#FFB020` | the accent — selection, the tally, the primary action |
| **Record** | `#FF3B30` | a recording in progress. Nothing else, ever. |
| **Good** | `#5BD6A0` | a measured state that is fine |
| **Warn** | `#FF6B5B` | a measured state that is not |
| **Text** | `#F2F5F7` | |
| **Dim** | `#8A949E` | labels, secondary text, an unsupported control |

The rule that keeps this from becoming decoration:

> **Signal marks what is in hand. Record marks what cannot be undone.**

There is no third accent, and neither of these two is ever used to make
something look nice. A panel that uses amber for emphasis has nothing left to
say when the user actually picks something up.

Dark is not a style choice here. The panel floats over a live picture, and
anything lighter would compete with the thing it is describing.

---

## Type

System faces, on purpose. The project's one hard property is that it builds from
a clean checkout with nothing but MSVC and the Windows SDK, and a bundled font
would be the first exception to that.

| | |
|---|---|
| Windows | Segoe UI Variable Display, falling back to Segoe UI on Windows 10 |
| Android | Roboto |
| Numbers that are read as data | monospace — an address, a command, a frame rate |

Labels are small, uppercase and tracked out. Values are larger and plain. That
pairing is what makes a row read as *ISO — 400* rather than as two words.

---

## Voice

The same voice as the README and the commit messages:

- **State the measurement.** "156 Mbit/s with 0 gaps", not "blazing fast".
- **Name the trade.** A take costs about 15ms of latency. Say so; do not hide it
  and do not apologise for it.
- **Explain the failure, not just the fix.** "Android does not refuse a
  microphone it will not give you" is worth more than "fixed audio bug".
- **Do not sell.** Nobody reading this needs convincing; they need to know what
  it does and what it costs.

Turkish and English both appear in this project. The product surfaces are
English because the protocol, the code and the device names are; conversation is
whatever the person prefers.

---

## Where each thing lives

| | |
|---|---|
| `brand/xcam-mark.svg` | the mark, readable source |
| `brand/xcam-mark-{512,1024}.png` | rendered, with tile |
| `brand/xcam-mark-1024-bare.png` | rendered, no tile |
| `tools/make-icons.py` | regenerates every icon from the geometry above |
| `windows/app/xcam.ico` | the Windows icon, checked in |
| `windows/app/ui/theme.h` | the palette, in code |
| `android/.../res/values/colors.xml` | the same palette, in code |
| `android/.../res/drawable/ic_launcher_*.xml` | the mark as vectors |

Run `py tools/make-icons.py` after changing the geometry. The `.ico` and the
PNGs are checked in so that a build never depends on Python being installed.
