// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#include "codegen/style/render_svg.h"

#include <QtCore/QFile>
#include <QtGui/QImage>
#include <QtGui/QPainter>
#include <QtSvg/QSvgRenderer>

namespace codegen::style {

int RenderSvg(const Options &options) {
	auto file = QFile(options.renderSvgInput);
	if (!file.open(QIODevice::ReadOnly)) {
		return 1;
	}
	auto svg = QSvgRenderer(file.readAll());
	if (!svg.isValid()) {
		return 1;
	}
	const auto size = options.renderSvgSize;
	auto image = QImage(size, size, QImage::Format_ARGB32_Premultiplied);
	image.fill(Qt::black);

	auto p = QPainter(&image);
	p.setRenderHint(QPainter::Antialiasing);
	p.setRenderHint(QPainter::SmoothPixmapTransform);
	svg.render(&p, QRectF(0, 0, size, size));
	p.end();

	return image.save(options.renderSvgOutput, "PNG") ? 0 : 1;
}

} // namespace codegen::style
