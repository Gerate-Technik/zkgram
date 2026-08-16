/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "ui/rect_part.h"

// Adapted for zkgram: real tdesktop .h files get QRect/QSize/std::vector
// for free from a shared PCH; this file has none, so made explicit here.
#include <QtCore/QRect>
#include <QtCore/QSize>
#include <vector>

namespace Ui {

struct GroupMediaLayout {
	QRect geometry;
	RectParts sides = RectPart::None;
};

std::vector<GroupMediaLayout> LayoutMediaGroup(
	const std::vector<QSize> &sizes,
	int maxWidth,
	int minWidth,
	int spacing);

RectParts GetCornersFromSides(RectParts sides);
QSize GetImageScaleSizeForGeometry(QSize original, QSize geometry);

} // namespace Ui
