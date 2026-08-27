#pragma once

// Every colour and measurement the panel uses, in one place. The look is meant
// to read as camera hardware rather than a settings dialog: dark glass floating
// over the picture, one warm accent, and generous spacing so nothing competes
// with the image underneath.

#include <cstdint>

#include <d2d1.h>

namespace xcam::theme {

inline D2D1_COLOR_F Rgba(uint32_t rgb, float alpha) {
    return D2D1::ColorF(
        ((rgb >> 16) & 0xFF) / 255.0f,
        ((rgb >> 8) & 0xFF) / 255.0f,
        (rgb & 0xFF) / 255.0f,
        alpha);
}

// Panels are translucent so the picture stays the subject. Dark enough that
// white text holds up over a bright frame, light enough not to feel like a
// modal blocking the view.
inline const D2D1_COLOR_F kPanel        = Rgba(0x0E1114, 0.82f);
inline const D2D1_COLOR_F kPanelRaised  = Rgba(0x171B20, 0.92f);
inline const D2D1_COLOR_F kPanelBorder  = Rgba(0xFFFFFF, 0.08f);

inline const D2D1_COLOR_F kText         = Rgba(0xF2F5F7, 1.00f);
inline const D2D1_COLOR_F kTextDim      = Rgba(0x8A949E, 1.00f);
inline const D2D1_COLOR_F kTextFaint    = Rgba(0x8A949E, 0.45f);   // unsupported controls

// One accent, used only for the thing currently in hand. Amber reads as a
// camera indicator and never collides with skin tones in the preview.
inline const D2D1_COLOR_F kAccent       = Rgba(0xFFB020, 1.00f);
inline const D2D1_COLOR_F kAccentSoft   = Rgba(0xFFB020, 0.18f);
inline const D2D1_COLOR_F kWarn         = Rgba(0xFF6B5B, 1.00f);
inline const D2D1_COLOR_F kGood         = Rgba(0x5BD6A0, 1.00f);

// The recording indicator. Deliberately not the accent: a take running is the
// one state that must never be mistaken for a control merely being selected.
inline const D2D1_COLOR_F kRecord       = Rgba(0xFF3B30, 1.00f);
inline const D2D1_COLOR_F kRecordSoft   = Rgba(0xFF3B30, 0.22f);

// The two accents have one rule between them, written down in docs/BRAND.md:
// Signal marks what is in hand, Record marks what cannot be undone. Neither is
// ever decorative, which is why there is no third.

inline const D2D1_COLOR_F kHover        = Rgba(0xFFFFFF, 0.10f);
inline const D2D1_COLOR_F kTrack        = Rgba(0xFFFFFF, 0.14f);

// Control surfaces. A control never fills with the accent, however "on" it is:
// five lit toggles in a row made of accent is a wall of colour that says
// nothing, which is the opposite of what the accent is for. The surface stays
// dark and the state is carried by a small light, the way it is on a mixing
// desk or a camera body.
inline const D2D1_COLOR_F kSurface      = Rgba(0x161B21, 0.92f);
inline const D2D1_COLOR_F kSurfaceHot   = Rgba(0x232A32, 0.96f);
inline const D2D1_COLOR_F kSurfaceOn    = Rgba(0x1D242C, 0.96f);
inline const D2D1_COLOR_F kBorderOn     = Rgba(0xFFB020, 0.50f);

// An indicator that is off is still visible -- an unlit lamp, not an absence.
inline const D2D1_COLOR_F kLedOff       = Rgba(0xFFFFFF, 0.17f);
inline const D2D1_COLOR_F kLedDisabled  = Rgba(0xFFFFFF, 0.07f);

// Metrics
inline constexpr float kPanelRadius   = 14.0f;
// Soft rectangles rather than lozenges. A fully rounded pill reads as a tag,
// and these are controls on an instrument.
inline constexpr float kChipRadius    = 8.0f;
inline constexpr float kPad           = 14.0f;
inline constexpr float kGap           = 8.0f;
inline constexpr float kChipHeight    = 32.0f;
inline constexpr float kRowHeight     = 46.0f;
inline constexpr float kProColumnWide = 108.0f;
inline constexpr float kRulerHeight   = 64.0f;

// Type ramp
inline constexpr float kSizeLabel     = 10.5f;    // uppercase, tracked out
inline constexpr float kSizeValue     = 17.0f;
inline constexpr float kSizeChip      = 12.5f;
inline constexpr float kSizeStat      = 12.0f;

// The panel fades away when the mouse sits still, so the default state of the
// window is just the picture.
inline constexpr double kIdleFadeAfterSeconds = 3.0;
inline constexpr double kFadeDuration         = 0.25;

}  // namespace xcam::theme
