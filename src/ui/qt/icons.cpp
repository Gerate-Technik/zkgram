#include "ui/qt/icons.hpp"

// lib_ui's own .cpp files get these implicitly through its PCH (ui/ui_pch.h);
// a downstream consumer like this file, which is not part of that PCH, needs
// base/basic_types.h explicitly before any lib_ui style header - it is what
// defines float64 and friends that style_core_scale.h/style_core_font.h use.
#include "base/basic_types.h"

#include "styles/style_zkgram_icons.h"

#include <QGuiApplication>
#include <QIconEngine>
#include <QPainter>
#include <QScreen>

namespace zkgram::ui::qt {
namespace {

const style::icon& iconFor(Glyph glyph) {
    switch (glyph) {
    case Glyph::Send:
        return st::zkgramIconSend;
    case Glyph::Mic:
        return st::zkgramIconMic;
    case Glyph::Attach:
        return st::zkgramIconAttach;
    case Glyph::Photo:
        return st::zkgramIconPhoto;
    case Glyph::Document:
        return st::zkgramIconDocument;
    case Glyph::Trash:
        return st::zkgramIconTrash;
    case Glyph::Pause:
        return st::zkgramIconPause;
    case Glyph::Play:
        return st::zkgramIconPlay;
    case Glyph::Search:
        return st::zkgramIconSearch;
    case Glyph::Check:
        return st::zkgramIconCheck;
    case Glyph::Lock:
        return st::zkgramIconLock;
    case Glyph::LockOnActive:
        return st::zkgramIconLockOnActive;
    }
    return st::zkgramIconSend;
}

// QIcon/QIconEngine only exists here as a bridge so the still-unconverted
// QPushButton-based RippleButton (Phase 5, not done yet) can keep taking a
// QIcon. Once buttons move to Ui::IconButton this whole engine goes away -
// that type paints its st:: icon directly, including real hover/disabled
// variants (icon + iconOver, see UI.md section 6), which this bridge only
// approximates (opacity fade for Disabled, no separate Active state, since
// there is no longer a runtime QColor to darken).
class GlyphIconEngine final : public QIconEngine {
public:
    explicit GlyphIconEngine(Glyph glyph) : glyph_(glyph) {}

    void paint(QPainter* painter, const QRect& rect, QIcon::Mode mode, QIcon::State state) override {
        Q_UNUSED(state);
        const auto opacity = (mode == QIcon::Disabled) ? 0.4 : 1.0;
        painter->save();
        painter->setOpacity(painter->opacity() * opacity);
        iconFor(glyph_).paintInCenter(*painter, QRectF(rect));
        painter->restore();
    }

    QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override {
        Q_UNUSED(state);
        if (size.isEmpty()) {
            return QPixmap();
        }
        QPixmap pixmap(size);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        paint(&painter, QRect(QPoint(0, 0), size), mode, state);
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
};

}  // namespace

void paintGlyph(QPainter& painter, const QRectF& rect, Glyph glyph) {
    iconFor(glyph).paintInCenter(painter, rect);
}

QIcon glyphIcon(Glyph glyph) {
    return QIcon(new GlyphIconEngine(glyph));
}

QPixmap glyphPixmap(Glyph glyph, int size, qreal devicePixelRatio) {
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
    iconFor(glyph).paintInCenter(painter, QRectF(0.0, 0.0, size * ratio, size * ratio));
    painter.end();
    pixmap.setDevicePixelRatio(ratio);
    return pixmap;
}

}  // namespace zkgram::ui::qt
