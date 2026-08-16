/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
// Adapted for zkgram: see cached_round_corners.cpp for what was trimmed and
// why (the CachedRoundCorners enum cache belongs to widgets zkgram does not
// have).
#pragma once

#include "ui/rect_part.h"
#include "ui/style/style_core_color.h"
#include "ui/style/style_core_types.h" // style::color = internal::Color
#include <QtGui/QPixmap>

enum class ImageRoundRadius;

namespace Ui {

struct CornersPixmaps {
	QPixmap p[4];
};

[[nodiscard]] CornersPixmaps PrepareCornerPixmaps(
	int radius,
	style::color bg,
	const style::color *sh = nullptr);
[[nodiscard]] CornersPixmaps PrepareCornerPixmaps(
	ImageRoundRadius radius,
	style::color bg,
	const style::color *sh = nullptr);
[[nodiscard]] CornersPixmaps PrepareInvertedCornerPixmaps(
	int radius,
	style::color bg);

enum class CachedCornerRadius {
	Small,
	ThumbSmall,
	ThumbLarge,
	BubbleSmall,
	BubbleLarge,

	kCount,
};
[[nodiscard]] int CachedCornerRadiusValue(CachedCornerRadius tag);

} // namespace Ui
