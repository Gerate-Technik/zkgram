#pragma once

#include <QColor>
#include <QIcon>
#include <QPixmap>

QT_BEGIN_NAMESPACE
class QPainter;
class QRectF;
QT_END_NAMESPACE

namespace zkgram::ui::qt {

// Every glyph the Qt UI draws for itself. They are drawn rather than shipped
// as image files for the same reason the old per-icon drawXIcon() helpers in
// main_window.cpp were: the project has no external asset pipeline, and a
// handful of vector paths is cheaper to carry than a set of PNGs at four
// scale factors each. Unlike those helpers, everything here is laid out on
// one shared 24x24 grid with one stroke weight (see kGridSize/kStroke in
// icons.cpp), so the icons read as a family instead of as eight separately
// eyeballed drawings, and each is re-rendered as vectors at whatever size
// and device pixel ratio it is asked for instead of being baked once into a
// fixed 24x24 pixmap that a HiDPI screen then blurs.
enum class Glyph {
    Send,      // composer send button, and "stop and send" on the record bar
    Mic,       // composer button while the field is empty
    Attach,    // composer attach (paperclip)
    Photo,     // "Photo or Video" in the attach menu
    Document,  // "Document" in the attach menu
    Trash,     // discard a recording
    Pause,     // pause a recording
    Play,      // resume a paused recording
    Search,    // leading glyph inside the two search fields
    Check,     // "sent" tick on an outgoing bubble
    Lock,      // encrypted-session indicator on a chat list row
};

// Paints the glyph centred in rect, scaled uniformly to fit, in color.
// Antialiasing is enabled on painter by this call and left enabled.
void paintGlyph(QPainter& painter, const QRectF& rect, Glyph glyph, const QColor& color);

// A QIcon that re-renders the glyph at whatever size and device pixel ratio
// Qt asks for. QIcon::Disabled reduces the colour's alpha and QIcon::Active
// darkens it slightly (tdesktop does the same on hover: menuIconFg #a8a8a8 ->
// menuIconFgOver #999999 in day-blue), so a disabled composer button no
// longer gets Qt's default washed-out greyscale of the pixmap.
QIcon glyphIcon(Glyph glyph, const QColor& color);

// One-off pixmap for the places that hold a QPixmap rather than a QIcon
// (a QLabel, say). size is in logical pixels; devicePixelRatio defaults to
// the ratio of the primary screen.
QPixmap glyphPixmap(Glyph glyph, int size, const QColor& color, qreal devicePixelRatio = 0.0);

}  // namespace zkgram::ui::qt
