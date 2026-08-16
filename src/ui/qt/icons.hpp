#pragma once

#include <QIcon>
#include <QPixmap>

QT_BEGIN_NAMESPACE
class QPainter;
class QRectF;
QT_END_NAMESPACE

namespace zkgram::ui::qt {

// Every glyph the Qt UI draws for itself. Phase 3 of the UI.md migration
// (see zkgram/.claude/UI.md, section 13c): each of these is now a real
// codegen_style icon {{ }} declaration in src/ui/qt/zkgram_icons.style,
// referencing the same real tdesktop asset as before (see that file's
// per-entry comments for exact Resources/icons/ paths) - the color is
// baked in at that .style declaration (a palette role, not a QColor passed
// at the call site), matching UI.md section 6's "icon {{ }}" convention.
// The GlyphIconEngine/paintGlyph/colorizeMask hand-rolled recoloring this
// file used to contain is gone: st::someIcon.paint(...) recolors for free.
enum class Glyph {
    Send,
    Mic,
    Attach,
    Photo,
    Document,
    Trash,
    Pause,
    Play,
    Search,
    Check,
    Lock,
    // White variant of Lock for a selected sidebar row or the secret-mode
    // toggle button - see zkgram_icons.style's zkgramIconLockOnActive
    // comment. Not a runtime QColor override (colors are baked into the
    // compiled icon, see UI.md section 6): a real second icon {{ }} entry.
    LockOnActive,
};

// Direct paint, centred in rect and scaled to fit - for the message-list/
// sidebar paintEvent() code (HistoryCanvas/SidebarCanvas, still pre-migration
// hand-painted widgets as of this phase, see UI.md section 13c) that draws
// glyphs straight into a QPainter rather than going through a QIcon.
void paintGlyph(QPainter& painter, const QRectF& rect, Glyph glyph);

// A QIcon that re-renders the glyph at whatever size and device pixel ratio
// Qt asks for, via the compiled st:: icon for this glyph.
QIcon glyphIcon(Glyph glyph);

// One-off pixmap for the places that hold a QPixmap rather than a QIcon
// (a QLabel, say). size is in logical pixels; devicePixelRatio defaults to
// the ratio of the primary screen.
QPixmap glyphPixmap(Glyph glyph, int size, qreal devicePixelRatio = 0.0);

}  // namespace zkgram::ui::qt
