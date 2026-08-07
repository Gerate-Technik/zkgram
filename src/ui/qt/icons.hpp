#pragma once

#include <QColor>
#include <QIcon>
#include <QPixmap>

QT_BEGIN_NAMESPACE
class QPainter;
class QRectF;
QT_END_NAMESPACE

namespace zkgram::ui::qt {

// Every glyph the Qt UI draws for itself - each one a real
// telegram_fork/tdesktop icon asset (Telegram/Resources/icons/...,
// embedded via resources.qrc), not a hand-drawn approximation. An
// earlier version of this file built every glyph from scratch as
// QPainterPath shapes "from memory" - close in spirit but visibly not
// the real silhouette once compared against the actual asset (see
// TODO.md). The source PNG for each is white-on-transparent and
// recolored at runtime (see colorizeMask() in icons.cpp), the same
// mask+palette technique tdesktop's own style::internal::MonoIcon uses
// (Telegram/lib_ui/ui/style/style_core_icon.cpp).
// Each comment names the real tdesktop asset this one loads (path under
// Telegram/Resources/icons/ in telegram_fork), not just what it is used
// for - see icons.cpp's glyphResourcePath().
enum class Glyph {
    Send,      // chat/input_send - composer send button, "stop and send" on the record bar
    Mic,       // chat/input_record - composer button while the field is empty
    Attach,    // chat/input_attach - composer attach (paperclip)
    Photo,     // menu/image - "Photo or Video" in the attach menu
    Document,  // menu/file - "Document" in the attach menu
    Trash,     // menu/delete - discard a recording
    Pause,     // media_pause - pause a recording
    Play,      // media_play - resume a paused recording
    Search,    // top_bar_search - leading glyph inside the two search fields
    Check,     // history_sent - "sent" tick on an outgoing bubble
    Lock,      // chat/mini_lock - encrypted-session indicator on a chat list row
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
