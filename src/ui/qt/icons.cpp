#include "ui/qt/icons.hpp"

#include <QGuiApplication>
#include <QHash>
#include <QIconEngine>
#include <QPainter>
#include <QRectF>
#include <QScreen>

namespace zkgram::ui::qt {
namespace {

// Path under resources.qrc for each glyph's real tdesktop asset - see
// icons.hpp's own per-glyph comment for which Telegram/Resources/icons/
// file this is copied from.
QString glyphResourcePath(Glyph glyph) {
    switch (glyph) {
    case Glyph::Send:
        return ":/icons/send.png";
    case Glyph::Mic:
        return ":/icons/mic.png";
    case Glyph::Attach:
        return ":/icons/attach.png";
    case Glyph::Photo:
        return ":/icons/photo.png";
    case Glyph::Document:
        return ":/icons/document.png";
    case Glyph::Trash:
        return ":/icons/trash.png";
    case Glyph::Pause:
        return ":/icons/pause.png";
    case Glyph::Play:
        return ":/icons/play.png";
    case Glyph::Search:
        return ":/icons/search.png";
    case Glyph::Check:
        return ":/icons/check.png";
    case Glyph::Lock:
        return ":/icons/lock.png";
    }
    return {};
}

// Loaded once per glyph, not once per paint call - the source PNGs are
// the tdesktop-authored master (up to 120x120), not a size ever actually
// painted at, so every real render already goes through
// QImage::scaled() regardless of cache.
const QImage& maskFor(Glyph glyph) {
    static QHash<Glyph, QImage> cache;
    auto it = cache.find(glyph);
    if (it == cache.end()) {
        it = cache.insert(glyph, QImage(glyphResourcePath(glyph)));
    }
    return it.value();
}

// The source PNGs under telegram_fork/tdesktop/Telegram/Resources/icons/
// are NOT already alpha masks - checked directly (Python/Pillow) rather
// than assumed: they are a white shape on a fully OPAQUE black
// background (alpha=255 everywhere, some without an alpha channel at
// all). tdesktop's own build-time codegen turns these designer source
// files into a real alpha-channel mask before compiling them in (see
// MonoIcon::ensureColorizedImage()/CreateIconMask() in
// Telegram/lib_ui/ui/style/style_core_icon.cpp, which reads a pixel's
// mask *opacity* straight from an alpha byte - that codegen step lives
// outside lib_ui and was not available to copy directly), so using the
// designer file's actual alpha channel directly (an earlier version of
// this function did that, via CompositionMode_SourceIn) painted a solid
// colored square instead of the glyph - alpha was 255 everywhere already.
//
// This does the same black-to-alpha conversion by hand: treat each
// pixel's luminance as its opacity (white shape = opaque, black
// background = transparent), same interpretation as a standard
// grayscale mask. Confirmed the source images do carry smooth
// intermediate gray values at their edges (real anti-aliasing, not a
// hard binary cutout), so this preserves smooth edges rather than
// producing jagged ones.
QImage colorizeMask(const QImage& mask, const QColor& color) {
    QImage source = mask.convertToFormat(QImage::Format_ARGB32);
    QImage result(source.size(), QImage::Format_ARGB32);
    for (int y = 0; y < source.height(); ++y) {
        const QRgb* srcLine = reinterpret_cast<const QRgb*>(source.constScanLine(y));
        QRgb* dstLine = reinterpret_cast<QRgb*>(result.scanLine(y));
        for (int x = 0; x < source.width(); ++x) {
            int alpha = qGray(srcLine[x]);
            dstLine[x] = qRgba(color.red(), color.green(), color.blue(), alpha);
        }
    }
    return result;
}

QColor colorForMode(const QColor& base, QIcon::Mode mode) {
    switch (mode) {
    case QIcon::Disabled: {
        QColor faded = base;
        faded.setAlphaF(base.alphaF() * 0.4);
        return faded;
    }
    case QIcon::Active:
        // menuIconFg #a8a8a8 -> menuIconFgOver #999999 in tdesktop's day-blue
        // theme is almost exactly this much darker.
        return base.darker(115);
    case QIcon::Normal:
    case QIcon::Selected:
        break;
    }
    return base;
}

class GlyphIconEngine final : public QIconEngine {
public:
    GlyphIconEngine(Glyph glyph, const QColor& color) : glyph_(glyph), color_(color) {}

    void paint(QPainter* painter, const QRect& rect, QIcon::Mode mode, QIcon::State state) override {
        Q_UNUSED(state);
        paintGlyph(*painter, QRectF(rect), glyph_, colorForMode(color_, mode));
    }

    QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override {
        Q_UNUSED(state);
        if (size.isEmpty()) {
            return QPixmap();
        }
        // size arrives already multiplied by the device pixel ratio (QIcon
        // does that before calling down here) and QIcon puts the ratio back
        // on whatever we return, so filling exactly this many device pixels
        // is what keeps the glyph sharp on a HiDPI screen.
        QPixmap pixmap(size);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        paintGlyph(painter, QRectF(QPointF(0.0, 0.0), QSizeF(size)), glyph_, colorForMode(color_, mode));
        return pixmap;
    }

    QSize actualSize(const QSize& size, QIcon::Mode mode, QIcon::State state) override {
        Q_UNUSED(mode);
        Q_UNUSED(state);
        return size;
    }

    QIconEngine* clone() const override { return new GlyphIconEngine(*this); }

    QString key() const override { return QStringLiteral("zkgram.glyph"); }

private:
    Glyph glyph_;
    QColor color_;
};

}  // namespace

void paintGlyph(QPainter& painter, const QRectF& rect, Glyph glyph, const QColor& color) {
    if (rect.isEmpty() || !color.isValid()) {
        return;
    }
    const QImage& mask = maskFor(glyph);
    if (mask.isNull()) {
        return;
    }
    // Scaled to fit rect, aspect ratio preserved and centered - the
    // source assets are not all the same aspect ratio (a tightly-cropped
    // paperclip is not square the way a padded circular mic icon is),
    // unlike the old QPainterPath version's fixed 24x24 grid assumption.
    QSizeF fitted = QSizeF(mask.size()).scaled(rect.size(), Qt::KeepAspectRatio);
    QRectF target(0, 0, fitted.width(), fitted.height());
    target.moveCenter(rect.center());

    painter.save();
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.drawImage(target, colorizeMask(mask, color));
    painter.restore();
}

QIcon glyphIcon(Glyph glyph, const QColor& color) {
    return QIcon(new GlyphIconEngine(glyph, color));
}

QPixmap glyphPixmap(Glyph glyph, int size, const QColor& color, qreal devicePixelRatio) {
    if (size <= 0) {
        return QPixmap();
    }
    qreal ratio = devicePixelRatio;
    if (ratio <= 0.0) {
        QScreen* screen = QGuiApplication::primaryScreen();
        ratio = screen != nullptr ? screen->devicePixelRatio() : 1.0;
    }
    QPixmap pixmap(QSize(qRound(size * ratio), qRound(size * ratio)));
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    paintGlyph(painter, QRectF(0.0, 0.0, size * ratio, size * ratio), glyph, color);
    painter.end();
    pixmap.setDevicePixelRatio(ratio);
    return pixmap;
}

}  // namespace zkgram::ui::qt
