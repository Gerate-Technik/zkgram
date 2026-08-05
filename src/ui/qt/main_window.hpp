#pragma once

#include <QElapsedTimer>
#include <QMainWindow>
#include <QMap>
#include <QPlainTextEdit>
#include <QPoint>
#include <QSet>
#include <QString>
#include <QVariantList>
#include <QVector>
#include <QWidget>

#include <cstdint>
#include <functional>

QT_BEGIN_NAMESPACE
class QLineEdit;
class QLabel;
class QPushButton;
class QStackedWidget;
class QAudioInput;
class QMediaCaptureSession;
class QMediaRecorder;
class QKeyEvent;
class QTimer;
class QMenu;
// Здесь, на глобальной области видимости, вместе с остальными Qt-классами,
// а не через "class QEventLoop* loop_" прямо в объявлении поля ниже: там
// это elaborated-type-specifier внутри namespace zkgram::ui::qt, то есть
// объявление отдельного типа zkgram::ui::qt::QEventLoop, который никогда
// не определяется. MSVC с нестрогим lookup всё равно находил глобальный
// ::QEventLoop и собирал, а GCC/Clang честно падают на incomplete type.
class QEventLoop;
QT_END_NAMESPACE

namespace zkgram::core {
class Session;
}

namespace zkgram::ui::qt {

// Fixed-height multi-line input (Telegram-style): Enter sends (emits
// submitted(), does not insert a newline), Shift+Enter inserts a newline.
// A plain QLineEdit cannot do this (no multi-line, no way to distinguish
// Enter from Shift+Enter), hence the small subclass instead of a signal
// connected to QLineEdit::returnPressed.
class ChatInput : public QPlainTextEdit {
    Q_OBJECT

public:
    explicit ChatInput(QWidget* parent = nullptr);

Q_SIGNALS:
    void submitted();

protected:
    void keyPressEvent(QKeyEvent* event) override;
};

// One full-window onboarding step, styled after the tdesktop intro screen
// (Telegram/SourceFiles/ui in the official client): a big round logo, a
// title, a subtitle, one input field, and a "Next" button, centered in the
// window. The same instance is reused for every step (local password,
// phone number, login code, 2FA password) - only the text/echo mode
// changes, see runSlide(). This mirrors how tdesktop's own intro widget
// reuses one layout across its steps rather than allocating a new page per
// step.
class AuthSlide : public QWidget {
    Q_OBJECT

public:
    explicit AuthSlide(QWidget* parent = nullptr);

    // Returns an error string to show under the field, or an empty string
    // if the value is acceptable - Next is refused (input stays focused)
    // until it returns empty. Optional; omit for fields with no format to
    // check (free-form passwords).
    using Validator = std::function<QString(const QString&)>;

    // Blocks (via a local QEventLoop, like QInputDialog::getText does)
    // until the user submits or the window is closed out from under it.
    // Called from the GUI thread only - see MainWindow::runStartupWizard()
    // and MainWindow::showRequestCredentialDialog().
    // initialError, если не пуст, сразу показывается под полем ввода: так
    // повторный запрос того же шага (неверный код, неверный 2FA-пароль -
    // см. TelegramClient::sendAuthRequestWithRetry) объясняет пользователю,
    // почему его спрашивают снова, а не выглядит как зависший экран.
    QString runSlide(const QString& title, const QString& subtitle, const QString& placeholder, bool masked,
                      Validator validator = {}, const QString& initialError = {});

    // Non-blocking: shows a title/subtitle with no input field or Next
    // button, for phases with nothing to ask the user but that are not
    // instant either (TDLib login taking a while). Without this the window
    // used to just sit on the last credential slide forever with zero
    // indication anything was still happening.
    void showStatus(const QString& title, const QString& subtitle);

    // Unblocks a pending runSlide() with an empty result - called when the
    // window is closed while a slide is waiting on input.
    void cancelPending();

private Q_SLOTS:
    void onNextClicked();

private:
    QLabel* logo_;
    QLabel* title_;
    QLabel* subtitle_;
    QLineEdit* input_;
    QLabel* error_;
    QPushButton* next_;
    QString result_;
    Validator validator_;
    QEventLoop* loop_ = nullptr;
};

// Outgoing/Incoming carry real sent/received text (edit/forward act on
// them); System is for connection status lines and has no actions.
// Namespace scope, not nested in MainWindow: main_window.cpp's own
// HistoryCanvas (the message list's custom-painted canvas, a free
// standing class, not a MainWindow member) needs to see this type too.
enum class MessageKind { Outgoing, Incoming, System };

// Multi-conversation chat window: a sidebar with every Telegram chat (from
// TDLib's own chat list, see core::Session::start()) and, on the right,
// whichever chat is currently selected - either its encrypted message
// history (bubbles, own/peer/system, images shown inline), or a "Start
// encrypted session" prompt if no crypto::CryptoLayer conversation has
// been started for it yet (see core::Session::startConversation - this is
// the explicit per-chat opt-in that replaced the old single
// pre-configured companion, see TODO.md "мультиюзерность"). Right-click on
// a message opens Copy/Edit/Forward. Q_INVOKABLE methods are called by
// QtUiProvider via QMetaObject::invokeMethod from a non-GUI thread, see
// qt_ui_provider.hpp.
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);

    // Session is created after MainWindow (it needs the UiProvider, which
    // holds a pointer to this window), so the link is set up separately.
    void setSession(zkgram::core::Session* session);

    // One full-window onboarding slide (local CryptoLayer password) shown
    // before core::Session exists, replacing the old startup QDialog -
    // runs on the GUI thread, blocking like a modal dialog would, via
    // AuthSlide::runSlide(). Returns accepted=false if the window was
    // closed before it was entered. No companion id slide anymore - chats
    // are picked from the sidebar once logged in, not pinned up front.
    struct StartupCredentials {
        QString password;
        bool accepted = false;
    };
    // dataDir must be the same absolute path passed to core::Session's
    // constructor - used only to check whether this is a returning
    // identity (dataDir/sign_private already exists) so the slide can ask
    // "Enter your password" instead of "Create a password". See main_qt.cpp
    // for why this has to be an absolute, exe-relative path rather than a
    // bare "data" resolved against whatever the current working directory
    // happens to be at launch.
    StartupCredentials runStartupWizard(const QString& dataDir);

    // CryptoLayer::Callbacks::updateStatus (stage/message, e.g. "Encryption
    // / Waiting for companion public key...") - only actually visible to
    // the user while still on the auth screen (see AuthSlide::showStatus);
    // once chatPage_ is showing, appendConversationStatus already covers
    // this via QtUiProvider::onStatus.
    Q_INVOKABLE void showConnectingProgress(const QString& stage, const QString& message);

    // text/colorHex describe the TDLib connection state shown as a status
    // pill in the header (see MainWindow::MainWindow for the color
    // palette) - this is about the one Telegram login, not any individual
    // conversation (see the per-chat methods below for those).
    Q_INVOKABLE void setConnectionStatus(const QString& text, const QString& colorHex);

    // Rebuilds the sidebar from a fresh chat list snapshot. Each entry is
    // a QVariantMap{id: qlonglong, title: QString, preview: QString,
    // unread: int, active: bool} - see QtUiProvider::onChatListUpdated for
    // why QVariantList/QVariantMap instead of a custom struct type.
    Q_INVOKABLE void updateChatList(QVariantList chats);

    // A history photo that was still downloading when its message list was
    // first shown (see maybeLoadPlainHistory()) just finished - finds the
    // matching StoredMessage by messageId and re-renders the conversation
    // so it shows the real image instead of its "Photo" placeholder text.
    Q_INVOKABLE void updateHistoryPhoto(qlonglong chatId, qlonglong messageId, const QString& path);

    // A history message's sender name resolved after the fact (see
    // maybeLoadPlainHistory()) - same idea as updateHistoryPhoto(), just
    // for the sender label shown above an incoming message's text instead
    // of the bubble's own content.
    Q_INVOKABLE void updateHistorySenderName(qlonglong chatId, qlonglong messageId, const QString& name);

    Q_INVOKABLE void appendConversationStatus(qlonglong chatId, const QString& stage, const QString& message);
    Q_INVOKABLE void appendConversationPeerText(qlonglong chatId, const QString& text);
    Q_INVOKABLE void appendConversationFileReceived(qlonglong chatId, const QString& filePath);
    Q_INVOKABLE void setConversationReady(qlonglong chatId);

    // Called by QtUiProvider via BlockingQueuedConnection: show a modal
    // dialog on the GUI thread and return the user's answer.
    Q_INVOKABLE bool showConfirmSignaturesDialog(qlonglong chatId, const QString& mySign,
                                                  const QString& companionSign);
    Q_INVOKABLE QString showRequestCredentialDialog(const QString& prompt, const QString& dataType);

private Q_SLOTS:
    // Q_SLOTS, not the "slots" keyword macro: this header ends up in the
    // same translation unit as Python.h (via core/session.hpp), which
    // declares its own struct field named "slots" and collides with it.
    // sendButton_'s one and only click handler - dispatches to either
    // onSendClicked() or onMicClicked() depending on whether the field
    // has text, same as real Telegram Desktop's single send/mic button
    // (see the sendButton_ comment in the header).
    void onSendOrMicClicked();
    void onSendClicked();
    void onSendFileClicked();
    void onMicClicked();
    void onRecorderStateChanged();
    // Кнопки панели записи (см. recordBar_): отмена выбрасывает запись, не
    // отправляя её, пауза/продолжение переключают QMediaRecorder, отправка
    // останавливает запись и уходит обычным путём sendFilePath().
    void onCancelRecordingClicked();
    void onPauseRecordingClicked();
    void onSendRecordingClicked();
    void onRecordingTick();
    void updateSendButtonIcon();
    void onSearchTextChanged(const QString& query);
    void showMessageContextMenu(const QPoint& pos);
    void onStartEncryptionClicked();
    void onChatSearchTextChanged(const QString& query);
    void onChatSearchDebounceTimeout();

protected:
    void closeEvent(class QCloseEvent* event) override;

private:
    struct StoredMessage {
        MessageKind kind;
        QString text;
        QString filePath;
        // TDLib message id, only meaningful for a plain-history message
        // whose photo was still downloading when it was first shown (see
        // updateHistoryPhoto()) - 0 for everything else (CryptoLayer
        // messages have no TDLib id to speak of here).
        qlonglong messageId = 0;
        // Sender's display name, incoming plain-history messages only -
        // empty for everything else (outgoing, System, or not yet
        // resolved, see updateHistorySenderName()).
        QString senderName;
    };

    // Appends to conversationMessages_[chatId] always; only actually drawn
    // into the visible messages_ list if chatId is the one currently
    // selected in the sidebar (see currentChatId_/renderCurrentConversation).
    void appendMessage(qlonglong chatId, MessageKind kind, const QString& text, const QString& filePath = QString(),
                        qlonglong messageId = 0, const QString& senderName = QString());
    // Rebuilds messages_ (a HistoryCanvas, see main_window.cpp) from
    // conversationMessages_[currentChatId_] - "attached"/"showTail" per
    // message (whether it joins the previous bubble/gets the pointed
    // tail, matching tdesktop's own Message::isAttachedToPrevious /
    // countMessageRounding) are computed here, layout and painting are
    // entirely HistoryCanvas's own job.
    void renderCurrentConversation();
    void sendFilePath(const QString& filePath, const QString& label);
    void setControlsEnabled(bool enabled);
    void updateConversationControlsVisibility();

    // Shared by updateChatList() and the live search results
    // (onChatSearchDebounceTimeout()) - both deal in the same
    // QVariantList{id, title, preview, unread, active} shape (see
    // QtUiProvider::onChatListUpdated), just from a different source.
    // updateConversationMetadata() only updates the title/active caches;
    // renderSidebarRows() only rebuilds chatListWidget_'s rows - kept
    // separate so a background chat-list push received while a search is
    // showing can update the caches without visually clobbering the
    // search results on screen.
    void updateConversationMetadata(const QVariantList& chats);
    void renderSidebarRows(const QVariantList& chats);
    // Actually calls renderSidebarRows(lastChatListSnapshot_) - the target
    // of sidebarRenderThrottleTimer_, see updateChatList().
    void flushSidebarRender();
    // Called from a plain std::function callback set on chatListWidget_
    // (a SidebarCanvas, see main_window.cpp), not a Qt signal connection -
    // SidebarCanvas has no signals of its own to connect to.
    void onChatListItemClicked(qlonglong chatId);

    // Fetches and renders a chat's plain-text history the first time it is
    // opened, if it has no encrypted session (see
    // core::Session::loadMessageHistory) - a no-op every subsequent time
    // that chat is reselected, or if it already has an active conversation.
    void maybeLoadPlainHistory(qlonglong chatId);

    // Fetches the next older page once the message list is scrolled near
    // its top, for the currently selected chat - see
    // core::Session::loadMoreMessageHistory()/historyOldestMessageId_.
    // Connected to messages_'s vertical scrollbar in the constructor.
    void maybeLoadMoreHistory();

    // maybeLoadMoreHistory() above only ever fires from the scrollbar
    // actually moving - if the loaded messages fit entirely within the
    // visible list (a short conversation, or just a tall window), there is
    // no scrollbar range to move at all, so that signal never fires and
    // the user has no way to ever see anything older than whatever
    // happened to fit on screen. Called after every history render
    // (initial load and each page loaded by maybeLoadMoreHistory itself)
    // to keep proactively loading more, deferred one event loop turn so
    // the scrollbar's range reflects the render that just happened, until
    // either there is something to scroll or history is exhausted.
    void ensureHistoryFillsViewport();

    zkgram::core::Session* session_ = nullptr;

    // centralWidget() is this stack: authSlide_ (index 0) while
    // logging in, chatPage_ (index 1) once TDLib login finishes (see
    // setConnectionStatus) - mirrors tdesktop replacing its whole intro
    // widget with the main window contents after login, rather than
    // popping dialogs on top of an already-visible chat window.
    QStackedWidget* centralStack_;
    AuthSlide* authSlide_;
    QWidget* chatPage_;

    // ---- sidebar (chat list) ----
    // Actually a SidebarCanvas* (main_window.cpp's own custom-painted,
    // virtualized chat list widget) - declared as the generic QWidget
    // base for the same reason messages_ is, see that member's own
    // comment. Every real use is in main_window.cpp, via a static_cast
    // back to SidebarCanvas.
    QWidget* chatListWidget_;
    // Live search-as-you-type over chats/contacts/global usernames (see
    // core::Session::searchChats) - replaces the old separate "username or
    // +phone" input plus a "New chat" button: typing here is enough, no
    // extra click needed, matching tdesktop's own dialogs search box.
    QLineEdit* chatSearchInput_;
    QTimer* searchDebounceTimer_;
    // Throttles renderSidebarRows() during a burst of chat-list pushes
    // (initial login, hundreds of chats streaming in page by page via
    // TelegramClient::requestMoreChats(), each with its own avatar
    // download completing independently) - without this, every single
    // push tore the whole sidebar down and rebuilt every row (and
    // recomputed every avatar pixmap) from scratch, dozens to hundreds of
    // times in rapid succession, which was enough to make the GUI thread
    // stop processing events entirely and the window read as "Not
    // Responding" to Windows. See updateChatList()/flushSidebarRender().
    QTimer* sidebarRenderThrottleTimer_;
    // The last real chat-list snapshot from updateChatList(), restored into
    // chatListWidget_ once the search box is cleared - search results
    // replace what chatListWidget_ shows, they never replace this cache.
    QVariantList lastChatListSnapshot_;

    // ---- currently selected conversation ----
    qlonglong currentChatId_ = 0;
    // Actually a HistoryCanvas* (main_window.cpp's own custom-painted
    // message list widget, see its class comment there) - declared here
    // as the generic QWidget base so this header does not need to expose
    // HistoryCanvas's own internals (HistoryItem, QTextDocument, ...).
    // Every real use is inside main_window.cpp, in the same translation
    // unit as HistoryCanvas's definition, via a static_cast back to it.
    QWidget* messages_;
    // Contains plainSendWarning_ and startEncryptionButton_ - hidden as a
    // whole (not just its children) when there is an active session, or
    // it would sit there as an empty 46px white bar. See
    // updateConversationControlsVisibility().
    QWidget* writeRestrictionBar_;
    // Shown alongside the input row (not instead of it) when the selected
    // chat has no encrypted session - see updateConversationControlsVisibility().
    QPushButton* startEncryptionButton_;
    // Shown above the input row whenever it is in "plain send" mode (see
    // onSendClicked()) - messages typed there go straight to Telegram
    // unencrypted, this is the one and only place that is made obvious,
    // since silently sending unencrypted from an app whose whole point is
    // encryption would be a real trust problem, not just a missing label.
    QLabel* plainSendWarning_;
    ChatInput* input_;
    // One button, not two - real Telegram Desktop's composer has a single
    // button that shows a mic icon when the field is empty and a send
    // icon once there is text to send (see Ui::Controls::SendButton in
    // telegram_fork/tdesktop's ui/controls/send_button.h/.cpp), not a
    // separate always-visible mic button next to an always-visible send
    // button.
    QPushButton* sendButton_;
    QPushButton* sendFileButton_;
    QLineEdit* searchInput_;
    QLabel* companionAvatar_;
    QLabel* companionLabel_;
    QLabel* statusDot_;
    QLabel* statusLabel_;

    // Per-chat state, keyed by chat id - lets switching the sidebar
    // selection redraw instantly without re-fetching anything.
    QMap<qlonglong, QVector<StoredMessage>> conversationMessages_;
    QMap<qlonglong, bool> conversationActive_;
    // Set only once setConversationReady() actually fires (the crypto
    // handshake finished), not the moment "Start encrypted session" is
    // clicked - conversationActive_ alone used to gate showing the input
    // row, so a message could be sent (and crash CryptoLayer, whose AES
    // key is still None mid handshake) before the companion's key had even
    // arrived. See TODO.md/session notes on the sendText-before-ready bug.
    QMap<qlonglong, bool> conversationReady_;
    QMap<qlonglong, QString> conversationTitles_;
    // True for a broadcast channel - gates the "Start encrypted session"
    // button off entirely, see updateConversationControlsVisibility().
    QMap<qlonglong, bool> conversationIsChannel_;
    QMap<qlonglong, QString> conversationPhotoPaths_;
    // Chats currently waiting on a probeZkgramPresence() answer - guards
    // against a second click starting a duplicate probe for the same chat
    // while one is already in flight, see onStartEncryptionClicked().
    QSet<qlonglong> probingChats_;
    // Chats whose plain-text history has already been requested (or is in
    // flight) - loadMessageHistory() is only ever called once per chat per
    // session, not every time it is reselected in the sidebar, see
    // onChatListItemClicked().
    QSet<qlonglong> historyLoadedChats_;
    // id of the oldest plain-history message currently shown per chat (0 =
    // none loaded, or history came back empty) - the pagination anchor for
    // maybeLoadMoreHistory(), same role as tdesktop's own oldest-loaded
    // tracking in History::loadBack() (see telegram_fork/tdesktop's
    // history/history.cpp).
    QMap<qlonglong, qlonglong> historyOldestMessageId_;
    // Set once a load-more request for a chat came back empty - there is
    // no more history before historyOldestMessageId_, stop asking.
    QSet<qlonglong> historyExhausted_;
    // In-flight guard so scrolling near the top repeatedly while a page is
    // still loading does not fire overlapping requests.
    QSet<qlonglong> historyLoadingMore_;
    // Counts ensureHistoryFillsViewport()'s own auto-triggered loads per
    // chat - a safety cap, not a normal stopping point. Each round
    // re-renders the whole (ever-growing) message list from scratch, so
    // an unbounded loop here would be the same O(n^2) growth pattern
    // already found and fixed once for the sidebar (see
    // TelegramClient::requestMoreChats()'s own cap in
    // telegram_client.cpp) - capped well past what any reasonable window
    // size could ever need (500 extra messages) so it never limits a
    // real conversation, only a genuine runaway.
    QMap<qlonglong, int> historyAutoFillRounds_;

    QAudioInput* audioInput_;
    QMediaCaptureSession* captureSession_;
    QMediaRecorder* recorder_;
    QString voiceFilePath_;

    // VOICE
    QWidget* composeRow_;
    QWidget* recordBar_;
    QLabel* recordDot_;
    QLabel* recordTime_;
    QPushButton* cancelRecordButton_;
    QPushButton* pauseRecordButton_;
    QPushButton* sendRecordButton_;
    QTimer* recordTimer_;
    QElapsedTimer recordElapsed_;
    qint64 recordAccumulatedMs_ = 0;
    bool recordDotVisible_ = true;
    bool recordCancelled_ = false;

    bool connected_ = false;
};

}  // namespace zkgram::ui::qt
