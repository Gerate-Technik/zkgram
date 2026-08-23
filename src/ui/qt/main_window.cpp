#include "ui/qt/main_window.hpp"

#include "monero/monero.h"
#include "ui/qt/chat_background.hpp"
#include "ui/qt/icons.hpp"

// Phase 5 (UI.md 13c): first real lib_ui widget type in this file.
#include "ui/widgets/buttons.h"
// Phase 6b (UI.md 13c): real Ui::PopupMenu for context/attach menus.
#include "ui/widgets/popup_menu.h"
// AuthSlide's card - real composition primitive (see UI.md §4/§7: no Qt
// layout manager), replacing the QVBoxLayout it used to hold its fields in.
#include "ui/wrap/vertical_layout.h"
// Phase 6b (UI.md 13c): real Ui::InputField for the composer/auth fields.
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/fields/masked_input_field.h"
#include "ui/widgets/fields/password_input.h"
#include "ui/widgets/sent_code_field.h"
#include "ui/widgets/labels.h"

#include "styles/style_zkgram_icons.h"

// Phase 6a (UI.md 13c): st::historyPeer*UserpicBg[2] (avatarPixmap()) come
// from lib_ui's own already-generated palette, not from
// style_zkgram_icons.h (which only pulls in ui/style/style_core.h, enough
// for the st:: symbols it declares itself, not the rest of the palette).
#include "styles/palette.h"
#include "styles/style_basic.h"
// st::msgWaveformBar/Skip/Min/Max (paintVoiceWaveform(), ported from real
// Telegram Desktop's HistoryView::PaintWaveform) - declared in
// ui/chat/chat.style, already part of the qt build's generate_styles() list
// (see CMakeLists.txt) but never included from here until now; every other
// st:: symbol this file uses so far came from colors.palette (via
// palette.h above), which covers colors but not plain pixel-size values
// like these.
#include "styles/style_chat.h"
// st::dialogsFilter (chatSearchInput_/searchInput_'s real InputField style,
// see MainWindow's construction site) - declared in dialogs/dialogs.style,
// already part of the qt build's generate_styles() list, not included from
// here until now for the same reason style_chat.h wasn't (see its own
// comment just above).
#include "styles/style_dialogs.h"

// Phase 6b (UI.md 13c): the real message bubble geometry/painting engine,
// copied from telegram_fork/tdesktop's ui/chat/ into src/ui/chat/ (see that
// folder's own file comments for what was trimmed and why) - not a second,
// hand-drawn approximation.
#include "ui/chat/chat_style.h"
#include "ui/chat/message_bubble.h"
#include "ui/chat/message_bar.h"
// style::ConvertScale() - lets the hand-painted HistoryCanvas/SidebarCanvas
// content (never routed through a .style file's own st:: metrics) respond
// to style::Scale() the same way real .style-declared sizes do.
#include "ui/style/style_core_scale.h"
// Phase 6b (UI.md 13c): the real album-mosaic layout algorithm, not a
// hand-rolled grid - see src/ui/grouped_layout.h's own header.
#include "ui/grouped_layout.h"

#include <rpl/rpl.h>

// SidebarCanvas's real scroll container (see its own class comment) - must
// come AFTER <rpl/rpl.h> above: scroll_area.h itself does not include the
// rpl operator headers it uses (rpl::combine_previous specifically), it
// relies on whatever the includer already pulled in. Included before
// rpl.h, this fails to compile with "combine_previous is not a member of
// rpl" - not a real gap in the vendored lib_rpl (rpl/combine_previous.h
// has a complete, correct implementation, and rpl.h already includes it),
// purely an include-order bug on this file's side.
#include "ui/widgets/scroll_area.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
#include <memory>

#include <QEasingCurve>
#include <QMouseEvent>
#include <QVariantAnimation>
#include <QAction>
#include <QAudioInput>
#include <QClipboard>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHash>
#include <QIcon>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QBrush>
#include <QLinearGradient>
#include <QPalette>
#include <QAudioOutput>
#include <QCheckBox>
#include <QEasingCurve>
#include <QEnterEvent>
#include <QMediaCaptureSession>
#include <QMediaFormat>
#include <QMediaPlayer>
#include <QMediaRecorder>
#include <QMenu>
#include <QMessageBox>
#include <QMutex>
#include <QMutexLocker>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QRegularExpression>
#include <QSplitter>
#include <QSplitterHandle>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStyle>
#include <QAbstractTextDocumentLayout>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextLayout>
#include <QTextStream>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

#include "core/session.hpp"

namespace zkgram::ui::qt {

namespace {

// Opened once and kept open rather than reopened per line - same change,
// and for the same reason, as telegram_client.cpp's logDebug(): several of
// these sit on paths that run per chat-list push on the GUI thread, and a
// synchronous open+write+close there is a stall the user can feel.
void logDebug(const QString& message) {
    static QMutex mutex;
    static QFile file(QCoreApplication::applicationDirPath() + "/zkgram_debug.log");
    QMutexLocker locker(&mutex);
    if (!file.isOpen() && !file.open(QIODevice::Append | QIODevice::Text)) {
        return;
    }
    QTextStream stream(&file);
    stream << QDateTime::currentDateTime().toString("HH:mm:ss.zzz") << " [" << QThread::currentThread() << "] "
            << message << "\n";
    stream.flush();
}

// Ui::IconButton/Ui::RoundButton size themselves via a direct resize() call
// in their own constructor/resizeToText() (see lib_ui/ui/widgets/buttons.cpp)
// - neither overrides QWidget::sizeHint(), because real tdesktop never
// places them inside a Qt QLayout at all (UI.md section 4: this codebase's
// own manual-positioning convention exists specifically because RpWidget
// doesn't cooperate with QLayout). zkgram's main_window.cpp does use
// QHBoxLayout/QVBoxLayout throughout, so any such button competing for
// space on a layout's main axis (width in an HBox, height in a VBox) gets
// its size overridden down to whatever QWidget's unset sizeHint()/
// minimumSizeHint() default to (effectively 0) the first time that layout
// actually activates - invisible until then, so this was never caught
// until a row went from hidden to visible at runtime. Locking in the
// already-correct post-construction size via setFixedSize() makes the
// layout respect it instead of overriding it.
void lockButtonSize(QWidget* button) {
    button->setFixedSize(button->size());
}

// A plain QLabel wearing a real Ui::FlatLabel style's values. All three
// labels this is used for (companionLabel_/plainSendWarning_/recordTime_)
// were real Ui::FlatLabel once and were reverted - one crashed inside
// Ui::Text::BlockParser on real chat titles, one went invisible, see
// TODO.md - so they stay QLabel, but their look no longer lives in a
// stylesheet: the font and the text color are read from the very same
// st::zkgram* FlatLabel declarations the FlatLabel version used, so
// reverting the widget type did not also revert to hand-picked hex.
void applyLabelStyle(QLabel* label, const style::FlatLabel& labelStyle) {
    label->setFont(labelStyle.style.font);
    QPalette palette = label->palette();
    palette.setColor(QPalette::WindowText, labelStyle.textFg->c);
    label->setPalette(palette);
}

// Flat panel background, painted from the palette. Replaces the last
// container rules style.qss carried (#central/#sidebar/#header/#composeBar/
// #recordBar/#writeRestrictionBar and the divider) - see TODO.md, the
// lib_ui list's point 6, which held the file open until exactly these moved
// into code. Real Telegram Desktop paints its own panels this way: a
// palette fill in the widget's own paint handler plus an st::lineWidth
// separator in st::shadowFg where two panels meet (Ui::Shadow, and
// HistoryView::TopBarWidget's own bottom line) - not a stylesheet, which
// needs WA_StyledBackground on every container and silently stops applying
// to any widget that paints itself, which is now most of this window.
enum class PanelEdge { None, Bottom, Right };

Ui::RpWidget* createPanel(QWidget* parent, const style::color& bg, PanelEdge edge = PanelEdge::None) {
    auto* panel = new Ui::RpWidget(parent);
    panel->paintRequest(
    ) | rpl::on_next(
        [panel, &bg, edge] {
            QPainter painter(panel);
            painter.fillRect(panel->rect(), bg);
            if (edge == PanelEdge::Bottom) {
                painter.fillRect(0, panel->height() - st::lineWidth, panel->width(), st::lineWidth, st::shadowFg);
            } else if (edge == PanelEdge::Right) {
                painter.fillRect(panel->width() - st::lineWidth, 0, st::lineWidth, panel->height(), st::shadowFg);
            }
        },
        panel->lifetime());
    return panel;
}

// The sidebar/conversation divider. QSplitter's stock handle is drawn by
// QStyle as a wide OS-themed groove; style.qss used to override it with a
// 1px line plus a hover tint, and this keeps exactly that, from the palette
// instead. QSplitterHandle is a plain QWidget (QSplitter::createHandle has
// to return one), so this is the one place here still overriding
// paintEvent/enterEvent directly rather than going through Ui::RpWidget.
class DividerHandle final : public QSplitterHandle {
public:
    DividerHandle(Qt::Orientation orientation, QSplitter* parent) : QSplitterHandle(orientation, parent) {}

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.fillRect(rect(), hovered_ ? st::windowSubTextFg : st::shadowFg);
    }
    void enterEvent(QEnterEvent* event) override {
        hovered_ = true;
        update();
        QSplitterHandle::enterEvent(event);
    }
    void leaveEvent(QEvent* event) override {
        hovered_ = false;
        update();
        QSplitterHandle::leaveEvent(event);
    }

private:
    bool hovered_ = false;
};

class MainSplitter final : public QSplitter {
public:
    MainSplitter(Qt::Orientation orientation, QWidget* parent) : QSplitter(orientation, parent) {
        setHandleWidth(st::lineWidth);
    }

protected:
    QSplitterHandle* createHandle() override {
        return new DividerHandle(orientation(), this);
    }
};

// Every button/menu glyph the window draws now comes from icons.cpp (one
// shared 24x24 grid, one stroke weight, re-rendered as vectors at whatever
// size and device pixel ratio Qt asks for) instead of a drawXIcon() helper
// per glyph baking its own 24x24 pixmap right here. Colors used to be a bare
// QColor passed at each call site (bypassing the palette entirely); they are
// now baked into the compiled icon itself (see zkgram_icons.style), so
// glyphIcon()/glyphPixmap()/paintGlyph() no longer take a color argument.

// Colored circle with the chat's first letter, same idea as tdesktop's own
// userpic placeholder for a peer with no profile photo (Telegram/
// SourceFiles/ui/empty_userpic.cpp: a solid color picked deterministically
// from the peer, plus the first letter of the name) - used as a fallback
// when the chat has no photo yet, or it has not finished downloading (see
// chatAvatarPixmap() below for the real-photo path).
// Physical pixels a size x size pixmap needs to stay crisp on the current
// screen's scaling (125%/150%/200% on Windows is common, not the
// exception) - same fallback glyphPixmap() in icons.cpp already uses.
// avatarPixmap()/chatAvatarPixmap() below did not account for this at
// all (a plain size x size QPixmap, 1 physical pixel per logical one),
// unlike every glyph icon in the app, which is why avatars specifically
// looked soft/pixelated next to everything else on a scaled display.
qreal currentDevicePixelRatio() {
    QScreen* screen = QGuiApplication::primaryScreen();
    return screen != nullptr ? screen->devicePixelRatio() : 1.0;
}

// Phase 6a (UI.md 13c): colors and color-selection algorithm ported
// directly from the real Ui::EmptyUserpic (Telegram/SourceFiles/ui/
// empty_userpic.cpp/.h in telegram_fork) - not the class itself, which
// pulls in ui/chat/chat_style.h, styles/style_chat.style,
// styles/style_dialogs.style and even info/channel_statistics/earn (a
// Telegram Stars-specific icon module used by one of its unrelated static
// paint helpers) just for a gradient circle with an initial. The exact
// numbers below are not invented: DecideColorIndex/ColorIndexToPaletteIndex
// (ui/chat/chat_style.cpp) and the historyPeer[1-8]UserpicBg/Bg2 palette
// pairs used by EmptyUserpic::UserpicColor(), copied verbatim.
uint8_t avatarColorIndex(qlonglong chatId) {
    constexpr uint8_t kSimpleColorIndexCount = 7;
    constexpr int8_t kPaletteIndexMap[] = {0, 7, 4, 1, 6, 3, 5};
    uint8_t simpleIndex = static_cast<uint8_t>(static_cast<quint64>(chatId) % kSimpleColorIndexCount);
    return static_cast<uint8_t>(kPaletteIndexMap[simpleIndex]);
}

// zkgramIconLock and friends already prove a palette color's real QColor is
// reached through operator->()->c (style::color, not QColor, is what the
// codegen'd st:: symbol actually is) - same access pattern here.
const style::color& avatarBgColor1(uint8_t paletteIndex) {
    switch (paletteIndex) {
    case 0:
        return st::historyPeer1UserpicBg;
    case 1:
        return st::historyPeer2UserpicBg;
    case 2:
        return st::historyPeer3UserpicBg;
    case 3:
        return st::historyPeer4UserpicBg;
    case 4:
        return st::historyPeer5UserpicBg;
    case 5:
        return st::historyPeer6UserpicBg;
    case 6:
        return st::historyPeer7UserpicBg;
    default:
        return st::historyPeer8UserpicBg;
    }
}

const style::color& avatarBgColor2(uint8_t paletteIndex) {
    switch (paletteIndex) {
    case 0:
        return st::historyPeer1UserpicBg2;
    case 1:
        return st::historyPeer2UserpicBg2;
    case 2:
        return st::historyPeer3UserpicBg2;
    case 3:
        return st::historyPeer4UserpicBg2;
    case 4:
        return st::historyPeer5UserpicBg2;
    case 5:
        return st::historyPeer6UserpicBg2;
    case 6:
        return st::historyPeer7UserpicBg2;
    default:
        return st::historyPeer8UserpicBg2;
    }
}

// Both avatar caches (the placeholder one in avatarPixmap() and the real-photo
// one in chatAvatarPixmap()) live behind this, rather than being plain
// function-local statics, because of when a function-local static is
// destroyed: at static destruction, which runs after main() has returned and
// therefore after QApplication is already gone. Destroying a QPixmap then is
// undefined - its platform backend died with the application - and it showed
// up as the process aborting with "free(): invalid pointer" *after* a clean
// shutdown, TDLib having already logged "Stop Td"/"Release closed client".
//
// qAddPostRoutine runs the cleanup from ~QCoreApplication instead, while the
// application is still alive, which is the supported moment to be releasing
// pixmaps. Heap-allocated and deleted only from there, so there is no second,
// later destruction to go wrong. GUI thread only (row layout and painting),
// so the lazy init needs no locking.
struct AvatarCaches {
    QHash<QString, QPixmap> placeholders;  // avatarPixmap()
    QHash<QString, QPixmap> photos;        // chatAvatarPixmap()
};

AvatarCaches* g_avatarCaches = nullptr;

AvatarCaches& avatarCaches() {
    if (g_avatarCaches == nullptr) {
        g_avatarCaches = new AvatarCaches();
        qAddPostRoutine([] {
            delete g_avatarCaches;
            g_avatarCaches = nullptr;
        });
    }
    return *g_avatarCaches;
}

// Cached the same way chatAvatarPixmap() caches real photos below, and for
// a stronger reason: right after login *no* avatar has downloaded yet, so
// every single sidebar row falls back to this placeholder, and each one
// meant allocating a pixmap and running a full antialiased
// gradient-ellipse-plus-glyph paint - hundreds of them, repeated from
// scratch on every sidebar rebuild, on the GUI thread. The result depends
// only on the initial letter, the palette index and the pixel geometry
// (never on the chat id itself beyond picking that index), so identical
// rows share one pixmap and a redraw of an unchanged list costs nothing.
// Safe to cache across the process lifetime because the style palette is
// loaded once at startup and never swapped at runtime.
QPixmap avatarPixmap(const QString& title, qlonglong chatId, int size) {
    QString trimmed = title.trimmed();
    QChar initial = trimmed.isEmpty() ? QChar('?') : trimmed.at(0).toUpper();
    uint8_t paletteIndex = avatarColorIndex(chatId);

    qreal ratio = currentDevicePixelRatio();

    QHash<QString, QPixmap>& cache = avatarCaches().placeholders;
    QString cacheKey = QString(initial) + QLatin1Char(':') + QString::number(paletteIndex) + QLatin1Char(':') +
                        QString::number(size) + QLatin1Char('@') + QString::number(ratio);
    auto cached = cache.constFind(cacheKey);
    if (cached != cache.constEnd()) {
        return cached.value();
    }

    int physicalSize = qRound(size * ratio);
    QPixmap pixmap(physicalSize, physicalSize);
    pixmap.fill(Qt::transparent);
    pixmap.setDevicePixelRatio(ratio);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    // Same two real palette colors and the same top-to-bottom gradient
    // EmptyUserpic::paint() uses, not an approximated lighter()/darker() of
    // one hand-picked color.
    QLinearGradient fill(0, 0, 0, size);
    fill.setColorAt(0.0, avatarBgColor1(paletteIndex)->c);
    fill.setColorAt(1.0, avatarBgColor2(paletteIndex)->c);
    painter.setPen(Qt::NoPen);
    painter.setBrush(fill);
    painter.drawEllipse(0, 0, size, size);

    // fontsize = (size * 13) / 33, same ratio EmptyUserpic::paint() uses.
    QFont font = painter.font();
    font.setPixelSize((size * 13) / 33);
    font.setBold(true);
    painter.setFont(font);
    painter.setPen(Qt::white);
    painter.drawText(QRect(0, 0, size, size), Qt::AlignCenter, QString(initial));
    // QPainter must be done with the pixmap before it is handed out/stored.
    painter.end();
    cache.insert(cacheKey, pixmap);
    return pixmap;
}

// The chat's real profile photo, cropped to a circle to match
// avatarPixmap()'s shape - falls back to the colored-initial placeholder
// when photoPath is empty (not downloaded yet, or the chat has no photo at
// all) or fails to load. TDLib gives us a square-ish "small" photo file on
// disk (see TelegramClient::requestChatPhoto); QPainterPath clipping does
// the circular crop, same technique used for every other drawn icon here.
//
// Cached by photoPath: the sidebar can now legitimately hold hundreds of
// rows (see TelegramClient::requestMoreChats() in telegram_client.cpp),
// and every one of those rows gets rebuilt from scratch on every
// throttled sidebar render (see MainWindow::flushSidebarRender()) - a
// decode-from-disk-and-composite for every single one of those rows every
// single time was real, unnecessary repeated work for a photo that had
// already been decoded moments earlier and has not changed.
QPixmap chatAvatarPixmap(const QString& title, qlonglong chatId, const QString& photoPath, int size) {
    if (photoPath.isEmpty()) {
        return avatarPixmap(title, chatId, size);
    }
    qreal ratio = currentDevicePixelRatio();
    QHash<QString, QPixmap>& cache = avatarCaches().photos;
    // Ratio in the key too, not just size: a plausible (if rare) multi-monitor
    // setup with different scale factors would otherwise hand back a pixmap
    // cached for the wrong screen's density.
    QString cacheKey =
        photoPath + QLatin1Char(':') + QString::number(size) + QLatin1Char('@') + QString::number(ratio);
    auto cached = cache.constFind(cacheKey);
    if (cached != cache.constEnd()) {
        return cached.value();
    }

    QPixmap source(photoPath);
    if (source.isNull()) {
        return avatarPixmap(title, chatId, size);
    }
    int physicalSize = qRound(size * ratio);
    QPixmap scaled =
        source.scaled(physicalSize, physicalSize, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    // QPixmap::scaled() does not carry a devicePixelRatio itself (result is
    // physicalSize x physicalSize at ratio 1) - without setting it here,
    // drawPixmap() below would read scaled's physical pixels as that many
    // logical ones and place/size it wrong once ratio != 1.
    scaled.setDevicePixelRatio(ratio);

    QPixmap result(physicalSize, physicalSize);
    result.fill(Qt::transparent);
    result.setDevicePixelRatio(ratio);
    QPainter painter(&result);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    QPainterPath clip;
    clip.addEllipse(0, 0, size, size);
    painter.setClipPath(clip);
    // Center-crop: scaled may be wider or taller than physicalSize x
    // physicalSize (KeepAspectRatioByExpanding only guarantees it covers
    // that on both axes, not that it matches exactly). drawPixmap here
    // takes logical coordinates (result has devicePixelRatio set), so
    // scaled's own physical size has to be converted back to logical
    // before centering, or a HiDPI scaled would be placed as if it were
    // still physicalSize x physicalSize logical and drawn off-center.
    QSizeF scaledLogical = QSizeF(scaled.size()) / ratio;
    painter.drawPixmap(QPointF((size - scaledLogical.width()) / 2, (size - scaledLogical.height()) / 2), scaled);
    painter.end();
    // Unbounded but self-limiting in practice: keyed by a real Telegram
    // account's own chat count, which does not grow without the user
    // adding more chats, unlike e.g. a cache keyed by something
    // per-message.
    cache.insert(cacheKey, result);
    return result;
}

// Real Telegram Desktop's own dialogs row time label (see
// Dialogs::Ui::DialogsLayout - not vendored, this is a same-shape port):
// today shows the clock time, the last 6 days show a weekday name, and
// anything older shows a short date - never a bare "days ago" count.
// SidebarCanvas::setRows() calls this once per chat-list push, not per
// paint (see SidebarRowData::timeText's own comment).
QString sidebarTimeText(qint64 unixSeconds) {
    QDateTime when = QDateTime::fromSecsSinceEpoch(unixSeconds);
    QDate today = QDate::currentDate();
    QDate whenDate = when.date();
    if (whenDate == today) {
        return when.toString("HH:mm");
    }
    if (whenDate.daysTo(today) < 7) {
        return when.toString("ddd");
    }
    return when.toString("dd.MM.yy");
}

// A small filled dot - the blinking "recording" light on the record bar,
// and the connection-state light next to the companion's name. The chat
// list's own encrypted-session indicator used to be one of these too; it is
// a Glyph::Lock now (see SidebarCanvas::paintRow), since a bare green dot
// there said nothing about *what* was active.
QPixmap dotPixmap(int size, const QColor& color) {
    QPixmap pixmap(size, size);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    painter.drawEllipse(0, 0, size, size);
    return pixmap;
}

// The real Telegram Desktop chat background is not one flat image: a
// tiled, mostly-transparent doodle pattern is blended over a solid color
// or gradient with a blend mode and reduced opacity (see
// Data::WallPaper's rendering in telegram_fork/tdesktop's
// data/data_wall_paper.cpp). Painted explicitly in
// HistoryCanvas::paintEvent() below (not via QPalette::Base/
// autoFillBackground - see git history for why that path silently
// stopped painting anything at all once MainWindow got a stylesheet).
//
// Real Telegram's default wallpaper is a freeform mesh gradient (4 moving
// color points, see chat_background.hpp's GenerateFreeformGradient), not a
// flat fill or a QLinearGradient - the two-stop QLinearGradient an earlier
// version of this file used, and the flat #74B4E0 fill that replaced it,
// were both placeholders, not attempts at the real thing.
//
// TODO(zkgram): kWallpaperColors below should be four st:: palette roles
// (zkgramWallpaperColor1..4), not raw QColor literals - see UI.md's own
// "no literal colors in code" rule, already violated once by the #74B4E0
// fill this replaces. Left as named constants for now rather than adding
// new .style declarations in the same pass as everything else changing
// here; tracked in TODO.md, not silently accepted as finished.
// Tightened to a narrower purple/lavender range (previous four colors
// were also nominally purple, but spanned enough hue/lightness that the
// gradient's outer, low-weight regions read as a colder, greener-looking
// mix once blended - a side effect of inverse-distance interpolation
// between points that were not close enough together). Kept the same 4
// distinct control points (still a real mesh gradient, not a flat fill),
// just clustered tighter in hue.
constexpr GradientColors kWallpaperColors = {
    QColor(0xE6, 0xDC, 0xF7),
    QColor(0xC7, 0xB3, 0xEE),
    QColor(0xDA, 0xCC, 0xF5),
    QColor(0xB8, 0xA0, 0xE8),
};

QImage tintedWallpaperPattern() {
    static const QImage pattern = [] {
        QImage raw(":/chat_wallpaper.png");
        if (raw.isNull()) {
            return QImage();
        }
        // Tinted as an alpha mask (SourceIn keeps the mask's own alpha,
        // replaces its color), not drawn as-is: real Telegram's doodle
        // pattern is a monochrome mask tonally matched to whatever
        // background sits under it, not a fixed-color image layered on
        // top regardless of theme.
        QImage tinted(raw.size(), QImage::Format_ARGB32_Premultiplied);
        tinted.fill(Qt::transparent);
        QPainter tp(&tinted);
        tp.drawImage(0, 0, raw);
        tp.setCompositionMode(QPainter::CompositionMode_SourceIn);
        tp.fillRect(tinted.rect(), QColor(0, 0, 0));
        return tinted;
    }();
    return pattern;
}

// Caches the generated gradient (+ pattern composited onto it) and only
// regenerates when the viewport size actually changes - GenerateFreeformGradient
// itself is cheap (a 64x64 buffer), but the per-pixel dithering pass over
// the final upscaled image is not free, and doing either on every single
// paintEvent (the previous QLinearGradient-based code's own approach,
// which was fine for a cheap gradient fill) would make scrolling visibly
// stutter.
class ChatBackground {
public:
    void setSize(QSize size, qreal devicePixelRatio) {
        if (size == size_ && devicePixelRatio == devicePixelRatio_) {
            return;
        }
        size_ = size;
        devicePixelRatio_ = devicePixelRatio;
        cacheValid_ = false;
    }

    void paint(QPainter& painter, const QRect& viewportRect) {
        ensureCached();
        if (!cache_.isNull()) {
            painter.drawImage(viewportRect, cache_);
        }
    }

private:
    void ensureCached() {
        if (cacheValid_ || size_.isEmpty()) {
            return;
        }
        // Generated at physical-pixel size with devicePixelRatio stamped
        // on the result - the same DPI-mismatch bug already fixed for
        // bubble corner assets (see ui_integration.cpp's own comment)
        // would otherwise blur this too on a fractional-scale display.
        QSize physicalSize = size_ * devicePixelRatio_;
        cache_ = GenerateFreeformGradient(kWallpaperColors, GradientPointsForPhase(0), physicalSize);
        cache_.setDevicePixelRatio(devicePixelRatio_);
        QImage pattern = tintedWallpaperPattern();
        if (!pattern.isNull()) {
            QPainter p(&cache_);
            p.setRenderHint(QPainter::SmoothPixmapTransform);
            p.setOpacity(0.16);
            p.drawTiledPixmap(QRect(QPoint(0, 0), size_), QPixmap::fromImage(pattern));
        }
        cacheValid_ = true;
    }

    QSize size_;
    qreal devicePixelRatio_ = 1.0;
    QImage cache_;
    bool cacheValid_ = false;
};

// Fullscreen photo viewer - real Telegram opens one of these on clicking
// any photo in a chat (a dimmed overlay, the image centered and scaled to
// fit, arrows/swipe between an album's other photos, Esc or a click
// outside the image to close). This covers the single-image case and
// album navigation between the photos the click's own bubble already has
// loaded; it does not fetch anything over the network itself, and there
// is no zoom/pan (real Telegram has both) - a real, working first version
// of the feature, not the complete thing.
class MediaViewer : public Ui::RpWidget {
public:
    explicit MediaViewer(QWidget* parent) : Ui::RpWidget(parent) {
        setAttribute(Qt::WA_StyledBackground, true);
        setFocusPolicy(Qt::StrongFocus);
        hide();
    }

    void open(const QVector<QPixmap>& images, int startIndex) {
        if (images.isEmpty()) {
            return;
        }
        images_ = images;
        index_ = qBound(0, startIndex, images_.size() - 1);
        if (parentWidget() != nullptr) {
            setGeometry(parentWidget()->rect());
        }
        show();
        raise();
        setFocus();
        update();
    }

    void close() {
        hide();
        images_.clear();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        if (images_.isEmpty()) {
            return;
        }
        QPainter painter(this);
        painter.fillRect(rect(), QColor(0, 0, 0, 220));
        const QPixmap& pixmap = images_[index_];
        QSize maxSize = size() - QSize(style::ConvertScale(80), style::ConvertScale(80));
        QPixmap scaled = pixmap.scaled(maxSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        imageRect_ = QRect(QPoint((width() - scaled.width()) / 2, (height() - scaled.height()) / 2), scaled.size());
        painter.drawPixmap(imageRect_, scaled);
        if (images_.size() > 1) {
            painter.setPen(Qt::white);
            QFont font = QGuiApplication::font();
            font.setPixelSize(style::ConvertScale(13));
            painter.setFont(font);
            QRect counterRect(0, height() - style::ConvertScale(36), width(), style::ConvertScale(24));
            painter.drawText(counterRect, Qt::AlignCenter, QString("%1 / %2").arg(index_ + 1).arg(images_.size()));
        }
    }

    void mousePressEvent(QMouseEvent* event) override {
        // Clicking the image itself does nothing (no zoom yet, see this
        // class's own comment) - only clicking OUTSIDE it closes, matching
        // real Telegram's "click away to dismiss".
        if (!imageRect_.contains(event->pos())) {
            close();
        }
    }

    void keyPressEvent(QKeyEvent* event) override {
        switch (event->key()) {
            case Qt::Key_Escape:
                close();
                return;
            case Qt::Key_Left:
                if (index_ > 0) {
                    --index_;
                    update();
                }
                return;
            case Qt::Key_Right:
                if (index_ + 1 < images_.size()) {
                    ++index_;
                    update();
                }
                return;
            default:
                break;
        }
        Ui::RpWidget::keyPressEvent(event);
    }

    void resizeEvent(QResizeEvent*) override {
        if (parentWidget() != nullptr) {
            setGeometry(parentWidget()->rect());
        }
    }

private:
    QVector<QPixmap> images_;
    int index_ = 0;
    QRect imageRect_;
};

// Hamburger button, leftmost in headerRow_ - opens mainMenuPanel_ below.
// Hand-painted (three lines) rather than a real .style icon{{}} asset: no
// existing Resources/icons/ source was verified for this glyph the way
// icons.cpp's existing set was (see icons.hpp's own comment on that
// convention), and a plain three-bar glyph does not need one to look right.
class HamburgerButton : public Ui::RpWidget {
public:
    explicit HamburgerButton(QWidget* parent) : Ui::RpWidget(parent) {
        setFixedSize(style::ConvertScale(32), style::ConvertScale(32));
        setCursor(Qt::PointingHandCursor);
    }

    std::function<void()> onClicked;

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        if (hovered_) {
            painter.setBrush(QColor(0, 0, 0, 20));
            painter.setPen(Qt::NoPen);
            painter.drawEllipse(rect());
        }
        painter.setPen(QPen(QColor(90, 90, 90), style::ConvertScale(2), Qt::SolidLine, Qt::RoundCap));
        int lineWidth = style::ConvertScale(16);
        int left = (width() - lineWidth) / 2;
        int spacing = style::ConvertScale(6);
        int centerY = height() / 2;
        for (int i = -1; i <= 1; ++i) {
            int y = centerY + i * spacing;
            painter.drawLine(left, y, left + lineWidth, y);
        }
    }
    // Ui::RpWidgetBase::enterEvent()/leaveEvent() are both `final`
    // (rp_widget.h) - enterEventHook()/leaveEventHook() are the real
    // extension points, same fix already applied to SidebarCanvas's own
    // leaveEvent override earlier this session (that one didn't override
    // enterEvent at all, so only leaveEvent needed it there).
    void enterEventHook(QEnterEvent*) override {
        hovered_ = true;
        update();
    }
    void leaveEventHook(QEvent*) override {
        hovered_ = false;
        update();
    }
    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton && onClicked) {
            onClicked();
        }
    }

private:
    bool hovered_ = false;
};

// Dimmed backdrop behind mainMenuPanel_ below - click anywhere on it closes
// the menu, same "click away to dismiss" idiom as MediaViewer above.
class MainMenuDimmer : public Ui::RpWidget {
public:
    explicit MainMenuDimmer(QWidget* parent) : Ui::RpWidget(parent) { hide(); }

    std::function<void()> onClicked;

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.fillRect(rect(), QColor(0, 0, 0, 120));
    }
    void mousePressEvent(QMouseEvent*) override {
        if (onClicked) {
            onClicked();
        }
    }
    void resizeEvent(QResizeEvent*) override {
        if (parentWidget() != nullptr) {
            setGeometry(parentWidget()->rect());
        }
    }
};

// The slide-out panel itself - a plain white sheet hosting a QVBoxLayout
// of ordinary child widgets (labels, MenuRow below), not hand-painted
// content of its own; only the flat background fill needs a paintEvent,
// since this codebase does not rely on QPalette::Base/autoFillBackground
// for backgrounds (see HistoryCanvas's own comment on why that path is
// avoided - it silently stops painting once a stylesheet is involved).
class MainMenuPanel : public Ui::RpWidget {
public:
    explicit MainMenuPanel(QWidget* parent) : Ui::RpWidget(parent) {}

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.fillRect(rect(), QColor(255, 255, 255));
    }
};

// One clickable row in mainMenuPanel_ ("My Profile", "Wallet", "New Group",
// ...) - hand-painted rather than a QPushButton: this codebase avoids
// setStyleSheet entirely (style.qss is gone - every widget here paints
// itself from the palette now, see createPanel), and a flat QPushButton with a
// left-aligned icon+label needs exactly that to look right. Mirrors
// RippleButton-era widgets elsewhere in spirit (hover highlight, a
// std::function click callback) without the ripple animation itself -
// not worth the extra complexity for a simple menu row.
class MenuRow : public Ui::RpWidget {
public:
    MenuRow(QWidget* parent, QString label, QString subtitle = QString())
        : Ui::RpWidget(parent), label_(std::move(label)), subtitle_(std::move(subtitle)) {
        // Real Telegram Desktop's own window/window_main_menu.style:
        // mainMenu's itemPadding is margins(76px, 13px, 28px, 13px) - 13px
        // top+bottom around a semiboldTextStyle (13px) label gives ~46px
        // for one line; this has no icon column (see this class's own
        // comment on why), so it is not the full 76px left inset, but the
        // vertical rhythm now matches instead of the earlier hand-picked
        // 42/52px.
        setFixedHeight(style::ConvertScale(subtitle_.isEmpty() ? 46 : 58));
        setCursor(Qt::PointingHandCursor);
    }

    std::function<void()> onClicked;

    void setSubtitle(const QString& subtitle) {
        subtitle_ = subtitle;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        if (hovered_) {
            // Real windowBgOver (#f1f1f1, see colors.palette) - a plain
            // opaque fill, not a translucent black tint (menu row hover in
            // real Telegram is not accent-colored, unlike the purple this
            // theme uses for actually-active/selected state elsewhere).
            painter.fillRect(rect(), QColor(0xf1, 0xf1, 0xf1));
        }
        int left = style::ConvertScale(26);
        QFont font = QGuiApplication::font();
        // Real mainMenu.itemStyle is semiboldTextStyle (13px semibold) -
        // this toolkit has no semibold weight readily available, bold at
        // the real 13px size is the closer match, not the earlier 15px.
        font.setPixelSize(style::ConvertScale(13));
        font.setBold(true);
        painter.setFont(font);
        painter.setPen(QColor(30, 30, 30));
        if (subtitle_.isEmpty()) {
            painter.drawText(QRect(left, 0, width() - left, height()), Qt::AlignVCenter | Qt::AlignLeft, label_);
        } else {
            painter.drawText(QRect(left, style::ConvertScale(8), width() - left, style::ConvertScale(20)),
                              Qt::AlignVCenter | Qt::AlignLeft, label_);
            QFont subFont = font;
            subFont.setBold(false);
            subFont.setPixelSize(style::ConvertScale(12));
            painter.setFont(subFont);
            painter.setPen(QColor(140, 140, 140));
            painter.drawText(QRect(left, height() - style::ConvertScale(26), width() - left, style::ConvertScale(18)),
                              Qt::AlignVCenter | Qt::AlignLeft, subtitle_);
        }
    }
    // Ui::RpWidgetBase::enterEvent()/leaveEvent() are both declared
    // `final` (rp_widget.h) - enterEventHook()/leaveEventHook() are the
    // real extension points, same fix already applied to SidebarCanvas's
    // own leaveEvent override earlier this session (see its own comment).
    void enterEventHook(QEnterEvent*) override {
        hovered_ = true;
        update();
    }
    void leaveEventHook(QEvent*) override {
        hovered_ = false;
        update();
    }
    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton && onClicked) {
            onClicked();
        }
    }

private:
    QString label_;
    QString subtitle_;
    bool hovered_ = false;
};

// A rounded rectangle path with an independent radius per corner - plain
// QPainterPath::addRoundedRect() only takes one radius for the whole
// shape, but a message bubble needs each corner to potentially differ
// (see paintBubbleBody()/HistoryCanvas below).
QPainterPath roundedRectPath(const QRectF& rect, double topLeft, double topRight, double bottomRight,
                              double bottomLeft) {
    QPainterPath path;
    path.moveTo(rect.left() + topLeft, rect.top());
    path.lineTo(rect.right() - topRight, rect.top());
    if (topRight > 0.01) {
        path.arcTo(rect.right() - 2 * topRight, rect.top(), 2 * topRight, 2 * topRight, 90, -90);
    }
    path.lineTo(rect.right(), rect.bottom() - bottomRight);
    if (bottomRight > 0.01) {
        path.arcTo(rect.right() - 2 * bottomRight, rect.bottom() - 2 * bottomRight, 2 * bottomRight, 2 * bottomRight,
                   0, -90);
    }
    path.lineTo(rect.left() + bottomLeft, rect.bottom());
    if (bottomLeft > 0.01) {
        path.arcTo(rect.left(), rect.bottom() - 2 * bottomLeft, 2 * bottomLeft, 2 * bottomLeft, -90, -90);
    }
    path.lineTo(rect.left(), rect.top() + topLeft);
    if (topLeft > 0.01) {
        path.arcTo(rect.left(), rect.top(), 2 * topLeft, 2 * topLeft, 180, -90);
    }
    path.closeSubpath();
    return path;
}

// Phase 6b (UI.md 13c): Ui::ChatStyle owns the real in/out message palette
// (backgrounds, tail icons, shadow, pre-rendered corner pixmaps) - built
// once from the live global palette (style::main_palette), not per-chat
// theming/wallpaper (zkgram has one fixed theme, see UI.md 13b), so a
// single process-lifetime instance is enough. Constructed lazily (first
// paint), well after InstallUiRuntime()'s style::StartManager() call.
Ui::ChatStyle& sharedChatStyle() {
    static Ui::ChatStyle style(style::main_palette::get());
    return style;
}

// Maps this codebase's own tailOnLeft/showTail/joinedTop flags (see
// buildItem()/paintItem() below) onto the real Ui::BubbleRounding the
// engine expects: Corner::Tail on the near-bottom corner tells
// Ui::PaintBubble to leave room there for the real tail icon instead of a
// rounded corner, Corner::Small there instead means "attached to the next
// bubble of the same kind, no tail this row".
Ui::BubbleRounding bubbleRounding(bool tailOnLeft, bool showTail, bool joinedTop) {
    using Corner = Ui::BubbleCornerRounding;
    const Corner nearTop = joinedTop ? Corner::Small : Corner::Large;
    const Corner nearBottom = showTail ? Corner::Tail : Corner::Small;
    Ui::BubbleRounding rounding;
    rounding.topLeft = tailOnLeft ? nearTop : Corner::Large;
    rounding.topRight = tailOnLeft ? Corner::Large : nearTop;
    rounding.bottomLeft = tailOnLeft ? nearBottom : Corner::Large;
    rounding.bottomRight = tailOnLeft ? Corner::Large : nearBottom;
    return rounding;
}

// Phase 6b (UI.md 13c): real values from Telegram/SourceFiles/ui/chat/
// chat.style in telegram_fork (bubbleRadiusLarge/bubbleRadiusSmall) - that
// file itself is not vendored (it needs the much larger ui/chat/chat_style.h
// + styles/style_chat.style + styles/style_chat_style.h dependency chain,
// the same class of pull-in empty_userpic.cpp had, see avatarPixmap()'s own
// comment above), but the two literal numbers are ported directly rather
// than guessed - kBubbleSmallRadius was 8.0 here before, the real value is
// 6.0 (bubbleRadiusSmall aliases lib_ui's own roundRadiusLarge, 6px).
// Every one of these used to be a raw constexpr design-pixel value, never
// touched by style::Scale() at all - HistoryCanvas/SidebarCanvas paint
// their own content directly rather than through st::-declared .style
// metrics (the only thing style::StartManager()'s scale argument actually
// resizes), so bumping the scale changed real lib_ui widgets (the
// composer, buttons, auth slides) but left every message bubble and the
// chat list pixel-for-pixel identical - "the scale didn't increase" for
// the one area most visible to the user. Turned into functions that run
// each value through style::ConvertScale() (the same helper real .style
// metrics resolve through internally) so they respond to the same scale
// knob instead of silently ignoring it.
double bubbleLargeRadius() { return style::ConvertScale(16.0); }
double bubbleSmallRadius() { return style::ConvertScale(6.0); }
// Real ui/chat/chat.style: msgPadding: margins(11px, 8px, 11px, 8px) - not
// one uniform inset, horizontal and vertical differ (a bubble breathes more
// side-to-side than top-to-bottom). Split into H/V rather than the single
// bubbleMargin() this used to be, per a grep audit against telegram_fork's
// real .style values.
int bubbleMarginH() { return style::ConvertScale(11); }
int bubbleMarginV() { return style::ConvertScale(8); }
int bubbleTailHeight() { return style::ConvertScale(6); }
// Real chat.style: msgMaxWidth: 430px, msgMinWidth: 160px.
int maxBubbleContentWidth() { return style::ConvertScale(430) - bubbleMarginH() * 2; }
int minBubbleContentWidth() { return style::ConvertScale(160); }
// Timestamp/tick run at the end of a bubble: the font size of the muted
// "HH:mm" span, and the side of the square the outgoing tick is drawn in.
// Real chat.style: msgDateFont: font(13px).
int bubbleMetaFontSize() { return style::ConvertScale(13); }
// Gap between the timestamp and the tick, and between the message text and
// the metadata block as a whole - real chat.style: historySendStateSkip is
// the room the tick takes next to the date, historyItemDateSpace (13px) the
// room the date needs after the text.
double metaTickGap() { return style::ConvertScale(3.0); }
double metaTextGap() { return style::ConvertScale(13.0); }
double tickSize() { return style::ConvertScale(13.0); }

// Paints one bubble's body (fill, shadow, corners, tail) at bodyRect via
// the real engine - shared between every text/image item HistoryCanvas
// paints. background/border are gone: Ui::PaintBubble reads the real
// in/out colors itself from sharedChatStyle().messageStyle(isOutgoing,
// selected), and real bubbles are separated by a soft shadow, not a
// stroke (the old bordered hex "#e4e4e5" outline was never how Telegram
// actually does this).
// outerWidth is the width of the surface being painted into (the canvas
// viewport), not the bubble's own width - Ui::PaintBubble/PaintSolidBubble
// forwards it straight to the tail icon's paint() as an rtlpoint()-style
// mirroring width (see message_bubble.cpp's PaintSolidBubble). Passing the
// bubble's own width there was harmless only because this codebase is
// LTR-only (no mirroring ever triggers), but it was still the wrong value
// per the real engine's own contract - kept correct here rather than
// relying on LTR to paper over it.
void paintBubbleBody(QPainter& painter, const QRect& bodyRect, int outerWidth, bool isOutgoing, bool tailOnLeft,
                      bool showTail, bool joinedTop) {
    Ui::SimpleBubble bubble{
        .st = &sharedChatStyle(),
        .geometry = bodyRect,
        .outerWidth = outerWidth,
        .selected = false,
        .shadowed = true,
        .outbg = isOutgoing,
        .rounding = bubbleRounding(tailOnLeft, showTail, joinedTop),
    };
    Ui::PaintBubble(painter, bubble);
}

// Inner padding between the bubble's own edge and its content - 0 for
// media that fills the bubble (a photo/album with no caption sits flush
// against the rounded edge, matching real Telegram), bubbleMarginH()/V()
// otherwise (text/system content, which does get breathing room). Distinct
// from HistoryCanvas::sideMargin(), which is the OUTER gap between the
// canvas edge and the bubble itself and never varies by content type.
int bubblePaddingH(bool fillsBubble) { return fillsBubble ? 0 : bubbleMarginH(); }
int bubblePaddingV(bool fillsBubble) { return fillsBubble ? 0 : bubbleMarginV(); }

// Radius to use for one bubble corner when clipping media to the bubble's
// own rounded shape (see paintItem()'s album/isImage branches) - mirrors
// bubbleRounding()'s own corner meanings, EXCEPT Corner::Tail: Ui::PaintBubble
// draws that corner square, not rounded, to sit flush against the tail
// icon beside it (see bubbleRounding()'s own comment). Clipping the media
// to a rounded arc there instead would cut a bite out of the photo where
// the actual bubble edge underneath is square, leaving a gap of visible
// background between the two - 0 here keeps the media's edge flush with
// the real bubble edge at that corner.
double bubbleCornerRadius(Ui::BubbleCornerRounding corner) {
    using Corner = Ui::BubbleCornerRounding;
    switch (corner) {
        case Corner::Large:
            return bubbleLargeRadius();
        case Corner::Small:
            return bubbleSmallRadius();
        default:
            return 0.0;
    }
}

QPainterPath bubbleClipPath(const QRect& body, const Ui::BubbleRounding& rounding) {
    return roundedRectPath(body, bubbleCornerRadius(rounding.topLeft), bubbleCornerRadius(rounding.topRight),
                            bubbleCornerRadius(rounding.bottomRight), bubbleCornerRadius(rounding.bottomLeft));
}

// One row's content and cached layout - the item model HistoryCanvas
// (below) lays out and paints directly, with no per-message child
// widget at all: real Telegram Desktop's own message list
// (HistoryView::ListWidget, see history_view_list_widget.cpp in
// telegram_fork/tdesktop) is a single self-painted, virtualized canvas
// with no child widget per message either, not a QAbstractItemView
// subclass wrapping stock rows - this mirrors that structurally, not
// just visually.
struct HistoryItem {
    MessageKind kind = MessageKind::System;
    QString rawText;  // plain text, for context-menu copy / in-conversation search filtering
    QPixmap pixmap;   // valid only when isImage
    bool isImage = false;
    // Non-empty only for a merged media-album entry (see
    // MainWindow::renderCurrentConversation()'s grouping pass) - a mosaic
    // of these paints inside one bubble instead of pixmap above. Geometry
    // (relative to the bubble's own content origin) computed once in
    // buildItem() via the real Ui::LayoutMediaGroup algorithm, not a
    // hand-rolled grid.
    QVector<QPixmap> albumPixmaps;
    QVector<QRect> albumLayout;
    // Original, full-resolution file paths - pixmap/albumPixmaps above are
    // downscaled to bubble/mosaic-cell size (see buildItem()), which looks
    // fine inline but was also what MediaViewer was opening, stretched up
    // to near-fullscreen - a real, visible quality loss on click. These are
    // what tryActivateImageAt() actually loads fresh from disk for the
    // viewer instead.
    QString imagePath;
    QVector<QString> albumImagePaths;
    // True for a standalone image or an album/mosaic with no caption text -
    // real Telegram gives media with nothing else in the bubble zero inner
    // padding (the photo fills the bubble edge-to-edge instead of sitting
    // inside a ~10px margin the way text does) - see bubblePadding().
    bool fillsBubble = false;
    bool attached = false;
    bool showTail = true;
    qlonglong messageId = 0;
    bool hidden = false;  // in-conversation search filter, see HistoryCanvas::setSearchFilter()

    // A companion's (or your own) voice message - painted as a play/pause
    // button plus a progress track instead of drawDocument()'s normal text
    // flow, see paintVoiceContent(). voicePath is empty until the audio has
    // downloaded (see MainWindow::updateHistoryPhoto(), reused for voice
    // too - both just replace a message's local file path on arrival).
    bool isVoice = false;
    qint32 voiceDuration = 0;
    QString voicePath;
    QByteArray voiceWaveform;

    // The bubble's own metadata: the timestamp, plus the delivery tick on an
    // outgoing message. Painted by paintMessageMeta() into the bubble's
    // bottom-right corner, over a run of non-breaking spaces of the same
    // width reserved at the end of the text (see buildItem) - deliberately
    // not part of the document's own text. Two reasons: a tick as a U+2713
    // character came out in whatever the system font happened to have for
    // it, at body-text weight; and an inline timestamp sits wherever the
    // last line happens to end, so on any bubble wider than its own last
    // line - which is most of them once the text wraps - it ended up
    // stranded mid-bubble instead of pinned to the corner real Telegram
    // pins it to.
    QString metaText;
    double metaWidth = 0.0;
    bool showTick = false;
    // Turns the sent tick into the "read by them" double tick - see
    // HistoryEntry::readByPeer and MainWindow::updateOutgoingReadUpTo().
    bool readByPeer = false;

    // shared_ptr, not unique_ptr: QVector<HistoryItem> (items_ below) needs
    // HistoryItem to stay copy-constructible for some of its own internal
    // reallocation paths, which a unique_ptr member blocks entirely.
    std::shared_ptr<QTextDocument> doc;  // text/system items only
    int contentWidth = 0;                // bubble/system inner width, pre-margin
    int contentHeight = 0;               // bubble/system inner height, pre-margin
    // Plain-text length of doc up to (not including) the appended
    // timestamp/tick run - see buildItem(). Mouse selection/copy is
    // clamped to this so dragging into or past the timestamp cannot pick
    // it up: only the message's own text is meant to be selectable, the
    // time and the sent-tick are metadata, not content.
    int textLength = 0;

    int top = 0;     // position within the canvas's total scrollable content
    int height = 0;  // full row height, including outer margins
};

// A single message's content this list wants shown - what MainWindow
// decides (grouping, sender attribution, history bookkeeping); layout and
// painting are entirely HistoryCanvas's own job below.
struct HistoryEntry {
    MessageKind kind = MessageKind::System;
    QString text;
    QString filePath;
    bool attached = false;
    QString senderName;
    bool showTail = true;
    qlonglong messageId = 0;
    // See StoredMessage::date's own comment (main_window.hpp) - 0 means
    // "render as happening right now".
    qint64 date = 0;
    // Non-empty only for a merged media-album entry (see groupAlbums() in
    // MainWindow) - every image that was part of the same
    // td_api::message::media_album_id_, in order. filePath above is
    // unused when this is set; the single-photo path stays exactly as it
    // was for a standalone image.
    QVector<QString> albumFilePaths;
    // See StoredMessage::isVoiceNote/voiceDuration's own comments -
    // filePath above is the voice note's local audio path when this is set.
    bool isVoiceNote = false;
    qint32 voiceDuration = 0;
    QByteArray voiceWaveform;
    // The quoted message's author and text, already resolved by
    // MainWindow::renderCurrentConversation() - empty when this message is
    // not a reply, or when the message it replies to is not loaded (an old
    // one further back than any page fetched so far). Resolved there rather
    // than here because that is the only place holding the whole
    // conversation to look the id up in.
    QString replyToSender;
    QString replyToText;
    // True only for an outgoing message the other side has already read -
    // drives the single/double tick, see
    // MainWindow::updateOutgoingReadUpTo().
    bool readByPeer = false;
};

// KineticScroller (a hand-rolled QScroller-based wheel/touch momentum
// class, tuned to match telegram_fork/tdesktop/Telegram/lib_ui/ui/widgets/
// scroll_area.cpp's own SetupScrollerPhysics()/viewportEvent()) used to
// live here, for both SidebarCanvas and HistoryCanvas. Removed along with
// each class's own last usage of it, once both moved onto a real
// Ui::ScrollArea (see their own class comments) - its built-in momentum
// scrolling covers this without a second, parallel implementation.

// The message list itself - a single custom-painted, virtualized canvas
// (Ui::RpWidget, owned by a real Ui::ScrollArea - see the MainWindow
// constructor's own construction site), not a QAbstractItemView subclass
// with a child widget per row, matching real Telegram Desktop's own
// architecture for this (see HistoryItem's own comment above for the
// direct citation). Virtualized: paintEvent only draws items intersecting
// the current viewport, however many messages are actually loaded.
class HistoryCanvas : public Ui::RpWidget {
public:
    explicit HistoryCanvas(QWidget* parent = nullptr) : Ui::RpWidget(parent) {
        // ClickFocus, not NoFocus: mouse text selection (see
        // mousePressEvent/keyPressEvent below) needs Ctrl+C to reach this
        // widget, which only happens once it can actually hold focus -
        // ClickFocus specifically (not the composer's TabFocus/StrongFocus)
        // because clicking into the list to select text is itself the
        // natural way to focus it, same as any real text view.
        setFocusPolicy(Qt::ClickFocus);
    }

    // Called once, right after construction, from the MainWindow
    // constructor's own setOwnedWidget() call site - HistoryCanvas needs
    // to reach back into its own enclosing Ui::ScrollArea for scroll
    // position (isNearBottom()/scrollToBottom()/prependItems()'s anchor
    // restore) and viewport size (background wallpaper sizing, see
    // resizeEvent), same as any owned widget that needs to know about its
    // own scrolling - Ui::ScrollArea does not hand this to its owned
    // widget automatically the way it hands out geometryChanged().
    void setScrollArea(Ui::ScrollArea* area) {
        scrollArea_ = area;
        // Own width (bubble left/right alignment, see computeBubbleLeft())
        // and background_'s own size (see paintEvent's own comment on why
        // it needs the scroll area's visible size, not this widget's full
        // content height) both track the scroll area's own geometry - same
        // "geometryChanged() -> resizeToWidth()" wiring the MainWindow
        // constructor already uses for SidebarCanvas/sidebarScroll.
        area->geometryChanged() | rpl::on_next(
            [this, area] {
                resizeToWidth(area->width());
                background_.setSize(area->size(), devicePixelRatioF());
                update();
            },
            area->lifetime());
    }

    int resizeGetHeight(int /*newWidth*/) override { return totalHeight_; }

    void clear() {
        items_.clear();
        totalHeight_ = 0;
        contentBottom_ = 0;
        lastTextItemIndex_ = -1;
        loadingMore_ = false;
        resetSelectionState();
        resizeToWidth(width());
        update();
    }

    // Full replace - O(n) (every item is re-measured and re-laid-out).
    // Used for chat switches and the handful of operations that already
    // have to touch the whole conversation anyway (loading an older page,
    // a photo/sender name resolving after the fact) - all rare/bulk, not
    // per-message, so O(n) here is not the same runaway pattern that was
    // fixed for the sidebar/history auto-fill.
    void setItems(const QVector<HistoryEntry>& entries) {
        resetSelectionState();
        loadingMore_ = false;
        items_.clear();
        items_.reserve(entries.size());
        for (const HistoryEntry& entry : entries) {
            items_.push_back(buildItem(entry));
        }
        relayoutFrom(0);
        contentBottom_ = totalHeight_;
        lastTextItemIndex_ = -1;
        for (int i = items_.size() - 1; i >= 0; --i) {
            if (!items_[i].isImage && items_[i].kind != MessageKind::System) {
                lastTextItemIndex_ = i;
                break;
            }
        }
        resizeToWidth(width());
        update();
    }

    // Appends one new item at the end - O(1) amortized, the common case
    // for a live incoming/outgoing message: does not touch any earlier
    // item's layout, only retracts the immediately preceding text
    // bubble's tail in place if this new message attaches to it (same
    // idea the old per-widget approach used, just internal to this class
    // now instead of MainWindow reaching into a bubble widget directly).
    void appendItem(const HistoryEntry& entry) {
        if (entry.attached && lastTextItemIndex_ >= 0) {
            HistoryItem& previous = items_[lastTextItemIndex_];
            if (previous.showTail) {
                previous.showTail = false;
                int delta = bubbleTailHeight();
                previous.height -= delta;
                totalHeight_ -= delta;
                contentBottom_ -= delta;
                for (int i = lastTextItemIndex_ + 1; i < items_.size(); ++i) {
                    items_[i].top -= delta;
                }
            }
        }
        HistoryItem item = buildItem(entry);
        // contentBottom_, not totalHeight_ - the two only coincide when
        // items_.front().top is 0, which stops being true after the first
        // prependItems() call (see its own comment for why older history
        // gets negative top coordinates instead of shifting everything
        // already loaded). contentBottom_ tracks "where the next appended
        // item goes" directly, independent of how far back scrollback
        // history has grown.
        item.top = contentBottom_;
        totalHeight_ += item.height;
        contentBottom_ += item.height;
        bool isTextItem = !item.isImage && entry.kind != MessageKind::System;
        items_.push_back(std::move(item));
        lastTextItemIndex_ = isTextItem ? items_.size() - 1 : -1;
        resizeToWidth(width());
        update();
    }

    // Inserts a page of older messages at the front.
    //
    // Two earlier versions of this existed: the original called
    // setItems() with the whole growing list on every page (O(n^2) over a
    // long scrollback session - redid every already-loaded message's
    // QTextDocument work on every page load); the version this replaces
    // avoided that by giving older pages NEGATIVE top coordinates instead
    // of shifting anything already loaded (O(k) in the new page only).
    // That negative-coordinate scheme is incompatible with migrating this
    // class onto Ui::RpWidget/Ui::ScrollArea (see zkgram/TODO.md, "Что
    // осталось для полного перехода на lib_ui" - a real widget's own
    // local coordinate origin cannot host content above y=0, Ui::ScrollArea
    // clips it away). Checked how real Telegram Desktop's own
    // HistoryView::ListWidget (a real Ui::RpWidget) handles this
    // (history_view_list_widget.cpp, saveScrollState()/restoreScrollState(),
    // ~line 1108/2458 in telegram_fork): it does NOT use negative
    // coordinates either - every relayout keeps everything in [0, height)
    // and shifting old items forward is a plain O(n) loop, same as the
    // very first version here. What makes that shift invisible to the
    // user is not avoiding it, but restoring scroll position by ANCHOR
    // ITEM instead of by raw pixel value: remember which item sits at the
    // current viewport top and how many pixels into it (_visibleTopItem/
    // _visibleTopFromItem there), then after the shift, scroll to that
    // same item's new top plus the same offset - the visible content
    // never appears to move even though its absolute coordinates did.
    // Ported that here (anchorIndex/anchorOffset below) rather than the
    // negative-coordinate trick, on the same reasoning: it is what the
    // real, heavily-used implementation actually does, not a guess.
    //
    // Still does not re-measure/rebuild any already-loaded item's own
    // QTextDocument content - only its top offset changes, which is a
    // plain int addition per item, not a re-layout.
    void prependItems(const QVector<HistoryEntry>& entries) {
        int anchorIndex = -1;
        int anchorOffset = 0;
        if (!items_.isEmpty() && scrollArea_) {
            int visibleTop = scrollArea_->scrollTop();
            anchorIndex = qMin(firstIndexNotBefore(visibleTop), items_.size() - 1);
            if (anchorIndex >= 0) {
                anchorOffset = visibleTop - items_[anchorIndex].top;
            }
        }

        QVector<HistoryItem> newItems;
        newItems.reserve(entries.size());
        int totalNewHeight = 0;
        for (const HistoryEntry& entry : entries) {
            HistoryItem item = buildItem(entry);
            totalNewHeight += item.height;
            newItems.push_back(std::move(item));
        }
        int y = 0;
        for (HistoryItem& item : newItems) {
            item.top = y;
            y += item.height;
        }
        for (HistoryItem& item : items_) {
            item.top += totalNewHeight;
        }
        // Only advance lastTextItemIndex_ (used by appendItem()'s tail
        // retraction) if it already pointed at a real text item - if the
        // newest message in the conversation is an image/system line
        // (-1), prepending older history does not change that.
        if (lastTextItemIndex_ >= 0) {
            lastTextItemIndex_ += newItems.size();
        }
        items_ = newItems + items_;
        totalHeight_ += totalNewHeight;
        // Every already-loaded item's top just moved forward by
        // totalNewHeight (see the shift loop above) - contentBottom_ is an
        // absolute coordinate into that same space, so it has to move
        // with everything else or appendItem() would place the next live
        // message at a now-stale position.
        contentBottom_ += totalNewHeight;
        resizeToWidth(width());
        if (anchorIndex >= 0 && scrollArea_) {
            int newAnchorIndex = anchorIndex + newItems.size();
            scrollArea_->scrollToY(items_[newAnchorIndex].top + anchorOffset);
        }
        update();
    }

    void scrollToBottom() {
        if (scrollArea_) {
            scrollArea_->scrollToY(scrollArea_->scrollTopMax());
        }
    }

    // Whether the viewport is at (or close enough to) the newest message
    // that auto-scrolling to the bottom on a new arrival would not be a
    // surprise - MainWindow::appendMessage() uses this to decide whether
    // a live message should pull the view down at all. Every real chat
    // client (this one previously did not) only autoscrolls when you were
    // already reading the bottom; if you have scrolled up into history, a
    // message arriving - typically from the other side, not something you
    // did - must not suddenly snap you back down to it. A small threshold
    // (not exact equality with maximum()) covers a user who is one or two
    // rows short of the very bottom, which still reads as "at the bottom"
    // to them.
    bool isNearBottom() const {
        return scrollArea_
            && scrollArea_->scrollTop() >= scrollArea_->scrollTopMax() - style::ConvertScale(80);
    }

    // Shown as a small fixed pill near the top of the viewport while an
    // older-history page fetch (see MainWindow::maybeLoadMoreHistory()) is
    // in flight - a real network round trip, not a synchronous local
    // operation, so scrolling into the not-yet-arrived edge with nothing
    // shown there read as "the app just freezes", not "still loading".
    // Real infinite-scroll UX guidance is exactly this: prefetch earlier
    // (see the scroll-threshold connect() in the MainWindow constructor)
    // AND show a visible loading state for whatever the prefetch has not
    // caught up with yet - a blank gap you scroll into always feels
    // broken even when the fetch is progressing completely normally.
    void setLoadingMore(bool loading) {
        if (loadingMore_ == loading) {
            return;
        }
        loadingMore_ = loading;
        update();
    }

    // Whether the currently loaded messages are tall enough to need
    // scrolling at all - MainWindow::ensureHistoryFillsViewport() uses
    // this to decide whether to keep auto-loading older pages on chat
    // open. Compares against the enclosing Ui::ScrollArea's own visible
    // height, not this widget's height() (which, post-migration, is
    // this canvas's real full content height - resizeGetHeight() returns
    // totalHeight_ - so it is always >= totalHeight_ by definition and
    // this check would always read "fills" if it used that instead).
    bool contentFillsViewport() const {
        return scrollArea_ && totalHeight_ >= scrollArea_->height();
    }

    // In-conversation text filter (see MainWindow::onSearchTextChanged) -
    // hides non-matching rows in place rather than rebuilding the list.
    void setSearchFilter(const QString& query) {
        for (HistoryItem& item : items_) {
            item.hidden = !query.isEmpty() && !item.rawText.contains(query, Qt::CaseInsensitive);
        }
        relayoutFrom(0);
        resizeToWidth(width());
        update();
    }

    struct HitResult {
        bool valid = false;
        MessageKind kind = MessageKind::System;
        QString text;
        // 0 for anything with no real TDLib id (a CryptoLayer message, a
        // System line) - only a plain message can be replied to by id, see
        // MainWindow's context menu.
        qlonglong messageId = 0;
        // Index into items_ - MainWindow needs this for the "Select"
        // context-menu action (HistoryCanvas::enterSelectionMode()), -1 if
        // valid is false.
        int index = -1;
    };
    // First index whose own bottom edge (top + height) is past y - items_
    // is laid out in strictly increasing top order (buildItem()/
    // appendItem()/prependItems() all guarantee this; a hidden search-
    // filtered item shares its neighbor's top rather than breaking the
    // ordering, see relayoutFrom()'s own comment), so a linear scan over
    // every loaded message just to find the handful actually near y is
    // pure waste once a long scrollback session has thousands of items in
    // items_ - paintEvent() already only PAINTS the visible slice, but
    // was still walking the entire vector every single frame to find it,
    // an O(n)-per-repaint cost that scrolling triggers continuously and
    // that grows without bound the longer a chat's history gets loaded -
    // exactly the "freezes further into scrollback" shape reported.
    // std::lower_bound turns that into O(log n) to find where to start;
    // itemAt()/itemIndexAt()/paintEvent() below all still scan forward
    // linearly from there and bail out (continue for hidden, break past
    // the target range) the moment they can, same as before - only the
    // starting point changed, not the correctness-bearing logic.
    int firstIndexNotBefore(int y) const {
        int lo = 0;
        int hi = items_.size();
        while (lo < hi) {
            int mid = lo + (hi - lo) / 2;
            if (items_[mid].top + items_[mid].height <= y) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        return lo;
    }

    // Swipe-to-reply, fired on release once a horizontal drag across a
    // bubble passed the trigger distance - see mouseMoveEvent(). A plain
    // callback rather than a signal, same reasoning as SidebarCanvas's
    // onRowClicked: this class has no Q_OBJECT of its own.
    std::function<void(HitResult)> onReplyRequested;

    // Same shape as itemAt(), addressed by index instead of by position -
    // the swipe gesture already knows which row it started on and must not
    // re-hit-test on release, where the cursor may well have left the row.
    HitResult itemInfo(int index) const {
        if (index < 0 || index >= items_.size()) {
            return HitResult{};
        }
        const HistoryItem& item = items_[index];
        return HitResult{true, item.kind, item.rawText, item.messageId, index};
    }

    // pos is in this widget's own local coordinates, which (post-migration
    // - see the class comment) directly equal absolute item-space
    // coordinates, same convention as QAbstractItemView::itemAt()
    // (customContextMenuRequested's own point), which this replaces.
    HitResult itemAt(const QPoint& pos) const {
        int y = pos.y();
        for (int i = firstIndexNotBefore(y); i < items_.size(); ++i) {
            const HistoryItem& item = items_[i];
            if (item.top > y) {
                break;
            }
            if (item.hidden) {
                continue;
            }
            if (y >= item.top && y < item.top + item.height) {
                return HitResult{true, item.kind, item.rawText, item.messageId, i};
            }
        }
        return HitResult{};
    }

    // Empty if nothing is currently selected (a plain click with no
    // drag leaves selectionStart_ == selectionEnd_, which is a
    // zero-length range - QTextCursor::selectedText() on that is
    // already ""). Used by MainWindow's right-click Copy action to
    // prefer a mouse selection over the whole message when one exists.
    QString selectedText() const {
        if (selectionItemIndex_ < 0 || selectionItemIndex_ >= items_.size()) {
            return {};
        }
        const HistoryItem& item = items_[selectionItemIndex_];
        if (!item.doc) {
            return {};
        }
        QTextCursor cursor(item.doc.get());
        cursor.setPosition(selectionStart_);
        cursor.setPosition(selectionEnd_, QTextCursor::KeepAnchor);
        // QTextCursor::selectedText() uses U+2029 (paragraph separator)
        // for line breaks inside the document, not '\n' - invisible in
        // the app itself, but a real newline is what every other program
        // a selection gets pasted into actually expects.
        QString text = cursor.selectedText();
        text.replace(QChar(0x2029), QChar('\n'));
        return text;
    }

protected:
    void paintEvent(QPaintEvent* event) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        // Painted by hand rather than left to QPalette::Base/
        // autoFillBackground - see git history for why that path silently
        // stopped painting anything at all once MainWindow got a
        // stylesheet. background_ owns its own cache (see ChatBackground's
        // own comment) and only regenerates on an actual resize, not on
        // every paint. Sized to the enclosing Ui::ScrollArea's own visible
        // window, not this widget's own (much taller, full-content) size -
        // see resizeEvent - and positioned at the current scroll offset,
        // so the wallpaper stays fixed relative to the window as content
        // scrolls past it. Before this class moved onto Ui::ScrollArea,
        // item painting itself subtracted the scroll offset, so a plain
        // (0,0)-origin rect was already window-relative for free; now
        // items paint at their real absolute position (see below), so the
        // background has to track the scroll offset explicitly instead.
        int scrollTop = scrollArea_ ? scrollArea_->scrollTop() : 0;
        QRect viewportRect(0, scrollTop, width(), scrollArea_ ? scrollArea_->height() : height());
        background_.paint(painter, viewportRect);
        QRect visible = event->rect();
        // Starts at the first item that could possibly intersect the
        // visible rect (see firstIndexNotBefore()'s own comment) instead
        // of scanning every loaded message from index 0 on every single
        // repaint - the actual painted subset is unchanged, only how fast
        // this loop finds it is.
        for (int i = firstIndexNotBefore(visible.top()); i < items_.size(); ++i) {
            const HistoryItem& item = items_[i];
            int itemTop = item.top;
            if (itemTop > visible.bottom()) {
                break;
            }
            if (item.hidden) {
                continue;
            }
            int itemBottom = itemTop + item.height;
            if (itemBottom < visible.top()) {
                continue;
            }
            paintItem(painter, item, itemTop, i == selectionItemIndex_, i);
        }
        if (loadingMore_) {
            paintLoadingIndicator(painter, viewportRect);
        }
    }

    // A small fixed pill, centered near the top of the viewport,
    // independent of scroll position - see setLoadingMore()'s own comment
    // for why this exists at all.
    void paintLoadingIndicator(QPainter& painter, const QRect& viewportRect) const {
        QString text = "Loading...";
        QFont font = QGuiApplication::font();
        font.setPixelSize(style::ConvertScale(12));
        QFontMetrics metrics(font);
        int paddingH = style::ConvertScale(14);
        int height = style::ConvertScale(26);
        int width = metrics.horizontalAdvance(text) + paddingH * 2;
        int top = style::ConvertScale(10);
        // viewportRect.top() is the current scroll offset, not always 0
        // post-migration (see paintEvent's own comment) - added here so
        // the pill stays anchored near the top of the visible WINDOW
        // rather than drifting to canvas-absolute y=10 (invisible once
        // scrolled past the very first screenful).
        QRect pillRect((viewportRect.width() - width) / 2, viewportRect.top() + top, width, height);
        painter.save();
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0, 0, 0, 140));
        painter.drawRoundedRect(pillRect, height / 2.0, height / 2.0);
        painter.setPen(Qt::white);
        painter.setFont(font);
        painter.drawText(pillRect, Qt::AlignCenter, text);
        painter.restore();
    }

    // No resizeEvent/wheelEvent overrides here anymore (unlike the
    // pre-migration QAbstractScrollArea version) - resizeGetHeight() +
    // resizeToWidth() (called from setScrollArea()'s own geometryChanged()
    // subscription below, same as clear()/setItems()/appendItem()/
    // prependItems()/setSearchFilter() above) cover layout, and the real
    // Ui::ScrollArea wrapping this canvas (see MainWindow's construction
    // site) covers wheel/touch/momentum scrolling on its own - same as
    // SidebarCanvas's own migration earlier.

    // Real character-level mouse text selection (click-drag highlights a
    // range, double-click selects a word, Ctrl+C copies it) - the message
    // list is a single custom-painted canvas (see the class comment
    // above), so none of this comes for free the way it would from a
    // real QLabel/QTextEdit per message; it has to be built by hand here,
    // same as real Telegram Desktop's own bubbles do (Ui::Text::String
    // selection state, see history_view_element.h).
    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() != Qt::LeftButton) {
            Ui::RpWidget::mousePressEvent(event);
            return;
        }
        int index = itemIndexAt(event->pos());
        bool selectable = index >= 0 && items_[index].kind != MessageKind::System;

        // Ctrl+click toggles selection directly - the same shortcut real
        // Telegram Desktop uses, and independent of both the context
        // menu's "Select" action and the long-press timer below (a
        // trackpad/right-click setup where either of those turns out to
        // be unreliable still has this as a plain, unambiguous left-click
        // gesture).
        if (event->modifiers().testFlag(Qt::ControlModifier)) {
            if (selectable) {
                if (!multiSelectMode_) {
                    enterSelectionMode(index);
                } else {
                    toggleItemSelected(index);
                }
            }
            return;
        }

        // Already in selection mode: a plain click just toggles the
        // checkbox under it, never starts a text drag-select (the two
        // never coexist, see setMultiSelectMode's own comment).
        if (multiSelectMode_) {
            if (selectable) {
                toggleItemSelected(index);
            }
            return;
        }

        // Long-press enters selection mode - started for ANY selectable
        // row (text, image, or album, unlike the text-drag-select below,
        // which only text bubbles support), cancelled by a drag past a
        // small threshold in mouseMoveEvent or by release before it fires.
        if (selectable) {
            longPressIndex_ = index;
            longPressStartPos_ = event->pos();
            if (!longPressTimer_) {
                longPressTimer_ = new QTimer(this);
                longPressTimer_->setSingleShot(true);
                connect(longPressTimer_, &QTimer::timeout, this, [this] {
                    if (longPressIndex_ >= 0) {
                        enterSelectionMode(longPressIndex_);
                    }
                });
            }
            longPressTimer_->start(450);
        }

        // Swipe-to-reply origin. Recorded before the text-selection path
        // below returns early for images/albums, so those can be swiped too
        // even though they can never be drag-selected.
        swipeIndex_ = selectable ? index : -1;
        swipeStartPos_ = event->pos();
        swipeOffset_ = 0;

        if (index < 0 || items_[index].isImage || !items_[index].doc) {
            clearSelection();
            Ui::RpWidget::mousePressEvent(event);
            return;
        }
        int pos = docPositionAt(items_[index], event->pos());
        selectionItemIndex_ = index;
        // Triple click - a press landing inside the double-click interval
        // and within startDragDistance() of the double click before it,
        // which is exactly how real Telegram detects it (Qt has no triple
        // click event of its own): see _trippleClickPoint/
        // _trippleClickStartTime and validateTrippleClickStartTime() in
        // ListWidget::mouseActionStart. The timer is restarted rather than
        // cleared so a fourth click keeps the paragraph instead of dropping
        // back to characters mid-gesture, same as there.
        bool tripleClick = tripleClickTimer_.isValid() &&
                            tripleClickTimer_.elapsed() < QApplication::doubleClickInterval() &&
                            (event->pos() - tripleClickPoint_).manhattanLength() < QApplication::startDragDistance();
        selectType_ = tripleClick ? SelectType::Paragraphs : SelectType::Chars;
        selectionAnchor_ = pos;
        if (tripleClick) {
            tripleClickTimer_.restart();
        } else {
            tripleClickTimer_.invalidate();
        }
        applySelectionRange(pos);
        selecting_ = true;
        update();
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        // A drag past a small threshold is not a long press - matches
        // how a real long-press gesture (touch or mouse) gets cancelled
        // by movement.
        if (longPressTimer_ && longPressTimer_->isActive() &&
            (event->pos() - longPressStartPos_).manhattanLength() > 6) {
            longPressTimer_->stop();
        }

        // Swipe-to-reply. Claimed only once the drag is clearly sideways
        // (twice as much horizontal as vertical, past a small deadzone), so
        // it cannot steal a vertical drag meant for scrolling or a diagonal
        // one meant for selecting text. Once claimed it keeps the gesture
        // for the rest of the press, which is why swipeActive_ short-
        // circuits the test - a swipe that happens to pass back through
        // near-vertical mid-drag must not flip back to selecting.
        if (!multiSelectMode_ && swipeIndex_ >= 0 && (event->buttons() & Qt::LeftButton)) {
            QPoint delta = event->pos() - swipeStartPos_;
            if (!swipeActive_ && qAbs(delta.x()) > kSwipeDeadzone() &&
                qAbs(delta.x()) > qAbs(delta.y()) * 2) {
                swipeActive_ = true;
                // The gesture is a swipe, so whatever text selection the
                // press started is not wanted.
                selecting_ = false;
                clearSelection();
                if (longPressTimer_) {
                    longPressTimer_->stop();
                }
            }
            if (swipeActive_) {
                // Clamped, and follows the finger in whichever direction it
                // went: Telegram's own swipe direction differs by platform
                // and layout direction, and refusing one of the two here
                // would just read as the gesture not working.
                swipeOffset_ = qBound(-kSwipeMaxOffset(), delta.x(), kSwipeMaxOffset());
                update();
                return;
            }
        }

        if (!selecting_ || selectionItemIndex_ < 0) {
            Ui::RpWidget::mouseMoveEvent(event);
            return;
        }
        applySelectionRange(docPositionAt(items_[selectionItemIndex_], event->pos()));
        update();
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        // A quick click (the long-press timer never fired, so no
        // selection-mode entry happened because of this press) landing on
        // an image/album opens the media viewer - checked here, on
        // release, rather than on press, so a long-press-to-select still
        // takes priority and a click that turns into a drag does not
        // accidentally open anything.
        bool wasQuickPress = longPressTimer_ && longPressTimer_->isActive();
        if (longPressTimer_) {
            longPressTimer_->stop();
        }
        selecting_ = false;

        if (swipeActive_) {
            int index = swipeIndex_;
            bool triggered = qAbs(swipeOffset_) >= kSwipeTriggerOffset();
            // Cleared before the callback runs: it re-renders the whole
            // conversation (MainWindow::startReply -> the reply bar
            // appearing resizes the list), and leaving a stale index behind
            // across that would point at items_ that no longer exist.
            swipeActive_ = false;
            swipeIndex_ = -1;
            swipeOffset_ = 0;
            update();
            if (triggered && onReplyRequested) {
                HitResult hit = itemInfo(index);
                if (hit.valid && hit.kind != MessageKind::System) {
                    onReplyRequested(hit);
                }
            }
            // Consumed: no image activation, no selection change.
            Ui::RpWidget::mouseReleaseEvent(event);
            return;
        }
        swipeIndex_ = -1;
        if (wasQuickPress && !multiSelectMode_ && event->button() == Qt::LeftButton &&
            !event->modifiers().testFlag(Qt::ControlModifier) &&
            (event->pos() - longPressStartPos_).manhattanLength() <= 6) {
            tryActivateImageAt(event->pos());
            tryToggleVoiceAt(event->pos());
        }
        Ui::RpWidget::mouseReleaseEvent(event);
    }

    void tryActivateImageAt(const QPoint& pos) {
        int index = itemIndexAt(pos);
        if (index < 0 || !onImageActivated) {
            return;
        }
        const HistoryItem& item = items_[index];
        int bubbleLeft = computeBubbleLeft(item);
        int paddingH = bubblePaddingH(item.fillsBubble);
        int paddingV = bubblePaddingV(item.fillsBubble);
        int rowMargin = kRowVerticalMargin(item.attached);
        int bubbleTop = item.top + rowMargin / 2;
        QPoint local = pos - QPoint(bubbleLeft + paddingH, bubbleTop + paddingV);
        if (!item.albumPixmaps.isEmpty()) {
            for (int k = 0; k < item.albumLayout.size(); ++k) {
                if (item.albumLayout[k].contains(local)) {
                    onImageActivated(loadFullResolutionOrFallback(item.albumImagePaths, item.albumPixmaps), k);
                    return;
                }
            }
        } else if (item.isImage && QRect(QPoint(0, 0), item.pixmap.size()).contains(local)) {
            QPixmap full;
            if (!item.imagePath.isEmpty() && full.load(item.imagePath)) {
                onImageActivated(QVector<QPixmap>{full}, 0);
            } else {
                onImageActivated(QVector<QPixmap>{item.pixmap}, 0);
            }
        }
    }

    // MediaViewer used to open the exact same downscaled QPixmap the
    // bubble/mosaic itself paints (item.pixmap/albumPixmaps, scaled to
    // bubble/cell size in buildItem()) - correct for an inline thumbnail,
    // but stretched up to near-fullscreen it read as a real quality drop
    // on click. Loads fresh from the original file instead, falling back
    // to the already-cached thumbnail only if that file is genuinely gone
    // (moved/deleted) so a click never opens a blank viewer.
    static QVector<QPixmap> loadFullResolutionOrFallback(const QVector<QString>& paths,
                                                           const QVector<QPixmap>& fallback) {
        QVector<QPixmap> result;
        result.reserve(fallback.size());
        for (int i = 0; i < fallback.size(); ++i) {
            QPixmap full;
            if (i < paths.size() && !paths[i].isEmpty() && full.load(paths[i])) {
                result.push_back(std::move(full));
            } else {
                result.push_back(fallback[i]);
            }
        }
        return result;
    }

    // A quick click anywhere in a voice bubble's content area toggles its
    // play/pause - not just the circular button itself, matching real
    // Telegram's own generously-sized tap target for this.
    void tryToggleVoiceAt(const QPoint& pos) {
        int index = itemIndexAt(pos);
        if (index < 0 || !onVoicePlayToggled) {
            return;
        }
        const HistoryItem& item = items_[index];
        if (!item.isVoice) {
            return;
        }
        onVoicePlayToggled(item.messageId, item.voicePath);
    }

    // Same bubbleLeft geometry paintItem() computes inline - factored out
    // here too so tryActivateImageAt()'s hit-testing stays exactly in
    // sync with where the image is actually painted, the same discipline
    // textOrigin() already follows for text hit-testing.
    int computeBubbleLeft(const HistoryItem& item) const {
        int viewportWidth = width();
        int paddingH = bubblePaddingH(item.fillsBubble);
        int bubbleOuterWidth = item.contentWidth + paddingH * 2;
        int bubbleLeft;
        if (item.kind == MessageKind::Outgoing) {
            bubbleLeft = viewportWidth - bubbleOuterWidth - sideMargin();
        } else if (item.kind == MessageKind::Incoming) {
            bubbleLeft = sideMargin();
        } else {
            bubbleLeft = (viewportWidth - bubbleOuterWidth) / 2;
        }
        if (multiSelectMode_ && item.kind != MessageKind::System) {
            bubbleLeft += qRound(checkboxGutterWidth() * selectionReveal_);
        }
        return bubbleLeft;
    }

    void mouseDoubleClickEvent(QMouseEvent* event) override {
        if (multiSelectMode_) {
            return;
        }
        int index = itemIndexAt(event->pos());
        if (index < 0 || items_[index].isImage || !items_[index].doc) {
            Ui::RpWidget::mouseDoubleClickEvent(event);
            return;
        }
        selectionItemIndex_ = index;
        // Word granularity, and it stays on for the rest of the press:
        // dragging out of a double click extends the selection whole word
        // at a time, not character by character - real Telegram's
        // TextSelectType::Words, set by ListWidget::switchToWordSelection()
        // and then reused by every mouseActionUpdate() until the button
        // goes up. selecting_ stays true here for exactly that reason.
        selectType_ = SelectType::Words;
        selectionAnchor_ = docPositionAt(items_[index], event->pos());
        applySelectionRange(selectionAnchor_);
        selecting_ = true;
        // Opens the triple-click window - see mousePressEvent().
        tripleClickPoint_ = event->pos();
        tripleClickTimer_.start();
        update();
    }

    void keyPressEvent(QKeyEvent* event) override {
        if (event->matches(QKeySequence::Copy)) {
            QString text = selectedText();
            if (!text.isEmpty()) {
                QGuiApplication::clipboard()->setText(text);
            }
            return;
        }
        Ui::RpWidget::keyPressEvent(event);
    }

    // ---- multi-message selection (real Telegram's "select messages" mode,
    // not the single-bubble text drag-select above) ------------------------
    //
    // Entered either by a long-press on a bubble (see mousePressEvent's
    // longPressTimer_) or by the context menu's "Select" action (see
    // MainWindow::showMessageContextMenu). While active, a plain click
    // toggles that row's checkbox instead of starting a text drag-select,
    // and every row shifts right by checkboxGutterWidth() to make room for
    // a checkbox on the left - selectionReveal_ animates that shift and the
    // checkbox's own fade/scale-in, the same idea as KineticScroller's
    // QVariantAnimation-driven glide above, not a hand-tuned easing curve
    // lifted from tdesktop's own (internal, not reusable standalone)
    // Ui::Animations machinery.
    //
    // Explicit public: here - everything above this point (paintEvent,
    // the mouse/key event overrides) sits under the class's own
    // protected: section (QWidget's own virtual overrides), and
    // MainWindow needs to call several of the following methods from
    // outside the class.
public:
    bool isMultiSelectMode() const { return multiSelectMode_; }

    void setMultiSelectMode(bool on) {
        if (multiSelectMode_ == on) {
            return;
        }
        multiSelectMode_ = on;
        if (!on) {
            selectedIndices_.clear();
        }
        clearSelection();  // a text drag-select and a checkbox-select never coexist
        animateSelectionReveal(on ? 1.0 : 0.0);
        if (onSelectionModeChanged) {
            onSelectionModeChanged(multiSelectMode_, static_cast<int>(selectedIndices_.size()));
        }
    }

    // Hard reset, no animation - for setItems()/clear() only.
    // selectedIndices_ is a set of positions into items_, meaningful only
    // against the exact items_ snapshot they were recorded against; once
    // that snapshot is replaced (a chat switch, a full conversation
    // rebuild) every recorded index is potentially stale. A crash report
    // showed this: leftover indices from a longer conversation outliving
    // a switch to a shorter one, then selectedItemsInOrder() reading
    // items_[index] out of bounds - a QVector does not bounds-check that
    // in a Release build, and the garbage HistoryItem read back
    // (QString/QPixmap/shared_ptr members included) corrupted the heap
    // the moment anything tried to copy or free it. Still notifies
    // MainWindow (so a selection toolbar left showing for the previous
    // chat does not get stuck visible), just without setMultiSelectMode's
    // animation - a snap-back matches "the conversation you were looking
    // at is gone" better than a glide would anyway.
    void resetSelectionState() {
        bool wasActive = multiSelectMode_;
        multiSelectMode_ = false;
        selectedIndices_.clear();
        selectionReveal_ = 0.0;
        if (selectionRevealAnim_) {
            selectionRevealAnim_->stop();
        }
        // A pending long-press (see mousePressEvent) references an index
        // into the items_ this call is about to replace - stopped here so
        // its timeout callback cannot fire against the new, unrelated
        // items_ later (enterSelectionMode() bounds-checks too, but there
        // is no reason to let a stale press outlive the rebuild it landed
        // before at all).
        if (longPressTimer_) {
            longPressTimer_->stop();
        }
        longPressIndex_ = -1;
        if (wasActive && onSelectionModeChanged) {
            onSelectionModeChanged(false, 0);
        }
    }

    void toggleItemSelected(int index) {
        if (index < 0 || index >= items_.size() || items_[index].kind == MessageKind::System) {
            return;
        }
        if (selectedIndices_.contains(index)) {
            selectedIndices_.remove(index);
        } else {
            selectedIndices_.insert(index);
        }
        // Leaving the last item unchecked exits selection mode entirely,
        // same as tapping every checkbox off in real Telegram - the user
        // is not left staring at an empty selection toolbar with nothing
        // to act on.
        if (selectedIndices_.isEmpty()) {
            setMultiSelectMode(false);
            return;
        }
        update();
        if (onSelectionModeChanged) {
            onSelectionModeChanged(true, static_cast<int>(selectedIndices_.size()));
        }
    }

    // In visual (top-to-bottom) order, not insertion/set order - Forward/
    // Copy read better that way, matching how the messages actually appear.
    QVector<HistoryItem> selectedItemsInOrder() const {
        QVector<int> indices(selectedIndices_.begin(), selectedIndices_.end());
        std::sort(indices.begin(), indices.end());
        QVector<HistoryItem> result;
        result.reserve(indices.size());
        for (int index : indices) {
            // Defensive: selectedIndices_ should never outlive the items_
            // snapshot it was built against (setItems()/clear() reset it,
            // see their own comments), but items_[index] on a QVector does
            // not bounds-check in a Release build - an out-of-range read
            // here silently returns garbage memory reinterpreted as a
            // HistoryItem (QString/QPixmap/shared_ptr members included),
            // and copying THAT corrupts the heap the first time anything
            // tries to retain/free one of those bogus pointers. A crash
            // report showed exactly this failure shape, so this check
            // stays even though the callers above are believed fixed.
            if (index < 0 || index >= items_.size()) {
                continue;
            }
            result.push_back(items_[index]);
        }
        return result;
    }

    // Real chat.style: msgSelectionOffset: 30px, msgSelectionCheck.size: 20px.
    static int checkboxGutterWidth() { return style::ConvertScale(30); }
    static int checkboxDiameter() { return style::ConvertScale(20); }

    void animateSelectionReveal(qreal target) {
        if (selectionRevealAnim_) {
            selectionRevealAnim_->stop();
        }
        selectionRevealAnim_ = new QVariantAnimation(this);
        selectionRevealAnim_->setStartValue(selectionReveal_);
        selectionRevealAnim_->setEndValue(target);
        selectionRevealAnim_->setDuration(180);
        selectionRevealAnim_->setEasingCurve(QEasingCurve::OutCubic);
        connect(selectionRevealAnim_, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
            selectionReveal_ = value.toReal();
            update();
        });
        // deleteOnHide-style self-cleanup, same pattern used elsewhere in
        // this file (e.g. Ui::PopupMenu) - no owner needs to track this
        // pointer past the animation's own lifetime.
        connect(selectionRevealAnim_, &QVariantAnimation::finished, selectionRevealAnim_, &QObject::deleteLater);
        selectionRevealAnim_->start();
    }

    void drawSelectionCheckbox(QPainter& painter, int index, int rowTop, int rowHeight) const {
        if (selectionReveal_ <= 0.001) {
            return;
        }
        int diameter = checkboxDiameter();
        int cx = sideMargin() + qRound(checkboxGutterWidth() * selectionReveal_) - checkboxGutterWidth() / 2;
        int cy = rowTop + rowHeight / 2;
        QRectF circle(cx - diameter / 2.0, cy - diameter / 2.0, diameter, diameter);
        bool checked = selectedIndices_.contains(index);
        painter.save();
        painter.setOpacity(selectionReveal_);
        painter.setRenderHint(QPainter::Antialiasing);
        if (checked) {
            painter.setBrush(st::msgOutBg->c);
            painter.setPen(Qt::NoPen);
        } else {
            painter.setBrush(QColor(255, 255, 255, 200));
            painter.setPen(QPen(QColor(255, 255, 255, 230), 1.5));
        }
        painter.drawEllipse(circle);
        if (checked) {
            QPainterPath check;
            check.moveTo(circle.left() + diameter * 0.28, circle.top() + diameter * 0.52);
            check.lineTo(circle.left() + diameter * 0.44, circle.top() + diameter * 0.70);
            check.lineTo(circle.left() + diameter * 0.74, circle.top() + diameter * 0.32);
            painter.setPen(QPen(Qt::white, qMax(1.5, diameter * 0.1), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            painter.drawPath(check);
        }
        painter.restore();
    }

private:
    // Set once via setScrollArea() right after construction - see its own
    // comment. Not owned (Ui::ScrollArea owns this widget, not the other
    // way around).
    Ui::ScrollArea* scrollArea_ = nullptr;
    ChatBackground background_;
    int selectionItemIndex_ = -1;
    int selectionStart_ = -1;
    int selectionEnd_ = -1;
    bool selecting_ = false;
    // The three selection granularities real Telegram has
    // (Ui::Text::TextSelectType): a plain drag moves character by
    // character, a drag out of a double click moves whole words, a triple
    // click takes the whole message.
    enum class SelectType { Chars, Words, Paragraphs };
    SelectType selectType_ = SelectType::Chars;
    // The end of the selection the drag is not moving. Needed on top of
    // selectionStart_ because in word mode BOTH ends are recomputed on
    // every move (each has to sit on its own word boundary), so neither of
    // them is the raw point the gesture started from any more.
    int selectionAnchor_ = -1;
    // Double-click position and age, for the triple-click test in
    // mousePressEvent(). An invalid timer means "no double click recently".
    QPoint tripleClickPoint_;
    QElapsedTimer tripleClickTimer_;

    // ---- swipe-to-reply -------------------------------------------------
    // Distances in logical pixels, scaled like every other size in this
    // file. Deadzone is where the gesture is still ambiguous; trigger is how
    // far it has to go to actually mean "reply"; max is where the bubble
    // stops following, so a long drag does not push it off screen.
    static int kSwipeDeadzone() { return style::ConvertScale(8); }
    static int kSwipeTriggerOffset() { return style::ConvertScale(44); }
    static int kSwipeMaxOffset() { return style::ConvertScale(64); }
    // Row the press landed on, -1 when the press was not on a swipeable
    // row or the gesture has ended.
    int swipeIndex_ = -1;
    QPoint swipeStartPos_;
    // Set once the drag is recognised as horizontal - until then the press
    // may still turn out to be a text selection or a scroll.
    bool swipeActive_ = false;
    // Signed: the bubble follows the cursor in whichever direction it went.
    int swipeOffset_ = 0;

    bool multiSelectMode_ = false;
    QSet<int> selectedIndices_;
    qreal selectionReveal_ = 0.0;
    QVariantAnimation* selectionRevealAnim_ = nullptr;
    // Started on mousePress, stopped on move-past-threshold/release - see
    // mousePressEvent/mouseMoveEvent/mouseReleaseEvent. Real Telegram uses
    // a touch/mouse long-press to enter selection mode without a menu;
    // this is the same idea via QTimer since Qt Widgets has no built-in
    // long-press gesture for a plain mouse.
    QTimer* longPressTimer_ = nullptr;
    int longPressIndex_ = -1;
    QPoint longPressStartPos_;

    // See setVoicePlaybackState()'s own comment - mirrors MainWindow's
    // voicePlayer_ state just enough to paint the currently-playing
    // bubble's icon/progress correctly, without this class knowing
    // anything about QMediaPlayer.
    qlonglong playingVoiceMessageId_ = 0;
    bool voicePlaying_ = false;
    qint64 voicePositionMs_ = 0;
    qint64 voiceDurationMs_ = 0;

public:
    // Public: MainWindow's own "Select" context-menu action and the
    // selection toolbar's Forward/Delete/Copy buttons both need to drive
    // this from outside. (chatty, kind, kind, ...) callback shape mirrors
    // SidebarCanvas::onRowClicked's existing std::function pattern in this
    // file rather than introducing Qt signals/Q_OBJECT for one callback.
    std::function<void(bool active, int selectedCount)> onSelectionModeChanged;

    // Fired by tryActivateImageAt() (see mouseReleaseEvent) when a quick
    // click lands on a photo/album bubble - MainWindow wires this to open
    // its MediaViewer. images/startIndex is the whole album (or a single-
    // element vector for a standalone photo) and which one was actually
    // clicked, so the viewer can open already showing the right one and
    // still let the user step to its neighbors.
    std::function<void(const QVector<QPixmap>&, int)> onImageActivated;

    // Fired by tryToggleVoiceAt() (see mouseReleaseEvent) when a quick
    // click lands on a voice-note bubble - messageId/path identify which
    // note, path possibly still empty if the audio has not finished
    // downloading yet (MainWindow ignores the tap in that case, same as a
    // photo that has not arrived).
    std::function<void(qlonglong messageId, const QString& path)> onVoicePlayToggled;

    // Pushed by MainWindow from voicePlayer_'s own Qt signals - lets the
    // currently-playing bubble (if any is visible) repaint its play/pause
    // icon and progress track without HistoryCanvas needing to know
    // anything about QMediaPlayer itself. messageId 0 means "nothing is
    // loaded", matching playingVoiceMessageId_'s own convention.
    void setVoicePlaybackState(qlonglong messageId, bool playing, qint64 positionMs, qint64 durationMs) {
        playingVoiceMessageId_ = messageId;
        voicePlaying_ = playing;
        voicePositionMs_ = positionMs;
        voiceDurationMs_ = durationMs;
        update();
    }

    void enterSelectionMode(int startIndex) {
        // Bounds-checked before touching mode state at all: the
        // long-press timer (see mousePressEvent) fires up to 450ms after
        // the press, during which a chat switch or live rebuild could
        // have replaced items_ with a shorter list - entering selection
        // mode anyway and letting toggleItemSelected()'s own bounds check
        // silently no-op would leave multiSelectMode_ stuck true with
        // nothing selected and no visible way out (the exact "toolbar
        // shows but there is no X" symptom a crash report also matched).
        if (startIndex < 0 || startIndex >= items_.size()) {
            return;
        }
        setMultiSelectMode(true);
        toggleItemSelected(startIndex);
    }
    void exitSelectionMode() { setMultiSelectMode(false); }

private:
    // Grows the anchor..pos pair out to whole words, or to the whole
    // message, depending on the granularity the gesture set. Real Telegram
    // does the same in Ui::Text::String::adjustSelection(), called through
    // Element::selectionFromStates() on every mouse move while a selection
    // is in progress - not only once when it starts, which is what makes a
    // word-mode drag snap outward as it passes each word boundary.
    void applySelectionRange(int pos) {
        if (selectionItemIndex_ < 0 || selectionItemIndex_ >= items_.size()) {
            return;
        }
        const HistoryItem& item = items_[selectionItemIndex_];
        if (selectType_ == SelectType::Paragraphs || !item.doc) {
            selectionStart_ = 0;
            selectionEnd_ = item.textLength;
            return;
        }
        int from = qMin(selectionAnchor_, pos);
        int to = qMax(selectionAnchor_, pos);
        if (selectType_ == SelectType::Words) {
            QTextCursor cursor(item.doc.get());
            cursor.setPosition(from);
            cursor.movePosition(QTextCursor::StartOfWord);
            from = cursor.position();
            cursor.setPosition(to);
            cursor.movePosition(QTextCursor::EndOfWord);
            to = cursor.position();
        }
        // Clamped the same way docPositionAt() clamps - a word at the end
        // of the text must not grow into the timestamp/tick span appended
        // after it (see textLength's own comment).
        selectionStart_ = qMin(from, item.textLength);
        selectionEnd_ = qMin(to, item.textLength);
    }

    void clearSelection() {
        if (selectionItemIndex_ >= 0) {
            selectionItemIndex_ = -1;
            selectionStart_ = selectionEnd_ = -1;
            selectionAnchor_ = -1;
            selectType_ = SelectType::Chars;
            update();
        }
    }

    // Same bubbleLeft/bubbleTop geometry paintItem() works out for the
    // bubble body rect, isolated here so hit-testing (mouse events) and
    // painting agree on where the document actually sits, without
    // duplicating paintItem() itself.
    QPointF textOrigin(const HistoryItem& item, int rowTop) const {
        int rowMargin = kRowVerticalMargin(item.attached);
        int bubbleTop = rowTop + rowMargin / 2;
        int viewportWidth = width();
        int bubbleOuterWidth = item.contentWidth + bubbleMarginH() * 2;
        int bubbleLeft;
        if (item.kind == MessageKind::Outgoing) {
            bubbleLeft = viewportWidth - bubbleOuterWidth - sideMargin();
        } else if (item.kind == MessageKind::Incoming) {
            bubbleLeft = sideMargin();
        } else {
            bubbleLeft = (viewportWidth - bubbleOuterWidth) / 2;
        }
        return QPointF(bubbleLeft + bubbleMarginH(), bubbleTop + bubbleMarginV());
    }

    int itemIndexAt(const QPoint& pos) const {
        int y = pos.y();
        for (int i = firstIndexNotBefore(y); i < items_.size(); ++i) {
            if (items_[i].top > y) {
                break;
            }
            if (items_[i].hidden) {
                continue;
            }
            if (y >= items_[i].top && y < items_[i].top + items_[i].height) {
                return i;
            }
        }
        return -1;
    }

    // Character position within item.doc closest to a viewport-local
    // point - Qt::FuzzyHit clamps to the nearest valid position rather
    // than requiring an exact hit, which is what lets a drag continue
    // past the actual text bounds (into the bubble's own padding, or
    // above/below the item) and still extend the selection sensibly.
    // Capped at item.textLength: the timestamp/tick run painted after the
    // real text is metadata, not content, and must not be selectable or
    // copyable - dragging into or past it just holds the selection at
    // the text's own end instead of reaching past it.
    int docPositionAt(const HistoryItem& item, const QPoint& viewportPos) const {
        int rowTop = item.top;
        QPointF docLocal = QPointF(viewportPos) - textOrigin(item, rowTop);
        int pos = item.doc->documentLayout()->hitTest(docLocal, Qt::FuzzyHit);
        if (pos < 0) {
            pos = 0;
        }
        return qMin(pos, item.textLength);
    }

    HistoryItem buildItem(const HistoryEntry& entry) const {
        HistoryItem item;
        item.kind = entry.kind;
        item.rawText = entry.text;
        item.attached = entry.attached;
        item.showTail = entry.showTail;
        item.messageId = entry.messageId;

        if (entry.isVoiceNote) {
            item.isVoice = true;
            item.voiceDuration = entry.voiceDuration;
            item.voicePath = entry.filePath;
            item.voiceWaveform = entry.voiceWaveform;
            item.rawText = "Voice message";
            item.contentWidth = style::ConvertScale(224);
            // 44px play button/waveform row (matching the real
            // msgFileLayout.thumbSize circle, see paintVoiceContent()) plus
            // a duration/time line below.
            item.contentHeight = style::ConvertScale(64);
            int tailExtra = entry.showTail ? bubbleTailHeight() : 0;
            item.height = item.contentHeight + bubbleMarginV() * 2 + tailExtra + kRowVerticalMargin(entry.attached);
            return item;
        }

        if (!entry.albumFilePaths.isEmpty()) {
            QVector<QPixmap> loaded;
            QVector<QString> loadedPaths;
            std::vector<QSize> sizes;
            for (const QString& path : entry.albumFilePaths) {
                QPixmap p;
                if (p.load(path)) {
                    sizes.push_back(p.size());
                    loaded.push_back(std::move(p));
                    loadedPaths.push_back(path);
                }
            }
            if (loaded.size() > 1) {
                // Real algorithm (Ui::LayoutMediaGroup, ui/grouped_layout.cpp -
                // groups by aspect ratio into balanced rows, not a naive
                // grid), not a guess at how Telegram's mosaic looks.
                // Real chat.style: historyGroupWidthMax (aliases
                // maxMediaSize: 430px), historyGroupWidthMin (aliases
                // minPhotoSize: 100px), historyGroupSkip: 4px.
                auto layout = Ui::LayoutMediaGroup(sizes, /*maxWidth=*/style::ConvertScale(430),
                                                    /*minWidth=*/style::ConvertScale(100), /*spacing=*/style::ConvertScale(4));
                int maxRight = 0;
                int maxBottom = 0;
                item.albumPixmaps.reserve(static_cast<int>(layout.size()));
                item.albumLayout.reserve(static_cast<int>(layout.size()));
                for (std::size_t k = 0; k < layout.size(); ++k) {
                    QRect rect = layout[k].geometry;
                    item.albumLayout.push_back(rect);
                    item.albumPixmaps.push_back(
                        loaded[static_cast<int>(k)].scaled(rect.size(), Qt::KeepAspectRatioByExpanding,
                                                            Qt::SmoothTransformation));
                    maxRight = qMax(maxRight, rect.right() + 1);
                    maxBottom = qMax(maxBottom, rect.bottom() + 1);
                }
                item.contentWidth = maxRight;
                item.contentHeight = maxBottom;
                item.fillsBubble = true;
                item.albumImagePaths = loadedPaths;
                int tailExtra = entry.showTail ? bubbleTailHeight() : 0;
                item.height =
                    item.contentHeight + bubblePaddingV(true) * 2 + tailExtra + kRowVerticalMargin(entry.attached);
                return item;
            }
        }

        QPixmap pixmap;
        item.isImage = !entry.filePath.isEmpty() && pixmap.load(entry.filePath);
        if (item.isImage) {
            item.imagePath = entry.filePath;
            // Real chat.style: maxMediaSize: 430px.
            item.pixmap = pixmap.scaledToWidth(style::ConvertScale(430), Qt::SmoothTransformation);
            item.contentWidth = item.pixmap.width();
            item.contentHeight = item.pixmap.height();
            item.fillsBubble = true;
            int tailExtra = entry.showTail ? bubbleTailHeight() : 0;
            item.height =
                item.contentHeight + bubblePaddingV(true) * 2 + tailExtra + kRowVerticalMargin(entry.attached);
            return item;
        }

        QFont bubbleFont = QGuiApplication::font();
        // Real ui/basic.style/chat.style: fsize: 13px (msgFont), same size
        // for every bubble's body text - the earlier 15px for non-System
        // messages was never a real value.
        bubbleFont.setPixelSize(style::ConvertScale(13));

        QString html;
        bool richText = entry.kind != MessageKind::System;
        if (entry.kind == MessageKind::System) {
            html = entry.text;
        } else {
            // entry.date == 0 means this message is genuinely happening
            // right now (a live append, not loaded from history) - see
            // StoredMessage::date's own comment for where a real value
            // comes from otherwise.
            QDateTime when =
                entry.date > 0 ? QDateTime::fromSecsSinceEpoch(entry.date) : QDateTime::currentDateTime();
            QFont metaFont = bubbleFont;
            metaFont.setPixelSize(bubbleMetaFontSize());
            QFontMetricsF metaMetrics(metaFont);
            item.metaText = when.toString("HH:mm");
            item.metaWidth = metaMetrics.horizontalAdvance(item.metaText);
            if (entry.kind == MessageKind::Outgoing) {
                item.showTick = true;
                item.readByPeer = entry.readByPeer;
                item.metaWidth += metaTickGap() + tickSize();
            }
            // Space for the metadata, not the metadata itself - it is
            // painted into the bubble's corner afterwards, see
            // paintMessageMeta(). Non-breaking spaces specifically:
            // ordinary trailing spaces are excluded from
            // QTextLine::naturalTextWidth() and from the document's own
            // idealWidth(), so they would neither hold the space open on
            // the last line nor widen a one-line bubble to make room.
            double spaceWidth = metaMetrics.horizontalAdvance(QChar(0x00A0));
            int spaces = spaceWidth > 0.0
                              ? static_cast<int>(std::ceil((item.metaWidth + metaTextGap()) / spaceWidth))
                              : 8;
            QString metaSlot(spaces, QChar(0x00A0));
            QString escaped = entry.text.toHtmlEscaped();
            escaped.replace("\n", "<br>");

            // Reply header, above everything else in the bubble - the accent
            // bar, the quoted author and one line of the quoted text, the
            // same three parts real Telegram shows.
            //
            // Built into this same QTextDocument rather than painted as its
            // own block with its own geometry: the document is what every
            // size in this class is derived from (idealWidth, size().height()
            // and paintMessageMeta's line metrics), so a header inside it is
            // measured and positioned by construction. A separately painted
            // one would have to have its height added by hand to each of the
            // paths that compute item.height, and any miss would put the
            // painting and the hit-testing out of step.
            //
            // A table, not a div with border-left: Qt's rich text engine
            // ignores CSS borders on blocks (verified by rendering both - the
            // border variant simply had no bar at all), while a table cell
            // with bgcolor does draw.
            QString replyHeaderHtml;
            if (!entry.replyToText.isEmpty() || !entry.replyToSender.isEmpty()) {
                QFont quoteFont = bubbleFont;
                quoteFont.setPixelSize(bubbleMetaFontSize());
                // Elided here rather than left to wrap: a quote is one line in
                // Telegram, and letting a long one wrap would also drag the
                // whole bubble out to the maximum width.
                QString quote = QFontMetrics(quoteFont)
                                     .elidedText(entry.replyToText.simplified(), Qt::ElideRight,
                                                  maxBubbleContentWidth() - style::ConvertScale(24));
                const QColor quoteColor =
                    (entry.kind == MessageKind::Outgoing ? st::msgOutDateFg : st::msgInDateFg)->c;
                QString sender = entry.replyToSender.isEmpty() ? QString("Message") : entry.replyToSender;
                replyHeaderHtml = QString("<table cellspacing=\"0\" cellpadding=\"0\"><tr>"
                                           "<td bgcolor=\"#3a8ee6\" width=\"3\"></td><td width=\"6\"></td>"
                                           "<td><b style=\"color:#3a8ee6;\">%1</b><br>"
                                           "<span style=\"color:%2; font-size:%3px;\">%4</span></td>"
                                           "</tr></table>")
                                      .arg(sender.toHtmlEscaped())
                                      .arg(quoteColor.name())
                                      .arg(bubbleMetaFontSize())
                                      .arg(quote.toHtmlEscaped());
            }

            // Everything up to here (the reply header, the message text, plus
            // the sender-name prefix added just below if any) is real content;
            // the timestamp/tick span appended after this point is metadata,
            // not content - measured by rendering this part alone into a
            // throwaway document, since HTML entity decoding (&nbsp;,
            // etc.) means character counts do not map 1:1 between source
            // HTML and the document's own plain-text positions.
            //
            // The header has to be inside this measurement, not prepended
            // afterwards: item.textLength is the cap on how far a selection
            // may reach (see documentPositionForSelection), so leaving the
            // header out would shift every position in the document past it
            // and clamp selection short of the end of the real text.
            QString textOnlyHtml = escaped;
            if (entry.kind == MessageKind::Incoming && !entry.attached && !entry.senderName.isEmpty()) {
                textOnlyHtml =
                    QString("<b style=\"color:#3a8ee6;\">%1</b><br>%2").arg(entry.senderName.toHtmlEscaped(), escaped);
            }
            textOnlyHtml = replyHeaderHtml + textOnlyHtml;
            {
                QTextDocument measuring;
                measuring.setHtml(textOnlyHtml);
                item.textLength = measuring.toPlainText().length();
            }

            // Only the reservation goes into the document; the timestamp
            // itself, in real msgInDateFg/msgOutDateFg, is painted over it
            // by paintMessageMeta().
            html = QString("%1<span style=\"font-size:%2px;\">%3</span>")
                       .arg(escaped)
                       .arg(bubbleMetaFontSize())
                       .arg(metaSlot);
            if (entry.kind == MessageKind::Incoming && !entry.attached && !entry.senderName.isEmpty()) {
                html = QString("<b style=\"color:#3a8ee6;\">%1</b><br>%2").arg(entry.senderName.toHtmlEscaped(), html);
            }
            html = replyHeaderHtml + html;
        }

        item.doc = std::make_shared<QTextDocument>();
        item.doc->setDefaultFont(bubbleFont);
        if (richText) {
            item.doc->setHtml(html);
        } else {
            item.doc->setPlainText(html);
        }
        item.doc->setTextWidth(maxBubbleContentWidth());
        if (entry.kind == MessageKind::System) {
            // No metadata span appended for system lines - the whole
            // thing is "text", nothing to clamp.
            item.textLength = item.doc->characterCount();
        }
        // ceil, not a truncating cast: idealWidth() is fractional, and
        // handing the document back a width even a fraction of a pixel
        // narrower than it asked for makes it wrap - so a bubble that fits
        // on one line got laid out on two, and the run of non-breaking
        // spaces reserved for the tick (see above) was pushed onto a line of
        // its own.
        int idealWidth = qBound(minBubbleContentWidth(), static_cast<int>(std::ceil(item.doc->idealWidth())),
                                 maxBubbleContentWidth());
        item.doc->setTextWidth(idealWidth);
        item.contentWidth = idealWidth;
        item.contentHeight = static_cast<int>(item.doc->size().height());

        int bubbleOuterHeight = item.contentHeight + bubbleMarginV() * 2;
        if (entry.kind != MessageKind::System && entry.showTail) {
            bubbleOuterHeight += bubbleTailHeight();
        }
        item.height = bubbleOuterHeight + kRowVerticalMargin(entry.attached);
        return item;
    }

    static int kRowVerticalMargin(bool attached) { return attached ? 2 : 7; }

    void paintItem(QPainter& painter, const HistoryItem& item, int rowTop, bool isSelected, int index) const {
        int rowMargin = kRowVerticalMargin(item.attached);
        int bubbleOuterHeight = item.height - rowMargin;
        int bubbleTop = rowTop + rowMargin / 2;
        int viewportWidth = width();

        int paddingH = bubblePaddingH(item.fillsBubble);
        int paddingV = bubblePaddingV(item.fillsBubble);
        int bubbleOuterWidth = item.contentWidth + paddingH * 2;
        int bubbleLeft;
        if (item.kind == MessageKind::Outgoing) {
            bubbleLeft = viewportWidth - bubbleOuterWidth - sideMargin();
        } else if (item.kind == MessageKind::Incoming) {
            bubbleLeft = sideMargin();
        } else {
            bubbleLeft = (viewportWidth - bubbleOuterWidth) / 2;
        }
        // A uniform rightward shift for every kind (Outgoing's already-
        // right-aligned position included) - the checkbox always lives in
        // the same freed strip near the left edge regardless of which side
        // a given bubble sits on, matching real Telegram's own selection
        // mode rather than a per-side squeeze.
        bool selectable = item.kind != MessageKind::System;
        if (multiSelectMode_ && selectable) {
            bubbleLeft += qRound(checkboxGutterWidth() * selectionReveal_);
        }
        // The row being swiped follows the cursor, so the gesture has visible
        // feedback while it is happening rather than only when it fires. Only
        // the bubble moves - the selection checkbox gutter and everything
        // else stay put, the same way Telegram slides just the message.
        if (swipeActive_ && index == swipeIndex_) {
            bubbleLeft += swipeOffset_;
        }

        if (!item.albumPixmaps.isEmpty()) {
            int albumTailExtra = item.showTail ? bubbleTailHeight() : 0;
            QRect body(bubbleLeft, bubbleTop, bubbleOuterWidth, bubbleOuterHeight - albumTailExtra);
            bool tailOnLeft = item.kind != MessageKind::Outgoing;
            Ui::BubbleRounding rounding =
                bubbleRounding(tailOnLeft, item.showTail, item.attached);
            paintBubbleBody(painter, body, viewportWidth, item.kind == MessageKind::Outgoing, tailOnLeft,
                             item.showTail, item.attached);
            // Clipped to the bubble's own rounded shape - media fills the
            // bubble edge-to-edge now (padding is 0, see bubblePadding()),
            // so without this its square corners would poke out past the
            // rounded background underneath.
            painter.save();
            painter.setClipPath(bubbleClipPath(body, rounding), Qt::IntersectClip);
            for (int k = 0; k < item.albumPixmaps.size(); ++k) {
                const QRect& rect = item.albumLayout[k];
                painter.drawPixmap(bubbleLeft + paddingH + rect.x(), bubbleTop + paddingV + rect.y(),
                                    item.albumPixmaps[k]);
            }
            painter.restore();
            drawSelectionCheckbox(painter, index, bubbleTop, bubbleOuterHeight);
            return;
        }

        if (item.isVoice) {
            bool isOutgoingVoice = item.kind == MessageKind::Outgoing;
            int voiceTailExtra = item.showTail ? bubbleTailHeight() : 0;
            QRect body(bubbleLeft, bubbleTop, bubbleOuterWidth, bubbleOuterHeight - voiceTailExtra);
            paintBubbleBody(painter, body, viewportWidth, isOutgoingVoice, !isOutgoingVoice, item.showTail,
                             item.attached);
            painter.save();
            painter.translate(bubbleLeft + paddingH, bubbleTop + paddingV);
            paintVoiceContent(painter, item);
            painter.restore();
            drawSelectionCheckbox(painter, index, bubbleTop, bubbleOuterHeight);
            return;
        }

        if (item.isImage) {
            int imageTailExtra = item.showTail ? bubbleTailHeight() : 0;
            QRect body(bubbleLeft, bubbleTop, bubbleOuterWidth, bubbleOuterHeight - imageTailExtra);
            bool tailOnLeft = item.kind != MessageKind::Outgoing;
            Ui::BubbleRounding rounding =
                bubbleRounding(tailOnLeft, item.showTail, item.attached);
            paintBubbleBody(painter, body, viewportWidth, item.kind == MessageKind::Outgoing, tailOnLeft,
                             item.showTail, item.attached);
            painter.save();
            painter.setClipPath(bubbleClipPath(body, rounding), Qt::IntersectClip);
            painter.drawPixmap(bubbleLeft + paddingH, bubbleTop + paddingV, item.pixmap);
            painter.restore();
            drawSelectionCheckbox(painter, index, bubbleTop, bubbleOuterHeight);
            return;
        }

        if (item.kind == MessageKind::System) {
            QRectF body(bubbleLeft, bubbleTop, bubbleOuterWidth, bubbleOuterHeight);
            QPainterPath path = roundedRectPath(body, bubbleLargeRadius(), bubbleLargeRadius(), bubbleLargeRadius(),
                                                 bubbleLargeRadius());
            // Phase 6b: real msgServiceBg/msgServiceFg (semi-transparent
            // dark green fill + active-blue text in day-blue), not the old
            // flat light gray guess.
            painter.fillPath(path, st::msgServiceBg->c);
            painter.setPen(st::msgServiceFg->c);
            painter.save();
            painter.translate(bubbleLeft + paddingH, bubbleTop + paddingV);
            drawDocument(painter, item, isSelected);
            painter.restore();
            return;
        }

        bool isOutgoing = item.kind == MessageKind::Outgoing;
        int tailExtra = item.showTail ? bubbleTailHeight() : 0;
        QRect body(bubbleLeft, bubbleTop, bubbleOuterWidth, bubbleOuterHeight - tailExtra);
        paintBubbleBody(painter, body, viewportWidth, isOutgoing, !isOutgoing, item.showTail, item.attached);
        painter.save();
        painter.translate(bubbleLeft + paddingH, bubbleTop + paddingV);
        drawDocument(painter, item, isSelected);
        if (!item.metaText.isEmpty()) {
            paintMessageMeta(painter, item);
        }
        painter.restore();
        drawSelectionCheckbox(painter, index, bubbleTop, bubbleOuterHeight);
    }

    // Formats seconds as Telegram's own voice-note duration label ("0:07",
    // "1:23") - no hours component, a voice note is never that long.
    static QString formatVoiceDuration(int totalSeconds) {
        int minutes = totalSeconds / 60;
        int seconds = totalSeconds % 60;
        return QString("%1:%2").arg(minutes).arg(seconds, 2, 10, QChar('0'));
    }

    // Painted entirely by hand rather than through QTextDocument (unlike
    // drawDocument() above) - a voice note's content is a play/pause
    // button, a progress track and a duration label, not flowing text, so
    // the tick-slot trick the rest of this class uses to reserve space
    // inside a text run does not apply here. Origin (0,0) is the bubble's
    // own content origin, same convention drawDocument() uses.
    void paintVoiceContent(QPainter& painter, const HistoryItem& item) const {
        bool isOutgoing = item.kind == MessageKind::Outgoing;
        // Real chat.style: msgFileLayout.thumbSize: 44px - the same circular
        // play-button size real Telegram uses for a voice note (it reuses
        // the generic file-attachment thumb layout).
        int diameter = style::ConvertScale(44);
        // Play button + waveform sit in a top row of exactly diameter's
        // height; duration/time gets whatever the bubble's own contentHeight
        // leaves below that (see buildItem()'s voice branch for the total).
        QRectF circleRect(0, 0, diameter, diameter);

        painter.save();
        painter.setRenderHint(QPainter::Antialiasing);
        QColor accent = st::windowBgActive->c;
        QColor circleFill = isOutgoing ? QColor(255, 255, 255, 60) : QColor(accent.red(), accent.green(), accent.blue(), 40);
        painter.setBrush(circleFill);
        QPen circlePen(accent, style::ConvertScale(1.5));
        painter.setPen(circlePen);
        painter.drawEllipse(circleRect);

        bool isThisPlaying = item.messageId != 0 && item.messageId == playingVoiceMessageId_;
        bool showPause = isThisPlaying && voicePlaying_;
        double iconInset = diameter * 0.28;
        QRectF iconRect = circleRect.adjusted(iconInset, iconInset, -iconInset, -iconInset);
        paintGlyph(painter, iconRect, showPause ? Glyph::Pause : Glyph::Play);

        int trackLeft = diameter + style::ConvertScale(10);
        int trackWidth = item.contentWidth - trackLeft;
        double progress = 0.0;
        if (isThisPlaying && voiceDurationMs_ > 0) {
            progress = qBound(0.0, static_cast<double>(voicePositionMs_) / static_cast<double>(voiceDurationMs_), 1.0);
        }
        paintVoiceWaveform(painter, item, isOutgoing, trackLeft, static_cast<int>(circleRect.center().y()),
                            trackWidth, progress);

        QFont durationFont = QGuiApplication::font();
        durationFont.setPixelSize(bubbleMetaFontSize());
        painter.setFont(durationFont);
        painter.setPen((isOutgoing ? st::msgOutDateFg : st::msgInDateFg)->c);
        int elapsedSeconds = static_cast<int>(voicePositionMs_ / 1000);
        QString durationText = formatVoiceDuration(isThisPlaying ? elapsedSeconds : item.voiceDuration);
        int durationTop = diameter + style::ConvertScale(2);
        QRectF durationRect(trackLeft, durationTop, trackWidth, item.contentHeight - durationTop);
        painter.drawText(durationRect, Qt::AlignLeft | Qt::AlignTop, durationText);
        painter.restore();
    }

    // Ported from real Telegram Desktop's own HistoryView::PaintWaveform()
    // (telegram_fork/tdesktop's history/view/media/history_view_document.cpp,
    // already vendored under src/history/view/media/ - see its own comment
    // for why it cannot be linked in directly: it is a method on a Media
    // subclass entangled with Data::Session/HistoryItem/Main::Session, none
    // of which this project vendors). The bar-layout math and every style
    // value below (st::msgWaveformBar/Skip/Min/Max, st::msgWaveform*Active/
    // Inactive) are copied from that function and its real, already-compiled
    // ui/chat/chat.style + colors.palette declarations - only the hover/
    // once-view particle-effect branches are dropped, since this player has
    // no hover-preview or self-destructing voice messages to support.
    void paintVoiceWaveform(QPainter& painter, const HistoryItem& item, bool isOutgoing, int left, int centerY,
                             int availableWidth, double progress) const {
        const QByteArray& wf = item.voiceWaveform;
        int barWidth = st::msgWaveformBar;
        int barSkip = st::msgWaveformSkip;
        int barMin = st::msgWaveformMin;
        int barMax = st::msgWaveformMax;
        int wfSize = !wf.isEmpty() ? static_cast<int>(wf.size()) : 100;
        int barCount = qMin(availableWidth / (barWidth + barSkip), wfSize);
        if (barCount <= 0 || availableWidth <= 0) {
            return;
        }
        int barNormValue = 1;
        for (unsigned char sample : wf) {
            barNormValue = qMax(barNormValue, int(sample));
        }
        barNormValue += 1;
        int maxDelta = barMax - barMin;
        int activeWidth = qRound(availableWidth * progress);
        QColor active = (isOutgoing ? st::msgWaveformOutActive : st::msgWaveformInActive)->c;
        QColor inactive = (isOutgoing ? st::msgWaveformOutInactive : st::msgWaveformInInactive)->c;
        painter.setPen(Qt::NoPen);
        int barLeft = 0;
        int sum = 0;
        int maxValue = 0;
        for (int i = 0; i < wfSize; ++i) {
            int value = !wf.isEmpty() ? static_cast<unsigned char>(wf[i]) : 0;
            if (sum + barCount < wfSize) {
                maxValue = qMax(maxValue, value);
                sum += barCount;
                continue;
            }
            sum = sum + barCount - wfSize;
            if (sum < (barCount + 1) / 2) {
                maxValue = qMax(maxValue, value);
            }
            int barValue = ((maxValue * maxDelta) + (barNormValue / 2)) / barNormValue;
            int barHeight = barMin + barValue;
            double barTop = centerY - barHeight / 2.0;
            const QColor& color = (left + barLeft >= left + activeWidth) ? inactive : active;
            painter.fillRect(QRectF(left + barLeft, barTop, barWidth, barHeight), color);
            barLeft += barWidth + barSkip;
            maxValue = (sum < (barCount + 1) / 2) ? 0 : value;
        }
    }

    // Paints a translucent highlight rect under the selected range, then
    // draws the document completely normally on top via
    // drawContents() - deliberately not QAbstractTextDocumentLayout's
    // own Selection/PaintContext mechanism (an earlier version of this
    // function used that instead), because a Selection there is a
    // QTextCharFormat overlay applied to the actual text, and changed
    // the text's own color instead of just sitting behind it. Painting a
    // background rect manually and leaving drawContents() completely
    // untouched guarantees the glyphs themselves are never altered.
    void drawDocument(QPainter& painter, const HistoryItem& item, bool isSelected) const {
        if (isSelected && selectionStart_ != selectionEnd_) {
            paintSelectionHighlight(painter, item);
        }
        item.doc->drawContents(&painter);
    }

    // One rect per text line the selection actually overlaps, each
    // spanning only the selected columns on that line (not the whole
    // line width) - handles a selection crossing multiple lines/blocks
    // the same way any real text editor's highlight does.
    void paintSelectionHighlight(QPainter& painter, const HistoryItem& item) const {
        int start = qMin(selectionStart_, selectionEnd_);
        int end = qMax(selectionStart_, selectionEnd_);
        painter.save();
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(64, 167, 227, 90));
        for (QTextBlock block = item.doc->begin(); block.isValid(); block = block.next()) {
            int blockStart = block.position();
            int blockEnd = blockStart + block.length();  // length() includes the block separator
            if (blockEnd <= start || blockStart >= end) {
                continue;
            }
            QTextLayout* layout = block.layout();
            if (layout == nullptr) {
                continue;
            }
            QPointF blockOrigin = item.doc->documentLayout()->blockBoundingRect(block).topLeft();
            int rangeStartInBlock = qMax(start, blockStart) - blockStart;
            int rangeEndInBlock = qMin(end, blockStart + block.length() - 1) - blockStart;
            for (int i = 0; i < layout->lineCount(); ++i) {
                QTextLine line = layout->lineAt(i);
                int lineStart = line.textStart();
                int lineEnd = lineStart + line.textLength();
                if (lineEnd <= rangeStartInBlock || lineStart >= rangeEndInBlock) {
                    continue;
                }
                int segStart = qMax(lineStart, rangeStartInBlock);
                int segEnd = qMin(lineEnd, rangeEndInBlock);
                qreal x1 = line.cursorToX(segStart);
                qreal x2 = line.cursorToX(segEnd);
                QRectF rect(blockOrigin.x() + qMin(x1, x2), blockOrigin.y() + line.y(), qAbs(x2 - x1), line.height());
                painter.drawRect(rect);
            }
        }
        painter.restore();
    }

    // Draws the outgoing "sent" tick into the space reserved for it at the
    // end of the last line, with painter already translated to the
    // document's own origin. Positioned from the text layout rather than
    // from the bubble's bottom-right corner: the last line is not
    // necessarily the widest one, so the timestamp this follows can sit well
    // to the left of the bubble's right edge.
    // Timestamp, and the delivery tick on our own messages, in the bubble's
    // bottom-right corner - where real Telegram puts them
    // (HistoryView::Message draws its "info" against the bubble's right
    // edge, not against the end of the text). Right-aligned to
    // item.contentWidth, which IS the bubble's inner width, so a message
    // whose last line is shorter than the bubble - every wrapped message
    // whose width was set by an earlier line - still keeps its timestamp in
    // the corner instead of trailing the last word. The space is reserved
    // on the last line in buildItem(), so nothing here can overlap text: if
    // the last line plus the metadata does not fit, the reserved run wraps
    // and the metadata gets a line of its own, again exactly as in
    // Telegram.
    void paintMessageMeta(QPainter& painter, const HistoryItem& item) const {
        QTextBlock block = item.doc->lastBlock();
        QTextLayout* layout = block.layout();
        if (layout == nullptr || layout->lineCount() == 0) {
            return;
        }
        QTextLine line = layout->lineAt(layout->lineCount() - 1);
        QPointF blockOrigin = item.doc->documentLayout()->blockBoundingRect(block).topLeft();
        double baseline = blockOrigin.y() + line.y() + line.ascent();
        double centerY = blockOrigin.y() + line.y() + line.height() / 2.0;
        double right = item.contentWidth;

        painter.save();
        if (item.showTick) {
            // Double tick once the other side has read it, single while it
            // is only delivered - the same two assets real Telegram Desktop
            // uses (history_sent / history_received). Both are the same
            // size, so nothing moves when a read receipt arrives.
            paintGlyph(painter, QRectF(right - tickSize(), centerY - tickSize() / 2.0, tickSize(), tickSize()),
                        item.readByPeer ? Glyph::CheckDouble : Glyph::Check);
            right -= tickSize() + metaTickGap();
        }
        QFont metaFont = painter.font();
        metaFont.setPixelSize(bubbleMetaFontSize());
        painter.setFont(metaFont);
        // Real msgInDateFg/msgOutDateFg (msgOutDateFg is the green matching
        // the outgoing bubble, not the same gray as an incoming one).
        painter.setPen((item.kind == MessageKind::Outgoing ? st::msgOutDateFg : st::msgInDateFg)->c);
        double textWidth = QFontMetricsF(metaFont).horizontalAdvance(item.metaText);
        painter.drawText(QPointF(right - textWidth, baseline), item.metaText);
        painter.restore();
    }

    // Recomputes top_ for every item from index onward, based on each
    // item's own (already-measured) height - O(n) from the given index,
    // used by the bulk operations above (setItems()/setSearchFilter()),
    // never per live message (see appendItem()'s O(1) path for that).
    void relayoutFrom(int index) {
        int y = index == 0 ? 0 : items_[index - 1].top + items_[index - 1].height;
        for (int i = index; i < items_.size(); ++i) {
            items_[i].top = y;
            if (items_[i].hidden) {
                // Height is left untouched (not zeroed) - the row simply
                // does not advance y while hidden, so its true height is
                // still there to resume from once the search filter
                // clears and it becomes visible again.
                continue;
            }
            y += items_[i].height;
        }
        totalHeight_ = y;
        // relayoutFrom(0) always starts items_.front() at top 0 (see y's
        // initial value above when index == 0, the only way this is ever
        // called - see setItems()/setSearchFilter()), so the shifted
        // layout prependItems() builds up over time (see its own comment)
        // is reset back to a plain 0-based layout here - contentBottom_
        // must be re-synced to match, or a live appendItem() call right
        // after a search-filter change would place its item using a
        // stale, no-longer-correct bottom coordinate.
        contentBottom_ = totalHeight_;
    }

    // No updateScrollRange() here anymore - the real Ui::ScrollArea
    // wrapping this canvas (see MainWindow's construction site) derives
    // its own scrollbar range from resizeGetHeight() automatically, the
    // same as any owned widget; this class no longer owns a scrollbar to
    // configure.

    // Real chat.style: msgMargin: margins(16px, 6px, 56px, 2px) - 16px is
    // the near-edge component (the 56px far side is room for a group
    // chat's inline sender avatar column, not applicable here).
    static int sideMargin() { return style::ConvertScale(16); }

    QVector<HistoryItem> items_;
    int totalHeight_ = 0;
    // Where the next appendItem() call should place its item - see
    // appendItem()'s own comment for why this cannot simply be
    // totalHeight_ once prependItems() has been called at least once.
    int contentBottom_ = 0;
    int lastTextItemIndex_ = -1;
    bool loadingMore_ = false;
};

// MainWindow::messages_ is declared as a plain QWidget* in the header (see
// its own comment there) so main_window.hpp does not need to expose
// HistoryCanvas's internals - every real call site is in this file, and
// always knows the true type, hence the cast.
HistoryCanvas* asCanvas(QWidget* widget) {
    return static_cast<HistoryCanvas*>(widget);
}

// Same reasoning as asCanvas() above - MediaViewer, like HistoryCanvas, is
// defined in this file rather than main_window.hpp, so MainWindow's own
// member is a plain QWidget* and gets cast back here at each use site.
MediaViewer* asMediaViewer(QWidget* widget) {
    return static_cast<MediaViewer*>(widget);
}

// RippleButton (a hand-rolled QPushButton + QVariantAnimation ripple) is
// gone as of Phase 5 (UI.md 13c) - every former call site now uses the real
// Ui::IconButton/Ui::RoundButton from lib_ui, which paint a real tdesktop
// ripple (RippleAnimation, see the .style ripple: field on each) natively.

// Real defaultDialogRow values (src/dialogs/dialogs.style): height 62px,
// photoSize 46px, padding margins(10,8,10,8), nameLeft/textLeft 68px -
// the old 64/46/10 here were pre-migration guesses, only photoSize/margin
// happened to already match.
// See the bubble-size functions above (main_window.cpp, HistoryCanvas) for
// why these are functions through style::ConvertScale() rather than raw
// constexpr values - same reasoning, this is SidebarCanvas's own
// self-painted content.
int sidebarRowHeight() { return style::ConvertScale(62); }
int sidebarAvatarSize() { return style::ConvertScale(46); }
int sidebarMargin() { return style::ConvertScale(10); }
int sidebarTextLeft() { return style::ConvertScale(68); }

// One sidebar row's content, laid out and painted by SidebarCanvas below -
// not a widget at all, same reasoning as HistoryItem for the message
// list.
struct SidebarRowData {
    qlonglong chatId = 0;
    QString nameElided;
    QString previewElided;
    QPixmap avatar;
    int unread = 0;
    bool active = false;
    // Real Telegram Desktop's own dialogs row shows the last message's
    // time in the top-right corner (dialogsDateFont/dialogsDateSkip,
    // dialogs.style) - pre-formatted here (not a raw qint64) since
    // building the row already computes nameElided/previewElided the same
    // way, and formatting per-paint would redo the same QDateTime work on
    // every repaint instead of once per chat-list update. Empty if the
    // chat has no last message at all (a brand new/empty chat).
    QString timeText;
};

// The sidebar chat list itself - a single custom-painted, virtualized
// canvas (Ui::RpWidget + manual layout/paint/hit-test), the same
// approach as HistoryCanvas and for the same reason: chatListWidget_ used
// to be a QListWidget with setItemWidget(), which is NOT virtualized -
// every row's widget exists for as long as it is in the list regardless
// of scroll position, real Telegram Desktop's own dialog list is a
// self-painted canvas for exactly this reason. Once
// TelegramClient::requestMoreChats() started loading a whole account's
// chat list instead of just its first page, that meant potentially
// hundreds of always-alive row widgets, and scrolling through them was
// nowhere near as smooth as the real client - virtualizing this the same
// way the message list already was fixes it the same way.
// A pure content widget - virtualized row painting (still one widget
// procedurally drawing every row, matching real Dialogs::InnerWidget's own
// architecture, see UI.md §7's note on HistoryView::ListWidget/HistoryCanvas
// following the same pattern) with no scrolling machinery of its own.
// Scrolling is a real Ui::ScrollArea wrapping this (see its construction
// site in MainWindow) - the hand-rolled KineticScroller class this used to
// drive itself as a QAbstractScrollArea is gone, replaced by
// Ui::ScrollArea's real momentum/touch scrolling (lib_ui/ui/widgets/
// scroll_area.*, already vendored - see UI.md's "never hand-roll what
// lib_ui already has" rule). A first attempt at this hit two apparent
// blockers that both turned out to be non-issues once actually root-
// caused: rpl::combine_previous "not a member of rpl" was an include-order
// bug in this file (scroll_area.h needs <rpl/rpl.h> included first - see
// this file's own top-of-file comment), not a gap in the vendored lib_rpl;
// and RpWidgetBase::leaveEvent() being `final` has a real extension point,
// leaveEventHook(), used below instead of trying to override the sealed
// method directly.
class SidebarCanvas : public Ui::RpWidget {
public:
    explicit SidebarCanvas(QWidget* parent = nullptr) : Ui::RpWidget(parent) {
        setMouseTracking(true);

        // Real values: dialogsTextStyle/nameStyle both key off fsize (13px,
        // ui/basic.style), name in semiboldFont, preview in normalFont;
        // dialogsUnreadFont is font(12px bold) - the old 14px/11px here
        // were pre-migration guesses.
        nameFont_ = font();
        nameFont_.setWeight(QFont::DemiBold);
        nameFont_.setPixelSize(style::ConvertScale(13));
        previewFont_ = font();
        previewFont_.setPixelSize(style::ConvertScale(13));
        badgeFont_ = font();
        badgeFont_.setPixelSize(style::ConvertScale(12));
        badgeFont_.setWeight(QFont::Bold);
        // dialogsDateFont (dialogs.style): font(13px), the row's own
        // regular weight - same size as name/preview, just its own color
        // role (dialogsDateFg).
        dateFont_ = font();
        dateFont_.setPixelSize(style::ConvertScale(13));
    }

    // Called from MainWindow::onChatListItemClicked() via a row click -
    // SidebarCanvas has no signals of its own, this is a plain callback
    // rather than needing Q_OBJECT/moc for a class that otherwise does
    // not need either.
    std::function<void(qlonglong)> onRowClicked;

    void setRows(const QVariantList& chats, qlonglong selectedChatId) {
        rows_.clear();
        rows_.reserve(chats.size());
        // Built once, not once per row: constructing a QFontMetrics loads
        // and locks the font engine, which is not free when the list is
        // hundreds of rows long and gets rebuilt on every chat-list push.
        const QFontMetrics nameMetrics(nameFont_);
        const QFontMetrics previewMetrics(previewFont_);
        for (const QVariant& entryVariant : chats) {
            QVariantMap entry = entryVariant.toMap();
            QString title = entry["title"].toString();
            QString preview = entry["preview"].toString();
            SidebarRowData row;
            row.chatId = entry["id"].toLongLong();
            row.unread = entry["unread"].toInt();
            row.active = entry["active"].toBool();
            row.avatar = chatAvatarPixmap(title, row.chatId, entry["photo"].toString(), sidebarAvatarSize());
            int textWidth = 260;  // generous; real elision happens against the actual row width at paint time
            row.nameElided = nameMetrics.elidedText(title, Qt::ElideRight, textWidth);
            QString previewText = preview.isEmpty() ? "No messages yet" : preview;
            row.previewElided = previewMetrics.elidedText(previewText, Qt::ElideRight, textWidth);
            qlonglong lastMessageDate = entry["lastMessageDate"].toLongLong();
            row.timeText = lastMessageDate > 0 ? sidebarTimeText(lastMessageDate) : QString();
            rows_.push_back(std::move(row));
        }
        selectedChatId_ = selectedChatId;
        hoveredIndex_ = -1;
        // Real RpWidget contract (UI.md §4/§7): resizeToWidth() re-runs
        // resizeGetHeight() below for the row count that just changed and
        // resizes this widget to match - the owning Ui::ScrollArea then
        // sees this widget's new height and adjusts its own scroll range
        // automatically (real Qt QScrollArea behavior), same effect the
        // old updateScrollRange()/verticalScrollBar()->setRange() had, but
        // driven by this widget's own size instead of a scrollbar object
        // this class no longer owns. The current scroll offset (owned by
        // the wrapping ScrollArea now) is preserved across a rebuild for
        // free - nothing here resets it, matching the old comment's intent.
        resizeToWidth(width());
        update();
    }

    void setSelectedChatId(qlonglong chatId) {
        if (selectedChatId_ == chatId) {
            return;
        }
        selectedChatId_ = chatId;
        update();
    }

    int rowCount() const { return rows_.size(); }

protected:
    // Real RpWidget contract (rp_widget.h) instead of a QAbstractScrollArea's
    // verticalScrollBar()->setRange() bookkeeping - this widget's own height
    // for a given width is simply every row stacked, no scroll math of its
    // own at all (the wrapping Ui::ScrollArea owns that entirely).
    int resizeGetHeight(int newWidth) override {
        return rows_.size() * sidebarRowHeight();
    }

    void paintEvent(QPaintEvent* event) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        // No scroll-offset subtraction needed any more: this widget's own
        // coordinate origin is the top of the full (scrollable) content,
        // and Qt/Ui::ScrollArea only ever calls paintEvent for the actually
        // visible sub-rect (event->rect()) - exactly what used to require
        // manually reading verticalScrollBar()->value() for.
        QRect visible = event->rect();
        // Was style.qss's "#chatListWidget { background-color: #ffffff; }":
        // the rows below paint themselves but not the gaps between them, so
        // without this the scrolled content shows through.
        painter.fillRect(visible, st::dialogsBg);
        for (int i = 0; i < rows_.size(); ++i) {
            int top = i * sidebarRowHeight();
            if (top + sidebarRowHeight() < visible.top() || top > visible.bottom()) {
                continue;
            }
            paintRow(painter, rows_[i], top, rows_[i].chatId == selectedChatId_, i == hoveredIndex_);
        }
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        int index = indexAt(event->pos());
        if (index != hoveredIndex_) {
            hoveredIndex_ = index;
            update();
        }
    }

    // RpWidgetBase::leaveEvent() is declared `final` (rp_widget.h) - this
    // is its real extension point instead, called from that sealed
    // override's own body.
    void leaveEventHook(QEvent*) override {
        if (hoveredIndex_ != -1) {
            hoveredIndex_ = -1;
            update();
        }
    }

    void mousePressEvent(QMouseEvent* event) override {
        int index = indexAt(event->pos());
        if (index >= 0 && onRowClicked) {
            onRowClicked(rows_[index].chatId);
        }
    }

    // No wheelEvent override any more - the real Ui::ScrollArea wrapping
    // this widget (see its construction site in MainWindow) handles wheel/
    // touch/momentum scrolling itself, the hand-rolled KineticScroller this
    // used to drive is gone.

private:
    int indexAt(const QPoint& pos) const {
        int index = pos.y() / sidebarRowHeight();
        return (index >= 0 && index < rows_.size()) ? index : -1;
    }

    // Phase 6a (UI.md 13c): every color here is now a real dialogsXxx
    // palette role (ui/colors.palette) instead of a literal hex - same
    // names real tdesktop's own Dialogs::Row painting code
    // (Telegram/SourceFiles/dialogs/dialogs_row.cpp) uses. Fonts/layout are
    // still this class's own pre-migration numbers, not yet ported to
    // st::-declared skips (a further, smaller pass, not blocking this one).
    void paintRow(QPainter& painter, const SidebarRowData& row, int top, bool selected, bool hovered) {
        const QColor background = (selected ? st::dialogsBgActive
                                             : hovered ? st::dialogsBgOver
                                                       : st::dialogsBg)
                                       ->c;
        painter.fillRect(QRect(0, top, width(), sidebarRowHeight()), background);

        painter.drawPixmap(sidebarMargin(), top + (sidebarRowHeight() - sidebarAvatarSize()) / 2, row.avatar);

        int textLeft = sidebarTextLeft();
        int badgeReserve = row.unread > 0 ? 40 : 0;
        int textWidth = width() - textLeft - sidebarMargin() - badgeReserve;

        // dialogsDateFont/dialogsDateSkip (dialogs.style): real Telegram
        // draws the last message's time top-right, on the same line as the
        // name - the name itself is elided (nameElided, computed against a
        // generous fixed width, see updateChatList()'s own comment) short
        // of the full row width specifically so this has somewhere to sit
        // without the two overlapping.
        int nameWidth = textWidth;
        if (!row.timeText.isEmpty()) {
            QFontMetrics dateMetrics(dateFont_);
            int dateWidth = dateMetrics.horizontalAdvance(row.timeText);
            QRect dateRect(width() - sidebarMargin() - dateWidth, top + 10, dateWidth, 20);
            painter.setPen((selected ? st::dialogsDateFgActive : st::dialogsDateFg)->c);
            painter.setFont(dateFont_);
            painter.drawText(dateRect, Qt::AlignLeft | Qt::AlignVCenter, row.timeText);
            nameWidth = qMax(0, textWidth - dateWidth - style::ConvertScale(5));
        }

        painter.setPen((selected ? st::dialogsNameFgActive : st::dialogsNameFg)->c);
        painter.setFont(nameFont_);
        painter.drawText(QRect(textLeft, top + 10, nameWidth, 20), Qt::AlignLeft | Qt::AlignVCenter, row.nameElided);

        int previewLeft = textLeft;
        if (row.active) {
            // A padlock = an encrypted session exists for this chat - see
            // zkgram_icons.style's zkgramIconLock/zkgramIconLockOnActive
            // comment for why the "active" (white) variant is needed on a
            // selected row.
            // top + 37 rather than the preview row's own top + 34: the
            // padlock's ink sits in the middle of its 13x13 box, so aligning
            // the boxes would leave it riding above the text's centre line.
            paintGlyph(painter, QRectF(previewLeft, top + 37, 13, 13),
                        selected ? Glyph::LockOnActive : Glyph::Lock);
            previewLeft += 17;
        }
        painter.setPen((selected ? st::dialogsTextFgActive : st::dialogsTextFg)->c);
        painter.setFont(previewFont_);
        painter.drawText(QRect(previewLeft, top + 34, textWidth - (previewLeft - textLeft), 20),
                          Qt::AlignLeft | Qt::AlignVCenter, row.previewElided);

        if (row.unread > 0) {
            // Real dialogsUnreadHeight/dialogsUnreadPadding (dialogs.style):
            // 19px tall, 5px padding each side - a circle for single-digit
            // counts since width floors at the height.
            QString badgeText = row.unread > 99 ? "99+" : QString::number(row.unread);
            QFontMetrics badgeMetrics(badgeFont_);
            int badgeHeight = style::ConvertScale(19);
            int badgePadding = style::ConvertScale(5);
            int badgeWidth = qMax(badgeHeight, badgeMetrics.horizontalAdvance(badgeText) + badgePadding * 2);
            // Aligned with the preview line (top + 34, 20px tall - the same
            // rect the preview text itself draws in below), not centered
            // across the whole row - real Telegram pairs the badge with
            // the message preview, under the time label, not floating in
            // the row's vertical middle.
            QRectF badgeRect(width() - sidebarMargin() - badgeWidth, top + 34 + (20 - badgeHeight) / 2.0,
                              badgeWidth, badgeHeight);
            painter.setPen(Qt::NoPen);
            painter.setBrush((selected ? st::dialogsUnreadBgActive : st::dialogsUnreadBg)->c);
            painter.drawRoundedRect(badgeRect, badgeHeight / 2.0, badgeHeight / 2.0);
            painter.setPen((selected ? st::dialogsUnreadFgActive : st::dialogsUnreadFg)->c);
            painter.setFont(badgeFont_);
            painter.drawText(badgeRect, Qt::AlignCenter, badgeText);
        }
    }

    QVector<SidebarRowData> rows_;
    qlonglong selectedChatId_ = 0;
    int hoveredIndex_ = -1;
    QFont nameFont_;
    QFont previewFont_;
    QFont badgeFont_;
    QFont dateFont_;
};

SidebarCanvas* asSidebar(QWidget* widget) {
    return static_cast<SidebarCanvas*>(widget);
}

// App icon (resources/app.ico, same file CMakeLists.txt separately embeds
// as the window/taskbar icon via app.rc) scaled up for the auth slide logo
// - avoids drawing/tracing any Telegram trademark artwork ourselves.
// Loaded from the Qt resource system (see resources.qrc),
// not a working-directory-relative path.
QPixmap loadAuthLogo(int size) {
    QPixmap icon(":/app.ico");
    if (icon.isNull()) {
        // Fallback so the slide still renders something if run from the
        // wrong working directory - a plain colored circle, not a logo.
        QPixmap fallback(size, size);
        fallback.fill(Qt::transparent);
        QPainter painter(&fallback);
        painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor("#2aabee"));
        painter.drawEllipse(0, 0, size, size);
        return fallback;
    }
    return icon.scaled(size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
}

}  // namespace

ChatInput::ChatInput(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    // zkgramComposeField (zkgram_icons.style): real historyComposeField
    // values (chat_helpers.style) - NOT defaultInputField, which is a
    // floating-label FORM field (semibold placeholder that shrinks/turns
    // blue on focus, heightMin 55px) wrong for a chat composer; its 55px
    // minimum overflowing this row's 36px is what clipped the placeholder
    // text in the screenshot that found this bug. No setFixedHeight()
    // here either, for the same reason - zkgramComposeField's own
    // heightMin (36px) is the real constraint now, not a second one
    // layered on top of it.
    field_ = new Ui::InputField(this, st::zkgramComposeField, Ui::InputField::Mode::MultiLine,
                                 rpl::single(QString("Message")));
    field_->setSubmitSettings(Ui::InputField::SubmitSettings::Enter);
    // Ui::RpWidget starts hidden by default, unlike plain QWidget - same
    // gap as startEncryptionButton_'s own earlier fix (see its comment in
    // this file), just hit again on a different widget.
    field_->show();
    layout->addWidget(field_);
    setFocusProxy(field_);

    field_->submits(
    ) | rpl::on_next([this](Qt::KeyboardModifiers) { Q_EMIT submitted(); }, field_->lifetime());
    field_->changes(
    ) | rpl::on_next([this] { Q_EMIT textChanged(); }, field_->lifetime());
}

QString ChatInput::toPlainText() const {
    return field_->getLastText();
}

void ChatInput::setPlainText(const QString& text) {
    field_->setText(text);
}

void ChatInput::clear() {
    field_->setText(QString());
}

void AuthSlide::paintEvent(QPaintEvent*) {
    // Real Telegram's own login screen is a flat white page (windowBg), not
    // a colored wallpaper - matches that exactly now, per explicit user
    // request, while the rest of the app (chat wallpaper, accent color)
    // keeps zkgram's purple identity. See zkgramAuthNextButton in
    // zkgram_icons.style for the matching real-blue button accent.
    QPainter painter(this);
    painter.fillRect(rect(), st::windowBg);
}

void AuthSlide::recenterCard() {
    // Manual centering of logo_+card_ as one group - the real "layout"
    // this class has, now that neither is inside a Qt layout manager (see
    // UI.md §4/§7 and this class's own constructor). card_ already knows
    // its own height (VerticalLayout::resizeGetHeight, computed from its
    // rows) whenever this runs - either from an actual resize, or from the
    // heightValue() subscription below firing because runSlide() just set
    // new text that wrapped onto a different number of lines.
    int totalHeight = logo_->height() + card_->height();
    int top = qMax(0, (height() - totalHeight) / 2);
    logo_->move((width() - logo_->width()) / 2, top);
    card_->moveToLeft((width() - card_->width()) / 2, top + logo_->height());
}

void AuthSlide::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    recenterCard();
    update();
}

AuthSlide::AuthSlide(QWidget* parent) : QWidget(parent) {
    setObjectName("authSlide");

    // logo_ sits above card_ as a plain sibling (see its own header
    // comment for why it cannot live inside the VerticalLayout) - both are
    // centered together manually in resizeEvent, the same "no Qt layout
    // manager, position children yourself" contract every real tdesktop
    // widget follows (UI.md §4/§7).
    logo_ = new QLabel(this);
    logo_->setPixmap(loadAuthLogo(96));
    logo_->setAlignment(Qt::AlignCenter);
    logo_->adjustSize();

    card_ = new Ui::VerticalLayout(this);
    card_->setObjectName("authCard");
    // Ui::RpWidget starts hidden by default (see title_'s own comment below
    // for the same gap) - card_ itself needs this too, or every child
    // inside it stays invisible regardless of their own show() calls,
    // since Qt visibility requires every ancestor to be visible as well.
    card_->show();

    static constexpr int kFieldMargin = 18;

    // Phase 6b (UI.md 13c): Ui::FlatLabel for title/subtitle/error (real
    // text rendering/wrap, not QLabel's).
    title_ = card_->add(object_ptr<Ui::FlatLabel>(card_, st::zkgramAuthTitle),
                         style::margins(0, kFieldMargin, 0, 0));
    title_->setObjectName("authTitle");
    // Ui::RpWidget starts hidden by default, unlike plain QWidget - same
    // gap as startEncryptionButton_'s own earlier fix; title_/subtitle_
    // are always visible whenever the slide itself is (unlike error_,
    // which stays conditionally hidden), so shown once here rather than
    // in runSlide()/showStatus().
    title_->show();

    subtitle_ = card_->add(object_ptr<Ui::FlatLabel>(card_, st::zkgramAuthDescription),
                            style::margins(0, kFieldMargin, 0, 0));
    subtitle_->setObjectName("authSubtitle");
    subtitle_->show();

    // Three real field types, not one QLineEdit toggled by echo mode: real
    // tdesktop has no single widget that is all of these - Ui::PasswordInput
    // (always-masked), Ui::MaskedInputField (plain phone-number entry), and
    // Ui::SentCodeField (digit-only, strips anything else live and can
    // auto-submit once enough digits are in - ui/widgets/sent_code_field.h)
    // are different C++ types. All three occupy the same layout slot;
    // runSlide() shows exactly one via activeInput() and setVisible().
    //
    // None of the three can go through card_->add() directly: Ui::
    // MaskedInputField/Ui::PasswordInput do not derive from Ui::RpWidget in
    // this project's vendored lib_ui (a real C2672/enable_if failure at
    // compile time, not a guess - same class of vendored-version mismatch
    // already found and documented for ui/widgets/multi_select.cpp, not
    // something to patch vendored source over). Worked around with
    // fieldSlot_: a plain Ui::RpWidget added to card_ that reserves the
    // right amount of layout space, real Telegram Desktop's own real field
    // height (defaultInputField.heightMin: 55px) - the three actual field
    // widgets are ordinary children of card_ (not VerticalLayout rows),
    // repositioned to match fieldSlot_'s geometry whenever it moves (see
    // the geometryValue() subscription right after).
    auto* fieldSlot_ = card_->add(object_ptr<Ui::RpWidget>(card_), style::margins(0, kFieldMargin, 0, 0));
    fieldSlot_->resize(1, style::ConvertScale(55));
    plainInput_ = new Ui::MaskedInputField(card_, st::zkgramAuthInputField);
    plainInput_->setObjectName("authInput");
    passwordInput_ = new Ui::PasswordInput(card_, st::zkgramAuthInputField);
    passwordInput_->setObjectName("authInputPassword");
    codeInput_ = new Ui::SentCodeField(card_, st::zkgramAuthInputField);
    codeInput_->setObjectName("authInputCode");
    fieldSlot_->geometryValue() | rpl::on_next(
        [this, fieldSlot_](const QRect& geometry) {
            plainInput_->setGeometry(geometry);
            passwordInput_->setGeometry(geometry);
            codeInput_->setGeometry(geometry);
        },
        fieldSlot_->lifetime());
    // No setAutoSubmit() length here - TDLib does not tell this codebase
    // how many digits the code actually is (it varies by region/carrier),
    // so auto-submitting at a guessed length would submit early for a
    // longer code. Enter still submits, same as the other two fields.
    codeInput_->submits() | rpl::on_next([this](Qt::KeyboardModifiers) { onNextClicked(); }, codeInput_->lifetime());

    error_ = card_->add(object_ptr<Ui::FlatLabel>(card_, st::zkgramAuthDescription),
                         style::margins(0, kFieldMargin, 0, 0));
    error_->setObjectName("authError");
    error_->setTextColorOverride(st::attentionButtonFg->c);
    error_->hide();

    // zkgramAuthNextButton: real intro.style introNextButton values
    // (300x42, 6px radius) on top of defaultActiveButton, not a new visual
    // design - see zkgram_icons.style's own comment. Its own declared
    // width (300px) is now what sizes it - no lockButtonSize() workaround
    // needed, that was only ever necessary for a QBoxLayout's stretch
    // squeezing a width:-34px (auto) button to 0.
    next_ = card_->add(object_ptr<Ui::RoundButton>(card_, rpl::single(QString("Next")), st::zkgramAuthNextButton),
                        style::margins(0, kFieldMargin, 0, kFieldMargin));
    next_->setObjectName("authNext");

    // card_'s own width drives every child's width in turn (VerticalLayout
    // rows are resized to the container's width minus their margins, see
    // resizeGetHeight()) - fixed at the real introStepWidth so title/
    // subtitle/fields/button all end up exactly 380px wide, matching real
    // Telegram's own intro step, not whatever QVBoxLayout's stretch used
    // to produce.
    card_->resizeToWidth(380);
    // Recenters whenever card_'s own height changes (a label re-wrapping
    // after runSlide() sets new text), not only on an actual AuthSlide
    // resize - the reactive rpl subscription §4/§7 describe, not a manual
    // poll.
    card_->heightValue() | rpl::on_next([this](int) { recenterCard(); }, card_->lifetime());

    next_->setClickedCallback([this] { onNextClicked(); });
    connect(plainInput_, &Ui::MaskedInputField::submitted, this, &AuthSlide::onNextClicked);
    connect(passwordInput_, &Ui::MaskedInputField::submitted, this, &AuthSlide::onNextClicked);
}

QWidget* AuthSlide::activeInput() const {
    if (passwordInput_->isVisible()) {
        return passwordInput_;
    }
    if (codeInput_->isVisible()) {
        return codeInput_;
    }
    return plainInput_;
}

QString AuthSlide::runSlide(const QString& title, const QString& subtitle, const QString& placeholder, bool masked,
                             Validator validator, const QString& initialError, bool isCode) {
    logDebug("runSlide: entered, title=" + title);
    title_->setText(title);
    subtitle_->setText(subtitle);
    plainInput_->setPlaceholder(rpl::single(placeholder));
    passwordInput_->setPlaceholder(rpl::single(placeholder));
    codeInput_->setPlaceholder(rpl::single(placeholder));
    plainInput_->setText(QString());
    passwordInput_->setText(QString());
    codeInput_->setText(QString());
    plainInput_->setVisible(!masked && !isCode);
    passwordInput_->setVisible(masked);
    codeInput_->setVisible(!masked && isCode);
    next_->show();
    activeInput()->setFocus();
    if (initialError.isEmpty()) {
        error_->hide();
    } else {
        error_->setText(initialError);
        error_->show();
    }
    validator_ = std::move(validator);

    logDebug("runSlide: entering loop.exec()");
    QEventLoop loop;
    loop_ = &loop;
    loop.exec();
    loop_ = nullptr;
    logDebug("runSlide: loop.exec() returned, result.length()=" + QString::number(result_.length()));
    return result_;
}

void AuthSlide::showStatus(const QString& title, const QString& subtitle) {
    title_->setText(title);
    subtitle_->setText(subtitle);
    error_->hide();
    plainInput_->hide();
    passwordInput_->hide();
    codeInput_->hide();
    next_->hide();
}

void AuthSlide::cancelPending() {
    result_.clear();
    if (loop_ != nullptr) {
        loop_->quit();
    }
}

void AuthSlide::onNextClicked() {
    logDebug("onNextClicked: entered");
    // getDigitsOnly(), not getLastText(), for the code field - SentCodeField
    // already strips non-digit/dash characters live (see its own fix()),
    // but this also drops any stray '-' separators it kept for display,
    // matching what the login-code API call actually expects.
    QString value = passwordInput_->isVisible() ? passwordInput_->getLastText()
                     : codeInput_->isVisible()   ? codeInput_->getDigitsOnly()
                                                  : plainInput_->getLastText();
    // Empty input just refocuses instead of submitting - the fields here
    // (password/phone/code) are always required, there is no valid empty
    // submission to accept.
    if (value.isEmpty()) {
        activeInput()->setFocus();
        return;
    }
    if (validator_) {
        QString message = validator_(value);
        if (!message.isEmpty()) {
            error_->setText(message);
            error_->show();
            activeInput()->setFocus();
            return;
        }
    }
    error_->hide();
    result_ = value;
    if (loop_ != nullptr) {
        loop_->quit();
    }
}

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle("zkgram");

    // st::windowBgOver, not the old literal #e6ebee: the only part of this
    // widget the user can ever see is the 1px divider gap between the two
    // panes covering it, so it takes the palette's own "surface behind the
    // panels" role rather than keeping a hand-picked hex alive for it.
    auto* central = createPanel(this, st::windowBgOver);
    central->setObjectName("central");
    chatPage_ = central;
    auto* rootLayout = new QHBoxLayout(central);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    // Sidebar/conversation split is user-resizable by dragging the divider,
    // same as tdesktop's own dialogs list - a fixed width did not match
    // real Telegram Desktop's behavior.
    auto* splitter = new MainSplitter(Qt::Horizontal, central);
    splitter->setObjectName("mainSplitter");
    splitter->setChildrenCollapsible(false);
    rootLayout->addWidget(splitter);

    // ---- sidebar: chat list + "start a new conversation" ----
    auto* sidebar = createPanel(splitter, st::dialogsBg, PanelEdge::Right);
    sidebar->setObjectName("sidebar");
    // Real window/window.style: columnMinimalWidthLeft: 260px.
    sidebar->setMinimumWidth(style::ConvertScale(260));
    auto* sidebarLayout = new QVBoxLayout(sidebar);
    sidebarLayout->setContentsMargins(0, 0, 0, 0);
    sidebarLayout->setSpacing(0);

    auto* searchRow = new QWidget(sidebar);
    auto* searchRowLayout = new QHBoxLayout(searchRow);
    searchRowLayout->setContentsMargins(10, 8, 10, 8);
    searchRowLayout->setSpacing(style::ConvertScale(8));
    // Top-left corner of the whole window, next to the chat-list search
    // field - matches real Telegram Desktop's own layout (the hamburger
    // lives in the sidebar's own top bar, not the conversation header on
    // the right, which is where this was placed before).
    auto* hamburgerButton = new HamburgerButton(searchRow);
    hamburgerButton->onClicked = [this] { toggleMainMenu(); };
    searchRowLayout->addWidget(hamburgerButton);
    // st::dialogsFilter: real values from telegram_fork/tdesktop's own
    // dialogs.style (rounded pill, filterInputInactiveBg/ActiveBg -
    // already vendored in src/dialogs/dialogs.style, already generated
    // for SidebarCanvas's own row metrics). Ui::InputField has no
    // QLineEdit::addAction() leading-icon slot, so the magnifier is a
    // plain sibling icon in the same row instead of embedded in the
    // field - a layout-level approximation of real Telegram's own
    // overlaid icon, not pixel-identical, but real lib_ui/real style
    // values rather than a hand-painted QLineEdit.
    auto* chatSearchIcon = new Ui::RpWidget(searchRow);
    chatSearchIcon->resize(style::ConvertScale(20), style::ConvertScale(20));
    chatSearchIcon->paintRequest(
    ) | rpl::on_next(
        [chatSearchIcon] {
            QPainter painter(chatSearchIcon);
            st::zkgramIconSearch.paint(painter, 0, 0, chatSearchIcon->width());
        },
        chatSearchIcon->lifetime());
    searchRowLayout->addWidget(chatSearchIcon);
    chatSearchInput_ =
        new Ui::InputField(searchRow, st::dialogsFilter, Ui::InputField::Mode::SingleLine, rpl::single(QString("Search")));
    chatSearchInput_->show();
    searchRowLayout->addWidget(chatSearchInput_);
    sidebarLayout->addWidget(searchRow);

    searchDebounceTimer_ = new QTimer(this);
    searchDebounceTimer_->setSingleShot(true);

    sidebarRenderThrottleTimer_ = new QTimer(this);
    sidebarRenderThrottleTimer_->setSingleShot(true);
    connect(sidebarRenderThrottleTimer_, &QTimer::timeout, this, &MainWindow::flushSidebarRender);

    conversationRenderThrottleTimer_ = new QTimer(this);
    conversationRenderThrottleTimer_->setSingleShot(true);
    connect(conversationRenderThrottleTimer_, &QTimer::timeout, this, [this] {
        // The chat may have been switched while this was pending; the new
        // one was rendered in full by the switch itself, so there is
        // nothing to redraw for the one this was queued for.
        if (pendingConversationRenderChatId_ != currentChatId_) {
            return;
        }
        renderCurrentConversation();
    });

    // Real Ui::ScrollArea (lib_ui/ui/widgets/scroll_area.*, already
    // vendored) wrapping SidebarCanvas as its owned content widget - real
    // momentum/touch scrolling and real scrollbar painting, replacing the
    // QAbstractScrollArea + hand-rolled KineticScroller this used to be.
    // See SidebarCanvas's own class comment for the rest of the migration.
    auto* sidebarScroll = new Ui::ScrollArea(sidebar);
    sidebarScroll->setObjectName("chatListScroll");
    auto sidebarCanvasOwned = object_ptr<SidebarCanvas>(sidebarScroll);
    auto* sidebarCanvas = sidebarScroll->setOwnedWidget(std::move(sidebarCanvasOwned)).data();
    sidebarCanvas->setObjectName("chatListWidget");
    sidebarCanvas->onRowClicked = [this](qlonglong chatId) { onChatListItemClicked(chatId); };
    chatListWidget_ = sidebarCanvas;
    // Ui::ScrollArea does not auto-resize its owned widget's width to the
    // viewport the way a plain QScrollArea with widgetResizable(true) would
    // (its own resizeEvent only recounts its custom scrollbars/shadows,
    // see scroll_area.cpp) - real tdesktop content widgets are told their
    // width explicitly by whatever owns the relationship, so this does
    // that: resizeToWidth() re-runs SidebarCanvas::resizeGetHeight() for
    // the scroll area's current width every time it changes.
    sidebarScroll->geometryChanged() | rpl::on_next(
        [sidebarScroll, sidebarCanvas] { sidebarCanvas->resizeToWidth(sidebarScroll->width()); },
        sidebarScroll->lifetime());
    sidebarLayout->addWidget(sidebarScroll);
    splitter->addWidget(sidebar);

    // ---- right side: header + selected conversation ----
    auto* conversationColumn = new QWidget(splitter);
    auto* layout = new QVBoxLayout(conversationColumn);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    headerRow_ = createPanel(conversationColumn, st::windowBg, PanelEdge::Bottom);
    headerRow_->setObjectName("header");
    auto* headerLayout = new QHBoxLayout(headerRow_);
    headerLayout->setContentsMargins(style::ConvertScale(12), style::ConvertScale(6), style::ConvertScale(12),
                                      style::ConvertScale(6));
    headerLayout->setSpacing(style::ConvertScale(10));
    companionAvatar_ = new QLabel(headerRow_);
    // Real info/info.style: topBarInfoButtonInnerSize: 42px.
    companionAvatar_->setFixedSize(style::ConvertScale(42), style::ConvertScale(42));
    companionAvatar_->hide();  // no chat selected yet, see onChatListItemClicked
    // REVERTED to QLabel (2026-08-17): Ui::FlatLabel here crashed on the
    // very first real chat click - Ui::Text::BlockParser::parse() (inside
    // FlatLabel::setText -> Text::String::setText) segfaulted on a null/
    // wild pointer given the chat's real TDLib title (dynamic content),
    // while the exact same st::zkgramCompanionLabel style with the fixed
    // string "zkgram" at construction did not crash, and AuthSlide's own
    // FlatLabels (title_/subtitle_, same defaultFlatLabel/defaultTextStyle
    // ancestry) call setText() repeatedly without issue. Root cause not
    // found yet (crash dump analysis via cdb pointed at BlockParser
    // dereferencing a small/invalid address, not obviously tied to any one
    // style field) - reverted rather than guessed at further, since a
    // crash on the single most common interaction (opening a chat) is not
    // acceptable to leave in place while still investigating. See TODO.md.
    companionLabel_ = new QLabel("zkgram", headerRow_);
    companionLabel_->setObjectName("companionLabel");
    // Still a QLabel (the Ui::FlatLabel attempt crashed on real chat titles,
    // see TODO.md), but no longer styled by a stylesheet rule: font and
    // color come from the same st::zkgramCompanionLabel declaration the
    // FlatLabel used, so both roads lead to the same palette values.
    applyLabelStyle(companionLabel_, st::zkgramCompanionLabel);
    // Per-chat secret-mode toggle - see secretModeOn_'s own comment in
    // the header for what this actually gates. Hidden by default
    // (updateSecretModeIndicator() shows it only once the current chat
    // actually has some secret content to reveal, see
    // hasSecretContent_), so a chat that never had any encrypted session
    // shows no toggle at all rather than a permanently useless one.
    secretModeToggle_ = new Ui::RoundButton(headerRow_, rpl::single(QString()), st::zkgramSecretModeToggle);
    secretModeToggle_->setObjectName("secretModeToggle");
    secretModeToggle_->hide();
    // Filters the message list below by substring - the currently open
    // conversation only, not a search across every chat. st::dialogsFilter
    // (real tdesktop values, see chatSearchInput_'s own comment above for
    // why the magnifier is a plain sibling icon, not embedded).
    searchInput_ =
        new Ui::InputField(headerRow_, st::dialogsFilter, Ui::InputField::Mode::SingleLine,
                            rpl::single(QString("Search in conversation")));
    searchInput_->show();
    searchInput_->setMaximumWidth(style::ConvertScale(180));
    // statusDot_/statusLabel_ are not added to the header - a persistent
    // "Connected" pill is not something real Telegram Desktop shows once
    // logged in (it only ever surfaces a transient "Connecting..."/
    // "Updating..." state, handled by showConnectingProgress() on the auth
    // screen instead). The widgets/logic still exist because
    // setConnectionStatus() also drives switching from the auth screen to
    // the chat page - they are just never shown.
    statusDot_ = new QLabel(headerRow_);
    statusDot_->setFixedSize(style::ConvertScale(9), style::ConvertScale(9));
    statusDot_->hide();
    statusLabel_ = new QLabel("Not connected", headerRow_);
    statusLabel_->setObjectName("statusLabel");
    statusLabel_->hide();
    headerLayout->addWidget(companionAvatar_);
    headerLayout->addWidget(companionLabel_);
    headerLayout->addWidget(secretModeToggle_);
    headerLayout->addStretch();
    auto* headerSearchIcon = new Ui::RpWidget(headerRow_);
    headerSearchIcon->resize(style::ConvertScale(20), style::ConvertScale(20));
    headerSearchIcon->paintRequest(
    ) | rpl::on_next(
        [headerSearchIcon] {
            QPainter painter(headerSearchIcon);
            st::zkgramIconSearch.paint(painter, 0, 0, headerSearchIcon->width());
        },
        headerSearchIcon->lifetime());
    headerLayout->addWidget(headerSearchIcon);
    headerLayout->addWidget(searchInput_);
    layout->addWidget(headerRow_);

    // Real Telegram's own selection toolbar replaces the chat header in
    // the same slot while HistoryCanvas is in multi-message selection mode
    // (see HistoryCanvas::onSelectionModeChanged/updateSelectionToolbar()) -
    // built once here, hidden until then, rather than swapped in/out of
    // the layout dynamically.
    selectionToolbar_ = new QWidget(conversationColumn);
    selectionToolbar_->setObjectName("selectionToolbar");
    selectionToolbar_->hide();
    auto* selectionLayout = new QHBoxLayout(selectionToolbar_);
    selectionLayout->setContentsMargins(style::ConvertScale(12), style::ConvertScale(6), style::ConvertScale(12),
                                         style::ConvertScale(6));
    selectionLayout->setSpacing(style::ConvertScale(10));
    auto* cancelSelectionButton = new Ui::IconButton(selectionToolbar_, st::zkgramCancelRecordButton);
    cancelSelectionButton->setToolTip("Cancel");
    cancelSelectionButton->setClickedCallback([this] { asCanvas(messages_)->exitSelectionMode(); });
    cancelSelectionButton->show();
    lockButtonSize(cancelSelectionButton);
    selectionCountLabel_ = new QLabel(selectionToolbar_);
    selectionCountLabel_->setObjectName("companionLabel");  // reuse header's label styling
    selectionLayout->addWidget(cancelSelectionButton);
    selectionLayout->addWidget(selectionCountLabel_);
    selectionLayout->addStretch();
    auto* copyButton = new Ui::IconButton(selectionToolbar_, st::zkgramAttachButton);
    copyButton->setToolTip("Copy");
    copyButton->setClickedCallback([this] { onSelectionCopyClicked(); });
    copyButton->show();
    lockButtonSize(copyButton);
    auto* forwardButton = new Ui::IconButton(selectionToolbar_, st::zkgramAttachButton);
    forwardButton->setToolTip("Forward");
    forwardButton->setClickedCallback([this] { onSelectionForwardClicked(); });
    forwardButton->show();
    lockButtonSize(forwardButton);
    auto* deleteButton = new Ui::IconButton(selectionToolbar_, st::zkgramCancelRecordButton);
    deleteButton->setToolTip("Delete");
    deleteButton->setClickedCallback([this] { onSelectionDeleteClicked(); });
    deleteButton->show();
    lockButtonSize(deleteButton);
    selectionLayout->addWidget(copyButton);
    selectionLayout->addWidget(forwardButton);
    selectionLayout->addWidget(deleteButton);
    layout->addWidget(selectionToolbar_);

    // Ui::RpWidget owned by a real Ui::ScrollArea, same migration
    // SidebarCanvas/sidebarScroll already went through (see that
    // construction site above) - not a QAbstractScrollArea anymore, see
    // HistoryCanvas's own class comment.
    auto* historyScroll = new Ui::ScrollArea(conversationColumn);
    historyScroll->setObjectName("messagesScroll");
    auto historyCanvasOwned = object_ptr<HistoryCanvas>(historyScroll);
    auto* historyCanvas = historyScroll->setOwnedWidget(std::move(historyCanvasOwned)).data();
    historyCanvas->setObjectName("messages");
    historyCanvas->setScrollArea(historyScroll);
    // Context menu policy/signal attached directly to historyCanvas, not
    // historyScroll - post-migration, mouse events land on the canvas
    // itself in its own absolute-content-coordinate local space, exactly
    // what showMessageContextMenu()'s pos -> HistoryCanvas::itemAt() call
    // expects (see itemAt()'s own comment).
    historyCanvas->setContextMenuPolicy(Qt::CustomContextMenu);
    // Wallpaper is painted directly in HistoryCanvas::paintEvent() (via
    // its own background_ member, see ChatBackground), not via
    // QPalette::Base/autoFillBackground here - that path silently stopped
    // painting anything at all once MainWindow got a stylesheet.
    messages_ = historyCanvas;
    historyCanvas->onSelectionModeChanged = [this](bool active, int count) {
        updateSelectionToolbar(active, count);
    };
    // Parented to chatPage_ (the whole split-view page, sidebar included),
    // not conversationColumn - a fullscreen viewer covering only the
    // message pane and leaving the sidebar visible beside it would not
    // match real Telegram's own fullscreen overlay.
    mediaViewer_ = new MediaViewer(chatPage_);
    // Swipe a message sideways to reply to it - the gesture half of the
    // same action the context menu's "Reply" performs, so both go through
    // startReply() and neither knows anything the other does not.
    historyCanvas->onReplyRequested = [this](HistoryCanvas::HitResult hit) {
        startReply(replyAuthorLabel(hit.kind), hit.text, hit.messageId);
    };
    historyCanvas->onImageActivated = [this](const QVector<QPixmap>& images, int index) {
        asMediaViewer(mediaViewer_)->open(images, index);
    };
    historyCanvas->onVoicePlayToggled = [this](qlonglong messageId, const QString& path) {
        onVoicePlayToggleRequested(messageId, path);
    };
    layout->addWidget(historyScroll);

    // ---- hamburger slide-out menu (profile + Monero wallet) ----
    // Parented to chatPage_, same reasoning as mediaViewer_ above - a
    // fullscreen dimmer and a panel that slides in from the true left edge
    // of the window, not just the message pane.
    mainMenuDimmer_ = new MainMenuDimmer(chatPage_);
    mainMenuDimmer_->setGeometry(chatPage_->rect());
    static_cast<MainMenuDimmer*>(mainMenuDimmer_)->onClicked = [this] { closeMainMenu(); };

    // Real window/window_main_menu.style: mainMenuWidth: 274px.
    const int kMainMenuWidth = style::ConvertScale(274);
    mainMenuPanel_ = new MainMenuPanel(chatPage_);
    mainMenuPanel_->setFixedWidth(kMainMenuWidth);
    mainMenuPanel_->hide();

    auto* menuLayout = new QVBoxLayout(mainMenuPanel_);
    menuLayout->setContentsMargins(0, style::ConvertScale(20), 0, 0);
    menuLayout->setSpacing(0);

    auto* profileSection = new QWidget(mainMenuPanel_);
    auto* profileLayout = new QVBoxLayout(profileSection);
    profileLayout->setContentsMargins(style::ConvertScale(20), 0, style::ConvertScale(20), style::ConvertScale(16));
    profileLayout->setSpacing(style::ConvertScale(6));

    mainMenuAvatar_ = new QLabel(profileSection);
    // Real window.style: mainMenuUserpic is UserpicButton(defaultUserpicButton)
    // with photoSize: 48px.
    int avatarSize = style::ConvertScale(48);
    mainMenuAvatar_->setFixedSize(avatarSize, avatarSize);
    mainMenuAvatar_->setPixmap(avatarPixmap("?", 0, avatarSize));

    mainMenuNameLabel_ = new QLabel("...", profileSection);
    {
        QFont nameFont = mainMenuNameLabel_->font();
        nameFont.setPixelSize(style::ConvertScale(17));
        nameFont.setBold(true);
        mainMenuNameLabel_->setFont(nameFont);
    }
    mainMenuUsernameLabel_ = new QLabel(QString(), profileSection);
    {
        QFont subFont = mainMenuUsernameLabel_->font();
        subFont.setPixelSize(style::ConvertScale(13));
        mainMenuUsernameLabel_->setFont(subFont);
        QPalette pal = mainMenuUsernameLabel_->palette();
        pal.setColor(QPalette::WindowText, QColor(140, 140, 140));
        mainMenuUsernameLabel_->setPalette(pal);
    }
    profileLayout->addWidget(mainMenuAvatar_);
    profileLayout->addSpacing(style::ConvertScale(8));
    profileLayout->addWidget(mainMenuNameLabel_);
    profileLayout->addWidget(mainMenuUsernameLabel_);
    menuLayout->addWidget(profileSection);

    auto addDivider = [this, menuLayout] {
        auto* divider = new QWidget(mainMenuPanel_);
        divider->setFixedHeight(style::ConvertScale(1));
        QPalette pal = divider->palette();
        pal.setColor(QPalette::Window, QColor(0, 0, 0, 25));
        divider->setAutoFillBackground(true);
        divider->setPalette(pal);
        menuLayout->addWidget(divider);
    };
    addDivider();

    auto* myProfileRow = new MenuRow(mainMenuPanel_, "My Profile");
    myProfileRow->onClicked = [this] { closeMainMenu(); };
    menuLayout->addWidget(myProfileRow);

    walletRow_ = new MenuRow(mainMenuPanel_, "Wallet", "Monero");
    static_cast<MenuRow*>(walletRow_)->onClicked = [this] { openWalletSection(); };
    menuLayout->addWidget(walletRow_);
    addDivider();

    // Wallet detail: address/balance, shown/hidden in place instead of a
    // separate page - toggled open by walletRow_ above, closed the same
    // way (tap it again). Real Telegram drills into a whole new page for
    // this; a simpler in-place reveal covers what was actually asked for
    // (profile info + wallet info) without a second navigation layer.
    walletDetail_ = new QWidget(mainMenuPanel_);
    walletDetail_->hide();
    auto* walletLayout = new QVBoxLayout(walletDetail_);
    walletLayout->setContentsMargins(style::ConvertScale(20), style::ConvertScale(4), style::ConvertScale(20),
                                      style::ConvertScale(12));
    walletLayout->setSpacing(style::ConvertScale(4));
    walletBalanceLabel_ = new QLabel("", walletDetail_);
    {
        QFont balanceFont = walletBalanceLabel_->font();
        balanceFont.setPixelSize(style::ConvertScale(15));
        balanceFont.setBold(true);
        walletBalanceLabel_->setFont(balanceFont);
    }
    walletAddressLabel_ = new QLabel("", walletDetail_);
    walletAddressLabel_->setWordWrap(true);
    {
        QFont addressFont = walletAddressLabel_->font();
        addressFont.setPixelSize(style::ConvertScale(12));
        walletAddressLabel_->setFont(addressFont);
        QPalette pal = walletAddressLabel_->palette();
        pal.setColor(QPalette::WindowText, QColor(120, 120, 120));
        walletAddressLabel_->setPalette(pal);
    }
    walletStatusLabel_ = new QLabel("", walletDetail_);
    {
        QFont statusFont = walletStatusLabel_->font();
        statusFont.setPixelSize(style::ConvertScale(12));
        walletStatusLabel_->setFont(statusFont);
        QPalette pal = walletStatusLabel_->palette();
        pal.setColor(QPalette::WindowText, QColor(200, 60, 60));
        walletStatusLabel_->setPalette(pal);
    }
    walletLayout->addWidget(walletBalanceLabel_);
    walletLayout->addWidget(walletAddressLabel_);
    walletLayout->addWidget(walletStatusLabel_);
    menuLayout->addWidget(walletDetail_);

    // Visual parity with real Telegram's own menu (see the screenshot this
    // was built from) - not wired to any feature yet, clicking any of
    // these just closes the menu.
    for (const char* label : {"New Group", "New Channel", "Contacts", "Calls", "Saved Messages"}) {
        auto* row = new MenuRow(mainMenuPanel_, QString(label));
        row->onClicked = [this] { closeMainMenu(); };
        menuLayout->addWidget(row);
    }
    addDivider();
    auto* settingsRow = new MenuRow(mainMenuPanel_, "Settings");
    settingsRow->onClicked = [this] { closeMainMenu(); };
    menuLayout->addWidget(settingsRow);

    // Night Mode: shown for visual parity with the real menu but not
    // wired to an actual dark theme - that would mean re-deriving every
    // color this app paints by hand at runtime (bubbles, sidebar, wallpaper,
    // the whole palette), a separate feature on its own, not part of "left
    // menu + profile + wallet". Disabled rather than a fake toggle that
    // silently does nothing when checked - a control that visibly does
    // nothing is more honest than one that looks like it works.
    auto* nightModeRow = new QWidget(mainMenuPanel_);
    nightModeRow->setFixedHeight(style::ConvertScale(42));
    auto* nightModeLayout = new QHBoxLayout(nightModeRow);
    nightModeLayout->setContentsMargins(style::ConvertScale(20), 0, style::ConvertScale(20), 0);
    auto* nightModeLabel = new QLabel("Night Mode", nightModeRow);
    auto* nightModeCheck = new QCheckBox(nightModeRow);
    nightModeCheck->setEnabled(false);
    nightModeCheck->setToolTip("Not implemented yet");
    nightModeLayout->addWidget(nightModeLabel);
    nightModeLayout->addStretch();
    nightModeLayout->addWidget(nightModeCheck);
    menuLayout->addWidget(nightModeRow);

    menuLayout->addStretch();
    mainMenuPanel_->setGeometry(-kMainMenuWidth, 0, kMainMenuWidth, chatPage_->height());

    mainMenuAnimation_ = new QPropertyAnimation(mainMenuPanel_, "geometry", this);
    mainMenuAnimation_->setDuration(220);
    mainMenuAnimation_->setEasingCurve(QEasingCurve::OutCubic);
    // Only actually hides the panel once its geometry animation finishes
    // CLOSED (mainMenuOpen_ already false by then, see closeMainMenu()) -
    // this same finished signal also fires at the end of the OPEN
    // animation, where hiding would immediately undo the reveal that just
    // happened. Connected once here rather than per-call in closeMainMenu(),
    // so there is exactly one persistent connection to reason about instead
    // of relying on Qt::UniqueConnection to dedupe repeated ones.
    connect(mainMenuAnimation_, &QPropertyAnimation::finished, this, [this] {
        if (!mainMenuOpen_) {
            mainMenuPanel_->hide();
        }
    });

    // ZKGRAM_MONERO_RPC_URL/_USER/_PASSWORD override monero-wallet-rpc's
    // endpoint/login - same env-var configuration idiom telegram_client.cpp
    // already uses for its own optional MTProto proxy
    // (configureProxyIfSet()), not a new pattern for this codebase. Falls
    // back to MoneroWalletRpc's own stagenet-default URL with no auth if
    // unset.
    QString moneroRpcUrl = qEnvironmentVariable("ZKGRAM_MONERO_RPC_URL");
    QString moneroRpcUser = qEnvironmentVariable("ZKGRAM_MONERO_RPC_USER");
    QString moneroRpcPassword = qEnvironmentVariable("ZKGRAM_MONERO_RPC_PASSWORD");
    moneroClient_ = moneroRpcUrl.isEmpty()
                         ? new zkgram::monero::MoneroWalletRpc(this)
                         : new zkgram::monero::MoneroWalletRpc(this, moneroRpcUrl, moneroRpcUser, moneroRpcPassword);

    // Same trigger point as tdesktop's HistoryView::ListWidget detecting
    // the top of the scroll area reached (see maybeLoadMoreHistory()) - a
    // threshold measured from the scrollbar's own current minimum, not a
    // fixed absolute value: value <= 40 assumed the range always started
    // at 0, which stopped being true once prependItems() started giving
    // older history negative top coordinates (see its own comment) - the
    // scrollbar's minimum goes negative after the first loaded page, so a
    // fixed "<= 40" either fires on every single scroll position
    // afterwards or, depending on how far negative things had gone,
    // stops firing at the right time at all. A generous threshold
    // (roughly a screen's worth of rows) rather than the old 40px also
    // gives the network request more lead time to complete BEFORE the
    // user's eye actually reaches the loaded edge - a request that only
    // starts a few pixels before the edge is a real fetch and, unlike a
    // synchronous local operation, has real network latency;
    // scrolling straight into that not-yet-arrived edge is what read as
    // "the app just freezes and messages don't load in time" - see also
    // setLoadingMore()'s own comment for the second half of that fix (a
    // visible indicator instead of blank space while it is in flight).
    historyScroll->scrollTopValue() | rpl::on_next(
        [this](int value) {
            if (value <= style::ConvertScale(600)) {
                maybeLoadMoreHistory();
            }
        },
        historyScroll->lifetime());

    // A write-restriction bar, shown instead of/alongside the composer
    // when there is no crypto::CryptoLayer session - the same structural
    // role as tdesktop's own write-restriction bar (join/mute/frozen/
    // premium-required states), see
    // Controls::WriteRestriction/ComposeControls::initWriteRestriction()
    // in telegram_fork/tdesktop's
    // history/view/controls/history_view_compose_controls.cpp: one flat
    // bar, historyComposeAreaBg background (white, #ffffff - resolved
    // from telegram_fork/tdesktop's own Resources/day-blue.tdesktop-theme,
    // historyComposeAreaBg -> msgInBg -> windowBg), height equal to the
    // composer's own button row (46px), a placeholderFg-colored label
    // (#999999) on the left, one action button on the right - not the
    // old two separate stacked bars (a big button row, then a whole
    // separate full-width colored warning strip) with large empty gaps
    // between them and the composer below.
    writeRestrictionBar_ = createPanel(conversationColumn, st::historyComposeAreaBg);
    QWidget* startEncryptionRow = writeRestrictionBar_;
    startEncryptionRow->setObjectName("writeRestrictionBar");
    startEncryptionRow->setFixedHeight(style::ConvertScale(46));
    auto* startEncryptionRowLayout = new QHBoxLayout(startEncryptionRow);
    startEncryptionRowLayout->setContentsMargins(style::ConvertScale(12), 0, style::ConvertScale(12), 0);
    // REVERTED to QLabel (2026-08-17): same batch as companionLabel_'s
    // real crash (see its own comment) - this one didn't crash but went
    // invisible instead (user report: the private/plain-mode text
    // disappeared after the FlatLabel conversion). Same root cause not
    // found yet, same precaution - reverted rather than guessed at again.
    plainSendWarning_ = new QLabel("Not encrypted - sent as plain Telegram text", startEncryptionRow);
    applyLabelStyle(plainSendWarning_, st::zkgramPlainSendWarning);
    plainSendWarning_->setObjectName("plainSendWarning");
    startEncryptionRowLayout->addWidget(plainSendWarning_);
    startEncryptionRowLayout->addStretch();
    // Phase 5 (UI.md 13c): same defaultActiveButton as next_ above - the old
    // setFixedHeight(30) is gone, defaultActiveButton's own height (34px) is
    // used instead of a hand-picked override.
    startEncryptionButton_ =
        new Ui::RoundButton(startEncryptionRow, rpl::single(QString("Start encrypted session")), st::defaultActiveButton);
    startEncryptionButton_->setObjectName("startEncryptionButton");
    startEncryptionButton_->show();
    // See lockButtonSize()'s own comment: without this, the real bug found
    // via runtime diagnostics - this HBox row's addStretch() squeezing the
    // button to 0 width the first time writeRestrictionBar_ actually goes
    // visible->layout-active, not a hidden-by-default issue.
    lockButtonSize(startEncryptionButton_);
    startEncryptionRowLayout->addWidget(startEncryptionButton_);
    layout->addWidget(startEncryptionRow);

    // Reply-to-message preview (Ui::MessageBar, real widget - see UI.md
    // 13c Phase 6b, zkgram_chat_style_stub.style's defaultMessageBar
    // comment) - hidden until startReply() is called from the message
    // context menu.
    replyBarRow_ = new QWidget(conversationColumn);
    replyBarRow_->setObjectName("replyBarRow");
    replyBarRow_->hide();
    auto* replyBarLayout = new QHBoxLayout(replyBarRow_);
    replyBarLayout->setContentsMargins(0, 0, 0, 0);
    // Ui::MessageBar is a plain class, not itself a QWidget/QObject - its
    // widget() is what actually lives in the layout/widget tree and gets
    // destroyed with replyBarRow_; replyBar_ itself is process-lifetime,
    // same as UiIntegration/the animations Manager in ui_integration.cpp.
    replyBar_ = new Ui::MessageBar(replyBarRow_, st::defaultMessageBar, [] { return false; });
    replyBar_->widget()->show();
    // Stretch factor 1: the message-bar content should take the row's
    // remaining width, not compete evenly with the fixed-size cancel
    // button next to it (see lockButtonSize()'s comment on that button).
    replyBarLayout->addWidget(replyBar_->widget().get(), 1);
    auto* cancelReplyButton = new Ui::IconButton(replyBarRow_, st::zkgramCancelRecordButton);
    cancelReplyButton->setToolTip("Cancel reply");
    cancelReplyButton->setClickedCallback([this] { cancelReply(); });
    cancelReplyButton->show();
    lockButtonSize(cancelReplyButton);
    replyBarLayout->addWidget(cancelReplyButton);
    layout->addWidget(replyBarRow_);

    // The composer bar itself - historyComposeAreaBg background (the
    // same white as the restriction bar above and the field inside it,
    // not a different shade - see historyComposeField.textBg in
    // chat_helpers.style, it is the very same token), no rounding
    // (radius: 0px there), 54px tall for a single line
    // (historySendPadding 9px top/bottom + historyComposeField.heightMin
    // 36px = 9+36+9), attach/send buttons 44x46 anchored to the bottom
    // edge, field vertically centered (giving the same symmetric 9px/9px
    // margin the real padding values produce) - see
    // ComposeControls::updateControlsGeometry()/updateHeight() in
    // telegram_fork/tdesktop's history_view_compose_controls.cpp for
    // where every one of these numbers comes from.
    auto* inputRow = createPanel(conversationColumn, st::historyComposeAreaBg);
    composeRow_ = inputRow;
    inputRow->setObjectName("composeBar");
    inputRow->setFixedHeight(style::ConvertScale(54));
    auto* inputLayout = new QHBoxLayout(inputRow);
    inputLayout->setContentsMargins(style::ConvertScale(2), 0, style::ConvertScale(2), 0);
    inputLayout->setSpacing(0);
    // Phase 5 (UI.md 13c): first RippleButton converted to the real
    // Ui::IconButton, style declared in zkgram_icons.style
    // (zkgramAttachButton) instead of setFixedSize()/setIconSize() calls
    // here - size, icon and hover state all come from the .style now.
    sendFileButton_ = new Ui::IconButton(inputRow, st::zkgramAttachButton);
    sendFileButton_->setObjectName("composeIconButton");
    sendFileButton_->setToolTip("Send a file");
    lockButtonSize(sendFileButton_);
    input_ = new ChatInput(inputRow);
    input_->setObjectName("composeField");
    // One button, not a separate always-visible mic button next to a
    // separate always-visible send button - see sendButton_'s own header
    // comment for the citation. Starts in its mic state (field starts
    // empty); updateSendButtonIcon() swaps it as the field's text
    // changes.
    sendButton_ = new Ui::IconButton(inputRow, st::zkgramSendButton);
    sendButton_->setObjectName("composeIconButton");
    sendButton_->setToolTip("Record a voice message");
    lockButtonSize(sendButton_);
    inputLayout->addWidget(sendFileButton_, 0, Qt::AlignBottom);
    inputLayout->addWidget(input_, 1, Qt::AlignVCenter);
    inputLayout->addWidget(sendButton_, 0, Qt::AlignBottom);
    layout->addWidget(inputRow);

    // Панель записи: занимает то же место и ту же высоту, что и строка
    // ввода, и показывается вместо неё (см. onRecorderStateChanged) - как
    // VoiceRecordBar в ComposeControls у Telegram Desktop. Слева корзина
    // (отменить и выбросить), затем мигающая красная точка и таймер,
    // справа пауза и отправка.
    recordBar_ = createPanel(conversationColumn, st::historyComposeAreaBg);
    recordBar_->setObjectName("recordBar");
    recordBar_->setFixedHeight(style::ConvertScale(54));
    recordBar_->hide();
    auto* recordLayout = new QHBoxLayout(recordBar_);
    recordLayout->setContentsMargins(style::ConvertScale(2), 0, style::ConvertScale(2), 0);
    recordLayout->setSpacing(0);

    cancelRecordButton_ = new Ui::IconButton(recordBar_, st::zkgramCancelRecordButton);
    cancelRecordButton_->setObjectName("composeIconButton");
    cancelRecordButton_->setToolTip("Discard recording");
    lockButtonSize(cancelRecordButton_);

    recordDot_ = new QLabel(recordBar_);
    recordDot_->setObjectName("recordDot");
    recordDot_->setFixedSize(style::ConvertScale(10), style::ConvertScale(10));
    recordDot_->setPixmap(dotPixmap(style::ConvertScale(10), QColor("#e41e3f")));

    // REVERTED to QLabel (2026-08-17): same batch/precaution as
    // companionLabel_/plainSendWarning_ (see their own comments) - not
    // individually confirmed broken, but 2 of 3 FlatLabel conversions in
    // this same pass already were (one crash, one invisible), so treating
    // this one as equally unproven rather than waiting for a third bug
    // report to roll it back too.
    recordTime_ = new QLabel("0:00,00", recordBar_);
    applyLabelStyle(recordTime_, st::zkgramRecordTime);
    recordTime_->setObjectName("recordTime");

    pauseRecordButton_ = new Ui::IconButton(recordBar_, st::zkgramPauseRecordButton);
    pauseRecordButton_->setObjectName("composeIconButton");
    pauseRecordButton_->setToolTip("Pause recording");
    lockButtonSize(pauseRecordButton_);

    sendRecordButton_ = new Ui::IconButton(recordBar_, st::zkgramSendRecordButton);
    sendRecordButton_->setObjectName("composeIconButton");
    sendRecordButton_->setToolTip("Stop and send");
    lockButtonSize(sendRecordButton_);

    recordLayout->addWidget(cancelRecordButton_, 0, Qt::AlignVCenter);
    recordLayout->addSpacing(8);
    recordLayout->addWidget(recordDot_, 0, Qt::AlignVCenter);
    recordLayout->addSpacing(10);
    recordLayout->addWidget(recordTime_, 0, Qt::AlignVCenter);
    recordLayout->addStretch();
    recordLayout->addWidget(pauseRecordButton_, 0, Qt::AlignVCenter);
    recordLayout->addWidget(sendRecordButton_, 0, Qt::AlignVCenter);
    layout->addWidget(recordBar_);

    recordTimer_ = new QTimer(this);
    // 50 мс: таймер показывает сотые доли секунды, как в Telegram, а
    // обновление реже этого делает последнюю цифру рвано скачущей.
    recordTimer_->setInterval(50);

    splitter->addWidget(conversationColumn);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({280, 700});

    audioInput_ = new QAudioInput(this);
    captureSession_ = new QMediaCaptureSession(this);
    captureSession_->setAudioInput(audioInput_);
    recorder_ = new QMediaRecorder(this);
    captureSession_->setRecorder(recorder_);
    QMediaFormat voiceFormat(QMediaFormat::FileFormat::Mpeg4Audio);
    voiceFormat.setAudioCodec(QMediaFormat::AudioCodec::AAC);
    recorder_->setMediaFormat(voiceFormat);

    // Plays back a companion's (or your own) incoming voice notes - see
    // HistoryCanvas::onVoicePlayToggled/paintVoiceContent above. Separate
    // from captureSession_/recorder_ above, which only ever write audio,
    // never play it back.
    voicePlayer_ = new QMediaPlayer(this);
    voiceAudioOutput_ = new QAudioOutput(this);
    voicePlayer_->setAudioOutput(voiceAudioOutput_);
    connect(voicePlayer_, &QMediaPlayer::positionChanged, this, &MainWindow::onVoicePlaybackStateChanged);
    connect(voicePlayer_, &QMediaPlayer::playbackStateChanged, this, &MainWindow::onVoicePlaybackStateChanged);
    connect(voicePlayer_, &QMediaPlayer::mediaStatusChanged, this, [this](QMediaPlayer::MediaStatus status) {
        // Real Telegram resets a finished voice note back to its start
        // (Play, not a frozen Pause icon at the end of the track) rather
        // than leaving it sitting at 100% - matches that instead of
        // relying on playbackStateChanged alone, which QMediaPlayer does
        // not always fire a further update for once EndOfMedia is reached.
        if (status == QMediaPlayer::EndOfMedia) {
            playingVoiceMessageId_ = 0;
            asCanvas(messages_)->setVoicePlaybackState(0, false, 0, 0);
        }
    });

    // Nothing selected in the sidebar yet.
    updateConversationControlsVisibility();

    authSlide_ = new AuthSlide(this);

    centralStack_ = new QStackedWidget(this);
    centralStack_->addWidget(authSlide_);  // index 0: shown first
    centralStack_->addWidget(chatPage_);   // index 1: shown once TDLib login finishes
    centralStack_->setCurrentWidget(authSlide_);
    setCentralWidget(centralStack_);
    resize(960, 640);

    // Must come after centralStack_/authSlide_/chatPage_ above:
    // setConnectionStatus (when not "Connected") touches
    // centralStack_->currentWidget(), which used to run here while
    // centralStack_ was still null - a guaranteed null-pointer crash on
    // every single startup (see TODO.md, crash in
    // MainWindow::setConnectionStatus called from MainWindow::MainWindow).
    setConnectionStatus("Not connected", "#bcc0c4");

    sendButton_->setClickedCallback([this] { onSendOrMicClicked(); });
    sendFileButton_->setClickedCallback([this] { onSendFileClicked(); });
    cancelRecordButton_->setClickedCallback([this] { onCancelRecordingClicked(); });
    pauseRecordButton_->setClickedCallback([this] { onPauseRecordingClicked(); });
    sendRecordButton_->setClickedCallback([this] { onSendRecordingClicked(); });
    connect(recordTimer_, &QTimer::timeout, this, &MainWindow::onRecordingTick);
    connect(input_, &ChatInput::submitted, this, &MainWindow::onSendClicked);
    connect(input_, &ChatInput::textChanged, this, &MainWindow::updateSendButtonIcon);
    connect(historyCanvas, &QWidget::customContextMenuRequested, this, &MainWindow::showMessageContextMenu);
    connect(recorder_, &QMediaRecorder::recorderStateChanged, this, &MainWindow::onRecorderStateChanged);
    // Ui::InputField::changes() carries no value (see ChatInput's own
    // constructor for the same pattern) - reads getLastText() itself
    // instead of relying on a signal argument, unlike QLineEdit::textChanged.
    searchInput_->changes(
    ) | rpl::on_next([this] { onSearchTextChanged(searchInput_->getLastText()); }, searchInput_->lifetime());
    startEncryptionButton_->setClickedCallback([this] { onStartEncryptionClicked(); });
    secretModeToggle_->setClickedCallback([this] { onSecretModeToggleClicked(); });
    chatSearchInput_->changes(
    ) | rpl::on_next([this] { onChatSearchTextChanged(chatSearchInput_->getLastText()); },
                      chatSearchInput_->lifetime());
    connect(searchDebounceTimer_, &QTimer::timeout, this, &MainWindow::onChatSearchDebounceTimeout);

    // Application-wide, not installed on input_/field_ directly: Key_Up
    // actually arrives at Ui::InputField's own internal child widget, not
    // at ChatInput or even Ui::InputField itself, so a filter scoped to
    // either of those would never see it - see eventFilter()'s own
    // comment for the rest of the reasoning (editLastOutgoingMessage()).
    qApp->installEventFilter(this);
}

void MainWindow::setSession(zkgram::core::Session* session) {
    session_ = session;
}

MainWindow::StartupCredentials MainWindow::runStartupWizard(const QString& dataDir) {
    centralStack_->setCurrentWidget(authSlide_);

    // This password is local only - it is never sent anywhere, TDLib/
    // Telegram do not see it. CryptoLayer uses it to encrypt your local
    // signing key on disk (dataDir/sign_private, see crypto_layer.py's
    // encrypt_write_file/USER_PASSWORD); it has nothing to do with
    // Telegram's own Two-Step Verification password asked later. If
    // dataDir/sign_private already exists, this is a returning identity and
    // the password must match the one used to create it, not a new one -
    // the title/subtitle below make that distinction explicit instead of
    // showing the same "Local password" wording either way, which is what
    // confused the first tester into thinking it was asking for a
    // password they had never set.
    StartupCredentials result;
    bool returningIdentity = QFileInfo::exists(dataDir + "/sign_private");
    QString title = returningIdentity ? "Enter your password" : "Create a password";
    QString subtitle =
        returningIdentity
            ? "This local identity already exists on this computer. Enter the password you set up for it."
            : "Choose a password to protect your local identity on this computer. You will need to enter this same "
              "password every time you open zkgram. It is not stored anywhere and cannot be recovered.";
    result.password = authSlide_->runSlide(title, subtitle, "Password", true);
    if (result.password.isEmpty()) {
        return result;  // window was closed, see AuthSlide::cancelPending()
    }
    result.accepted = true;
    return result;
}

void MainWindow::closeEvent(QCloseEvent* event) {
    authSlide_->cancelPending();
    QMainWindow::closeEvent(event);
}

void MainWindow::showConnectingProgress(const QString& stage, const QString& message) {
    // Once chatPage_ is visible (connected_ == true), don't yank the user
    // back to the auth screen for a late/stray status update.
    if (connected_) {
        return;
    }
    centralStack_->setCurrentWidget(authSlide_);
    authSlide_->showStatus("Connecting", stage + ": " + message);
}

void MainWindow::setControlsEnabled(bool enabled) {
    input_->setEnabled(enabled);
    sendButton_->setEnabled(enabled);
    sendFileButton_->setEnabled(enabled);
}

void MainWindow::updateConversationControlsVisibility() {
    bool hasSelection = currentChatId_ != 0;
    bool isActive = hasSelection && conversationActive_.value(currentChatId_, false);
    bool isReady = hasSelection && conversationReady_.value(currentChatId_, false);
    bool isChannel = hasSelection && conversationIsChannel_.value(currentChatId_, false);
    // Encryption is a 1:1 protocol between two CryptoLayer nodes - a
    // channel's one-to-many audience cannot use it, so the bar never
    // appears there regardless of isActive.
    writeRestrictionBar_->setVisible(hasSelection && !isActive && !isChannel);
    // A chat with no encrypted session at all can still be written to
    // directly, unencrypted, straight to Telegram - the same chats
    // maybeLoadPlainHistory() already reads as plain content instead of
    // ciphertext (a contact not running zkgram, a group, a channel);
    // without this such a chat could be read but never actually replied
    // to unless the other side also ran zkgram. Once isActive (handshake
    // started), this reverts to hidden until isReady - CryptoLayer's AES
    // key is still None mid handshake, sending before that crashes it
    // instead of failing gracefully, same as before.
    bool canSendPlain = hasSelection && !isActive && !isChannel;
    bool showInput = isReady || canSendPlain;
    input_->setVisible(showInput);
    sendButton_->setVisible(showInput);
    sendFileButton_->setVisible(showInput);
    setControlsEnabled(showInput);
    // plainSendWarning_ is shown/hidden as part of writeRestrictionBar_
    // as a whole above - same canSendPlain condition, no need to also
    // toggle it individually.
}

void MainWindow::onSecretModeToggleClicked() {
    if (currentChatId_ == 0) {
        return;
    }
    bool wasOn = secretModeOn_.value(currentChatId_, false);
    secretModeOn_[currentChatId_] = !wasOn;
    renderCurrentConversation();
    updateSecretModeIndicator();
}

void MainWindow::startReply(const QString& senderTitle, const QString& text, qlonglong messageId) {
    replyToText_ = text;
    replyToMessageId_ = messageId;
    Ui::MessageBarContent content;
    content.title = senderTitle;
    content.text = TextWithEntities{.text = text};
    replyBar_->set(std::move(content));
    replyBarRow_->show();
    input_->setFocus();
}

void MainWindow::quoteFromMessage(const StoredMessage& target, QString* author, QString* text) const {
    *author = target.kind == MessageKind::Outgoing ? QString("You") : target.senderName;
    // A media message with no caption has no text to quote - name the kind
    // instead of quoting an empty line.
    *text = !target.text.isEmpty() ? target.text : (!target.filePath.isEmpty() ? QString("Photo") : QString());
}

void MainWindow::quoteForReply(qlonglong chatId, qlonglong replyToMessageId, QString* author, QString* text) const {
    author->clear();
    text->clear();
    if (replyToMessageId == 0) {
        return;
    }
    // Secret content is only quotable while secret mode is on for this chat -
    // a reply must not surface a hidden message's text on a plain bubble.
    // Same rule the bulk rebuild applies by indexing only what it is about
    // to draw.
    bool secretVisible = secretModeOn_.value(chatId, false);
    // constFind, not value(): QMap::value() returns a copy, and copying the
    // whole conversation on every appended message is exactly the kind of
    // per-message cost this file has had to remove elsewhere.
    auto chat = conversationMessages_.constFind(chatId);
    if (chat == conversationMessages_.constEnd()) {
        return;
    }
    for (const StoredMessage& candidate : *chat) {
        if (candidate.messageId == replyToMessageId && (!candidate.isSecret || secretVisible)) {
            quoteFromMessage(candidate, author, text);
            return;
        }
    }
}

QString MainWindow::replyAuthorLabel(MessageKind kind) const {
    return kind == MessageKind::Outgoing ? QString("You") : companionLabel_->text();
}

qlonglong MainWindow::pickForwardTarget() {
    // Plain Qt Widgets rather than a lib_ui box: this is one modal list with
    // a filter, and Ui::BoxContent brings its own layer/window management
    // that nothing else in this file uses yet (UI.md section 13c).
    QDialog dialog(this);
    dialog.setWindowTitle("Forward to");
    dialog.resize(style::ConvertScale(360), style::ConvertScale(460));
    auto* layout = new QVBoxLayout(&dialog);

    auto* filter = new QLineEdit(&dialog);
    filter->setPlaceholderText("Search");
    layout->addWidget(filter);

    auto* list = new QListWidget(&dialog);
    layout->addWidget(list, 1);
    // The sidebar's own snapshot, so the picker offers exactly the chats the
    // user can already see, in the same order, with no second source of
    // truth to drift.
    for (const QVariant& entryVariant : lastChatListSnapshot_) {
        QVariantMap entry = entryVariant.toMap();
        auto* row = new QListWidgetItem(entry["title"].toString(), list);
        row->setData(Qt::UserRole, entry["id"].toLongLong());
    }

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    // Nothing selected yet, so there is nothing to forward to - enabled by
    // the selection handler below.
    buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
    buttons->button(QDialogButtonBox::Ok)->setText("Forward");

    connect(filter, &QLineEdit::textChanged, list, [list](const QString& query) {
        QString trimmed = query.trimmed();
        for (int i = 0; i < list->count(); ++i) {
            QListWidgetItem* row = list->item(i);
            row->setHidden(!trimmed.isEmpty() && !row->text().contains(trimmed, Qt::CaseInsensitive));
        }
    });
    connect(list, &QListWidget::itemSelectionChanged, buttons, [list, buttons] {
        buttons->button(QDialogButtonBox::Ok)->setEnabled(list->currentItem() != nullptr);
    });
    // Double-click is the shortcut every list-picker has - same as picking a
    // row and confirming.
    connect(list, &QListWidget::itemDoubleClicked, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    if (dialog.exec() != QDialog::Accepted || list->currentItem() == nullptr) {
        return 0;
    }
    return list->currentItem()->data(Qt::UserRole).toLongLong();
}

void MainWindow::forwardTextTo(qlonglong chatId, const QString& text, const QString& originalAuthor) {
    if (session_ == nullptr || chatId == 0) {
        return;
    }
    // Attribution kept in the text itself. TDLib's real forwardMessages
    // (which preserves the original author as message metadata) is not
    // exposed by this project's transport layer, and an encrypted forward
    // could not use it anyway - CryptoLayer carries text, not Telegram
    // messages. A prefix is the honest approximation of both.
    QString forwarded = originalAuthor.isEmpty() ? QString("Forwarded:\n%1").arg(text)
                                                  : QString("Forwarded from %1:\n%2").arg(originalAuthor, text);
    // Decided by the DESTINATION chat, not the one being forwarded from -
    // the old Forward always called sendText(), which is the encrypted path,
    // so forwarding into any chat without an active session (that is, almost
    // all of them) failed instead of sending.
    bool isActive = conversationActive_.value(chatId, false);
    try {
        if (isActive) {
            session_->sendText(chatId, forwarded.toStdString());
        } else {
            session_->sendPlainText(chatId, forwarded.toStdString());
        }
    } catch (const std::exception& e) {
        appendMessage(currentChatId_, MessageKind::System, QString("Forward failed: %1").arg(e.what()),
                       /*isSecret=*/false);
        return;
    }
    // Only the open chat has anything on screen to append to; a forward to
    // some other chat shows up there when it is next opened, from history.
    if (chatId == currentChatId_) {
        appendMessage(chatId, MessageKind::Outgoing, forwarded, isActive);
    } else {
        appendMessage(currentChatId_, MessageKind::System,
                       QString("Forwarded to %1").arg(conversationTitles_.value(chatId, QString::number(chatId))),
                       /*isSecret=*/false);
    }
}

void MainWindow::cancelReply() {
    replyToText_.clear();
    replyToMessageId_ = 0;
    replyBarRow_->hide();
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::KeyPress) {
        auto* keyEvent = static_cast<QKeyEvent*>(event);
        if (keyEvent->key() == Qt::Key_Up && keyEvent->modifiers() == Qt::NoModifier) {
            auto* widget = qobject_cast<QWidget*>(watched);
            // input_->isAncestorOf(widget) - not widget == input_ - because
            // the key actually lands on Ui::InputField's own internal
            // child, several levels below the ChatInput wrapper (see this
            // function's own header comment).
            if (widget != nullptr && input_ != nullptr && input_->isAncestorOf(widget) &&
                input_->toPlainText().isEmpty()) {
                editLastOutgoingMessage();
                return true;
            }
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::editLastOutgoingMessage() {
    if (currentChatId_ == 0) {
        return;
    }
    const QVector<StoredMessage>& stored = conversationMessages_.value(currentChatId_);
    for (auto it = stored.crbegin(); it != stored.crend(); ++it) {
        if (it->kind == MessageKind::Outgoing) {
            // Same local-only convenience as the context menu's "Edit"
            // action (see showMessageContextMenu()'s own comment) - just
            // reachable without a right-click/long-press: refills the
            // composer with the text, does not touch what the companion
            // already received.
            input_->setPlainText(it->text);
            input_->setFocus();
            return;
        }
    }
}

void MainWindow::updateSelectionToolbar(bool active, int selectedCount) {
    headerRow_->setVisible(!active);
    selectionToolbar_->setVisible(active);
    if (active) {
        selectionCountLabel_->setText(QString("%1 selected").arg(selectedCount));
    }
}

void MainWindow::onSelectionCopyClicked() {
    QVector<HistoryItem> selected = asCanvas(messages_)->selectedItemsInOrder();
    QStringList lines;
    lines.reserve(selected.size());
    for (const HistoryItem& item : selected) {
        lines.push_back(item.rawText);
    }
    QGuiApplication::clipboard()->setText(lines.join("\n"));
    asCanvas(messages_)->exitSelectionMode();
}

void MainWindow::onSelectionForwardClicked() {
    if (session_ == nullptr || currentChatId_ == 0) {
        return;
    }
    // Same "resend to the currently open conversation" convention as the
    // single-message Forward in showMessageContextMenu() - always via
    // sendText (encrypted), see that action's own comment for why there is
    // no plain-send fallback here either.
    QVector<HistoryItem> selected = asCanvas(messages_)->selectedItemsInOrder();
    for (const HistoryItem& item : selected) {
        QString forwarded = "Forwarded: " + item.rawText;
        session_->sendText(currentChatId_, forwarded.toStdString());
        appendMessage(currentChatId_, MessageKind::Outgoing, forwarded, /*isSecret=*/true);
    }
    asCanvas(messages_)->exitSelectionMode();
}

void MainWindow::onSelectionDeleteClicked() {
    // Local-only, same limitation already documented on the single-message
    // "Edit" action in showMessageContextMenu(): neither TDLib's real
    // message deletion nor anything in crypto::CryptoLayer's stream
    // protocol is wired up here, so this removes the selected messages
    // from this device's own view/cache without notifying the companion or
    // Telegram's servers - real Telegram's own "delete for me" behavior,
    // not "delete for everyone".
    //
    // Only removes messages with a real TDLib id (item.messageId != 0,
    // i.e. plain/history messages) - a crypto::CryptoLayer (secret) or
    // System message has no stable per-message identity in
    // StoredMessage/HistoryItem to match against at all (messageId is 0
    // for every one of them; core::DataRegistry already solves this with
    // synthetic negative ids, see core/data_registry.hpp, but the UI does
    // not read from that registry yet - see TODO.md). Selecting only
    // secret messages and deleting is therefore a silent no-op for now,
    // not a hidden crash, but it is a real, known gap, not a finished
    // feature.
    if (currentChatId_ == 0) {
        return;
    }
    QVector<HistoryItem> selected = asCanvas(messages_)->selectedItemsInOrder();
    QSet<qlonglong> messageIdsToRemove;
    for (const HistoryItem& item : selected) {
        if (item.messageId != 0) {
            messageIdsToRemove.insert(item.messageId);
        }
    }
    QVector<StoredMessage>& stored = conversationMessages_[currentChatId_];
    if (!messageIdsToRemove.isEmpty()) {
        stored.erase(std::remove_if(stored.begin(), stored.end(),
                                     [&](const StoredMessage& message) {
                                         return message.messageId != 0 &&
                                                messageIdsToRemove.contains(message.messageId);
                                     }),
                      stored.end());
    }
    asCanvas(messages_)->exitSelectionMode();
    renderCurrentConversation();
    cacheConversationHistory(currentChatId_);
}

void MainWindow::updateSecretModeIndicator() {
    if (currentChatId_ == 0 || !hasSecretContent_.contains(currentChatId_)) {
        // Nothing secret ever happened in this chat - no toggle to show,
        // matches the write-restriction bar/plain-only composer this
        // chat already has (see updateConversationControlsVisibility()).
        secretModeToggle_->hide();
        return;
    }
    bool on = secretModeOn_.value(currentChatId_, false);
    secretModeToggle_->setText(rpl::single(QString(on ? "Secret" : "Normal")));
    // See lockButtonSize()'s own comment - this button's text (and so its
    // resizeToText()-computed width) changes at runtime, so the fixed size
    // has to be re-locked here too, not just once after construction.
    lockButtonSize(secretModeToggle_);
    // Direct per-instance background override (RoundButton::setBrushOverride),
    // not setStyleSheet() - see zkgram_icons.style's zkgramSecretModeToggle
    // comment for why the "Normal" red is a runtime QColor and not a second
    // palette-backed .style instance.
    secretModeToggle_->setBrushOverride(on ? QColor("#40a7e3") : QColor("#e41e3f"));
    secretModeToggle_->setToolTip(on ? "Secret mode: showing encrypted and plain messages. Click to hide encrypted "
                                        "messages again."
                                      : "Normal mode: encrypted messages are hidden. Click to reveal them.");
    secretModeToggle_->show();
}

void MainWindow::setConnectionStatus(const QString& text, const QString& colorHex) {
    statusLabel_->setText(text);
    statusDot_->setStyleSheet(QString("background-color: %1; border-radius: 5px;").arg(colorHex));

    // "Connected" here means "logged in to Telegram" (TDLib-level), not
    // any particular conversation being ready - see setConversationReady()
    // for that.
    connected_ = text == "Connected";
    if (connected_) {
        centralStack_->setCurrentWidget(chatPage_);
    } else if (centralStack_->currentWidget() != chatPage_) {
        // Same visibility gap as showConnectingProgress: statusLabel_ above
        // lives in chatPage_'s header, invisible while still stuck on
        // authSlide_ - "Connecting..."/"Connection failed" needs to show up
        // somewhere the user can actually see it. Skipped once chatPage_
        // has been shown at least once - a later disconnect/ping-timeout
        // should update the header pill in place, not yank the user back
        // to the auth screen.
        authSlide_->showStatus(text, QString());
    }
}

void MainWindow::updateMyProfile(const QString& name, const QString& username) {
    QString displayName = name.isEmpty() ? "You" : name;
    mainMenuNameLabel_->setText(displayName);
    mainMenuUsernameLabel_->setText(username.isEmpty() ? QString() : "@" + username);
    int avatarSize = mainMenuAvatar_->width();
    mainMenuAvatar_->setPixmap(avatarPixmap(displayName, 0, avatarSize));
}

void MainWindow::toggleMainMenu() {
    if (mainMenuOpen_) {
        closeMainMenu();
    } else {
        mainMenuOpen_ = true;
        int panelWidth = mainMenuPanel_->width();
        mainMenuDimmer_->setGeometry(chatPage_->rect());
        mainMenuDimmer_->show();
        mainMenuDimmer_->raise();
        mainMenuPanel_->setGeometry(-panelWidth, 0, panelWidth, chatPage_->height());
        mainMenuPanel_->show();
        mainMenuPanel_->raise();
        mainMenuAnimation_->stop();
        mainMenuAnimation_->setStartValue(mainMenuPanel_->geometry());
        mainMenuAnimation_->setEndValue(QRect(0, 0, panelWidth, chatPage_->height()));
        mainMenuAnimation_->start();
    }
}

void MainWindow::closeMainMenu() {
    if (!mainMenuOpen_) {
        return;
    }
    mainMenuOpen_ = false;
    mainMenuDimmer_->hide();
    int panelWidth = mainMenuPanel_->width();
    mainMenuAnimation_->stop();
    mainMenuAnimation_->setStartValue(mainMenuPanel_->geometry());
    mainMenuAnimation_->setEndValue(QRect(-panelWidth, 0, panelWidth, chatPage_->height()));
    mainMenuAnimation_->start();
}

void MainWindow::openWalletSection() {
    bool showing = walletDetail_->isVisible();
    walletDetail_->setVisible(!showing);
    if (!showing && !walletInfoLoaded_) {
        refreshWalletInfo();
    }
}

void MainWindow::refreshWalletInfo() {
    walletInfoLoaded_ = true;
    walletBalanceLabel_->setText("Loading...");
    walletAddressLabel_->setText(QString());
    walletStatusLabel_->setText(QString());

    // Both requests are independent HTTP round trips (see
    // MoneroWalletRpc::call()) - each updates only its own label on
    // return, so one failing (e.g. the daemon answered get_address but not
    // get_balance) does not blank out whatever the other already showed.
    moneroClient_->fetchBalance([this](bool ok, std::uint64_t balance, std::uint64_t unlocked, const QString& error) {
        if (!ok) {
            walletBalanceLabel_->setText(QString());
            walletStatusLabel_->setText("Could not reach monero-wallet-rpc: " + error);
            return;
        }
        QString text = zkgram::monero::MoneroWalletRpc::formatAtomicXmr(balance);
        if (unlocked != balance) {
            text += QString(" (%1 unlocked)").arg(zkgram::monero::MoneroWalletRpc::formatAtomicXmr(unlocked));
        }
        walletBalanceLabel_->setText(text);
    });
    moneroClient_->fetchPrimaryAddress([this](bool ok, const QString& address, const QString& error) {
        if (!ok) {
            walletAddressLabel_->setText(QString());
            if (walletStatusLabel_->text().isEmpty()) {
                walletStatusLabel_->setText("Could not reach monero-wallet-rpc: " + error);
            }
            return;
        }
        walletAddressLabel_->setText(address);
    });
}

void MainWindow::updateConversationMetadata(const QVariantList& chats) {
    for (const QVariant& entryVariant : chats) {
        QVariantMap entry = entryVariant.toMap();
        qlonglong id = entry["id"].toLongLong();
        conversationTitles_[id] = entry["title"].toString();
        conversationActive_[id] = entry["active"].toBool();
        conversationIsChannel_[id] = entry["channel"].toBool();
        QString photo = entry["photo"].toString();
        if (!photo.isEmpty()) {
            conversationPhotoPaths_[id] = photo;
        }
    }
}

void MainWindow::renderSidebarRows(const QVariantList& chats) {
    logDebug(QString("renderSidebarRows: rendering %1 chats, chatListWidget_ rowCount before=%2")
                 .arg(chats.size())
                 .arg(asSidebar(chatListWidget_)->rowCount()));
    // The scroll position is deliberately not reset here any more. It used
    // to be, because a rebuild could leave the offset past the end of a
    // momentarily shorter list and the sidebar would look empty - but
    // SidebarCanvas::setRows() calls updateScrollRange(), and
    // QScrollBar::setRange() already clamps the current value into the new
    // range, so that state is not reachable. Resetting on top of that just
    // meant the list snapped back to the first row on every push, which
    // made the sidebar feel unusable for as long as updates kept arriving.
    asSidebar(chatListWidget_)->setRows(chats, currentChatId_);
    logDebug(QString("renderSidebarRows: done, chatListWidget_ rowCount after=%1")
                 .arg(asSidebar(chatListWidget_)->rowCount()));
}

void MainWindow::updateChatList(QVariantList chats) {
    // TelegramClient::publishChatList() only includes chats whose TDLib
    // position order is currently non-zero (see telegram_client.cpp) - and
    // TDLib itself was observed to push a position of 0 for every chat
    // simultaneously, transiently, while re-syncing its own chat list
    // internally (confirmed via logging: 20 of 22 chat-list pushes in one
    // session were empty, sandwiched between two pushes with real data
    // moments apart), before immediately restoring the real positions.
    // Applying every such push as it arrives made the sidebar flicker
    // empty almost the entire time. If we already have real chats and this
    // push would wipe them to nothing, treat it as transient and ignore it
    // - a genuinely empty account still renders correctly on first login,
    // since lastChatListSnapshot_ starts empty too.
    if (chats.isEmpty() && !lastChatListSnapshot_.isEmpty()) {
        logDebug("updateChatList: ignoring transient empty chat list push, keeping last known non-empty list");
        return;
    }
    // Nothing visible changed - skip the metadata rescan and, more
    // importantly, do not schedule a sidebar rebuild. TDLib pushes plenty
    // of updates that do not alter anything this list actually shows (a
    // chat's position reshuffled to the same effective order, a photo id
    // changing for a file that resolves to the same path, ...), and
    // rebuilding every row to redraw an identical list is the most
    // expensive possible way to do nothing.
    if (chats == lastChatListSnapshot_) {
        return;
    }
    lastChatListSnapshot_ = chats;
    updateConversationMetadata(chats);
    // A background chat-list push (new message, reordering, ...) must not
    // visually clobber search results currently on screen - it still
    // updates lastChatListSnapshot_/the title caches above, just does not
    // re-render until the search box is cleared.
    if (chatSearchInput_->getLastText().trimmed().isEmpty()) {
        // Throttled, not called directly: TelegramClient::requestMoreChats()
        // can push updateChatList many times in a tight burst while paging
        // through a large account's full chat list (see its own comment in
        // telegram_client.cpp), each carrying every chat known so far -
        // rendering all of that synchronously, every single time, meant
        // tearing down and rebuilding potentially hundreds of sidebar rows
        // (each with its own avatar pixmap decode) over and over, fast
        // enough to stop the GUI thread from processing paint/input events
        // at all and make the window read as "Not Responding". If nothing
        // is already scheduled, render soon (150ms - imperceptible for a
        // one-off update); if a render is already pending, this update's
        // data is already captured in lastChatListSnapshot_ above and will
        // be picked up when that pending render actually runs, so nothing
        // more to do here.
        //
        // TelegramClient::publishChatList() now coalesces on its own side
        // too, so far fewer pushes ever reach this function - this throttle
        // stays as the second half of the same defence, since it is what
        // bounds the cost of the render itself (the GUI thread's work),
        // independently of how often the transport decides to publish.
        if (!sidebarRenderThrottleTimer_->isActive()) {
            sidebarRenderThrottleTimer_->start(150);
        }
    }
    updateConversationControlsVisibility();
}

void MainWindow::flushSidebarRender() {
    renderSidebarRows(lastChatListSnapshot_);
}

void MainWindow::onChatSearchTextChanged(const QString& query) {
    // Instant local filter over the chat list already in memory (your own
    // chats/contacts) on every keystroke, zero network latency - real
    // Telegram Desktop's own dialogs search filters its local list
    // immediately, only the global public-username half needs a network
    // round trip (see session_->searchChats below). Without this the
    // sidebar sat completely unchanged for the whole debounce+network
    // window after every keystroke, which read as "does not update until
    // I stop typing" even though a search was already technically queued.
    QString trimmed = query.trimmed();
    if (trimmed.isEmpty()) {
        renderSidebarRows(lastChatListSnapshot_);
    } else {
        QVariantList localMatches;
        for (const QVariant& entryVariant : lastChatListSnapshot_) {
            QVariantMap entry = entryVariant.toMap();
            if (entry["title"].toString().contains(trimmed, Qt::CaseInsensitive)) {
                localMatches.append(entry);
            }
        }
        renderSidebarRows(localMatches);
    }
    // Still debounced (shorter than before - 150ms, not 300ms): the
    // network call underneath (global username search) has real latency
    // and cost regardless of how fast the local filter above is, so
    // firing it on every single keystroke would be wasteful even though
    // the visible list already updated instantly.
    searchDebounceTimer_->start(150);
}

void MainWindow::onChatSearchDebounceTimeout() {
    QString query = chatSearchInput_->getLastText().trimmed();
    logDebug(QString("onChatSearchDebounceTimeout: query=\"%1\"").arg(query));
    if (query.isEmpty()) {
        renderSidebarRows(lastChatListSnapshot_);
        return;
    }
    if (session_ == nullptr) {
        return;
    }
    session_->searchChats(query.toStdString(), [this, query](const std::vector<zkgram::core::ChatListEntry>& results) {
        QVariantList list;
        list.reserve(static_cast<int>(results.size()));
        for (const auto& entry : results) {
            QVariantMap map;
            map["id"] = static_cast<qlonglong>(entry.id);
            map["title"] = QString::fromStdString(entry.title);
            map["preview"] = QString::fromStdString(entry.lastMessagePreview);
            map["unread"] = entry.unreadCount;
            map["active"] = entry.hasActiveConversation;
            list.append(map);
        }
        QMetaObject::invokeMethod(this, [this, list, query] {
            // Guards against an older, slower search response landing
            // after a newer keystroke already changed the query - without
            // this a stale result set could flash back on screen.
            if (chatSearchInput_->getLastText().trimmed() != query) {
                return;
            }
            updateConversationMetadata(list);
            renderSidebarRows(list);
        });
    });
}

void MainWindow::onChatListItemClicked(qlonglong chatId) {
    logDebug(QString("onChatListItemClicked: entered, chatId=%1, chatListWidget_ rowCount=%2, searchText=\"%3\"")
                 .arg(chatId)
                 .arg(asSidebar(chatListWidget_)->rowCount())
                 .arg(chatSearchInput_->getLastText()));
    currentChatId_ = chatId;
    cancelReply();  // a pending reply belongs to the chat it was started in

    // Opening a chat (whether from the normal list or a search result)
    // closes the search, same as tdesktop: back to the regular dialog list
    // underneath whatever was just opened.
    if (!chatSearchInput_->getLastText().isEmpty()) {
        chatSearchInput_->setText(QString());
        renderSidebarRows(lastChatListSnapshot_);
    } else {
        asSidebar(chatListWidget_)->setSelectedChatId(currentChatId_);
    }

    QString title = conversationTitles_.value(currentChatId_, "zkgram");
    companionLabel_->setText(title);
    companionAvatar_->setPixmap(chatAvatarPixmap(title, currentChatId_, conversationPhotoPaths_.value(currentChatId_),
                                                  style::ConvertScale(42)));
    companionAvatar_->show();
    maybeLoadCachedHistory(currentChatId_);
    updateSecretModeIndicator();
    renderCurrentConversation();
    updateConversationControlsVisibility();
    maybeLoadPlainHistory(currentChatId_);
    // Opening a chat deliberately does not mark it read - the unread badge
    // staying put is the intended behaviour, see the note in
    // telegram::TelegramClient.
}

void MainWindow::maybeLoadCachedHistory(qlonglong chatId) {
    if (session_ == nullptr || cacheLoadedChats_.contains(chatId)) {
        return;
    }
    cacheLoadedChats_.insert(chatId);
    // Populates conversationMessages_ directly, not via appendMessage()
    // in a loop - renderCurrentConversation() (called right after this,
    // in onChatListItemClicked) already does one full rebuild from
    // conversationMessages_ itself; looping appendMessage() here would
    // instead fire one appendItem() per cached message against whatever
    // chat HistoryCanvas currently shows, which at this point in
    // onChatListItemClicked is still the *previous* selection.
    std::vector<zkgram::core::PlainMessage> cached = session_->loadCachedHistory(chatId);
    if (cached.empty()) {
        return;
    }
    QVector<StoredMessage> cachedMessages;
    cachedMessages.reserve(static_cast<int>(cached.size()));
    for (const auto& message : cached) {
        // isSecret=true: this is core::LocalCache content, decrypted
        // CryptoLayer history from a previous run - never shown unless
        // secretModeOn_ is explicitly toggled on for this chat (see
        // renderCurrentConversation()), which it is not by default here,
        // unlike the live path (appendConversationPeerText() etc.) which
        // gets an implicit reveal from setConversationReady()/
        // onStartEncryptionClicked() because those mean the user is
        // acting on this chat's encryption right now - loading a chat
        // that merely *has* old cached secret history is not the same
        // deliberate action.
        cachedMessages.push_back(StoredMessage{message.isOutgoing ? MessageKind::Outgoing : MessageKind::Incoming,
                                                QString::fromStdString(message.text),
                                                QString::fromStdString(message.photoPath),
                                                static_cast<qlonglong>(message.id),
                                                QString::fromStdString(message.senderName),
                                                /*isSecret=*/true, message.date});
    }
    hasSecretContent_.insert(chatId);
    // Prepended, not appended: conversationMessages_[chatId] can already
    // hold live messages that arrived while this chat was not the
    // selected one (an active session keeps receiving in the
    // background) - cached history is always older than anything live,
    // so it belongs before that, not after it.
    conversationMessages_[chatId] = cachedMessages + conversationMessages_[chatId];
}

void MainWindow::maybeLoadPlainHistory(qlonglong chatId) {
    // Only for a chat with no encrypted session: its history was never
    // CryptoLayer ciphertext, so there is nothing to decrypt and it can be
    // shown as ordinary Telegram content, same as any normal client would
    // (channels, groups, and contacts who do not run zkgram). Once a chat
    // becomes an active encrypted conversation, its plain history stops
    // being relevant to show alongside the decrypted live messages.
    if (session_ == nullptr || conversationActive_.value(chatId, false) || historyLoadedChats_.contains(chatId)) {
        return;
    }
    historyLoadedChats_.insert(chatId);
    session_->loadMessageHistory(chatId, [this, chatId](const std::vector<zkgram::core::PlainMessage>& messages) {
        QMetaObject::invokeMethod(this, [this, chatId, messages] {
            // Built as one list and put in front of whatever is already
            // stored, rather than appended message by message: a live
            // message can arrive between this request going out and its
            // answer coming back (appendPlainMessageReceived stores it as
            // soon as the chat is marked as having history), and appending
            // would then leave the whole history sitting *below* it. Older
            // before newer either way, and the dedup below keeps the
            // overlapping message from appearing twice.
            QSet<qlonglong>& seen = seenMessageIds_[chatId];
            QVector<StoredMessage> history;
            history.reserve(static_cast<int>(messages.size()));
            for (const auto& message : messages) {
                auto messageId = static_cast<qlonglong>(message.id);
                if (messageId != 0) {
                    if (seen.contains(messageId)) {
                        continue;
                    }
                    seen.insert(messageId);
                }
                // photoPath is only non-empty here if the photo was already
                // cached locally by TDLib - otherwise this starts out as
                // the "Photo" placeholder text from previewFor() and
                // updateHistoryPhoto() swaps it for the real image once
                // TelegramClient::requestHistoryPhoto()'s download finishes
                // (see telegram_client.cpp).
                // isSecret=false: this function only ever runs for a chat
                // with no active session (see the !conversationActive_
                // guard above), so its Telegram content was always
                // ordinary plain text/media, never CryptoLayer ciphertext.
                history.push_back(StoredMessage{
                    message.isOutgoing ? MessageKind::Outgoing : MessageKind::Incoming,
                    QString::fromStdString(message.text),
                    QString::fromStdString(message.isVoiceNote ? message.voiceNotePath : message.photoPath),
                    messageId, QString::fromStdString(message.senderName), /*isSecret=*/false, message.date,
                    message.mediaAlbumId, message.isVoiceNote, message.voiceNoteDuration,
                    QByteArray(reinterpret_cast<const char*>(message.voiceWaveform.data()),
                               static_cast<int>(message.voiceWaveform.size())),
                    static_cast<qlonglong>(message.replyToMessageId)});
            }
            if (!history.isEmpty()) {
                conversationMessages_[chatId] = history + conversationMessages_[chatId];
                if (chatId == currentChatId_) {
                    // A full rebuild rather than prependItems(): this runs
                    // once per chat per session and the list it rebuilds is
                    // at most this one page plus the handful of messages
                    // that raced it, not the ever-growing list
                    // maybeLoadMoreHistory() deliberately avoids rebuilding.
                    renderCurrentConversation();
                }
            }
            // messages is chronological (oldest-first, see
            // TelegramClient::fetchMessageHistory) - front() is the
            // pagination anchor for maybeLoadMoreHistory(). An empty first
            // page means an empty chat, not "more to load" - mark it
            // exhausted right away so scrolling never fires a request that
            // could only ever come back empty too.
            if (!messages.empty()) {
                historyOldestMessageId_[chatId] = static_cast<qlonglong>(messages.front().id);
            } else {
                historyExhausted_.insert(chatId);
            }
            ensureHistoryFillsViewport();
        });
    });
}

void MainWindow::ensureHistoryFillsViewport() {
    qlonglong chatId = currentChatId_;
    constexpr int kMaxAutoFillRounds = 10;
    if (chatId == 0 || historyExhausted_.contains(chatId) || historyLoadingMore_.contains(chatId) ||
        !historyOldestMessageId_.contains(chatId) || historyAutoFillRounds_.value(chatId, 0) >= kMaxAutoFillRounds) {
        return;
    }
    // Item/layout geometry - and so the scrollbar's actual range - is not
    // final immediately after the render that just happened, only once Qt
    // has had a chance to lay everything out on its next event loop turn.
    QTimer::singleShot(0, this, [this, chatId] {
        if (chatId != currentChatId_) {
            return;
        }
        if (!asCanvas(messages_)->contentFillsViewport()) {
            historyAutoFillRounds_[chatId] = historyAutoFillRounds_.value(chatId, 0) + 1;
            maybeLoadMoreHistory();
        }
    });
}

void MainWindow::maybeLoadMoreHistory() {
    qlonglong chatId = currentChatId_;
    if (session_ == nullptr || chatId == 0 || conversationActive_.value(chatId, false) ||
        historyExhausted_.contains(chatId) || historyLoadingMore_.contains(chatId)) {
        return;
    }
    qlonglong oldestId = historyOldestMessageId_.value(chatId, 0);
    if (oldestId == 0) {
        // Either the first page has not come back yet, or the chat is
        // empty (see maybeLoadPlainHistory's historyExhausted_ handling
        // for the latter) - nothing to anchor a page-before request to.
        return;
    }
    historyLoadingMore_.insert(chatId);
    if (chatId == currentChatId_) {
        asCanvas(messages_)->setLoadingMore(true);
    }
    session_->loadMoreMessageHistory(chatId, oldestId, [this, chatId](const std::vector<zkgram::core::PlainMessage>& messages) {
        QMetaObject::invokeMethod(this, [this, chatId, messages] {
            historyLoadingMore_.remove(chatId);
            if (chatId == currentChatId_) {
                asCanvas(messages_)->setLoadingMore(false);
            }
            if (messages.empty()) {
                historyExhausted_.insert(chatId);
                return;
            }
            QSet<qlonglong>& seen = seenMessageIds_[chatId];
            QVector<StoredMessage> older;
            older.reserve(static_cast<int>(messages.size()));
            for (const auto& message : messages) {
                auto messageId = static_cast<qlonglong>(message.id);
                if (messageId != 0) {
                    // A page of older messages cannot normally overlap
                    // anything already on screen (it is anchored strictly
                    // before the oldest one), but the ids are recorded here
                    // all the same so the set stays a complete answer to
                    // "is this message already shown".
                    if (seen.contains(messageId)) {
                        continue;
                    }
                    seen.insert(messageId);
                }
                // isSecret=false - see maybeLoadPlainHistory()'s identical
                // comment, the same !conversationActive_ guard applies
                // here (this function's own entry check, above).
                older.push_back(StoredMessage{
                    message.isOutgoing ? MessageKind::Outgoing : MessageKind::Incoming,
                    QString::fromStdString(message.text),
                    QString::fromStdString(message.isVoiceNote ? message.voiceNotePath : message.photoPath),
                    messageId, QString::fromStdString(message.senderName), /*isSecret=*/false, message.date,
                    message.mediaAlbumId, message.isVoiceNote, message.voiceNoteDuration,
                    QByteArray(reinterpret_cast<const char*>(message.voiceWaveform.data()),
                               static_cast<int>(message.voiceWaveform.size())),
                    static_cast<qlonglong>(message.replyToMessageId)});
            }
            if (older.isEmpty()) {
                // Every message on this page was already on screen. Move
                // the anchor past them anyway so the next request asks for
                // something older instead of fetching this page forever.
                historyOldestMessageId_[chatId] = static_cast<qlonglong>(messages.front().id);
                ensureHistoryFillsViewport();
                return;
            }

            // "attached"/"showTail" computed for just this page (plus the
            // one boundary check against the message it will now sit
            // right before) instead of a full renderCurrentConversation()
            // rebuild of the whole, ever-growing conversation - see
            // HistoryCanvas::prependItems()'s own comment for why a full
            // rebuild on every page load was the actual cause of a real
            // freeze on chats needing several pages to fill the view.
            QVector<bool> attached(older.size(), false);
            for (int i = 1; i < older.size(); ++i) {
                attached[i] = older[i].kind == older[i - 1].kind && older[i].kind != MessageKind::System &&
                              older[i].senderName == older[i - 1].senderName;
            }
            bool hasNext = !conversationMessages_[chatId].isEmpty();  // before prepending
            MessageKind nextKind = hasNext ? conversationMessages_[chatId].front().kind : MessageKind::System;
            QString nextSenderName = hasNext ? conversationMessages_[chatId].front().senderName : QString();

            QVector<HistoryEntry> entries;
            entries.reserve(older.size());
            for (int i = 0; i < older.size(); ++i) {
                bool showTail;
                if (i == older.size() - 1) {
                    showTail = !(hasNext && older[i].kind == nextKind && older[i].kind != MessageKind::System &&
                                 older[i].senderName == nextSenderName);
                } else {
                    showTail = !attached[i + 1];
                }
                HistoryEntry entry{older[i].kind, older[i].text, older[i].filePath, attached[i], older[i].senderName,
                                    showTail, older[i].messageId, older[i].date};
                entry.isVoiceNote = older[i].isVoiceNote;
                entry.voiceDuration = older[i].voiceDuration;
                entry.voiceWaveform = older[i].voiceWaveform;
                entries.push_back(entry);
            }

            // messages is chronological oldest-first, same as the initial
            // load - front() becomes the new pagination anchor.
            historyOldestMessageId_[chatId] = static_cast<qlonglong>(messages.front().id);
            conversationMessages_[chatId] = older + conversationMessages_[chatId];
            if (chatId == currentChatId_) {
                asCanvas(messages_)->prependItems(entries);
            }
            ensureHistoryFillsViewport();
        });
    });
}

void MainWindow::onStartEncryptionClicked() {
    if (session_ == nullptr || currentChatId_ == 0 || conversationIsChannel_.value(currentChatId_, false)) {
        return;
    }
    qlonglong chatId = currentChatId_;

    // The probe is informational only, not a gate: crypto::CryptoLayer's
    // init() (called from startConversation() below) is what actually
    // generates and writes this device's local identity key
    // (data/sign_private) - the very first thing it does, before it ever
    // touches the network. Blocking startConversation() on the probe
    // answering used to also block that local key from ever being created
    // when no zkgram peer was available to reply, which looked like "the
    // password never gets saved, it keeps asking to create a new one every
    // time" even on the very same machine - the file genuinely never
    // existed yet. Starting for real regardless of the probe result keeps
    // that working; a failed probe now only adds a warning message.
    //
    // crypto::CryptoLayer's constructor deliberately throws
    // std::runtime_error on a Python-side init failure (see
    // crypto_layer.cpp) - Session's own constructor already catches this
    // for the very first conversation, but startConversation() (used here,
    // for starting encryption on an already-open chat) never did, so a
    // real init failure crashed the whole process uncaught instead of
    // showing an error - the same class of gap TODO.md already documents
    // being fixed for Session's constructor, just missed here.
    try {
        session_->startConversation(chatId);
    } catch (const std::exception& e) {
        appendMessage(chatId, MessageKind::System, QString("Failed to start encrypted session: %1").arg(e.what()),
                      /*isSecret=*/true);
        return;
    }
    conversationActive_[chatId] = true;
    // Clicking this button is itself the deliberate reveal action for
    // this chat - see setConversationReady()'s identical reasoning for
    // why that is a free pass on secretModeOn_'s "off by default" rule.
    // Set here already, not only once onReady fires, so the handshake's
    // own progress ("Starting...", "Waiting for companion...", the probe
    // note below) is visible too instead of silently hidden until ready.
    secretModeOn_[chatId] = true;
    updateConversationControlsVisibility();
    // Updates secretModeToggle_ too (via hasSecretContent_), see
    // appendMessage()'s own isSecret branch.
    appendMessage(chatId, MessageKind::System, "Starting encrypted session...", /*isSecret=*/true);

    if (!probingChats_.contains(chatId)) {
        probingChats_.insert(chatId);
        session_->probeZkgramPresence(chatId, [this, chatId](bool present) {
            QMetaObject::invokeMethod(this, [this, chatId, present] {
                probingChats_.remove(chatId);
                if (!present) {
                    appendMessage(chatId, MessageKind::System,
                                  "Note: this contact does not appear to be running zkgram right now - the "
                                  "encrypted session may not complete until they do.",
                                  /*isSecret=*/true);
                }
            });
        });
    }
}

void MainWindow::appendMessage(qlonglong chatId, MessageKind kind, const QString& text, bool isSecret,
                                const QString& filePath, qlonglong messageId, const QString& senderName,
                                qint64 date, qint64 mediaAlbumId, bool isVoiceNote, int voiceDuration,
                                const QByteArray& voiceWaveform, qlonglong replyToMessageId) {
    // Same sender AND same kind AND same isSecret, not just same kind:
    // two different people's consecutive messages in a group chat must
    // not read as one attached block with only the first person's name
    // shown - and a plain message immediately followed by a secret one
    // (or vice versa) must not visually join into one bubble either, the
    // two are never really "the same conversation flow" the way two
    // consecutive plain (or two consecutive secret) messages are.
    const QVector<StoredMessage>& stored = conversationMessages_[chatId];
    bool attached = !stored.isEmpty() && stored.back().kind == kind && kind != MessageKind::System &&
                     stored.back().senderName == senderName && stored.back().isSecret == isSecret;
    // Stamped with the real time now when the caller does not already know
    // one (date == 0, the default - a genuinely-live encrypted send/status
    // line, which has no TDLib timestamp to speak of): isSecret messages
    // get cached (see cacheConversationHistory() below), and without a real
    // value here a cached-and-reloaded message would come back on the next
    // launch still reading as "now" instead of when it actually was sent
    // (see StoredMessage::date's own comment). A live plain message passes
    // its own real date through instead (see appendPlainMessageReceived()).
    if (date == 0) {
        date = QDateTime::currentDateTime().toSecsSinceEpoch();
    }
    conversationMessages_[chatId].push_back(StoredMessage{kind, text, filePath, messageId, senderName, isSecret, date,
                                                            mediaAlbumId, isVoiceNote, voiceDuration, voiceWaveform,
                                                            replyToMessageId});
    if (isSecret) {
        hasSecretContent_.insert(chatId);
        if (chatId == currentChatId_) {
            updateSecretModeIndicator();
        }
    }
    // Only actually drawn if this chat is the one on screen AND (it is
    // not secret, or secret mode is currently toggled on for it) - see
    // secretModeOn_'s own comment. Still stored above either way:
    // toggling secret mode on afterwards must show it without a refetch.
    bool visible = chatId == currentChatId_ && (!isSecret || secretModeOn_.value(chatId, false));
    if (visible) {
        // HistoryCanvas::appendItem() retracts the previous bubble's tail
        // in place if this one attaches to it, and lays out and paints
        // this new one - O(1) amortized, not a full conversation rebuild,
        // which matters for a long-running session with many messages.
        // Outgoing (you just sent this - a deliberate action, real
        // Telegram always jumps you to it regardless of where you were
        // scrolled) always scrolls; anything else (an incoming message,
        // a system/status line) only scrolls if the view was already at
        // the bottom - see isNearBottom()'s own comment for the bug this
        // fixes (any new message used to yank the view down out of
        // history you were actively scrolled up reading).
        bool shouldScrollToBottom = kind == MessageKind::Outgoing || asCanvas(messages_)->isNearBottom();
        HistoryEntry entry{kind, text, filePath, attached, senderName, true, messageId, date};
        entry.isVoiceNote = isVoiceNote;
        entry.voiceDuration = voiceDuration;
        entry.voiceWaveform = voiceWaveform;
        // Resolved here as well, not only in renderCurrentConversation():
        // this path draws the message immediately, and leaving the quote
        // empty meant a reply you just sent (or one arriving live) showed up
        // with no header at all until the chat was reopened and rebuilt.
        quoteForReply(chatId, replyToMessageId, &entry.replyToSender, &entry.replyToText);
        asCanvas(messages_)->appendItem(entry);
        if (shouldScrollToBottom) {
            asCanvas(messages_)->scrollToBottom();
        }
    }
    if (kind != MessageKind::System && isSecret) {
        cacheConversationHistory(chatId);
    }
}

void MainWindow::cacheConversationHistory(qlonglong chatId) {
    if (session_ == nullptr) {
        return;
    }
    const QVector<StoredMessage>& stored = conversationMessages_.value(chatId);
    std::vector<zkgram::core::PlainMessage> messages;
    messages.reserve(stored.size());
    for (const StoredMessage& message : stored) {
        // Only secret content belongs in core::LocalCache - plain
        // content is already recoverable from Telegram/TDLib itself, and
        // more importantly must never end up inside the encrypted-history
        // cache file at all (see the whole secretModeOn_ separation this
        // is part of).
        if (message.kind == MessageKind::System || !message.isSecret) {
            continue;
        }
        zkgram::core::PlainMessage plain;
        plain.id = message.messageId;
        plain.isOutgoing = message.kind == MessageKind::Outgoing;
        plain.text = message.text.toStdString();
        plain.photoPath = message.filePath.toStdString();
        plain.senderName = message.senderName.toStdString();
        messages.push_back(std::move(plain));
    }
    session_->cacheHistory(chatId, messages);
}

void MainWindow::updateHistoryPhoto(qlonglong chatId, qlonglong messageId, const QString& path) {
    QVector<StoredMessage>& messages = conversationMessages_[chatId];
    for (StoredMessage& message : messages) {
        if (message.messageId == messageId && message.messageId != 0) {
            message.filePath = path;
            break;
        }
    }
    if (chatId == currentChatId_) {
        scheduleConversationRender();
    }
}

void MainWindow::updateHistorySenderName(qlonglong chatId, qlonglong messageId, const QString& name) {
    QVector<StoredMessage>& messages = conversationMessages_[chatId];
    for (StoredMessage& message : messages) {
        if (message.messageId == messageId && message.messageId != 0) {
            message.senderName = name;
            break;
        }
    }
    if (chatId == currentChatId_) {
        scheduleConversationRender();
    }
}

void MainWindow::updateOutgoingReadUpTo(qlonglong chatId, qlonglong messageId) {
    qlonglong& known = conversationReadOutboxUpTo_[chatId];
    // Monotonic: TDLib can re-send an older value while re-syncing, and a
    // message that has been read does not become unread again.
    if (messageId <= known) {
        return;
    }
    known = messageId;
    if (chatId == currentChatId_) {
        // Coalesced, not immediate: a chat catching up after being opened on
        // the other device pushes a run of these, and each one would
        // otherwise rebuild the whole conversation just to swap some ticks.
        scheduleConversationRender();
    }
}

void MainWindow::scheduleConversationRender() {
    pendingConversationRenderChatId_ = currentChatId_;
    // Same shape as updateChatList()'s sidebar throttle: if a render is
    // already pending it will pick up this change too (the render always
    // reads the current conversationMessages_), so there is nothing to
    // restart or extend - which also means a long burst still renders
    // every ~120ms rather than being starved until it ends.
    if (!conversationRenderThrottleTimer_->isActive()) {
        conversationRenderThrottleTimer_->start(120);
    }
}

void MainWindow::renderCurrentConversation() {
    const QVector<StoredMessage>& stored = conversationMessages_.value(currentChatId_);

    // The one place secretModeOn_ actually gets enforced for a full
    // rebuild (appendMessage()'s own single-item path enforces it
    // separately, see there) - a secret message is included only if
    // secret mode is currently toggled on for this chat, never based on
    // whether a session happens to be active/ready. Filtered into its
    // own vector first, then the existing attached/showTail two-pass
    // runs over *that*, not the unfiltered list - grouping must only
    // ever look at what is actually about to be drawn, or a hidden
    // secret message sitting between two plain ones would wrongly break
    // (or wrongly preserve) their attachment.
    bool secretVisible = secretModeOn_.value(currentChatId_, false);
    QVector<StoredMessage> conversation;
    conversation.reserve(stored.size());
    for (const StoredMessage& message : stored) {
        if (!message.isSecret || secretVisible) {
            conversation.push_back(message);
        }
    }

    // Two passes, not one: whether a bubble is "attached to previous"
    // (joins its top corner, computed by looking backward) is independent
    // from whether it is "last in its group" (gets the tail, computed by
    // looking forward at whether the NEXT message continues its group) -
    // both are needed at once, computed up front rather than patching an
    // already-built earlier entry mid-loop the way appendItem() does for
    // a single incoming message.
    QVector<bool> attached(conversation.size(), false);
    for (int i = 1; i < conversation.size(); ++i) {
        attached[i] = conversation[i].kind == conversation[i - 1].kind && conversation[i].kind != MessageKind::System &&
                      conversation[i].senderName == conversation[i - 1].senderName &&
                      conversation[i].isSecret == conversation[i - 1].isSecret;
    }

    // Quote lookup for the reply headers below, built from `conversation`
    // (what is actually about to be drawn) rather than from `stored`: a
    // reply pointing at a secret message must not surface that message's
    // text as a quote on a plain bubble while secret mode is off. A reply
    // whose target is not here - filtered out, or simply older than any
    // page loaded so far - just gets no header, and picks one up on a later
    // render if that page is fetched.
    QHash<qlonglong, const StoredMessage*> byMessageId;
    byMessageId.reserve(conversation.size());
    for (const StoredMessage& message : conversation) {
        if (message.messageId != 0) {
            byMessageId.insert(message.messageId, &message);
        }
    }
    // Everything at or below this id has been read by the other side, see
    // updateOutgoingReadUpTo(). 0 means "nothing known yet", which leaves
    // every outgoing message on the plain single (delivered) tick.
    const qlonglong readUpTo = conversationReadOutboxUpTo_.value(currentChatId_, 0);

    // Fills in the parts of an entry that depend on the rest of the
    // conversation rather than on the message alone.
    auto resolveEntry = [&](HistoryEntry& entry, const StoredMessage& message) {
        if (message.replyToMessageId != 0) {
            auto quoted = byMessageId.constFind(message.replyToMessageId);
            if (quoted != byMessageId.constEnd()) {
                // Index for the lookup, shared helper for what to show - see
                // quoteFromMessage().
                quoteFromMessage(*quoted.value(), &entry.replyToSender, &entry.replyToText);
            }
        }
        // Only a real TDLib id can be compared against the server's read
        // marker. A CryptoLayer message has none (id 0), so it stays on the
        // single tick rather than claiming a read state nobody reported.
        entry.readByPeer = message.kind == MessageKind::Outgoing && message.messageId != 0 && readUpTo != 0 &&
                            message.messageId <= readUpTo;
    };

    QVector<HistoryEntry> entries;
    entries.reserve(conversation.size());
    // Telegram delivers every photo of an album as its own separate
    // message, sharing one non-zero media_album_id - grouping consecutive
    // messages that share one into a single bubble (a mosaic, see
    // paintItem()'s Ui::LayoutMediaGroup use) is entirely the client's own
    // job, not something TDLib does for you. A run of length 1 (album id
    // set but somehow only one member ever arrived/loaded) falls back to
    // the ordinary single-image path below instead of a one-photo
    // "mosaic".
    for (int i = 0; i < conversation.size();) {
        qint64 albumId = conversation[i].mediaAlbumId;
        if (albumId != 0) {
            int j = i;
            QVector<QString> albumPaths;
            while (j < conversation.size() && conversation[j].mediaAlbumId == albumId) {
                if (!conversation[j].filePath.isEmpty()) {
                    albumPaths.push_back(conversation[j].filePath);
                }
                ++j;
            }
            if (albumPaths.size() > 1) {
                bool showTail = (j == conversation.size()) || !attached[j];
                // The caption (if any) lives on exactly one message of the
                // group in real Telegram - render it once, whichever
                // member actually has it, not once per photo.
                QString caption;
                for (int k = i; k < j; ++k) {
                    if (!conversation[k].text.isEmpty()) {
                        caption = conversation[k].text;
                        break;
                    }
                }
                HistoryEntry entry{conversation[i].kind, caption,   QString(), attached[i], conversation[i].senderName,
                                    showTail,             conversation[i].messageId, conversation[i].date};
                entry.albumFilePaths = albumPaths;
                resolveEntry(entry, conversation[i]);
                entries.push_back(entry);
                i = j;
                continue;
            }
        }
        bool showTail = (i == conversation.size() - 1) || !attached[i + 1];
        HistoryEntry entry{conversation[i].kind,       conversation[i].text,      conversation[i].filePath,
                            attached[i],                conversation[i].senderName, showTail,
                            conversation[i].messageId, conversation[i].date};
        entry.isVoiceNote = conversation[i].isVoiceNote;
        entry.voiceDuration = conversation[i].voiceDuration;
        entry.voiceWaveform = conversation[i].voiceWaveform;
        resolveEntry(entry, conversation[i]);
        entries.push_back(entry);
        ++i;
    }
    asCanvas(messages_)->setItems(entries);
    asCanvas(messages_)->scrollToBottom();
    // setItems() above resets HistoryCanvas's own loadingMore_ flag along
    // with the rest of its per-conversation state (see setLoadingMore()'s
    // own comment) - re-synced here in case a page fetch for this exact
    // chat was already in flight (started before this rebuild, e.g. this
    // function ran because a live message arrived while scrolled up) so
    // the indicator does not just vanish out from under a request that is
    // still genuinely pending.
    asCanvas(messages_)->setLoadingMore(historyLoadingMore_.contains(currentChatId_));
}

void MainWindow::appendConversationStatus(qlonglong chatId, const QString& stage, const QString& message) {
    appendMessage(chatId, MessageKind::System, QString("[%1] %2").arg(stage, message), /*isSecret=*/true);
}

void MainWindow::appendConversationPeerText(qlonglong chatId, const QString& text) {
    appendMessage(chatId, MessageKind::Incoming, text, /*isSecret=*/true);
}

void MainWindow::appendConversationFileReceived(qlonglong chatId, const QString& filePath) {
    appendMessage(chatId, MessageKind::Incoming, "File: " + filePath, /*isSecret=*/true, filePath);
}

// Matches TDLib's echo of one of our own sends against the optimistic copy
// appendMessage() already drew for it, and fills in the real message id (and
// the server's own timestamp) on that copy. Returns false when nothing here
// matches - see the caller for what that means.
//
// Matching is by content, not by a pending-send queue: TDLib gives a message
// a temporary id on send and swaps it for the permanent one later
// (updateMessageSendSucceeded), and this code deliberately does not track
// that handshake - it only ever sees the finished message. Text equality
// covers a plain send; a file send's optimistic copy holds "<label><path>"
// as its text while the echo carries TDLib's own preview ("Photo", or the
// document's file name), so those match on the file name instead. Oldest
// unmatched copy first, so sending the same text twice fills the two in the
// order they were sent.
bool MainWindow::backfillOutgoingMessageId(qlonglong chatId, qlonglong messageId, const QString& text,
                                            const QString& filePath, qint64 date) {
    if (messageId == 0) {
        return false;
    }
    QVector<StoredMessage>& stored = conversationMessages_[chatId];
    QString echoFileName = filePath.isEmpty() ? QString() : QFileInfo(filePath).fileName();
    for (StoredMessage& candidate : stored) {
        if (candidate.kind != MessageKind::Outgoing || candidate.messageId != 0 || candidate.isSecret) {
            continue;
        }
        QString candidateFileName =
            candidate.filePath.isEmpty() ? QString() : QFileInfo(candidate.filePath).fileName();
        bool sameText = !text.isEmpty() && candidate.text == text;
        bool sameFile = !candidateFileName.isEmpty() &&
                         (candidateFileName == echoFileName || candidateFileName == text);
        if (!sameText && !sameFile) {
            continue;
        }
        candidate.messageId = messageId;
        if (date != 0) {
            // The server's time, not the moment this device happened to
            // call send - the difference is what a reloaded history would
            // show later, and the two must not disagree.
            candidate.date = date;
        }
        seenMessageIds_[chatId].insert(messageId);
        if (chatId == currentChatId_) {
            // The drawn item carries the id too (it is what a right-click or
            // a swipe reads to reply to this message), and a message already
            // on screen quoting this one can only resolve its header once
            // the id is known - both come out of a rebuild, coalesced
            // because a burst of sends produces a burst of echoes.
            scheduleConversationRender();
        }
        return true;
    }
    return false;
}

void MainWindow::appendPlainMessageReceived(qlonglong chatId, qlonglong messageId, const QString& text,
                                             const QString& senderName, const QString& filePath, bool isOutgoing,
                                             qlonglong date, qlonglong mediaAlbumId, qlonglong replyToMessageId,
                                             bool isVoiceNote, int voiceDuration, const QString& voicePath,
                                             const QByteArray& voiceWaveform) {
    // Only for chats whose history has already been asked for, i.e. ones
    // the user has opened at least once this run. For any other chat the
    // message is deliberately dropped: its conversation holds nothing yet,
    // and the first thing opening it does is fetch the newest page of
    // history, which contains this message anyway - keeping it here instead
    // would put one lone new message above the whole history that follows.
    if (!historyLoadedChats_.contains(chatId)) {
        return;
    }
    // TDLib's own echo of a message THIS device just sent. onSendClicked()/
    // sendFilePath() already appended an optimistic MessageKind::Outgoing
    // copy the moment the user hit send (messageId 0), so drawing this echo
    // too would show every plain outgoing send twice.
    //
    // The echo used to be dropped outright, which cost more than it looked
    // like: it is the ONLY place the real TDLib id of our own message ever
    // arrives, so without it that message stayed id 0 for the rest of the
    // session, and everything keyed by message id quietly did nothing for
    // it. Replying to it sent no reply link at all (TDLib reads reply-to
    // id 0 as "not a reply", so the other side just saw a plain message);
    // an incoming reply TO it had nothing to resolve its quote against, so
    // it rendered with no header; and the read marker
    // (updateOutgoingReadUpTo, which compares ids) could never reach it, so
    // it kept a single tick no matter what the other side did. All three
    // were reported together, and all three are the same missing id.
    if (isOutgoing) {
        backfillOutgoingMessageId(chatId, messageId, text, filePath, date);
        // Either way the echo itself is not drawn. An unmatched one is not
        // necessarily a message sent from another device (which would be
        // worth showing): in a chat with an active encrypted session every
        // send also goes out as an ordinary Telegram message carrying
        // CryptoLayer's ciphertext, and so does the presence probe - both
        // come back here as outgoing echoes with nothing to match, and
        // drawing them would put raw ciphertext on screen as if it were a
        // message. Telling those apart from a genuine other-device send
        // needs more than this function knows.
        return;
    }
    if (messageId != 0) {
        QSet<qlonglong>& seen = seenMessageIds_[chatId];
        if (seen.contains(messageId)) {
            // Already shown, from the history page this raced - see
            // maybeLoadPlainHistory().
            return;
        }
        seen.insert(messageId);
    }
    // filePath carries photoPath here (empty for a voice note); voicePath
    // is what StoredMessage::filePath actually needs to hold for one - see
    // its own comment for why the two are stored in the same field.
    appendMessage(chatId, MessageKind::Incoming, text, /*isSecret=*/false, isVoiceNote ? voicePath : filePath,
                  messageId, senderName, date, mediaAlbumId, isVoiceNote, voiceDuration, voiceWaveform,
                  replyToMessageId);
}

void MainWindow::setConversationReady(qlonglong chatId) {
    conversationActive_[chatId] = true;
    conversationReady_[chatId] = true;
    appendMessage(chatId, MessageKind::System, "Encrypted session ready", /*isSecret=*/true);
    // Auto-reveal, set AFTER the append above (not before): flipping it
    // first would make that one message paint once via appendMessage()'s
    // own live-append path and then again via the renderCurrentConversation()
    // rebuild below - set after, and the rebuild is the only paint,
    // picking up this message and any other secret content this chat
    // had accumulated while still hidden. The user just deliberately
    // finished setting up this exact session (clicked "Start encrypted
    // session" themselves, waited through the handshake) - showing what
    // they just built without an extra click is the expected outcome of
    // that action, not an exception to secretModeOn_'s "off by default"
    // rule. A chat's *cached* secret history from a previous run gets no
    // such free pass (see maybeLoadCachedHistory()) - only a session
    // actually completed in this run does.
    secretModeOn_[chatId] = true;
    if (chatId == currentChatId_) {
        renderCurrentConversation();
        updateSecretModeIndicator();
    }
    updateConversationControlsVisibility();
}

void MainWindow::showMessageContextMenu(const QPoint& pos) {
    HistoryCanvas::HitResult hit = asCanvas(messages_)->itemAt(pos);
    if (!hit.valid) {
        return;
    }
    MessageKind kind = hit.kind;
    if (kind == MessageKind::System) {
        return;
    }
    // A mouse selection (see HistoryCanvas::selectedText()) takes
    // priority over the whole message, same as right-clicking selected
    // text in any real text view - falls back to the whole message when
    // nothing is actually selected (the common case: right-click without
    // dragging first).
    QString selected = asCanvas(messages_)->selectedText();
    QString text = selected.isEmpty() ? hit.text : selected;

    // Phase 6b (UI.md 13c): Ui::PopupMenu (real rounded dark/light card),
    // not QMenu's plain OS menu. deleteOnHide defaults true, so the raw
    // new/no-delete here matches real tdesktop's own usage pattern - the
    // menu deletes itself once dismissed.
    auto* menu = new Ui::PopupMenu(this);
    menu->addAction("Copy text", [text] { QGuiApplication::clipboard()->setText(text); });
    // Real Telegram-style editing (TDLib editMessageText, keyed by message
    // id) is not something crypto::CryptoLayer supports: it is a stream
    // session per companion with no message-id concept. "Edit" here is a
    // local convenience only: it does not change what the companion
    // already received, it just refills the input field so the corrected
    // text can be sent as a new message.
    if (kind == MessageKind::Outgoing) {
        menu->addAction("Edit", [this, text] {
            input_->setPlainText(text);
            input_->setFocus();
        });
    }
    // "Reply" shows the quoted-preview bar (real Ui::MessageBar). When
    // hit.messageId is a real TDLib id (a plain, non-encrypted message),
    // onSendClicked() links the next send to it via TDLib's own reply
    // mechanism; a crypto::CryptoLayer message has no message-id concept
    // to link against (hit.messageId 0 there), so it still falls back to
    // quoting the text into the sent message - see replyToMessageId_'s own
    // comment.
    // Same action the swipe gesture performs (see
    // HistoryCanvas::onReplyRequested) - both go through startReply().
    menu->addAction("Reply", [this, text, kind, messageId = hit.messageId] {
        startReply(replyAuthorLabel(kind), text, messageId);
    });
    // "Forward" asks where to send it, like any messenger, instead of the
    // old behaviour of quietly resending into the chat already open.
    menu->addAction("Forward", [this, text, kind] {
        // Same deferral as the attach menu, for the same reason - see
        // runAfterMenuClosed(). pickForwardTarget() is a modal QDialog.
        runAfterMenuClosed([this, text, kind] {
            qlonglong target = pickForwardTarget();
            if (target != 0) {
                forwardTextTo(target, text, replyAuthorLabel(kind));
            }
        });
    });
    // Enters the same multi-message selection mode a long-press on this
    // bubble would - see HistoryCanvas::enterSelectionMode()'s own comment.
    // The menu-driven entry point exists because a long-press has no
    // discoverable affordance on a plain desktop mouse the way it does on
    // touch, where real Telegram's own equivalent lives.
    menu->addAction("Select", [this, index = hit.index] { asCanvas(messages_)->enterSelectionMode(index); });
    menu->popup(asCanvas(messages_)->mapToGlobal(pos));
}

bool MainWindow::showConfirmSignaturesDialog(qlonglong chatId, const QString& mySign, const QString& companionSign) {
    QString title = conversationTitles_.value(chatId, QString::number(chatId));
    QString message =
        "Chat: " + title + "\nMy signature: " + mySign + "\nCompanion signature: " + companionSign + "\n\nDo they match?";
    auto answer = QMessageBox::question(this, "Confirm companion signature", message);
    return answer == QMessageBox::Yes;
}

QString MainWindow::showRequestCredentialDialog(const QString& prompt, const QString& dataType) {
    (void)dataType;
    logDebug("showRequestCredentialDialog: entered, prompt=" + prompt);

    // Reused for phone number, login code, and 2FA password prompts (see
    // TelegramClient::requestAuthInput / CryptoLayer's request_data) - same
    // full-window slide as runStartupWizard(), styled like tdesktop's own
    // intro steps instead of a popup dialog. Copy matches tdesktop's actual
    // strings (Telegram/Resources/langs/lang.strings: lng_phone_title,
    // lng_code_desc) where there is a direct equivalent. Only mask input
    // that is actually a password: hiding a phone number or SMS code as
    // the user types it is just bad UX, not extra security (there is no
    // shoulder-surfing threat model here that a phone number needs hiding
    // from).
    // Промпт может нести причину предыдущей неудачи, отделённую переводом
    // строки (см. promptWithError в telegram_client.cpp): первая строка -
    // сам шаг, по ней и выбирается слайд, остальное - текст ошибки от
    // TDLib. Разбирать нужно именно так, а не искать ключевые слова во всём
    // промпте: сообщение вроде PASSWORD_HASH_INVALID содержит "password" и
    // увело бы шаг ввода кода на слайд двухфакторного пароля.
    QString step = prompt.section('\n', 0, 0);
    QString lastError = prompt.section('\n', 1).trimmed();

    bool isPassword = step.contains("password", Qt::CaseInsensitive);
    bool isCode = step.contains("code", Qt::CaseInsensitive);
    QString title = "Your phone number";
    QString subtitle = "Please confirm your country code\nand enter your phone number.";
    QString placeholder = "Phone number";
    AuthSlide::Validator validator;
    if (isCode) {
        title = "Enter code";
        subtitle = "We've sent an activation code to your phone.\nPlease enter it below.";
        placeholder = "Code";
    } else if (isPassword) {
        title = "Two-Step Verification";
        subtitle = "You have enabled Two-Step Verification,\nso your account is protected with an additional password.";
        placeholder = "Password";
    } else {
        // Phone step: require a leading "+" and 7-15 digits after it
        // (loose E.164 shape) - catches the common mistake of typing a
        // local number without a country code before it ever reaches
        // TDLib, instead of a "phone number not found" failure later.
        validator = [](const QString& value) -> QString {
            static const QRegularExpression phonePattern("^\\+[1-9][0-9\\s]{6,15}$");
            if (!phonePattern.match(value).hasMatch()) {
                return "Enter a valid phone number with a country code, e.g. +14155552671";
            }
            return QString();
        };
    }

    centralStack_->setCurrentWidget(authSlide_);
    logDebug("showRequestCredentialDialog: calling runSlide");
    QString result = authSlide_->runSlide(title, subtitle, placeholder, isPassword, validator, lastError, isCode);
    logDebug("showRequestCredentialDialog: runSlide returned, result.length()=" + QString::number(result.length()));
    return result;
}

void MainWindow::onSendOrMicClicked() {
    if (recorder_->recorderState() == QMediaRecorder::RecordingState) {
        onMicClicked();  // stops and sends the recording, see onRecorderStateChanged()
        return;
    }
    if (input_->toPlainText().trimmed().isEmpty()) {
        onMicClicked();  // starts recording
    } else {
        onSendClicked();
    }
}

void MainWindow::onSendClicked() {
    if (session_ == nullptr || currentChatId_ == 0) {
        return;
    }
    QString text = input_->toPlainText().trimmed();
    if (text.isEmpty()) {
        return;
    }
    // No active encrypted session for this chat -> send as a real,
    // unencrypted Telegram message instead (see
    // updateConversationControlsVisibility()/Session::sendPlainText()) -
    // the only alternative used to be no way to send anything at all
    // until the other side also started running zkgram.
    bool isActive = conversationActive_.value(currentChatId_, false);
    // A plain send with a real TDLib message id to reply to gets a true
    // reply link instead of quoted text - see replyToMessageId_'s own
    // comment. Encrypted sends (session_->sendText) have no equivalent yet
    // (see TODO.md), so they still fall back to the same text-quoting
    // convenience as before.
    qlonglong replyId = (!isActive) ? replyToMessageId_ : 0;
    if (isActive && !replyToText_.isEmpty()) {
        text = "> " + replyToText_.replace("\n", "\n> ") + "\n" + text;
    }
    if (!replyToText_.isEmpty() || replyToMessageId_ != 0) {
        cancelReply();
    }
    try {
        if (isActive) {
            session_->sendText(currentChatId_, text.toStdString());
        } else {
            session_->sendPlainText(currentChatId_, text.toStdString(), replyId);
        }
    } catch (const std::exception& e) {
        appendMessage(currentChatId_, MessageKind::System, QString("Send failed: %1").arg(e.what()), isActive);
        return;
    }
    // isSecret mirrors isActive exactly - what actually left this device
    // (encrypted via sendText, or plain via sendPlainText) is what
    // determines which mode this message belongs to, not anything about
    // the UI's current secretModeOn_ state.
    //
    // replyId is carried into the local copy as well, so the reply header
    // appears on your own message straight away rather than only after the
    // chat is reopened and the history reloaded (this optimistic copy is
    // all the user sees until then - TDLib's echo of our own send is
    // deliberately dropped, see appendPlainMessageReceived).
    appendMessage(currentChatId_, MessageKind::Outgoing, text, isActive, QString(), /*messageId=*/0, QString(),
                   /*date=*/0, /*mediaAlbumId=*/0, /*isVoiceNote=*/false, /*voiceDuration=*/0,
                   /*voiceWaveform=*/QByteArray(), replyId);
    input_->clear();
}

void MainWindow::sendFilePath(const QString& filePath, const QString& label) {
    if (session_ == nullptr || currentChatId_ == 0) {
        return;
    }
    bool isActive = conversationActive_.value(currentChatId_, false);
    try {
        if (isActive) {
            session_->sendFile(currentChatId_, filePath.toStdString());
        } else {
            session_->sendPlainFile(currentChatId_, filePath.toStdString());
        }
    } catch (const std::exception& e) {
        appendMessage(currentChatId_, MessageKind::System, QString("Send failed: %1").arg(e.what()), isActive);
        return;
    }
    appendMessage(currentChatId_, MessageKind::Outgoing, label + filePath, isActive, filePath);
}

// Runs something after the popup menu that asked for it has closed.
//
// A menu action's callback fires while Ui::PopupMenu is still up and still
// holding Qt's popup mouse/keyboard grab - verified here by watching the X
// window list: the menu window is still mapped while the file dialog is on
// screen. Opening a modal from inside that callback therefore spins a nested
// event loop underneath an active grab. With Qt's own widget dialog that
// merely looks wrong (the menu hangs around behind it); with a NATIVE file
// dialog - xdg-desktop-portal or GTK, a window belonging to another process,
// which is what a normal Linux desktop gives you - the grab means the dialog
// never receives input at all: the app dims and nothing happens, which is
// exactly what was reported. Real Telegram Desktop never opens a modal
// straight out of a menu action either (base::call_delayed/Ui::PostponeCall
// around every such call site).
//
// A queued call is enough: the menu hides and releases the grab as the
// action finishes, before control returns to the event loop.
void MainWindow::runAfterMenuClosed(std::function<void()> action) {
    QMetaObject::invokeMethod(this, std::move(action), Qt::QueuedConnection);
}

void MainWindow::onSendFileClicked() {
    // Меню над скрепкой вместо сразу открывающегося файлового диалога -
    // как в Telegram, где скрепка сначала спрашивает тип вложения.
    //
    // Только два пункта из телеграмовских пяти: Gift Premium, Checklist и
    // Wallet - это функции самой платформы Telegram, к которым у zkgram
    // нет ни доступа, ни смысла (сообщения тут шифруются до попадания в
    // Telegram). Заводить пункты-заглушки, которые ничего не делают, хуже,
    // чем не показывать их вовсе.
    //
    // Разделение влияет не только на фильтр в диалоге: telegram_client.cpp
    // выбирает inputMessagePhoto или inputMessageDocument по расширению
    // (см. looksLikeImagePath), поэтому выбранная через «Photo or Video»
    // картинка уйдёт именно фотографией, с превью на обеих сторонах.
    // Phase 6b (UI.md 13c): Ui::PopupMenu, real icons (zkgramIconPhoto/
    // Document, the same st::-declared icons the buttons use) instead of
    // QMenu's QIcon-from-pixmap conversion.
    auto* menu = new Ui::PopupMenu(this);
    menu->addAction(
        "Photo or Video",
        [this] {
            runAfterMenuClosed([this] {
                QString filePath = QFileDialog::getOpenFileName(
                    this, "Send photo or video", QString(),
                    "Photos and videos (*.jpg *.jpeg *.png *.gif *.webp *.bmp *.mp4 *.mov *.mkv *.avi *.webm);;"
                    "All files (*)");
                if (!filePath.isEmpty()) {
                    sendFilePath(filePath, "File: ");
                }
            });
        },
        &st::zkgramIconPhoto);
    menu->addAction(
        "Document",
        [this] {
            runAfterMenuClosed([this] {
                QString filePath = QFileDialog::getOpenFileName(this, "Send document");
                if (!filePath.isEmpty()) {
                    sendFilePath(filePath, "File: ");
                }
            });
        },
        &st::zkgramIconDocument);

    // Меню раскрывается вверх от кнопки, а не вниз: скрепка стоит в самом
    // низу окна, и вниз ему просто некуда развернуться.
    QPoint anchor = sendFileButton_->mapToGlobal(QPoint(0, 0));
    menu->popup(QPoint(anchor.x(), anchor.y() - menu->sizeHint().height()));
}

void MainWindow::onMicClicked() {
    if (recorder_->recorderState() == QMediaRecorder::RecordingState) {
        onSendRecordingClicked();
        return;
    }

    QString dir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    voiceFilePath_ = dir + "/zkgram-voice-" + QDateTime::currentDateTime().toString("yyyyMMdd-hhmmss") + ".m4a";
    recordCancelled_ = false;
    recorder_->setOutputLocation(QUrl::fromLocalFile(voiceFilePath_));
    recorder_->record();
}

void MainWindow::onCancelRecordingClicked() {
    // Файл дописывается уже после stop(), в StoppedState, поэтому просто
    // не отправить его здесь нельзя - надо пометить запись отменённой и
    // удалить файл в onRecorderStateChanged(), когда он готов.
    recordCancelled_ = true;
    recorder_->stop();
}

void MainWindow::onPauseRecordingClicked() {
    if (recorder_->recorderState() == QMediaRecorder::PausedState) {
        recorder_->record();
        return;
    }
    if (recorder_->recorderState() == QMediaRecorder::RecordingState) {
        recorder_->pause();
    }
}

void MainWindow::onSendRecordingClicked() {
    recordCancelled_ = false;
    recorder_->stop();
}

void MainWindow::onRecordingTick() {
    // Мигание красной точки, как в Telegram: полсекунды видно, полсекунды
    // нет. На паузе точка гасится и не мигает - иначе пауза визуально не
    // отличалась бы от активной записи.
    bool paused = recorder_->recorderState() == QMediaRecorder::PausedState;
    if (paused) {
        recordDot_->setVisible(false);
    } else {
        int phase = static_cast<int>((recordAccumulatedMs_ + recordElapsed_.elapsed()) / 500) % 2;
        bool visible = phase == 0;
        if (visible != recordDotVisible_) {
            recordDotVisible_ = visible;
            recordDot_->setVisible(visible);
        }
    }

    qint64 totalMs = recordAccumulatedMs_ + (paused ? 0 : recordElapsed_.elapsed());
    qint64 minutes = totalMs / 60000;
    qint64 seconds = (totalMs / 1000) % 60;
    qint64 centis = (totalMs % 1000) / 10;
    recordTime_->setText(
        QString("%1:%2,%3").arg(minutes).arg(seconds, 2, 10, QChar('0')).arg(centis, 2, 10, QChar('0')));
}

void MainWindow::onRecorderStateChanged() {
    QMediaRecorder::RecorderState state = recorder_->recorderState();
    bool recording = state == QMediaRecorder::RecordingState;
    bool paused = state == QMediaRecorder::PausedState;
    bool active = recording || paused;

    // Панель записи показывается вместо строки ввода, а не вдобавок к ней:
    // иначе окно подросло бы на 54 пикселя ровно на время записи и вся
    // история дёрнулась бы вверх и обратно.
    recordBar_->setVisible(active);
    composeRow_->setVisible(!active);

    if (recording) {
        // Продолжение после паузы: накопленное время уже сложено в
        // recordAccumulatedMs_ ниже, отсчёт стартует заново с нуля.
        recordElapsed_.restart();
        if (!recordTimer_->isActive()) {
            recordTimer_->start();
        }
        pauseRecordButton_->setIconOverride(nullptr);  // back to the style's default (pause) icon
        pauseRecordButton_->setToolTip("Pause recording");
    } else if (paused) {
        recordAccumulatedMs_ += recordElapsed_.elapsed();
        pauseRecordButton_->setIconOverride(&st::zkgramPlayButtonIcon, &st::zkgramPlayButtonIconOver);
        pauseRecordButton_->setToolTip("Resume recording");
    } else {
        recordTimer_->stop();
        recordAccumulatedMs_ = 0;
        recordDotVisible_ = true;
        recordDot_->setVisible(true);
        recordTime_->setText("0:00,00");
        // Back to whatever sendButton_ should show for the field's
        // current text (mic if still empty, send if not) - not
        // hardcoded back to the mic icon, in case text was typed while
        // recording was in progress.
        updateSendButtonIcon();
    }

    // The recorder only finishes writing the output file once it has fully
    // transitioned to StoppedState, not at the moment stop() was called, so
    // the actual send happens here rather than in onMicClicked().
    if (!active && !voiceFilePath_.isEmpty()) {
        QString path = voiceFilePath_;
        voiceFilePath_.clear();
        if (recordCancelled_) {
            recordCancelled_ = false;
            QFile::remove(path);  // отменённая запись не должна оставаться во временной папке
            return;
        }
        sendFilePath(path, "Voice message: ");
    }
}

void MainWindow::onVoicePlayToggleRequested(qlonglong messageId, const QString& path) {
    if (path.isEmpty()) {
        return;  // audio still downloading - see TelegramClient::onHistoryVoiceReady
    }
    bool isSameNote = messageId != 0 && messageId == playingVoiceMessageId_;
    if (isSameNote && voicePlayer_->playbackState() == QMediaPlayer::PlayingState) {
        voicePlayer_->pause();
        return;
    }
    if (isSameNote && voicePlayer_->playbackState() == QMediaPlayer::PausedState) {
        voicePlayer_->play();
        return;
    }
    // A different voice note (or nothing) was loaded - real Telegram only
    // ever plays one voice note at a time, switching stops whatever was
    // playing before instead of overlapping the two.
    playingVoiceMessageId_ = messageId;
    voicePlayer_->setSource(QUrl::fromLocalFile(path));
    voicePlayer_->play();
}

void MainWindow::onVoicePlaybackStateChanged() {
    if (playingVoiceMessageId_ == 0) {
        return;
    }
    bool playing = voicePlayer_->playbackState() == QMediaPlayer::PlayingState;
    asCanvas(messages_)->setVoicePlaybackState(playingVoiceMessageId_, playing, voicePlayer_->position(),
                                                voicePlayer_->duration());
}

void MainWindow::updateSendButtonIcon() {
    // real Telegram Desktop's single send/mic button (see sendButton_'s
    // own header comment) - icon depends only on whether there is text
    // to send, colors match historySendIconFg (#40a7e3, the active/blue
    // state) and historyComposeIconFg (#999999, the same gray the attach
    // button uses) resolved from telegram_fork/tdesktop's own
    // Resources/day-blue.tdesktop-theme.
    if (recorder_->recorderState() == QMediaRecorder::RecordingState) {
        return;  // onRecorderStateChanged() owns the icon while actively recording
    }
    bool hasText = !input_->toPlainText().trimmed().isEmpty();
    sendButton_->setIconOverride(hasText ? &st::zkgramSendButtonIcon : nullptr,
                                  hasText ? &st::zkgramSendButtonIconOver : nullptr);
    sendButton_->setToolTip(hasText ? "Send" : "Record a voice message");
}

void MainWindow::onSearchTextChanged(const QString& query) {
    // Hides non-matching rows in place rather than rebuilding the list -
    // this is filtering the currently open conversation, not a real
    // cross-chat search (see the sidebar for that).
    asCanvas(messages_)->setSearchFilter(query);
}

}  // namespace zkgram::ui::qt
