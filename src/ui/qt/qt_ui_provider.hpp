#pragma once

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

    void onReady(zkgram::core::ConversationId conversation) override;
    void onPingTimeout(zkgram::core::ConversationId conversation) override;
    void onDisconnect(zkgram::core::ConversationId conversation) override;

    // Блокирующий маршалинг на GUI-поток: показывает модальный QDialog и
    // ждёт ответа пользователя, прежде чем вернуть значение вызывающему
    // (crypto) потоку.
    bool confirmSignatures(zkgram::core::ConversationId conversation, const std::string& mySign,
                            const std::string& companionSign) override;
    std::string requestCredential(const std::string& prompt, const std::string& dataType) override;

private:
    MainWindow* window_;
};

}  // namespace zkgram::ui::qt
