#include "ui/qt/icons.hpp"

#include <QGuiApplication>
#include <QPoint>
#include <QSize>
#include <QIconEngine>
#include <QPainter>
#include <QPainterPath>
#include <QRectF>
#include <QScreen>
#include <QTransform>

namespace zkgram::ui::qt {
namespace {

// The one grid every glyph below is drawn on, and the one stroke weight they
// share. Keeping both constant across the whole set is what makes the icons
// look like a family: before this file each drawXIcon() in main_window.cpp
// picked its own margins (the paperclip reached x=22.5, the trash can stopped
// at 20) and its own visual weight, so in a row of buttons they read as
// different sizes even though every pixmap was 24x24.
constexpr qreal kGridSize = 24.0;
constexpr qreal kStroke = 2.0;
// Secondary detail inside a glyph that already has a 2.0 outline (the ruled
// lines on the document sheet). A full-weight line there competes with the
// outline instead of sitting inside it.
constexpr qreal kDetailStroke = 1.5;

QPen glyphPen(const QColor& color, qreal width = kStroke) {
    return QPen(color, width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
}

// Filled shapes get a thin stroke of their own fill colour on top, which
// rounds off their corners by half the stroke width. Cheaper than building
// every corner arc by hand, and it is what keeps the send arrow and the play
// triangle from looking sharp next to the rounded pause bars.
void fillSoftened(QPainter& painter, const QPainterPath& path, const QColor& color, qreal rounding) {
    painter.setPen(glyphPen(color, rounding));
    painter.setBrush(color);
    painter.drawPath(path);
}

// A paper plane pointing right - the same silhouette as tdesktop's own send
// button icon (historySend in chat_helpers.style), rather than the lopsided
// quadrilateral this used to draw. What makes it read as a folded plane
// instead of an arrowhead is the long thin wedge cut into the trailing edge,
// running almost all the way to the tip; a shallow notch turns the whole
// shape back into a chevron.
void drawSend(QPainter& painter, const QColor& color) {
    QPainterPath plane;
    plane.moveTo(21.6, 12.0);  // tip
    plane.lineTo(2.6, 3.2);    // upper trailing corner
    plane.lineTo(2.6, 9.9);    // wedge, upper edge
    plane.lineTo(17.2, 12.0);  // wedge, inner point
    plane.lineTo(2.6, 14.1);   // wedge, lower edge
    plane.lineTo(2.6, 20.8);   // lower trailing corner
    plane.closeSubpath();
    // Only a light rounding here: the wedge is 4.2 wide where it meets the
    // trailing edge, and a heavier stroke would close it up.
    fillSoftened(painter, plane, color, 0.8);
}

void drawMic(QPainter& painter, const QColor& color) {
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    painter.drawRoundedRect(QRectF(9.2, 2.9, 5.6, 11.0), 2.8, 2.8);

    QPainterPath stand;
    stand.moveTo(5.6, 11.4);
    // Bottom half of a circle centred on the capsule's lower end: in Qt's
    // arc convention 0 degrees is 3 o'clock and positive angles run
    // anticlockwise, so 180 -> 360 is the half that passes under it.
    stand.arcTo(QRectF(5.6, 5.0, 12.8, 12.8), 180.0, 180.0);
    stand.moveTo(12.0, 17.8);
    stand.lineTo(12.0, 20.8);
    stand.moveTo(8.7, 20.8);
    stand.lineTo(15.3, 20.8);
    painter.setPen(glyphPen(color));
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(stand);
}

// A paperclip: two arms joined by a half-circle at the bottom, and a short
// inner stub folded back over the top. Drawn upright so both U-turns can be
// exact half-circles of a known radius, then rotated 45 degrees as a whole -
// the old version approximated the same shape with hand-placed cubic control
// points, which left the two arms visibly non-parallel and the turns lumpy.
void drawAttach(QPainter& painter, const QColor& color) {
    QPainterPath clip;
    clip.moveTo(15.4, 4.2);
    clip.lineTo(15.4, 16.0);
    clip.arcTo(QRectF(8.6, 12.6, 6.8, 6.8), 0.0, -180.0);  // under the bottom
    clip.lineTo(8.6, 6.4);
    clip.arcTo(QRectF(8.6, 4.7, 3.4, 3.4), 180.0, -180.0);  // over the top
    clip.lineTo(12.0, 13.5);

    QTransform lean;
    lean.translate(kGridSize / 2.0, kGridSize / 2.0);
    lean.rotate(45.0);
    lean.translate(-12.0, -11.8);  // the upright clip's own centre of ink

    painter.setPen(glyphPen(color));
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(lean.map(clip));
}

void drawPhoto(QPainter& painter, const QColor& color) {
    const QRectF frame(3.4, 5.2, 17.2, 13.6);
    painter.setPen(glyphPen(color));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(frame, 3.2, 3.2);

    // Sun and hills are filled, and clipped to the inside of the frame so
    // they cannot poke through its rounded corners the way the old stroked
    // zigzag did.
    QPainterPath inside;
    inside.addRoundedRect(frame.adjusted(kStroke / 2.0, kStroke / 2.0, -kStroke / 2.0, -kStroke / 2.0), 2.4, 2.4);
    painter.save();
    painter.setClipPath(inside);
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    painter.drawEllipse(QPointF(8.7, 9.9), 1.7, 1.7);

    QPainterPath hills;
    hills.moveTo(3.4, 18.8);
    hills.lineTo(9.6, 12.2);
    hills.lineTo(13.4, 16.2);
    hills.lineTo(15.8, 13.6);
    hills.lineTo(20.6, 18.8);
    hills.closeSubpath();
    painter.drawPath(hills);
    painter.restore();
}

void drawDocument(QPainter& painter, const QColor& color) {
    painter.setPen(glyphPen(color));
    painter.setBrush(Qt::NoBrush);

    QPainterPath sheet;
    sheet.moveTo(13.9, 3.2);
    sheet.lineTo(6.4, 3.2);
    sheet.lineTo(6.4, 20.8);
    sheet.lineTo(17.6, 20.8);
    sheet.lineTo(17.6, 6.9);
    sheet.closeSubpath();  // the 45-degree cut across the top right corner
    sheet.moveTo(13.9, 3.2);
    sheet.lineTo(13.9, 6.9);
    sheet.lineTo(17.6, 6.9);  // ...and the fold that makes it read as a corner
    painter.drawPath(sheet);

    painter.setPen(glyphPen(color, kDetailStroke));
    painter.drawLine(QPointF(9.4, 12.6), QPointF(14.6, 12.6));
    painter.drawLine(QPointF(9.4, 16.2), QPointF(14.6, 16.2));
}

void drawTrash(QPainter& painter, const QColor& color) {
    QPainterPath can;
    can.moveTo(4.6, 6.8);
    can.lineTo(19.4, 6.8);  // lid
    can.moveTo(9.4, 6.8);
    can.lineTo(9.4, 4.6);
    can.lineTo(14.6, 4.6);
    can.lineTo(14.6, 6.8);  // handle
    can.moveTo(6.6, 6.8);
    can.lineTo(7.6, 20.4);
    can.lineTo(16.4, 20.4);
    can.lineTo(17.4, 6.8);  // tapered body
    can.moveTo(10.6, 10.4);
    can.lineTo(10.6, 16.8);
    can.moveTo(13.4, 10.4);
    can.lineTo(13.4, 16.8);  // ribs
    painter.setPen(glyphPen(color));
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(can);
}

void drawPause(QPainter& painter, const QColor& color) {
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    painter.drawRoundedRect(QRectF(7.7, 4.9, 3.4, 14.2), 1.7, 1.7);
    painter.drawRoundedRect(QRectF(12.9, 4.9, 3.4, 14.2), 1.7, 1.7);
}

void drawPlay(QPainter& painter, const QColor& color) {
    QPainterPath triangle;
    triangle.moveTo(8.9, 5.9);
    triangle.lineTo(17.9, 12.0);
    triangle.lineTo(8.9, 18.1);
    triangle.closeSubpath();
    // Rounded off harder than the send arrow: the play triangle sits right
    // next to the pause bars on the record bar, whose corners are a full
    // 1.7 radius.
    fillSoftened(painter, triangle, color, 2.4);
}

void drawSearch(QPainter& painter, const QColor& color) {
    painter.setPen(glyphPen(color));
    painter.setBrush(Qt::NoBrush);
    painter.drawEllipse(QPointF(10.6, 10.6), 6.0, 6.0);
    painter.drawLine(QPointF(14.9, 14.9), QPointF(20.0, 20.0));
}

void drawCheck(QPainter& painter, const QColor& color) {
    // Slightly heavier than the shared weight on purpose: this one is drawn
    // at ~13px inside a message bubble, not at the 24px the button icons get,
    // and at that size a 2.0 stroke thins out to a hairline.
    painter.setPen(glyphPen(color, 2.4));
    painter.setBrush(Qt::NoBrush);
    QPainterPath tick;
    tick.moveTo(4.6, 12.4);
    tick.lineTo(9.6, 17.4);
    tick.lineTo(19.4, 6.6);
    painter.drawPath(tick);
}

void drawLock(QPainter& painter, const QColor& color) {
    QPainterPath shackle;
    shackle.moveTo(8.4, 10.8);
    shackle.lineTo(8.4, 9.0);
    shackle.arcTo(QRectF(8.4, 5.4, 7.2, 7.2), 180.0, -180.0);  // over the top
    shackle.lineTo(15.6, 10.8);
    painter.setPen(glyphPen(color));
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(shackle);

    // The body is solid rather than outlined: this glyph is used at ~12px on
    // a chat list row, where an outlined body closes up into a smudge.
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    painter.drawRoundedRect(QRectF(5.2, 10.4, 13.6, 9.8), 2.6, 2.6);
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

// Renders the glyph as vectors every time Qt asks for a size, instead of
// handing out one pre-baked 24x24 pixmap that then gets scaled: a 24px icon
// on a 2x screen is a real 48px render here, and a 16px menu icon is drawn at
// 16 rather than downsampled from 24.
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

    // Scalable: whatever was asked for is what will be drawn.
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
    const qreal scale = qMin(rect.width(), rect.height()) / kGridSize;

    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.save();
    painter.translate(rect.center());
    painter.scale(scale, scale);
    painter.translate(-kGridSize / 2.0, -kGridSize / 2.0);

    switch (glyph) {
    case Glyph::Send:
        drawSend(painter, color);
        break;
    case Glyph::Mic:
        drawMic(painter, color);
        break;
    case Glyph::Attach:
        drawAttach(painter, color);
        break;
    case Glyph::Photo:
        drawPhoto(painter, color);
        break;
    case Glyph::Document:
        drawDocument(painter, color);
        break;
    case Glyph::Trash:
        drawTrash(painter, color);
        break;
    case Glyph::Pause:
        drawPause(painter, color);
        break;
    case Glyph::Play:
        drawPlay(painter, color);
        break;
    case Glyph::Search:
        drawSearch(painter, color);
        break;
    case Glyph::Check:
        drawCheck(painter, color);
        break;
    case Glyph::Lock:
        drawLock(painter, color);
        break;
    }

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
