#pragma once

#include <QColor>
#include <QImage>
#include <QPointF>
#include <QSize>
#include <array>

// Real Telegram's default wallpaper is not a QLinearGradient: it is a
// freeform mesh gradient (inverse-distance interpolation between 4 moving
// color points, see GenerateFreeformGradient's own comment) with a tinted
// doodle pattern layered on top. HistoryCanvas's previous background was a
// flat QColor("#74B4E0") fill plus an untinted tiled PNG - a placeholder,
// not an attempt at the real thing. This is that real thing.
namespace zkgram::ui::qt {

using GradientColors = std::array<QColor, 4>;
using GradientPoints = std::array<QPointF, 4>;

// Renders a freeform mesh gradient at size (physical pixels - caller sets
// devicePixelRatio on the result if drawing at a scale, see ChatBackground)
// from 4 colors positioned at points (normalized 0..1 canvas coordinates).
// Internally computed in a small buffer and upscaled - see the .cpp for why
// computing this per-pixel at full window resolution is not viable.
QImage GenerateFreeformGradient(const GradientColors& colors, const GradientPoints& points, QSize size);

// The 4 active control point positions for animation phase (any int,
// wrapped mod 8) - phase 0 is the resting/default state. Consecutive
// phases share half their points and rotate the other half one step
// around a fixed 8-position orbit, which is what gives the background its
// "breathing" look as phase advances (see chat_background.cpp's
// kOrbit comment) rather than a jarring recompute.
GradientPoints GradientPointsForPhase(int phase);

}  // namespace zkgram::ui::qt
