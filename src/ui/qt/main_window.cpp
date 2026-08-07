#include "ui/qt/main_window.hpp"

#include "ui/qt/icons.hpp"

#include <algorithm>
#include <cmath>
#include <exception>
#include <memory>

#include <QAbstractScrollArea>
#include <QEasingCurve>
#include <QMouseEvent>
#include <QScroller>
#include <QScrollerProperties>
#include <QVariantAnimation>
#include <QWheelEvent>
#include <QAction>
#include <QAudioInput>
#include <QClipboard>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QDateTime>
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
#include <QLabel>
#include <QLineEdit>
#include <QBrush>
#include <QLinearGradient>
#include <QPalette>
#include <QMediaCaptureSession>
#include <QMediaFormat>
#include <QMediaRecorder>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPushButton>
#include <QScrollBar>
#include <QRegularExpression>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStyle>
#include <QAbstractTextDocumentLayout>
#include <QTextBlock>
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

void logDebug(const QString& message) {
    QFile file(QCoreApplication::applicationDirPath() + "/zkgram_debug.log");
    if (file.open(QIODevice::Append | QIODevice::Text)) {
        QTextStream(&file) << QDateTime::currentDateTime().toString("HH:mm:ss.zzz") << " ["
                            << QThread::currentThread() << "] " << message << "\n";
    }
}

QString loadStylesheet() {
    QFile file(":/style.qss");
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return QString();
    }
    return QTextStream(&file).readAll();
}

// Every button/menu glyph the window draws now comes from icons.cpp (one
// shared 24x24 grid, one stroke weight, re-rendered as vectors at whatever
// size and device pixel ratio Qt asks for) instead of a drawXIcon() helper
// per glyph baking its own 24x24 pixmap right here. These are the colors
// they are tinted with, taken from tdesktop's day-blue theme rather than
// repeated as bare hex literals at every call site the way they used to be.
const QColor kIconFg("#999999");         // menuIconFg: idle composer/menu glyph
const QColor kIconAccentFg("#40a7e3");   // windowBgActive: the send arrow
const QColor kIconDestructiveFg("#e41e3f");  // attentionButtonFg: discard a recording
const QColor kIconEncryptedFg("#4fae4e");    // the same green as statusDot_ for an encrypted session

// Colored circle with the chat's first letter, same idea as tdesktop's own
// userpic placeholder for a peer with no profile photo (Telegram/
// SourceFiles/ui/empty_userpic.cpp: a solid color picked deterministically
// from the peer, plus the first letter of the name) - used as a fallback
// when the chat has no photo yet, or it has not finished downloading (see
// chatAvatarPixmap() below for the real-photo path).
QPixmap avatarPixmap(const QString& title, int size) {
    static const QVector<QColor> kAvatarColors = {
        QColor("#e17076"), QColor("#eda86c"), QColor("#a695e7"), QColor("#7bc862"),
        QColor("#6ec9cb"), QColor("#65aadd"), QColor("#ee7aae"),
    };
    QString trimmed = title.trimmed();
    QChar initial = trimmed.isEmpty() ? QChar('?') : trimmed.at(0).toUpper();
    int colorIndex = trimmed.isEmpty() ? 0 : static_cast<int>(qHash(trimmed) % static_cast<uint>(kAvatarColors.size()));

    QPixmap pixmap(size, size);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    // Vertical two-stop gradient rather than one flat fill: real Telegram's
    // placeholder userpics have been gradients for several versions now
    // (Ui::EmptyUserpic paints a QLinearGradient between a lighter top and a
    // deeper bottom shade of the peer's color), and against a white chat
    // list a flat disc of saturated color reads as a sticker pasted on top.
    // Derived from the one palette entry instead of adding a second table of
    // hand-picked stops.
    const QColor base = kAvatarColors[colorIndex];
    QLinearGradient fill(0, 0, 0, size);
    fill.setColorAt(0.0, base.lighter(112));
    fill.setColorAt(1.0, base.darker(108));
    painter.setPen(Qt::NoPen);
    painter.setBrush(fill);
    painter.drawEllipse(0, 0, size, size);

    QFont font = painter.font();
    font.setPixelSize(size / 2);
    font.setBold(true);
    painter.setFont(font);
    painter.setPen(Qt::white);
    painter.drawText(QRect(0, 0, size, size), Qt::AlignCenter, QString(initial));
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
QPixmap chatAvatarPixmap(const QString& title, const QString& photoPath, int size) {
    if (photoPath.isEmpty()) {
        return avatarPixmap(title, size);
    }
    static QHash<QString, QPixmap> cache;
    QString cacheKey = photoPath + QLatin1Char(':') + QString::number(size);
    auto cached = cache.constFind(cacheKey);
    if (cached != cache.constEnd()) {
        return cached.value();
    }

    QPixmap source(photoPath);
    if (source.isNull()) {
        return avatarPixmap(title, size);
    }
    QPixmap scaled = source.scaled(size, size, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);

    QPixmap result(size, size);
    result.fill(Qt::transparent);
    QPainter painter(&result);
    painter.setRenderHint(QPainter::Antialiasing);
    QPainterPath clip;
    clip.addEllipse(0, 0, size, size);
    painter.setClipPath(clip);
    // Center-crop: scaled may be wider or taller than size x size (KeepAspectRatioByExpanding
    // only guarantees it covers size x size on both axes, not that it matches exactly).
    painter.drawPixmap((size - scaled.width()) / 2, (size - scaled.height()) / 2, scaled);
    painter.end();
    // Unbounded but self-limiting in practice: keyed by a real Telegram
    // account's own chat count, which does not grow without the user
    // adding more chats, unlike e.g. a cache keyed by something
    // per-message.
    cache.insert(cacheKey, result);
    return result;
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
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    painter.drawEllipse(0, 0, size, size);
    return pixmap;
}

// The real Telegram Desktop chat background is not one flat image: a
// tiled, mostly-transparent doodle pattern is blended over a solid color
// or gradient with a blend mode and reduced opacity (see
// Data::WallPaper's rendering in telegram_fork/tdesktop's
// data/data_wall_paper.cpp) - approximated here by pre-compositing that
// same combination into a single tile once, then handing the tile to
// HistoryCanvas's viewport (see messages_) as a repeating QPalette brush (see
// where this is called in MainWindow's constructor). A flat
// background-image: url(...) in QSS could only ever show the pattern
// pixmap verbatim at full opacity with no blend mode or color layer under
// it - not achievable in QSS alone, hence doing the compositing here
// instead. CompositionMode_SoftLight is Qt's equivalent of the CSS
// soft-light blend mode real tdesktop mentions using for this.
QPixmap chatWallpaperTile() {
    QPixmap pattern(":/chat_wallpaper.png");
    if (pattern.isNull()) {
        return pattern;
    }
    QPixmap tile(pattern.size());
    QPainter painter(&tile);
    painter.setRenderHint(QPainter::Antialiasing);

    QLinearGradient gradient(0, 0, tile.width(), tile.height());
    gradient.setColorAt(0.0, QColor("#d5e6f7"));
    gradient.setColorAt(1.0, QColor("#c3d9ee"));
    painter.fillRect(tile.rect(), gradient);

    painter.setOpacity(0.35);
    painter.setCompositionMode(QPainter::CompositionMode_SoftLight);
    painter.drawPixmap(0, 0, pattern);
    painter.end();
    return tile;
}

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

// A small rounded point growing out of the bubble's bottom corner facing
// the avatar side - the same idea as tdesktop's own composited
// historyBubbleTailOutLeft/historyBubbleTailOutRight image assets (see
// ui/chat/message_bubble.cpp in telegram_fork/tdesktop), just drawn as a
// path instead of a bundled bitmap, since tdesktop's actual tail images
// are generated at its own build time from theme resources and are not
// checked into source control to copy (same reason chat_wallpaper.png was
// used as-is rather than reconstructed - see that investigation earlier
// in this project's history).
QPainterPath tailPath(const QRectF& body, bool tailOnLeft) {
    QPainterPath path;
    double x = tailOnLeft ? body.left() : body.right();
    double y = body.bottom();
    double dir = tailOnLeft ? -1.0 : 1.0;
    path.moveTo(x, y - 8);
    path.cubicTo(x, y - 2, x + dir * 2, y, x + dir * 6, y + 1);
    path.cubicTo(x + dir * 3, y + 1, x, y + 3, x, y);
    path.closeSubpath();
    return path;
}

constexpr double kBubbleLargeRadius = 16.0;
constexpr double kBubbleSmallRadius = 8.0;
constexpr int kBubbleMargin = 10;
constexpr int kBubbleTailHeight = 6;
constexpr int kMaxBubbleContentWidth = 420 - kBubbleMargin * 2;
constexpr int kMinBubbleContentWidth = 40;
// Timestamp/tick run at the end of a bubble: the font size of the muted
// "HH:mm" span, and the side of the square the outgoing tick is drawn in.
constexpr int kBubbleMetaFontSize = 11;
constexpr double kTickSize = 13.0;

// Fills+strokes one bubble's body (rounded rect plus an optional tail) at
// bodyRect - shared between every text/system item HistoryCanvas paints,
// factored out of the per-corner-radius logic so it is not duplicated
// per item kind.
void paintBubbleBody(QPainter& painter, const QRectF& bodyRect, const QColor& background, bool bordered,
                      bool tailOnLeft, bool showTail, bool joinedTop) {
    double nearTopRadius = joinedTop ? kBubbleSmallRadius : kBubbleLargeRadius;
    double nearBottomRadius = showTail ? kBubbleLargeRadius : kBubbleSmallRadius;
    double topLeft = tailOnLeft ? nearTopRadius : kBubbleLargeRadius;
    double topRight = tailOnLeft ? kBubbleLargeRadius : nearTopRadius;
    double bottomLeft = tailOnLeft ? nearBottomRadius : kBubbleLargeRadius;
    double bottomRight = tailOnLeft ? kBubbleLargeRadius : nearBottomRadius;

    QPainterPath path = roundedRectPath(bodyRect, topLeft, topRight, bottomRight, bottomLeft);
    if (showTail) {
        path = path.united(tailPath(bodyRect, tailOnLeft));
    }
    painter.fillPath(path, background);
    if (bordered) {
        painter.setPen(QPen(QColor("#e4e4e5"), 1));
        painter.drawPath(path);
    }
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
    bool attached = false;
    bool showTail = true;
    qlonglong messageId = 0;
    bool hidden = false;  // in-conversation search filter, see HistoryCanvas::setSearchFilter()

    // Outgoing "sent" tick. Drawn as a Glyph::Check over the run of
    // non-breaking spaces of this width reserved at the end of the text (see
    // where the item is measured, and paintSentTick) rather than being a
    // U+2713 character in the document itself: as a character it came out in
    // whatever the system font happened to have for it, at body-text weight,
    // and it inherited the timestamp's baseline instead of sitting centred
    // on the line the way Telegram's does.
    bool showTick = false;
    double tickSlotWidth = 0.0;

    // shared_ptr, not unique_ptr: QVector<HistoryItem> (items_ below) needs
    // HistoryItem to stay copy-constructible for some of its own internal
    // reallocation paths, which a unique_ptr member blocks entirely.
    std::shared_ptr<QTextDocument> doc;  // text/system items only
    int contentWidth = 0;                // bubble/system inner width, pre-margin
    int contentHeight = 0;               // bubble/system inner height, pre-margin

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
};

// Eases wheel-scrolled movement into place instead of the default
// per-tick instant jump, on both the sidebar and message list. Real
// Telegram Desktop feeds wheel input into a tuned Ui::Animations-driven
// QScroller kinetic state machine (see
// telegram_fork/tdesktop/Telegram/lib_ui/ui/widgets/scroll_area.cpp,
// ScrollArea::viewportEvent()/_scroller->handleInput() -
// DecelerationFactor 0.6, MaximumVelocity 0.95, overshoot off) - that
// exact input-synthesis wiring is internal to lib_ui and not reusable
// standalone, so this reaches the same *visible* result (a glide into
// the target position rather than a stepped jump) through a
// QVariantAnimation on the scrollbar value instead, same mechanism this
// codebase already uses for RippleButton's ripple.
// Real Telegram Desktop's own mechanism for kinetic wheel/trackpad
// scrolling: a tuned QScroller fed directly from wheel input, not a
// hand-written easing animation (an earlier version of this class did
// that instead - it was an approximation of the *effect*, not the real
// mechanism, and it looked wrong). Structure and every tuned constant
// copied directly from telegram_fork/tdesktop/Telegram/lib_ui/ui/widgets/
// scroll_area.cpp: SetupScrollerPhysics() for the QScrollerProperties,
// ScrollArea::viewportEvent()'s QEvent::Wheel handling for the
// phase-based press/move/release dispatch into QScroller::handleInput().
//
// Only phased wheel events (Qt::ScrollBegin/Update/End/Momentum - what a
// trackpad or a precision mouse wheel sends) drive the scroller here,
// exactly like tdesktop: a plain discrete wheel click
// (Qt::NoScrollPhase, the common case for an ordinary USB/wireless mouse
// on Windows) is left alone and falls through to
// QAbstractScrollArea's own default wheel handling, same as in the real
// client - not a limitation introduced here, tdesktop's own scroller
// genuinely does not smooth a plain mouse wheel either.
class KineticScroller {
public:
    explicit KineticScroller(QWidget* target) : target_(target), scroller_(QScroller::scroller(target)) {
        QScrollerProperties props = scroller_->scrollerProperties();
        using P = QScrollerProperties;
        // The Qt default 0.125 gives a ~4s, slippery, constant-deceleration
        // glide that "never slows down" - 0.6 brings a hard flick down to
        // ~1-1.6s, close to macOS native momentum (tdesktop's own comment).
        props.setScrollMetric(P::DecelerationFactor, 0.6);
        // The default cap 0.5 m/s clips hard flicks below the feel tdesktop
        // tunes for; this metric is m/s, converted to pixels internally.
        props.setScrollMetric(P::MaximumVelocity, 0.95);
        props.setScrollMetric(P::AcceleratingFlickMaximumTime, 0.0);
        props.setScrollMetric(P::MaximumClickThroughVelocity, 0.0);
        props.setScrollMetric(P::DragStartDistance, 0.0);
        props.setScrollMetric(P::VerticalOvershootPolicy, QVariant::fromValue(P::OvershootAlwaysOff));
        props.setScrollMetric(P::HorizontalOvershootPolicy, QVariant::fromValue(P::OvershootAlwaysOff));
        scroller_->setScrollerProperties(props);
    }

    // Returns true if the event was consumed (a phased event, handed to
    // the scroller) - the caller should treat false as "let default
    // wheel handling run", matching tdesktop's own fallthrough to
    // QScrollArea::viewportEvent(e) for a non-phased event.
    bool handleWheelEvent(QWheelEvent* event) {
        switch (event->phase()) {
        case Qt::ScrollBegin:
        case Qt::ScrollUpdate: {
            bool wasNull = wheelPos_.isNull();
            if (wasNull) {
                wheelPos_ = QPoint(target_->width(), target_->height()) / 2;
            } else {
                wheelPos_ += event->angleDelta();
            }
            scroller_->handleInput(wasNull ? QScroller::InputPress : QScroller::InputMove, wheelPos_,
                                    QDateTime::currentMSecsSinceEpoch());
            return true;
        }
        case Qt::ScrollEnd:
        case Qt::ScrollMomentum:
            if (!wheelPos_.isNull()) {
                scroller_->handleInput(QScroller::InputRelease, wheelPos_, QDateTime::currentMSecsSinceEpoch());
                wheelPos_ = {};
            }
            return true;
        default:
            return false;
        }
    }

private:
    QWidget* target_;
    QScroller* scroller_;
    QPoint wheelPos_;
};

// The message list itself - a single custom-painted, virtualized canvas
// (QAbstractScrollArea + manual layout/paint/hit-test), not a
// QAbstractItemView subclass with a child widget per row, matching real
// Telegram Desktop's own architecture for this (see HistoryItem's own
// comment above for the direct citation). Virtualized: paintEvent only
// draws items intersecting the current viewport, however many messages
// are actually loaded.
class HistoryCanvas : public QAbstractScrollArea {
public:
    explicit HistoryCanvas(QWidget* parent = nullptr) : QAbstractScrollArea(parent) {
        setFocusPolicy(Qt::NoFocus);
        setFrameShape(QFrame::NoFrame);
        setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        kineticScroller_ = std::make_unique<KineticScroller>(this);
    }

    void clear() {
        items_.clear();
        totalHeight_ = 0;
        lastTextItemIndex_ = -1;
        updateScrollRange();
        viewport()->update();
    }

    // Full replace - O(n) (every item is re-measured and re-laid-out).
    // Used for chat switches and the handful of operations that already
    // have to touch the whole conversation anyway (loading an older page,
    // a photo/sender name resolving after the fact) - all rare/bulk, not
    // per-message, so O(n) here is not the same runaway pattern that was
    // fixed for the sidebar/history auto-fill.
    void setItems(const QVector<HistoryEntry>& entries) {
        items_.clear();
        items_.reserve(entries.size());
        for (const HistoryEntry& entry : entries) {
            items_.push_back(buildItem(entry));
        }
        relayoutFrom(0);
        lastTextItemIndex_ = -1;
        for (int i = items_.size() - 1; i >= 0; --i) {
            if (!items_[i].isImage && items_[i].kind != MessageKind::System) {
                lastTextItemIndex_ = i;
                break;
            }
        }
        updateScrollRange();
        viewport()->update();
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
                int delta = kBubbleTailHeight;
                previous.height -= delta;
                totalHeight_ -= delta;
                for (int i = lastTextItemIndex_ + 1; i < items_.size(); ++i) {
                    items_[i].top -= delta;
                }
            }
        }
        HistoryItem item = buildItem(entry);
        item.top = totalHeight_;
        totalHeight_ += item.height;
        bool isTextItem = !item.isImage && entry.kind != MessageKind::System;
        items_.push_back(std::move(item));
        lastTextItemIndex_ = isTextItem ? items_.size() - 1 : -1;
        updateScrollRange();
        viewport()->update();
    }

    // Inserts a page of older messages at the front - O(n) to shift every
    // already-loaded item's top by the new content's height, but
    // crucially does NOT re-measure or rebuild any of them, only the new
    // page's own items. Calling setItems() with the whole (growing) list
    // instead, once per page, was found to redo the same expensive
    // QTextDocument work for every already-loaded message on every single
    // page load - an O(n^2) pattern for a chat that needs several pages
    // to fill the view (see MainWindow::ensureHistoryFillsViewport()),
    // the exact same class of freeze already found and fixed once for
    // the sidebar (see TelegramClient::requestMoreChats()'s own comment
    // in telegram_client.cpp).
    void prependItems(const QVector<HistoryEntry>& entries) {
        QVector<HistoryItem> newItems;
        newItems.reserve(entries.size());
        int y = 0;
        for (const HistoryEntry& entry : entries) {
            HistoryItem item = buildItem(entry);
            item.top = y;
            y += item.height;
            newItems.push_back(std::move(item));
        }
        int shift = y;
        for (HistoryItem& existing : items_) {
            existing.top += shift;
        }
        // Only advance lastTextItemIndex_ (used by appendItem()'s tail
        // retraction) if it already pointed at a real text item - if the
        // newest message in the conversation is an image/system line
        // (-1), prepending older history does not change that.
        if (lastTextItemIndex_ >= 0) {
            lastTextItemIndex_ += newItems.size();
        }
        items_ = newItems + items_;
        totalHeight_ += shift;
        updateScrollRange();
        viewport()->update();
    }

    void scrollToBottom() { verticalScrollBar()->setValue(verticalScrollBar()->maximum()); }

    // In-conversation text filter (see MainWindow::onSearchTextChanged) -
    // hides non-matching rows in place rather than rebuilding the list.
    void setSearchFilter(const QString& query) {
        for (HistoryItem& item : items_) {
            item.hidden = !query.isEmpty() && !item.rawText.contains(query, Qt::CaseInsensitive);
        }
        relayoutFrom(0);
        updateScrollRange();
        viewport()->update();
    }

    struct HitResult {
        bool valid = false;
        MessageKind kind = MessageKind::System;
        QString text;
    };
    // pos is in viewport-local coordinates, same convention as
    // QAbstractItemView::itemAt() (customContextMenuRequested's own
    // point), which is what this replaces.
    HitResult itemAt(const QPoint& pos) const {
        int y = pos.y() + verticalScrollBar()->value();
        for (const HistoryItem& item : items_) {
            if (item.hidden) {
                continue;
            }
            if (y >= item.top && y < item.top + item.height) {
                return HitResult{true, item.kind, item.rawText};
            }
        }
        return HitResult{};
    }

protected:
    void paintEvent(QPaintEvent* event) override {
        QPainter painter(viewport());
        painter.setRenderHint(QPainter::Antialiasing);
        int scrollY = verticalScrollBar()->value();
        QRect visible = event->rect();
        for (const HistoryItem& item : items_) {
            if (item.hidden) {
                continue;
            }
            int itemTop = item.top - scrollY;
            int itemBottom = itemTop + item.height;
            // Virtualization: only items intersecting the actually
            // exposed rect are painted at all, regardless of how many
            // messages are loaded in total.
            if (itemBottom < visible.top() || itemTop > visible.bottom()) {
                continue;
            }
            paintItem(painter, item, itemTop);
        }
    }

    void resizeEvent(QResizeEvent* event) override {
        QAbstractScrollArea::resizeEvent(event);
        updateScrollRange();
    }

    void wheelEvent(QWheelEvent* event) override {
        if (kineticScroller_->handleWheelEvent(event)) {
            event->accept();
        } else {
            QAbstractScrollArea::wheelEvent(event);
        }
    }

private:
    std::unique_ptr<KineticScroller> kineticScroller_;
    HistoryItem buildItem(const HistoryEntry& entry) const {
        HistoryItem item;
        item.kind = entry.kind;
        item.rawText = entry.text;
        item.attached = entry.attached;
        item.showTail = entry.showTail;
        item.messageId = entry.messageId;

        QPixmap pixmap;
        item.isImage = !entry.filePath.isEmpty() && pixmap.load(entry.filePath);
        if (item.isImage) {
            item.pixmap = pixmap.scaledToWidth(280, Qt::SmoothTransformation);
            item.contentWidth = item.pixmap.width();
            item.contentHeight = item.pixmap.height();
            int tailExtra = entry.showTail ? kBubbleTailHeight : 0;
            item.height = item.contentHeight + kBubbleMargin * 2 + tailExtra + kRowVerticalMargin(entry.attached);
            return item;
        }

        QFont bubbleFont = QGuiApplication::font();
        bubbleFont.setPixelSize(entry.kind == MessageKind::System ? 13 : 15);

        QString html;
        bool richText = entry.kind != MessageKind::System;
        if (entry.kind == MessageKind::System) {
            html = entry.text;
        } else {
            QString time = QDateTime::currentDateTime().toString("HH:mm");
            // Space for the tick, not the tick itself - it is painted over
            // this run afterwards (see HistoryItem::showTick/paintSentTick).
            // Non-breaking spaces specifically: ordinary trailing spaces are
            // excluded from QTextLine::naturalTextWidth(), which is what
            // paintSentTick measures the run back from, and they would not
            // widen the bubble to make room either.
            QString tickSlot;
            if (entry.kind == MessageKind::Outgoing) {
                QFont metaFont = bubbleFont;
                metaFont.setPixelSize(kBubbleMetaFontSize);
                double spaceWidth = QFontMetricsF(metaFont).horizontalAdvance(QChar(0x00A0));
                int spaces = spaceWidth > 0.0 ? static_cast<int>(std::ceil((kTickSize + 2.0) / spaceWidth)) : 4;
                tickSlot = QString(spaces, QChar(0x00A0));
                item.showTick = true;
                item.tickSlotWidth = spaces * spaceWidth;
            }
            QString escaped = entry.text.toHtmlEscaped();
            escaped.replace("\n", "<br>");
            html = QString("%1&nbsp;&nbsp;<span style=\"color:#a0acb6; font-size:%2px;\">%3%4</span>")
                       .arg(escaped)
                       .arg(kBubbleMetaFontSize)
                       .arg(time, tickSlot);
            if (entry.kind == MessageKind::Incoming && !entry.attached && !entry.senderName.isEmpty()) {
                html = QString("<b style=\"color:#3a8ee6;\">%1</b><br>%2").arg(entry.senderName.toHtmlEscaped(), html);
            }
        }

        item.doc = std::make_shared<QTextDocument>();
        item.doc->setDefaultFont(bubbleFont);
        if (richText) {
            item.doc->setHtml(html);
        } else {
            item.doc->setPlainText(html);
        }
        item.doc->setTextWidth(kMaxBubbleContentWidth);
        // ceil, not a truncating cast: idealWidth() is fractional, and
        // handing the document back a width even a fraction of a pixel
        // narrower than it asked for makes it wrap - so a bubble that fits
        // on one line got laid out on two, and the run of non-breaking
        // spaces reserved for the tick (see above) was pushed onto a line of
        // its own.
        int idealWidth = qBound(kMinBubbleContentWidth, static_cast<int>(std::ceil(item.doc->idealWidth())),
                                 kMaxBubbleContentWidth);
        item.doc->setTextWidth(idealWidth);
        item.contentWidth = idealWidth;
        item.contentHeight = static_cast<int>(item.doc->size().height());

        int bubbleOuterHeight = item.contentHeight + kBubbleMargin * 2;
        if (entry.kind != MessageKind::System && entry.showTail) {
            bubbleOuterHeight += kBubbleTailHeight;
        }
        item.height = bubbleOuterHeight + kRowVerticalMargin(entry.attached);
        return item;
    }

    static int kRowVerticalMargin(bool attached) { return attached ? 2 : 7; }

    void paintItem(QPainter& painter, const HistoryItem& item, int rowTop) const {
        int rowMargin = kRowVerticalMargin(item.attached);
        int bubbleOuterHeight = item.height - rowMargin;
        int bubbleTop = rowTop + rowMargin / 2;
        int viewportWidth = viewport()->width();

        int bubbleOuterWidth = item.contentWidth + kBubbleMargin * 2;
        int bubbleLeft;
        if (item.kind == MessageKind::Outgoing) {
            bubbleLeft = viewportWidth - bubbleOuterWidth - kSideMargin;
        } else if (item.kind == MessageKind::Incoming) {
            bubbleLeft = kSideMargin;
        } else {
            bubbleLeft = (viewportWidth - bubbleOuterWidth) / 2;
        }

        if (item.isImage) {
            int imageTailExtra = item.showTail ? kBubbleTailHeight : 0;
            QRectF body(bubbleLeft, bubbleTop, bubbleOuterWidth, bubbleOuterHeight - imageTailExtra);
            bool tailOnLeft = item.kind != MessageKind::Outgoing;
            QColor background = item.kind == MessageKind::Outgoing ? QColor("#def1fd") : QColor("#ffffff");
            paintBubbleBody(painter, body, background, item.kind != MessageKind::Outgoing, tailOnLeft, item.showTail,
                             item.attached);
            painter.drawPixmap(bubbleLeft + kBubbleMargin, bubbleTop + kBubbleMargin, item.pixmap);
            return;
        }

        if (item.kind == MessageKind::System) {
            QRectF body(bubbleLeft, bubbleTop, bubbleOuterWidth, bubbleOuterHeight);
            QPainterPath path = roundedRectPath(body, kBubbleLargeRadius, kBubbleLargeRadius, kBubbleLargeRadius,
                                                 kBubbleLargeRadius);
            painter.fillPath(path, QColor("#eef0f2"));
            painter.setPen(QColor("#8a8a8a"));
            painter.save();
            painter.translate(bubbleLeft + kBubbleMargin, bubbleTop + kBubbleMargin);
            item.doc->drawContents(&painter);
            painter.restore();
            return;
        }

        bool isOutgoing = item.kind == MessageKind::Outgoing;
        int tailExtra = item.showTail ? kBubbleTailHeight : 0;
        QRectF body(bubbleLeft, bubbleTop, bubbleOuterWidth, bubbleOuterHeight - tailExtra);
        paintBubbleBody(painter, body, isOutgoing ? QColor("#def1fd") : QColor("#ffffff"), !isOutgoing, !isOutgoing,
                         item.showTail, item.attached);
        painter.save();
        painter.translate(bubbleLeft + kBubbleMargin, bubbleTop + kBubbleMargin);
        item.doc->drawContents(&painter);
        if (item.showTick) {
            paintSentTick(painter, item);
        }
        painter.restore();
    }

    // Draws the outgoing "sent" tick into the space reserved for it at the
    // end of the last line, with painter already translated to the
    // document's own origin. Positioned from the text layout rather than
    // from the bubble's bottom-right corner: the last line is not
    // necessarily the widest one, so the timestamp this follows can sit well
    // to the left of the bubble's right edge.
    void paintSentTick(QPainter& painter, const HistoryItem& item) const {
        QTextBlock block = item.doc->lastBlock();
        QTextLayout* layout = block.layout();
        if (layout == nullptr || layout->lineCount() == 0) {
            return;
        }
        QTextLine line = layout->lineAt(layout->lineCount() - 1);
        QPointF blockOrigin = item.doc->documentLayout()->blockBoundingRect(block).topLeft();
        double right = blockOrigin.x() + line.x() + line.naturalTextWidth();
        double centerY = blockOrigin.y() + line.y() + line.height() / 2.0;
        // Centred in the reserved run rather than flush to its start, so the
        // slack left over from rounding the run up to a whole number of
        // spaces is split between the timestamp side and the bubble's margin.
        double left = right - item.tickSlotWidth + (item.tickSlotWidth - kTickSize) / 2.0;
        paintGlyph(painter, QRectF(left, centerY - kTickSize / 2.0, kTickSize, kTickSize), Glyph::Check,
                    kIconAccentFg);
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
    }

    void updateScrollRange() {
        int viewportHeight = viewport()->height();
        verticalScrollBar()->setRange(0, qMax(0, totalHeight_ - viewportHeight));
        verticalScrollBar()->setPageStep(viewportHeight);
    }

    static constexpr int kSideMargin = 8;

    QVector<HistoryItem> items_;
    int totalHeight_ = 0;
    int lastTextItemIndex_ = -1;
};

// MainWindow::messages_ is declared as a plain QWidget* in the header (see
// its own comment there) so main_window.hpp does not need to expose
// HistoryCanvas's internals - every real call site is in this file, and
// always knows the true type, hence the cast.
HistoryCanvas* asCanvas(QWidget* widget) {
    return static_cast<HistoryCanvas*>(widget);
}

// A QPushButton with a real Telegram Desktop-style ripple: a circle grows
// from the click point and fades out over the button's own shape, the
// same effect real tdesktop uses on every one of its buttons (see
// Ui::RippleButton in telegram_fork/tdesktop's ui/widgets/buttons.h) -
// QPushButton alone has no such feedback, just its normal
// pressed/hover palette swap. Does not need its own Q_OBJECT/moc: it only
// uses QVariantAnimation as a plain QObject connect() context for a
// lambda, it does not declare any new signal/slot/property of its own.
class RippleButton : public QPushButton {
public:
    explicit RippleButton(QWidget* parent = nullptr) : QPushButton(parent) { setUpRippleAnimation(); }
    RippleButton(const QIcon& icon, const QString& text, QWidget* parent = nullptr)
        : QPushButton(icon, text, parent) {
        setUpRippleAnimation();
    }

protected:
    void mousePressEvent(QMouseEvent* event) override {
        QPushButton::mousePressEvent(event);
        if (!isEnabled()) {
            return;
        }
        rippleCenter_ = event->pos();
        rippleAnimation_->stop();
        rippleAnimation_->setStartValue(0.0);
        rippleAnimation_->setEndValue(1.0);
        rippleAnimation_->start();
    }

    void paintEvent(QPaintEvent* event) override {
        QPushButton::paintEvent(event);
        if (rippleProgress_ <= 0.0 || rippleProgress_ >= 1.0) {
            return;
        }
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        // Clipped to (an approximation of) the button's own rounded
        // shape - style.qss rounds every button heavily enough (pill or
        // circular) that a fully-rounded clip reads correctly for both,
        // without needing this class to know each button's exact radius.
        QPainterPath clip;
        clip.addRoundedRect(rect(), height() / 2.0, height() / 2.0);
        painter.setClipPath(clip);
        double maxRadius = std::hypot(static_cast<double>(width()), static_cast<double>(height()));
        double radius = maxRadius * rippleProgress_;
        int alpha = static_cast<int>(60 * (1.0 - rippleProgress_));
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0, 0, 0, alpha));
        painter.drawEllipse(QPointF(rippleCenter_), radius, radius);
    }

private:
    void setUpRippleAnimation() {
        rippleAnimation_ = new QVariantAnimation(this);
        rippleAnimation_->setDuration(400);
        rippleAnimation_->setEasingCurve(QEasingCurve::OutCubic);
        connect(rippleAnimation_, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
            rippleProgress_ = value.toDouble();
            update();
        });
    }

    QPoint rippleCenter_;
    double rippleProgress_ = 0.0;
    QVariantAnimation* rippleAnimation_ = nullptr;
};

constexpr int kSidebarRowHeight = 64;
constexpr int kSidebarAvatarSize = 46;
constexpr int kSidebarMargin = 10;

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
};

// The sidebar chat list itself - a single custom-painted, virtualized
// canvas (QAbstractScrollArea + manual layout/paint/hit-test), the same
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
class SidebarCanvas : public QAbstractScrollArea {
public:
    explicit SidebarCanvas(QWidget* parent = nullptr) : QAbstractScrollArea(parent) {
        setFocusPolicy(Qt::NoFocus);
        setFrameShape(QFrame::NoFrame);
        setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setMouseTracking(true);
        viewport()->setMouseTracking(true);
        kineticScroller_ = std::make_unique<KineticScroller>(this);

        nameFont_ = font();
        nameFont_.setWeight(QFont::DemiBold);
        nameFont_.setPixelSize(14);
        previewFont_ = font();
        previewFont_.setPixelSize(13);
        badgeFont_ = font();
        badgeFont_.setPixelSize(11);
        badgeFont_.setWeight(QFont::DemiBold);
    }

    // Called from MainWindow::onChatListItemClicked() via a row click -
    // SidebarCanvas has no signals of its own, this is a plain callback
    // rather than needing Q_OBJECT/moc for a class that otherwise does
    // not need either.
    std::function<void(qlonglong)> onRowClicked;

    void setRows(const QVariantList& chats, qlonglong selectedChatId) {
        rows_.clear();
        rows_.reserve(chats.size());
        for (const QVariant& entryVariant : chats) {
            QVariantMap entry = entryVariant.toMap();
            QString title = entry["title"].toString();
            QString preview = entry["preview"].toString();
            SidebarRowData row;
            row.chatId = entry["id"].toLongLong();
            row.unread = entry["unread"].toInt();
            row.active = entry["active"].toBool();
            row.avatar = chatAvatarPixmap(title, entry["photo"].toString(), kSidebarAvatarSize);
            int textWidth = 260;  // generous; real elision happens against the actual row width at paint time
            row.nameElided = QFontMetrics(nameFont_).elidedText(title, Qt::ElideRight, textWidth);
            QString previewText = preview.isEmpty() ? "No messages yet" : preview;
            row.previewElided = QFontMetrics(previewFont_).elidedText(previewText, Qt::ElideRight, textWidth);
            rows_.push_back(std::move(row));
        }
        selectedChatId_ = selectedChatId;
        hoveredIndex_ = -1;
        updateScrollRange();
        viewport()->update();
    }

    void setSelectedChatId(qlonglong chatId) {
        if (selectedChatId_ == chatId) {
            return;
        }
        selectedChatId_ = chatId;
        viewport()->update();
    }

    void scrollToTop() { verticalScrollBar()->setValue(0); }

    int rowCount() const { return rows_.size(); }

protected:
    void paintEvent(QPaintEvent* event) override {
        QPainter painter(viewport());
        painter.setRenderHint(QPainter::Antialiasing);
        int scrollY = verticalScrollBar()->value();
        QRect visible = event->rect();
        for (int i = 0; i < rows_.size(); ++i) {
            int top = i * kSidebarRowHeight - scrollY;
            if (top + kSidebarRowHeight < visible.top() || top > visible.bottom()) {
                continue;
            }
            paintRow(painter, rows_[i], top, rows_[i].chatId == selectedChatId_, i == hoveredIndex_);
        }
    }

    void resizeEvent(QResizeEvent* event) override {
        QAbstractScrollArea::resizeEvent(event);
        updateScrollRange();
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        int index = indexAt(event->pos());
        if (index != hoveredIndex_) {
            hoveredIndex_ = index;
            viewport()->update();
        }
    }

    void leaveEvent(QEvent*) override {
        if (hoveredIndex_ != -1) {
            hoveredIndex_ = -1;
            viewport()->update();
        }
    }

    void mousePressEvent(QMouseEvent* event) override {
        int index = indexAt(event->pos());
        if (index >= 0 && onRowClicked) {
            onRowClicked(rows_[index].chatId);
        }
    }

    void wheelEvent(QWheelEvent* event) override {
        if (kineticScroller_->handleWheelEvent(event)) {
            event->accept();
        } else {
            QAbstractScrollArea::wheelEvent(event);
        }
    }

private:
    std::unique_ptr<KineticScroller> kineticScroller_;
    int indexAt(const QPoint& pos) const {
        int y = pos.y() + verticalScrollBar()->value();
        int index = y / kSidebarRowHeight;
        return (index >= 0 && index < rows_.size()) ? index : -1;
    }

    void updateScrollRange() {
        int viewportHeight = viewport()->height();
        verticalScrollBar()->setRange(0, qMax(0, rows_.size() * kSidebarRowHeight - viewportHeight));
        verticalScrollBar()->setPageStep(viewportHeight);
    }

    void paintRow(QPainter& painter, const SidebarRowData& row, int top, bool selected, bool hovered) {
        QColor background = selected ? QColor("#419fd9") : (hovered ? QColor("#f1f1f1") : QColor("#ffffff"));
        painter.fillRect(QRect(0, top, viewport()->width(), kSidebarRowHeight), background);

        painter.drawPixmap(kSidebarMargin, top + (kSidebarRowHeight - kSidebarAvatarSize) / 2, row.avatar);

        int textLeft = kSidebarMargin + kSidebarAvatarSize + 10;
        int badgeReserve = row.unread > 0 ? 40 : 0;
        int textWidth = viewport()->width() - textLeft - kSidebarMargin - badgeReserve;

        painter.setPen(selected ? QColor("#ffffff") : QColor("#222222"));
        painter.setFont(nameFont_);
        painter.drawText(QRect(textLeft, top + 10, textWidth, 20), Qt::AlignLeft | Qt::AlignVCenter, row.nameElided);

        int previewLeft = textLeft;
        if (row.active) {
            // A padlock = an encrypted session exists for this chat. Keeps
            // the green of the plain dot this used to be (and of statusDot_,
            // the overall connection light) while actually naming what is
            // active - on a selected row, where the background is the blue
            // accent, the green would not read at all, so it goes white with
            // the rest of the row's text.
            // top + 37 rather than the preview row's own top + 34: the
            // padlock's ink sits in the middle of its 13x13 box, so aligning
            // the boxes would leave it riding above the text's centre line.
            paintGlyph(painter, QRectF(previewLeft, top + 37, 13, 13), Glyph::Lock,
                        selected ? QColor("#ffffff") : kIconEncryptedFg);
            previewLeft += 17;
        }
        painter.setPen(selected ? QColor("#ffffff") : QColor("#707579"));
        painter.setFont(previewFont_);
        painter.drawText(QRect(previewLeft, top + 34, textWidth - (previewLeft - textLeft), 20),
                          Qt::AlignLeft | Qt::AlignVCenter, row.previewElided);

        if (row.unread > 0) {
            QString badgeText = row.unread > 99 ? "99+" : QString::number(row.unread);
            QFontMetrics badgeMetrics(badgeFont_);
            int badgeWidth = qMax(20, badgeMetrics.horizontalAdvance(badgeText) + 12);
            constexpr int kBadgeHeight = 20;
            QRectF badgeRect(viewport()->width() - kSidebarMargin - badgeWidth,
                              top + (kSidebarRowHeight - kBadgeHeight) / 2.0, badgeWidth, kBadgeHeight);
            painter.setPen(Qt::NoPen);
            painter.setBrush(selected ? QColor("#ffffff") : QColor("#40a7e3"));
            painter.drawRoundedRect(badgeRect, kBadgeHeight / 2.0, kBadgeHeight / 2.0);
            painter.setPen(selected ? QColor("#419fd9") : QColor("#ffffff"));
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
};

SidebarCanvas* asSidebar(QWidget* widget) {
    return static_cast<SidebarCanvas*>(widget);
}

// App icon (resources/app.ico, same file CMakeLists.txt separately embeds
// as the window/taskbar icon via app.rc) scaled up for the auth slide logo
// - avoids drawing/tracing any Telegram trademark artwork ourselves.
// Loaded from the Qt resource system (see resources.qrc / loadStylesheet),
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
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor("#2aabee"));
        painter.drawEllipse(0, 0, size, size);
        return fallback;
    }
    return icon.scaled(size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
}

}  // namespace

ChatInput::ChatInput(QWidget* parent) : QPlainTextEdit(parent) {
    setPlaceholderText("Message");
    setTabChangesFocus(true);
    setFixedHeight(36);
}

void ChatInput::keyPressEvent(QKeyEvent* event) {
    bool isEnter = event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter;
    if (isEnter && !(event->modifiers() & Qt::ShiftModifier)) {
        event->accept();
        Q_EMIT submitted();
        return;
    }
    QPlainTextEdit::keyPressEvent(event);
}

AuthSlide::AuthSlide(QWidget* parent) : QWidget(parent) {
    setObjectName("authSlide");

    auto* outer = new QHBoxLayout(this);
    outer->addStretch();

    auto* card = new QWidget(this);
    card->setObjectName("authCard");
    card->setFixedWidth(360);
    auto* cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(0, 0, 0, 0);
    cardLayout->setSpacing(18);
    cardLayout->addStretch();

    logo_ = new QLabel(card);
    logo_->setPixmap(loadAuthLogo(96));
    logo_->setAlignment(Qt::AlignCenter);
    cardLayout->addWidget(logo_);

    title_ = new QLabel(card);
    title_->setObjectName("authTitle");
    title_->setAlignment(Qt::AlignCenter);
    title_->setWordWrap(true);
    cardLayout->addWidget(title_);

    subtitle_ = new QLabel(card);
    subtitle_->setObjectName("authSubtitle");
    subtitle_->setAlignment(Qt::AlignCenter);
    subtitle_->setWordWrap(true);
    cardLayout->addWidget(subtitle_);

    input_ = new QLineEdit(card);
    input_->setObjectName("authInput");
    input_->setAlignment(Qt::AlignCenter);
    cardLayout->addWidget(input_);

    error_ = new QLabel(card);
    error_->setObjectName("authError");
    error_->setAlignment(Qt::AlignCenter);
    error_->setWordWrap(true);
    error_->hide();
    cardLayout->addWidget(error_);

    next_ = new RippleButton(QIcon(), "Next", card);
    next_->setObjectName("authNext");
    cardLayout->addWidget(next_);

    cardLayout->addStretch();
    outer->addWidget(card);
    outer->addStretch();

    connect(next_, &QPushButton::clicked, this, &AuthSlide::onNextClicked);
    connect(input_, &QLineEdit::returnPressed, this, &AuthSlide::onNextClicked);
}

QString AuthSlide::runSlide(const QString& title, const QString& subtitle, const QString& placeholder, bool masked,
                             Validator validator, const QString& initialError) {
    logDebug("runSlide: entered, title=" + title);
    title_->setText(title);
    subtitle_->setText(subtitle);
    input_->setPlaceholderText(placeholder);
    input_->setEchoMode(masked ? QLineEdit::Password : QLineEdit::Normal);
    input_->clear();
    input_->show();
    next_->show();
    input_->setFocus();
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
    input_->hide();
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
    QString value = input_->text();
    // Empty input just refocuses instead of submitting - the fields here
    // (password/phone/code) are always required, there is no valid empty
    // submission to accept.
    if (value.isEmpty()) {
        input_->setFocus();
        return;
    }
    if (validator_) {
        QString message = validator_(value);
        if (!message.isEmpty()) {
            error_->setText(message);
            error_->show();
            input_->setFocus();
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

    auto* central = new QWidget(this);
    central->setObjectName("central");
    chatPage_ = central;
    auto* rootLayout = new QHBoxLayout(central);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    // Sidebar/conversation split is user-resizable by dragging the divider,
    // same as tdesktop's own dialogs list - a fixed width did not match
    // real Telegram Desktop's behavior.
    auto* splitter = new QSplitter(Qt::Horizontal, central);
    splitter->setObjectName("mainSplitter");
    splitter->setChildrenCollapsible(false);
    rootLayout->addWidget(splitter);

    // ---- sidebar: chat list + "start a new conversation" ----
    auto* sidebar = new QWidget(splitter);
    sidebar->setObjectName("sidebar");
    sidebar->setMinimumWidth(220);
    auto* sidebarLayout = new QVBoxLayout(sidebar);
    sidebarLayout->setContentsMargins(0, 0, 0, 0);
    sidebarLayout->setSpacing(0);

    auto* searchRow = new QWidget(sidebar);
    auto* searchRowLayout = new QHBoxLayout(searchRow);
    searchRowLayout->setContentsMargins(10, 8, 10, 8);
    chatSearchInput_ = new QLineEdit(searchRow);
    chatSearchInput_->setPlaceholderText("Search");
    // Magnifier inside the field rather than a bare rounded box with a
    // "Search" placeholder - the same leading glyph tdesktop puts in its own
    // dialogs search field. QLineEdit::addAction() reserves the space and
    // keeps the text clear of it on its own, so nothing here has to nudge
    // the field's padding.
    chatSearchInput_->addAction(glyphIcon(Glyph::Search, kIconFg), QLineEdit::LeadingPosition);
    searchRowLayout->addWidget(chatSearchInput_);
    sidebarLayout->addWidget(searchRow);

    searchDebounceTimer_ = new QTimer(this);
    searchDebounceTimer_->setSingleShot(true);

    sidebarRenderThrottleTimer_ = new QTimer(this);
    sidebarRenderThrottleTimer_->setSingleShot(true);
    connect(sidebarRenderThrottleTimer_, &QTimer::timeout, this, &MainWindow::flushSidebarRender);

    auto* sidebarCanvas = new SidebarCanvas(sidebar);
    sidebarCanvas->setObjectName("chatListWidget");
    sidebarCanvas->onRowClicked = [this](qlonglong chatId) { onChatListItemClicked(chatId); };
    chatListWidget_ = sidebarCanvas;
    sidebarLayout->addWidget(chatListWidget_);
    splitter->addWidget(sidebar);

    // ---- right side: header + selected conversation ----
    auto* conversationColumn = new QWidget(splitter);
    auto* layout = new QVBoxLayout(conversationColumn);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* header = new QWidget(conversationColumn);
    header->setObjectName("header");
    auto* headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(12, 6, 12, 6);
    headerLayout->setSpacing(10);
    companionAvatar_ = new QLabel(header);
    companionAvatar_->setFixedSize(38, 38);
    companionAvatar_->hide();  // no chat selected yet, see onChatListItemClicked
    companionLabel_ = new QLabel("zkgram", header);
    companionLabel_->setObjectName("companionLabel");
    // Filters the message list below by substring - the currently open
    // conversation only, not a search across every chat.
    searchInput_ = new QLineEdit(header);
    searchInput_->setPlaceholderText("Search in conversation");
    searchInput_->setMaximumWidth(180);
    searchInput_->addAction(glyphIcon(Glyph::Search, kIconFg), QLineEdit::LeadingPosition);
    // statusDot_/statusLabel_ are not added to the header - a persistent
    // "Connected" pill is not something real Telegram Desktop shows once
    // logged in (it only ever surfaces a transient "Connecting..."/
    // "Updating..." state, handled by showConnectingProgress() on the auth
    // screen instead). The widgets/logic still exist because
    // setConnectionStatus() also drives switching from the auth screen to
    // the chat page - they are just never shown.
    statusDot_ = new QLabel(header);
    statusDot_->setFixedSize(9, 9);
    statusDot_->hide();
    statusLabel_ = new QLabel("Not connected", header);
    statusLabel_->setObjectName("statusLabel");
    statusLabel_->hide();
    headerLayout->addWidget(companionAvatar_);
    headerLayout->addWidget(companionLabel_);
    headerLayout->addStretch();
    headerLayout->addWidget(searchInput_);
    layout->addWidget(header);

    auto* historyCanvas = new HistoryCanvas(conversationColumn);
    historyCanvas->setObjectName("messages");
    // Set on the viewport, not historyCanvas itself: for a
    // QAbstractScrollArea, mouse events actually land on the viewport
    // child widget, so that is where a context-menu policy/signal has to
    // be attached for showMessageContextMenu()'s pos to come through
    // reliably in viewport-local coordinates, matching what
    // HistoryCanvas::itemAt() expects.
    historyCanvas->viewport()->setContextMenuPolicy(Qt::CustomContextMenu);
    // See chatWallpaperTile() - QPalette::Base is what a
    // QAbstractScrollArea's viewport actually paints its background with.
    {
        QPalette wallpaperPalette = historyCanvas->viewport()->palette();
        wallpaperPalette.setBrush(QPalette::Base, QBrush(chatWallpaperTile()));
        historyCanvas->viewport()->setPalette(wallpaperPalette);
        historyCanvas->viewport()->setAutoFillBackground(true);
    }
    messages_ = historyCanvas;
    layout->addWidget(messages_);

    // Same trigger point as tdesktop's HistoryView::ListWidget detecting
    // the top of the scroll area reached (see maybeLoadMoreHistory()) -
    // a small threshold, not scrollbar value == 0 exactly, so the request
    // fires a little before the user actually hits the very top.
    connect(historyCanvas->verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int value) {
        if (value <= 40) {
            maybeLoadMoreHistory();
        }
    });

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
    writeRestrictionBar_ = new QWidget(conversationColumn);
    QWidget* startEncryptionRow = writeRestrictionBar_;
    startEncryptionRow->setObjectName("writeRestrictionBar");
    startEncryptionRow->setFixedHeight(46);
    auto* startEncryptionRowLayout = new QHBoxLayout(startEncryptionRow);
    startEncryptionRowLayout->setContentsMargins(12, 0, 12, 0);
    plainSendWarning_ = new QLabel("Not encrypted - sent as plain Telegram text", startEncryptionRow);
    plainSendWarning_->setObjectName("plainSendWarning");
    startEncryptionRowLayout->addWidget(plainSendWarning_);
    startEncryptionRowLayout->addStretch();
    startEncryptionButton_ = new RippleButton(QIcon(), "Start encrypted session", startEncryptionRow);
    startEncryptionButton_->setObjectName("startEncryptionButton");
    startEncryptionButton_->setFixedHeight(30);
    startEncryptionRowLayout->addWidget(startEncryptionButton_);
    layout->addWidget(startEncryptionRow);

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
    auto* inputRow = new QWidget(conversationColumn);
    composeRow_ = inputRow;
    inputRow->setObjectName("composeBar");
    inputRow->setFixedHeight(54);
    auto* inputLayout = new QHBoxLayout(inputRow);
    inputLayout->setContentsMargins(2, 0, 2, 0);
    inputLayout->setSpacing(0);
    sendFileButton_ = new RippleButton(glyphIcon(Glyph::Attach, kIconFg), QString(), inputRow);
    sendFileButton_->setObjectName("composeIconButton");
    sendFileButton_->setFixedSize(44, 46);
    sendFileButton_->setToolTip("Send a file");
    input_ = new ChatInput(inputRow);
    input_->setObjectName("composeField");
    // One button, not a separate always-visible mic button next to a
    // separate always-visible send button - see sendButton_'s own header
    // comment for the citation. Starts in its mic state (field starts
    // empty); updateSendButtonIcon() swaps it as the field's text
    // changes.
    sendButton_ = new RippleButton(glyphIcon(Glyph::Mic, kIconFg), QString(), inputRow);
    sendButton_->setObjectName("composeIconButton");
    sendButton_->setFixedSize(44, 46);
    sendButton_->setToolTip("Record a voice message");
    inputLayout->addWidget(sendFileButton_, 0, Qt::AlignBottom);
    inputLayout->addWidget(input_, 1, Qt::AlignVCenter);
    inputLayout->addWidget(sendButton_, 0, Qt::AlignBottom);
    layout->addWidget(inputRow);

    // Панель записи: занимает то же место и ту же высоту, что и строка
    // ввода, и показывается вместо неё (см. onRecorderStateChanged) - как
    // VoiceRecordBar в ComposeControls у Telegram Desktop. Слева корзина
    // (отменить и выбросить), затем мигающая красная точка и таймер,
    // справа пауза и отправка.
    recordBar_ = new QWidget(conversationColumn);
    recordBar_->setObjectName("recordBar");
    recordBar_->setFixedHeight(54);
    recordBar_->hide();
    auto* recordLayout = new QHBoxLayout(recordBar_);
    recordLayout->setContentsMargins(2, 0, 2, 0);
    recordLayout->setSpacing(0);

    cancelRecordButton_ = new RippleButton(glyphIcon(Glyph::Trash, kIconDestructiveFg), QString(), recordBar_);
    cancelRecordButton_->setObjectName("composeIconButton");
    cancelRecordButton_->setFixedSize(44, 46);
    cancelRecordButton_->setToolTip("Discard recording");

    recordDot_ = new QLabel(recordBar_);
    recordDot_->setObjectName("recordDot");
    recordDot_->setFixedSize(10, 10);
    recordDot_->setPixmap(dotPixmap(10, QColor("#e41e3f")));

    recordTime_ = new QLabel("0:00,00", recordBar_);
    recordTime_->setObjectName("recordTime");

    pauseRecordButton_ = new RippleButton(glyphIcon(Glyph::Pause, kIconFg), QString(), recordBar_);
    pauseRecordButton_->setObjectName("composeIconButton");
    pauseRecordButton_->setFixedSize(44, 46);
    pauseRecordButton_->setToolTip("Pause recording");

    sendRecordButton_ = new RippleButton(glyphIcon(Glyph::Send, kIconAccentFg), QString(), recordBar_);
    sendRecordButton_->setObjectName("composeIconButton");
    sendRecordButton_->setFixedSize(44, 46);
    sendRecordButton_->setToolTip("Stop and send");

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

    // Colors/spacing live in src/ui/qt/style.qss, not here - edit that file
    // to restyle the window, no rebuild needed.
    setStyleSheet(loadStylesheet());

    connect(sendButton_, &QPushButton::clicked, this, &MainWindow::onSendOrMicClicked);
    connect(sendFileButton_, &QPushButton::clicked, this, &MainWindow::onSendFileClicked);
    connect(cancelRecordButton_, &QPushButton::clicked, this, &MainWindow::onCancelRecordingClicked);
    connect(pauseRecordButton_, &QPushButton::clicked, this, &MainWindow::onPauseRecordingClicked);
    connect(sendRecordButton_, &QPushButton::clicked, this, &MainWindow::onSendRecordingClicked);
    connect(recordTimer_, &QTimer::timeout, this, &MainWindow::onRecordingTick);
    connect(input_, &ChatInput::submitted, this, &MainWindow::onSendClicked);
    connect(input_, &QPlainTextEdit::textChanged, this, &MainWindow::updateSendButtonIcon);
    connect(historyCanvas->viewport(), &QWidget::customContextMenuRequested, this, &MainWindow::showMessageContextMenu);
    connect(recorder_, &QMediaRecorder::recorderStateChanged, this, &MainWindow::onRecorderStateChanged);
    connect(searchInput_, &QLineEdit::textChanged, this, &MainWindow::onSearchTextChanged);
    connect(startEncryptionButton_, &QPushButton::clicked, this, &MainWindow::onStartEncryptionClicked);
    connect(chatSearchInput_, &QLineEdit::textChanged, this, &MainWindow::onChatSearchTextChanged);
    connect(searchDebounceTimer_, &QTimer::timeout, this, &MainWindow::onChatSearchDebounceTimeout);
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
    asSidebar(chatListWidget_)->setRows(chats, currentChatId_);
    // A rebuild (this runs on every chat-list push from TDLib, including
    // the burst of updates while avatars are downloading right after
    // login) can leave the scroll position sitting past the end of the
    // newly rebuilt (possibly shorter, momentarily) list - the sidebar
    // then looks completely empty even though every row is really there,
    // just scrolled out of view. Reset to the top every time so this can
    // never happen.
    asSidebar(chatListWidget_)->scrollToTop();
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
    lastChatListSnapshot_ = chats;
    updateConversationMetadata(chats);
    // A background chat-list push (new message, reordering, ...) must not
    // visually clobber search results currently on screen - it still
    // updates lastChatListSnapshot_/the title caches above, just does not
    // re-render until the search box is cleared.
    if (chatSearchInput_->text().trimmed().isEmpty()) {
        // Throttled, not called directly: TelegramClient::requestMoreChats()
        // can push updateChatList dozens to hundreds of times in a tight
        // burst while paging through a large account's full chat list (see
        // its own comment in telegram_client.cpp), each carrying every
        // chat known so far - rendering all of that synchronously, every
        // single time, meant tearing down and rebuilding potentially
        // hundreds of sidebar rows (each with its own avatar pixmap decode)
        // over and over, fast enough to stop the GUI thread from
        // processing paint/input events at all and make the window read as
        // "Not Responding". If nothing is already scheduled, render soon
        // (150ms - imperceptible for a one-off update); if a render is
        // already pending, this update's data is already captured in
        // lastChatListSnapshot_ above and will be picked up when that
        // pending render actually runs, so nothing more to do here.
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
    QString query = chatSearchInput_->text().trimmed();
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
            if (chatSearchInput_->text().trimmed() != query) {
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
                 .arg(chatSearchInput_->text()));
    currentChatId_ = chatId;

    // Opening a chat (whether from the normal list or a search result)
    // closes the search, same as tdesktop: back to the regular dialog list
    // underneath whatever was just opened.
    if (!chatSearchInput_->text().isEmpty()) {
        chatSearchInput_->clear();
        renderSidebarRows(lastChatListSnapshot_);
    } else {
        asSidebar(chatListWidget_)->setSelectedChatId(currentChatId_);
    }

    QString title = conversationTitles_.value(currentChatId_, "zkgram");
    companionLabel_->setText(title);
    companionAvatar_->setPixmap(chatAvatarPixmap(title, conversationPhotoPaths_.value(currentChatId_), 38));
    companionAvatar_->show();
    maybeLoadCachedHistory(currentChatId_);
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
        cachedMessages.push_back(StoredMessage{message.isOutgoing ? MessageKind::Outgoing : MessageKind::Incoming,
                                                QString::fromStdString(message.text),
                                                QString::fromStdString(message.photoPath),
                                                static_cast<qlonglong>(message.id),
                                                QString::fromStdString(message.senderName)});
    }
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
                history.push_back(StoredMessage{message.isOutgoing ? MessageKind::Outgoing : MessageKind::Incoming,
                                                 QString::fromStdString(message.text),
                                                 QString::fromStdString(message.photoPath), messageId,
                                                 QString::fromStdString(message.senderName)});
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
        if (asCanvas(messages_)->verticalScrollBar()->maximum() == 0) {
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
    session_->loadMoreMessageHistory(chatId, oldestId, [this, chatId](const std::vector<zkgram::core::PlainMessage>& messages) {
        QMetaObject::invokeMethod(this, [this, chatId, messages] {
            historyLoadingMore_.remove(chatId);
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
                older.push_back(StoredMessage{message.isOutgoing ? MessageKind::Outgoing : MessageKind::Incoming,
                                               QString::fromStdString(message.text),
                                               QString::fromStdString(message.photoPath), messageId,
                                               QString::fromStdString(message.senderName)});
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
                entries.push_back(HistoryEntry{older[i].kind, older[i].text, older[i].filePath, attached[i],
                                                older[i].senderName, showTail, older[i].messageId});
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
    session_->startConversation(chatId);
    conversationActive_[chatId] = true;
    updateConversationControlsVisibility();
    appendMessage(chatId, MessageKind::System, "Starting encrypted session...");

    if (!probingChats_.contains(chatId)) {
        probingChats_.insert(chatId);
        session_->probeZkgramPresence(chatId, [this, chatId](bool present) {
            QMetaObject::invokeMethod(this, [this, chatId, present] {
                probingChats_.remove(chatId);
                if (!present) {
                    appendMessage(chatId, MessageKind::System,
                                  "Note: this contact does not appear to be running zkgram right now - the "
                                  "encrypted session may not complete until they do.");
                }
            });
        });
    }
}

void MainWindow::appendMessage(qlonglong chatId, MessageKind kind, const QString& text, const QString& filePath,
                                qlonglong messageId, const QString& senderName) {
    // Same sender AND same kind, not just same kind: two different
    // people's consecutive messages in a group chat must not read as one
    // attached block with only the first person's name shown.
    bool attached = !conversationMessages_[chatId].isEmpty() && conversationMessages_[chatId].back().kind == kind &&
                     kind != MessageKind::System && conversationMessages_[chatId].back().senderName == senderName;
    conversationMessages_[chatId].push_back(StoredMessage{kind, text, filePath, messageId, senderName});
    if (chatId == currentChatId_) {
        // HistoryCanvas::appendItem() retracts the previous bubble's tail
        // in place if this one attaches to it, and lays out and paints
        // this new one - O(1) amortized, not a full conversation rebuild,
        // which matters for a long-running session with many messages.
        asCanvas(messages_)->appendItem(HistoryEntry{kind, text, filePath, attached, senderName, true, messageId});
        asCanvas(messages_)->scrollToBottom();
    }
    if (kind != MessageKind::System && conversationActive_.value(chatId, false)) {
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
        if (message.kind == MessageKind::System) {
            continue;  // status lines, not real conversation content - see the header comment
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
        renderCurrentConversation();
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
        renderCurrentConversation();
    }
}

void MainWindow::renderCurrentConversation() {
    const QVector<StoredMessage>& conversation = conversationMessages_.value(currentChatId_);

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
                      conversation[i].senderName == conversation[i - 1].senderName;
    }

    QVector<HistoryEntry> entries;
    entries.reserve(conversation.size());
    for (int i = 0; i < conversation.size(); ++i) {
        bool showTail = (i == conversation.size() - 1) || !attached[i + 1];
        entries.push_back(HistoryEntry{conversation[i].kind, conversation[i].text, conversation[i].filePath,
                                        attached[i], conversation[i].senderName, showTail, conversation[i].messageId});
    }
    asCanvas(messages_)->setItems(entries);
    asCanvas(messages_)->scrollToBottom();
}

void MainWindow::appendConversationStatus(qlonglong chatId, const QString& stage, const QString& message) {
    appendMessage(chatId, MessageKind::System, QString("[%1] %2").arg(stage, message));
}

void MainWindow::appendConversationPeerText(qlonglong chatId, const QString& text) {
    appendMessage(chatId, MessageKind::Incoming, text);
}

void MainWindow::appendConversationFileReceived(qlonglong chatId, const QString& filePath) {
    appendMessage(chatId, MessageKind::Incoming, "File: " + filePath, filePath);
}

void MainWindow::appendPlainMessageReceived(qlonglong chatId, qlonglong messageId, const QString& text,
                                             const QString& senderName, const QString& filePath) {
    // Only for chats whose history has already been asked for, i.e. ones
    // the user has opened at least once this run. For any other chat the
    // message is deliberately dropped: its conversation holds nothing yet,
    // and the first thing opening it does is fetch the newest page of
    // history, which contains this message anyway - keeping it here instead
    // would put one lone new message above the whole history that follows.
    if (!historyLoadedChats_.contains(chatId)) {
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
    appendMessage(chatId, MessageKind::Incoming, text, filePath, messageId, senderName);
}

void MainWindow::setConversationReady(qlonglong chatId) {
    conversationActive_[chatId] = true;
    conversationReady_[chatId] = true;
    appendMessage(chatId, MessageKind::System, "Encrypted session ready");
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
    QString text = hit.text;

    QMenu menu(this);
    QAction* copyAction = menu.addAction("Copy text");
    // Real Telegram-style editing (TDLib editMessageText, keyed by message
    // id) is not something crypto::CryptoLayer supports: it is a stream
    // session per companion with no message-id concept. "Edit" here is a
    // local convenience only: it does not change what the companion
    // already received, it just refills the input field so the corrected
    // text can be sent as a new message.
    QAction* editAction = (kind == MessageKind::Outgoing) ? menu.addAction("Edit") : nullptr;
    // "Forward" resends the text to the currently open conversation as a
    // new message, marked as forwarded - picking a different destination
    // chat is not implemented (would need its own picker UI).
    QAction* forwardAction = menu.addAction("Forward");

    QAction* chosen = menu.exec(asCanvas(messages_)->viewport()->mapToGlobal(pos));
    if (chosen == nullptr) {
        return;
    }
    if (chosen == copyAction) {
        QGuiApplication::clipboard()->setText(text);
    } else if (chosen == editAction) {
        input_->setPlainText(text);
        input_->setFocus();
    } else if (chosen == forwardAction && session_ != nullptr && currentChatId_ != 0) {
        QString forwarded = "Forwarded: " + text;
        session_->sendText(currentChatId_, forwarded.toStdString());
        appendMessage(currentChatId_, MessageKind::Outgoing, forwarded);
    }
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
    QString title = "Your phone number";
    QString subtitle = "Please confirm your country code\nand enter your phone number.";
    QString placeholder = "Phone number";
    AuthSlide::Validator validator;
    if (step.contains("code", Qt::CaseInsensitive)) {
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
    QString result = authSlide_->runSlide(title, subtitle, placeholder, isPassword, validator, lastError);
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
    try {
        if (isActive) {
            session_->sendText(currentChatId_, text.toStdString());
        } else {
            session_->sendPlainText(currentChatId_, text.toStdString());
        }
    } catch (const std::exception& e) {
        appendMessage(currentChatId_, MessageKind::System, QString("Send failed: %1").arg(e.what()));
        return;
    }
    appendMessage(currentChatId_, MessageKind::Outgoing, text);
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
        appendMessage(currentChatId_, MessageKind::System, QString("Send failed: %1").arg(e.what()));
        return;
    }
    appendMessage(currentChatId_, MessageKind::Outgoing, label + filePath, filePath);
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
    QMenu menu(this);
    QAction* photoAction = menu.addAction(glyphIcon(Glyph::Photo, kIconFg), "Photo or Video");
    QAction* documentAction = menu.addAction(glyphIcon(Glyph::Document, kIconFg), "Document");

    // Меню раскрывается вверх от кнопки, а не вниз: скрепка стоит в самом
    // низу окна, и вниз ему просто некуда развернуться.
    QPoint anchor = sendFileButton_->mapToGlobal(QPoint(0, 0));
    QAction* chosen = menu.exec(QPoint(anchor.x(), anchor.y() - menu.sizeHint().height()));
    if (chosen == nullptr) {
        return;
    }

    QString filePath;
    if (chosen == photoAction) {
        filePath = QFileDialog::getOpenFileName(
            this, "Send photo or video", QString(),
            "Photos and videos (*.jpg *.jpeg *.png *.gif *.webp *.bmp *.mp4 *.mov *.mkv *.avi *.webm);;"
            "All files (*)");
    } else if (chosen == documentAction) {
        filePath = QFileDialog::getOpenFileName(this, "Send document");
    }
    if (filePath.isEmpty()) {
        return;
    }
    sendFilePath(filePath, "File: ");
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
        pauseRecordButton_->setIcon(glyphIcon(Glyph::Pause, kIconFg));
        pauseRecordButton_->setToolTip("Pause recording");
    } else if (paused) {
        recordAccumulatedMs_ += recordElapsed_.elapsed();
        pauseRecordButton_->setIcon(glyphIcon(Glyph::Play, kIconFg));
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
    sendButton_->setIcon(hasText ? glyphIcon(Glyph::Send, kIconAccentFg) : glyphIcon(Glyph::Mic, kIconFg));
    sendButton_->setToolTip(hasText ? "Send" : "Record a voice message");
}

void MainWindow::onSearchTextChanged(const QString& query) {
    // Hides non-matching rows in place rather than rebuilding the list -
    // this is filtering the currently open conversation, not a real
    // cross-chat search (see the sidebar for that).
    asCanvas(messages_)->setSearchFilter(query);
}

}  // namespace zkgram::ui::qt
