#pragma once

#include <atomic>

#include <QObject>

#include "core/ui_provider.hpp"

namespace zkgram::ui::qt {

class MainWindow;

// UiProvider реализация поверх Qt Widgets. Методы UiProvider вызываются
// из цепочки crypto/GIL-колбэков, не из Qt GUI-потока, поэтому каждый
// override маршалит вызов на GUI-поток через QMetaObject::invokeMethod
// (см. docs/README.md, раздел про UI-плагины).
class QtUiProvider : public QObject, public zkgram::core::UiProvider {
    Q_OBJECT

public:
    explicit QtUiProvider(MainWindow* window);

    void onChatListUpdated(const std::vector<zkgram::core::ChatListEntry>& chats) override;

    void onStatus(zkgram::core::ConversationId conversation, const std::string& stage,
                  const std::string& message) override;
    void onTextReceived(zkgram::core::ConversationId conversation, const std::string& text) override;
    void onFileReceived(zkgram::core::ConversationId conversation, const std::string& filePath) override;
    void onPlainMessageReceived(zkgram::core::ConversationId conversation,
                                 const zkgram::core::PlainMessage& message) override;
    void onHistoryPhotoReady(zkgram::core::ConversationId conversation, std::int64_t messageId,
                              const std::string& path) override;
    void onHistorySenderNameReady(zkgram::core::ConversationId conversation, std::int64_t messageId,
                                   const std::string& name) override;
    void onOutgoingReadUpTo(zkgram::core::ConversationId conversation, std::int64_t messageId) override;
    void onHistoryVoiceReady(zkgram::core::ConversationId conversation, std::int64_t messageId,
                              const std::string& path) override;

    void onReady(zkgram::core::ConversationId conversation) override;
    void onPingTimeout(zkgram::core::ConversationId conversation) override;
    void onDisconnect(zkgram::core::ConversationId conversation) override;

    // Блокирующий маршалинг на GUI-поток: показывает модальный QDialog и
    // ждёт ответа пользователя, прежде чем вернуть значение вызывающему
    // (crypto) потоку.
    bool confirmSignatures(zkgram::core::ConversationId conversation, const std::string& mySign,
                            const std::string& companionSign) override;
    std::string requestCredential(const std::string& prompt, const std::string& dataType) override;

    // Called once the GUI event loop has ended, before the session is torn
    // down, so the two blocking overrides above stop waiting on it.
    //
    // Both marshal with Qt::BlockingQueuedConnection, which parks the
    // calling thread until the GUI thread runs the invocation. After
    // QApplication::exec() returns there is no such thread any more, so a
    // call arriving then waits forever - and the caller is TDLib's receive
    // thread, which Session::stop() is at that moment trying to join. The
    // process would sit there, never exiting, until it was killed. Closing
    // the window mid-login is exactly when this happens: TDLib is blocked
    // asking for the phone number or the code.
    //
    // After this is called, requestCredential() returns an empty string and
    // confirmSignatures() returns false. Both already mean "the user
    // cancelled" to their callers (see sendAuthRequestWithRetry's own
    // comment on empty input in telegram_client.cpp), which unwinds the
    // login and lets the receive loop exit so the join can finish.
    void beginShutdown();

private:
    MainWindow* window_;
    std::atomic<bool> shuttingDown_{false};
};

}  // namespace zkgram::ui::qt
