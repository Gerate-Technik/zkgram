#include "ui/qt/qt_ui_provider.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QMetaObject>
#include <QString>
#include <QTextStream>
#include <QThread>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>

#include "ui/qt/main_window.hpp"

namespace zkgram::ui::qt {

namespace {

// Void signals can always be queued: the GUI thread will process them on
// its next event loop iteration, even when called from that same thread.
constexpr Qt::ConnectionType kFireAndForget = Qt::QueuedConnection;

// Temporary diagnostic-only logging, same as main_window.cpp's logDebug()
// (a separate copy, not shared, purely because it lives in a different
// anonymous namespace/.cpp - remove both together once the Next-button
// bug is found, see TODO.md).
void logDebug(const QString& message) {
    QFile file(QCoreApplication::applicationDirPath() + "/zkgram_debug.log");
    if (file.open(QIODevice::Append | QIODevice::Text)) {
        QTextStream(&file) << QDateTime::currentDateTime().toString("HH:mm:ss.zzz") << " ["
                            << QThread::currentThread() << "] " << message << "\n";
    }
}

}  // namespace

QtUiProvider::QtUiProvider(MainWindow* window) : window_(window) {}

void QtUiProvider::onChatListUpdated(const std::vector<zkgram::core::ChatListEntry>& chats) {
    // QVariantList/QVariantMap, not a custom struct type: both are already
    // registered Qt meta types, so this crosses QMetaObject::invokeMethod's
    // Q_ARG boundary with no extra qRegisterMetaType boilerplate.
    QVariantList list;
    list.reserve(static_cast<int>(chats.size()));
    for (const auto& chat : chats) {
        QVariantMap entry;
        entry["id"] = static_cast<qlonglong>(chat.id);
        entry["title"] = QString::fromStdString(chat.title);
        entry["preview"] = QString::fromStdString(chat.lastMessagePreview);
        entry["unread"] = chat.unreadCount;
        entry["active"] = chat.hasActiveConversation;
        entry["channel"] = chat.isChannel;
        entry["photo"] = QString::fromStdString(chat.photoPath);
        list.append(entry);
    }
    QMetaObject::invokeMethod(window_, "updateChatList", kFireAndForget, Q_ARG(QVariantList, list));
}

void QtUiProvider::onStatus(zkgram::core::ConversationId conversation, const std::string& stage,
                             const std::string& message) {
    QString stageQ = QString::fromStdString(stage);
    QString messageQ = QString::fromStdString(message);
    QMetaObject::invokeMethod(window_, "appendConversationStatus", kFireAndForget,
                               Q_ARG(qlonglong, conversation), Q_ARG(QString, stageQ), Q_ARG(QString, messageQ));
}

void QtUiProvider::onTextReceived(zkgram::core::ConversationId conversation, const std::string& text) {
    QMetaObject::invokeMethod(window_, "appendConversationPeerText", kFireAndForget, Q_ARG(qlonglong, conversation),
                               Q_ARG(QString, QString::fromStdString(text)));
}

void QtUiProvider::onFileReceived(zkgram::core::ConversationId conversation, const std::string& filePath) {
    QMetaObject::invokeMethod(window_, "appendConversationFileReceived", kFireAndForget,
                               Q_ARG(qlonglong, conversation), Q_ARG(QString, QString::fromStdString(filePath)));
}

void QtUiProvider::onPlainMessageReceived(zkgram::core::ConversationId conversation,
                                           const zkgram::core::PlainMessage& message) {
    QMetaObject::invokeMethod(window_, "appendPlainMessageReceived", kFireAndForget, Q_ARG(qlonglong, conversation),
                               Q_ARG(qlonglong, static_cast<qlonglong>(message.id)),
                               Q_ARG(QString, QString::fromStdString(message.text)),
                               Q_ARG(QString, QString::fromStdString(message.senderName)),
                               Q_ARG(QString, QString::fromStdString(message.photoPath)));
}

void QtUiProvider::onHistoryPhotoReady(zkgram::core::ConversationId conversation, std::int64_t messageId,
                                        const std::string& path) {
    QMetaObject::invokeMethod(window_, "updateHistoryPhoto", kFireAndForget, Q_ARG(qlonglong, conversation),
                               Q_ARG(qlonglong, static_cast<qlonglong>(messageId)),
                               Q_ARG(QString, QString::fromStdString(path)));
}

void QtUiProvider::onHistorySenderNameReady(zkgram::core::ConversationId conversation, std::int64_t messageId,
                                             const std::string& name) {
    QMetaObject::invokeMethod(window_, "updateHistorySenderName", kFireAndForget, Q_ARG(qlonglong, conversation),
                               Q_ARG(qlonglong, static_cast<qlonglong>(messageId)),
                               Q_ARG(QString, QString::fromStdString(name)));
}

void QtUiProvider::onReady(zkgram::core::ConversationId conversation) {
    QMetaObject::invokeMethod(window_, "setConversationReady", kFireAndForget, Q_ARG(qlonglong, conversation));
}

void QtUiProvider::onPingTimeout(zkgram::core::ConversationId conversation) {
    QMetaObject::invokeMethod(window_, "appendConversationStatus", kFireAndForget, Q_ARG(qlonglong, conversation),
                               Q_ARG(QString, QString("Session")), Q_ARG(QString, QString("ping timeout")));
}

void QtUiProvider::onDisconnect(zkgram::core::ConversationId conversation) {
    QMetaObject::invokeMethod(window_, "appendConversationStatus", kFireAndForget, Q_ARG(qlonglong, conversation),
                               Q_ARG(QString, QString("Session")), Q_ARG(QString, QString("disconnected")));
}

bool QtUiProvider::confirmSignatures(zkgram::core::ConversationId conversation, const std::string& mySign,
                                      const std::string& companionSign) {
    // Must return a value, so it cannot use a fire-and-forget queued call.
    // A queued connection back to our own thread would deadlock the GUI
    // event loop, so fall back to a direct call when already on that thread.
    Qt::ConnectionType connectionType =
        QThread::currentThread() == window_->thread() ? Qt::DirectConnection : Qt::BlockingQueuedConnection;

    bool result = false;
    QMetaObject::invokeMethod(window_, "showConfirmSignaturesDialog", connectionType, Q_RETURN_ARG(bool, result),
                               Q_ARG(qlonglong, conversation), Q_ARG(QString, QString::fromStdString(mySign)),
                               Q_ARG(QString, QString::fromStdString(companionSign)));
    return result;
}

std::string QtUiProvider::requestCredential(const std::string& prompt, const std::string& dataType) {
    Qt::ConnectionType connectionType =
        QThread::currentThread() == window_->thread() ? Qt::DirectConnection : Qt::BlockingQueuedConnection;
    logDebug(QString("requestCredential: entered, prompt=%1, connectionType=%2")
                 .arg(QString::fromStdString(prompt))
                 .arg(connectionType == Qt::DirectConnection ? "Direct" : "BlockingQueued"));

    QString result;
    bool invoked = QMetaObject::invokeMethod(window_, "showRequestCredentialDialog", connectionType,
                                              Q_RETURN_ARG(QString, result),
                                              Q_ARG(QString, QString::fromStdString(prompt)),
                                              Q_ARG(QString, QString::fromStdString(dataType)));
    logDebug(QString("requestCredential: invokeMethod returned %1, result.length()=%2")
                 .arg(invoked)
                 .arg(result.length()));
    return result.toStdString();
}

}  // namespace zkgram::ui::qt
