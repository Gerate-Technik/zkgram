/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
// Adapted for zkgram: trimmed to the subset ui/chat/chat_style.cpp actually
// calls (PrepareCornerPixmaps by explicit radius, PrepareInvertedCornerPixmaps,
// CachedCornerRadiusValue). The original file's CachedRoundCorners enum cache
// (BoxCorners/MenuCorners/OverviewVideoCorners/... and their FillRoundRect/
// FillRoundShadow/StartCachedCorners/FinishCachedCorners plumbing) belongs to
// widgets zkgram does not have (boxes, overview, media viewer, sticker panel,
// bot keyboard) and pulled in styles/style_layers.h, style_overview.h,
// style_media_view.h and style_chat_helpers.h just to declare it - none of
// that is needed for message bubbles, so it was dropped rather than vendored.
#include "ui/cached_round_corners.h"
#include "ui/chat/chat_style.h"
#include "ui/painter.h"
#include "ui/ui_utility.h"
#include "ui/image/image_prepare.h"
#include "styles/style_chat.h"

namespace Ui {
namespace {

[[nodiscard]] std::array<QImage, 4> PrepareCorners(int32 radius, const QBrush &brush, const style::color *shadow = nullptr) {
	int32 r = radius * style::DevicePixelRatio(), s = st::msgShadow * style::DevicePixelRatio();
	QImage rect(r * 3, r * 3 + (shadow ? s : 0), QImage::Format_ARGB32_Premultiplied);
	rect.fill(Qt::transparent);
	{
		auto p = QPainter(&rect);
		PainterHighQualityEnabler hq(p);

		p.setCompositionMode(QPainter::CompositionMode_Source);
		p.setPen(Qt::NoPen);
		if (shadow) {
			p.setBrush((*shadow)->b);
			p.drawRoundedRect(0, s, r * 3, r * 3, r, r);
		}
		p.setBrush(brush);
		p.drawRoundedRect(0, 0, r * 3, r * 3, r, r);
	}
	auto result = std::array<QImage, 4>();
	result[0] = rect.copy(0, 0, r, r);
	result[1] = rect.copy(r * 2, 0, r, r);
	result[2] = rect.copy(0, r * 2, r, r + (shadow ? s : 0));
	result[3] = rect.copy(r * 2, r * 2, r, r + (shadow ? s : 0));
	return result;
}

} // namespace

CornersPixmaps PrepareCornerPixmaps(int radius, style::color bg, const style::color *sh) {
	auto images = PrepareCorners(radius, bg, sh);
	auto result = CornersPixmaps();
	for (int j = 0; j < 4; ++j) {
		result.p[j] = PixmapFromImage(std::move(images[j]));
		result.p[j].setDevicePixelRatio(style::DevicePixelRatio());
	}
	return result;
}

CornersPixmaps PrepareCornerPixmaps(ImageRoundRadius radius, style::color bg, const style::color *sh) {
	switch (radius) {
	case ImageRoundRadius::Small:
		return PrepareCornerPixmaps(st::roundRadiusSmall, bg, sh);
	case ImageRoundRadius::Large:
		return PrepareCornerPixmaps(st::roundRadiusLarge, bg, sh);
	}
	Unexpected("Image round radius in PrepareCornerPixmaps.");
}

CornersPixmaps PrepareInvertedCornerPixmaps(int radius, style::color bg) {
	const auto size = radius * style::DevicePixelRatio();
	auto circle = style::colorizeImage(
		style::createInvertedCircleMask(radius * 2),
		bg);
	circle.setDevicePixelRatio(style::DevicePixelRatio());
	auto result = CornersPixmaps();
	const auto fill = [&](int index, int xoffset, int yoffset) {
		result.p[index] = PixmapFromImage(
			circle.copy(QRect(xoffset, yoffset, size, size)));
	};
	fill(0, 0, 0);
	fill(1, size, 0);
	fill(2, size, size);
	fill(3, 0, size);
	return result;
}

[[nodiscard]] int CachedCornerRadiusValue(CachedCornerRadius tag) {
	using Radius = CachedCornerRadius;
	switch (tag) {
	case Radius::Small: return st::roundRadiusSmall;
	case Radius::ThumbSmall: return MsgFileThumbRadiusSmall();
	case Radius::ThumbLarge: return MsgFileThumbRadiusLarge();
	case Radius::BubbleSmall: return BubbleRadiusSmall();
	case Radius::BubbleLarge: return BubbleRadiusLarge();
	}
	Unexpected("Radius tag in CachedCornerRadiusValue.");
}

} // namespace Ui
