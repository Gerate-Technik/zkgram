#include "core/session.hpp"

#include <filesystem>
#include <fstream>
#include <utility>
#include <vector>

#include "platform/paths.hpp"

namespace zkgram::core {

namespace {

// Next to the exe, not a hardcoded "C:/Users/User/..." path - the earlier
// hardcoded path only ever worked on the machine it was written on, so a
// beta tester's copy of zkgram.exe silently produced no log file at all
// (std::ofstream just failed to open, guarded by `if (file)` below) and
// there was no way to see what was happening on their machine.
std::string logFilePath() {
    std::string dir = zkgram::platform::executableDir();
    if (dir.empty()) {
        return "zkgram_debug.log";
    }
    return (std::filesystem::path(dir) / "zkgram_debug.log").string();
}

// Temporary diagnostic-only logging, same as telegram_client.cpp's
// logDebug() - see TODO.md, the "Connecting..." investigation.
void logDebug(const std::string& message) {
    std::ofstream file(logFilePath(), std::ios::app);
    if (file) {
        file << "[session] " << message << "\n";
    }
}
}  // namespace

namespace {
std::string localCacheDir() {
    std::string appData = zkgram::platform::appDataDir();
    if (appData.empty()) {
        return {};
    }
    return appData + "/tdata";
}
}  // namespace

Session::Session(std::shared_ptr<UiProvider> uiProvider, std::string dataDir, std::string password)
    : uiProvider_(std::move(uiProvider)), dataDir_(std::move(dataDir)), password_(std::move(password)),
      localCache_(localCacheDir(), password_), telegramClient_(dataDir_) {
    telegramClient_.setAuthInputProvider(
        [this](const std::string& prompt) { return uiProvider_->requestCredential(prompt, "str"); });

    // One process-wide dispatcher, not one per conversation:
    // telegram::TelegramClient only holds a single onBytesReceived/
    // onFileReceived callback each (see telegram_client.hpp) - registering
    // a new one per conversation would silently replace the previous
    // conversation's, breaking every conversation opened before the most
    // recent one. Routing to the right crypto::CryptoLayer ingester by
    // chat id happens here instead, via conversationOnBytes_/
    // conversationOnFile_ (populated in wireAndStartConversation()).
    telegramClient_.onBytesReceived([this](telegram::ChatId chatId, const telegram::Bytes& data) {
        std::function<void(const crypto::Bytes&)> onBytes;
        {
            std::lock_guard<std::mutex> lock(conversationsMutex_);
            auto it = conversationOnBytes_.find(chatId);
            if (it != conversationOnBytes_.end()) {
                onBytes = it->second;
            }
        }
        if (onBytes) {
            onBytes(data);
        }
    });
    telegramClient_.onFileReceived([this](telegram::ChatId chatId, const std::string& filePath) {
        std::function<void(const std::string&)> onFile;
        {
            std::lock_guard<std::mutex> lock(conversationsMutex_);
            auto it = conversationOnFile_.find(chatId);
            if (it != conversationOnFile_.end()) {
                onFile = it->second;
            }
        }
        if (onFile) {
            onFile(filePath);
        }
        // No "else" branch anymore: a plain (non-encrypted) chat's incoming
        // document/photo already reaches the UI as a full PlainMessage
        // (real id/date/mediaAlbumId, a "Document"/"Photo" placeholder
        // text) via onPlainMessageReceived below, fired directly from
        // telegram::TelegramClient::onNewMessage. This used to synthesize
        // a second, id-less message here from just the downloaded path,
        // which duplicated every plain incoming attachment as two bubbles.
    });
    // The plain-content half of onBytesReceived above: a chat that has no
    // CryptoLayer session has nothing to decrypt, and its live messages used
    // to be dropped right here (the bytes went looking for an ingester that
    // was never registered), which is why a message arriving in an open
    // plain chat only showed up after reopening it and refetching history.
    telegramClient_.onPlainMessageReceived([this](telegram::ChatId chatId, const telegram::PlainMessage& message) {
        {
            std::lock_guard<std::mutex> lock(conversationsMutex_);
            if (conversationOnBytes_.count(chatId) != 0) {
                // Encrypted chat: the bytes path above already took this
                // message, and its readable form is whatever CryptoLayer
                // decrypts it into, not the ciphertext sitting in message.text.
                return;
            }
        }
        PlainMessage plain;
        plain.id = message.id;
        plain.isOutgoing = message.isOutgoing;
        plain.text = message.text;
        plain.date = message.date;
        plain.photoPath = message.photoPath;
        plain.senderName = message.senderName;
        plain.mediaAlbumId = message.mediaAlbumId;
        plain.replyToMessageId = message.replyToMessageId;
        dataRegistry_.upsertMessage(MessageData{chatId, message.id, MessageOrigin::Plain, message.isOutgoing,
                                                 message.text, message.photoPath, message.senderName, message.date,
                                                 message.mediaAlbumId, message.replyToMessageId});
        uiProvider_->onPlainMessageReceived(chatId, plain);
    });
    telegramClient_.onHistoryPhotoReady([this](telegram::ChatId chatId, telegram::MessageId messageId, const std::string& path) {
        if (MessageData* existing = dataRegistry_.message(chatId, messageId)) {
            existing->photoPath = path;
            dataRegistry_.notifyUpdated(existing);
        }
        uiProvider_->onHistoryPhotoReady(chatId, messageId, path);
    });
    telegramClient_.onHistorySenderNameReady(
        [this](telegram::ChatId chatId, telegram::MessageId messageId, const std::string& name) {
            if (MessageData* existing = dataRegistry_.message(chatId, messageId)) {
                existing->senderName = name;
                dataRegistry_.notifyUpdated(existing);
            }
            uiProvider_->onHistorySenderNameReady(chatId, messageId, name);
        });

    uiProvider_->onInit();
}

Session::~Session() {
    stop();
}

void Session::start() {
    logDebug("start: calling telegramClient_.connect()");
    telegramClient_.connect();
    logDebug("start: connect() returned, loading chat list");
    telegramClient_.loadChatList([this](const std::vector<telegram::ChatSummary>& chats) {
        std::vector<ChatListEntry> entries;
        entries.reserve(chats.size());
        std::lock_guard<std::mutex> lock(conversationsMutex_);
        for (const auto& chat : chats) {
            ChatListEntry entry;
            entry.id = chat.id;
            entry.title = chat.title;
            entry.lastMessagePreview = chat.lastMessagePreview;
            entry.unreadCount = chat.unreadCount;
            entry.hasActiveConversation = conversations_.count(chat.id) != 0;
            entry.isChannel = chat.isChannel;
            entry.photoPath = chat.photoPath;
            // Only actually written when something about this peer changed.
            // A chat-list push carries every known chat, but a push is
            // almost always triggered by a change to one or two of them -
            // re-writing all N regardless meant N PeerData overwrites and N
            // peerUpdated_ emissions (each of which copies the whole slot
            // list, see Signal::emit) per push, telling every subscriber
            // that hundreds of unchanged chats had just changed.
            PeerData peer{entry.id,
                           entry.title,
                           entry.lastMessagePreview,
                           entry.unreadCount,
                           entry.hasActiveConversation,
                           entry.isChannel,
                           entry.photoPath};
            const PeerData* known = dataRegistry_.peer(entry.id);
            if (known == nullptr || !(*known == peer)) {
                dataRegistry_.upsertPeer(std::move(peer));
            }
            entries.push_back(std::move(entry));
        }
        uiProvider_->onChatListUpdated(entries);
    });
}

void Session::searchChats(const std::string& query, std::function<void(const std::vector<ChatListEntry>&)> onResults) {
    telegramClient_.searchChats(query, [this, onResults](const std::vector<telegram::ChatSummary>& chats) {
        std::vector<ChatListEntry> entries;
        entries.reserve(chats.size());
        std::lock_guard<std::mutex> lock(conversationsMutex_);
        for (const auto& chat : chats) {
            ChatListEntry entry;
            entry.id = chat.id;
            entry.title = chat.title;
            entry.lastMessagePreview = chat.lastMessagePreview;
            entry.unreadCount = chat.unreadCount;
            entry.hasActiveConversation = conversations_.count(chat.id) != 0;
            entry.isChannel = chat.isChannel;
            entry.photoPath = chat.photoPath;
            entries.push_back(std::move(entry));
        }
        onResults(entries);
    });
}

namespace {
std::vector<PlainMessage> convertHistory(const std::vector<telegram::PlainMessage>& messages) {
    std::vector<PlainMessage> converted;
    converted.reserve(messages.size());
    for (const auto& message : messages) {
        converted.push_back(PlainMessage{message.id, message.isOutgoing, message.text, message.date, message.photoPath,
                                          message.senderName, message.mediaAlbumId, message.replyToMessageId});
    }
    return converted;
}
}  // namespace

void Session::loadMessageHistory(ConversationId chatId, std::function<void(const std::vector<PlainMessage>&)> onResult) {
    telegramClient_.loadMessageHistory(chatId, [onResult](const std::vector<telegram::PlainMessage>& messages) {
        onResult(convertHistory(messages));
    });
}

void Session::loadMoreMessageHistory(ConversationId chatId, std::int64_t beforeMessageId,
                                      std::function<void(const std::vector<PlainMessage>&)> onResult) {
    telegramClient_.loadMoreMessageHistory(chatId, beforeMessageId,
                                            [onResult](const std::vector<telegram::PlainMessage>& messages) {
                                                onResult(convertHistory(messages));
                                            });
}


void Session::probeZkgramPresence(ConversationId chatId, std::function<void(bool)> onResult) {
    telegramClient_.probeZkgramPresence(chatId, std::move(onResult));
}

void Session::startConversation(ConversationId chatId) {
    {
        std::lock_guard<std::mutex> lock(conversationsMutex_);
        if (conversations_.count(chatId) != 0) {
            return;  // already active, see the header comment
        }
    }
    wireAndStartConversation(chatId);
}

void Session::startConversationWithCompanion(
    const std::string& companion, std::function<void(ConversationId, const std::string&)> onResolved) {
    telegramClient_.resolveCompanion(companion, [this, onResolved](telegram::ChatId chatId, const std::string& error) {
        if (!error.empty()) {
            onResolved(0, error);
            return;
        }
        startConversation(chatId);
        onResolved(chatId, "");
    });
}

void Session::wireAndStartConversation(ConversationId chatId) {
    logDebug("wireAndStartConversation: chatId=" + std::to_string(chatId));
    crypto::CryptoLayer::Callbacks callbacks;

    // crypto -> telegram (исходящее): CryptoLayer вызывает это, когда module.sender
    // должен отправить уже зашифрованные данные - к этому конкретному чату.
    callbacks.sendBytes = [this, chatId](const crypto::Bytes& data) { telegramClient_.sendBytes(chatId, data); };
    callbacks.sendFile = [this, chatId](const std::string& filePath) { telegramClient_.sendFile(chatId, filePath); };

    // telegram -> crypto (входящее): CryptoLayer регистрирует свои ingester/file_ingester
    // для ЭТОГО чата - сохраняем их, единый диспетчер в конструкторе находит нужный по chatId.
    callbacks.registerReceiver = [this, chatId](std::function<void(const crypto::Bytes&)> onBytes,
                                                 std::function<void(const std::string&)> onFile) {
        std::lock_guard<std::mutex> lock(conversationsMutex_);
        conversationOnBytes_[chatId] = std::move(onBytes);
        conversationOnFile_[chatId] = std::move(onFile);
    };

    // crypto -> UI: пробрасываем в zkgram::core::UiProvider с привязкой к chatId,
    // ничего Python-специфичного наружу не течёт.
    callbacks.updateStatus = [this, chatId](const std::string& stage, const std::string& message, const std::string&) {
        logDebug("wireAndStartConversation: chatId=" + std::to_string(chatId) + " updateStatus stage=" + stage +
                 " message=" + message);
        uiProvider_->onStatus(chatId, stage, message);
    };
    callbacks.onTextReceived = [this, chatId](long, const std::string& text) {
        uiProvider_->onTextReceived(chatId, text);
    };
    callbacks.onFileReceived = [this, chatId](long, const std::string& filePath, const std::string&) {
        uiProvider_->onFileReceived(chatId, filePath);
    };
    callbacks.checkSignatures = [this, chatId](const std::string& mySign, const std::string& companionSign) {
        logDebug("wireAndStartConversation: chatId=" + std::to_string(chatId) + " checkSignatures asked, mySign=" +
                 mySign + " companionSign=" + companionSign);
        bool result = uiProvider_->confirmSignatures(chatId, mySign, companionSign);
        logDebug("wireAndStartConversation: chatId=" + std::to_string(chatId) +
                 " checkSignatures result=" + (result ? "accepted" : "rejected"));
        return result;
    };
    callbacks.onReady = [this, chatId]() {
        logDebug("wireAndStartConversation: chatId=" + std::to_string(chatId) + " onReady");
        {
            std::lock_guard<std::mutex> lock(conversationsMutex_);
            readyConversations_.insert(chatId);
        }
        uiProvider_->onReady(chatId);
    };
    callbacks.onPingTimeout = [this, chatId]() {
        logDebug("wireAndStartConversation: chatId=" + std::to_string(chatId) + " onPingTimeout");
        uiProvider_->onPingTimeout(chatId);
    };
    callbacks.onDisconnect = [this, chatId]() {
        logDebug("wireAndStartConversation: chatId=" + std::to_string(chatId) + " onDisconnect");
        {
            std::lock_guard<std::mutex> lock(conversationsMutex_);
            readyConversations_.erase(chatId);
        }
        uiProvider_->onDisconnect(chatId);
    };
    // CryptoLayer never actually calls this in practice (see
    // ui_provider.hpp) - wired anyway for interface completeness.
    callbacks.requestData = [this](const std::string& prompt, const std::string& dataType) {
        return uiProvider_->requestCredential(prompt, dataType);
    };

    auto cryptoLayer =
        std::make_unique<crypto::CryptoLayer>(dataDir_, password_, std::to_string(chatId), std::move(callbacks));
    crypto::CryptoLayer* cryptoLayerPtr = cryptoLayer.get();
    {
        std::lock_guard<std::mutex> lock(conversationsMutex_);
        conversations_[chatId] = std::move(cryptoLayer);
    }

    // init() blocks (waits on the companion's public key/signature) - see
    // the header comment on initThreads_.
    std::lock_guard<std::mutex> lock(conversationsMutex_);
    initThreads_[chatId] = std::thread([this, chatId, cryptoLayerPtr] {
        logDebug("wireAndStartConversation: chatId=" + std::to_string(chatId) + " init() thread starting");
        try {
            cryptoLayerPtr->init();
            logDebug("wireAndStartConversation: chatId=" + std::to_string(chatId) + " init() returned normally");
        } catch (const std::exception& e) {
            logDebug("wireAndStartConversation: chatId=" + std::to_string(chatId) + " init() threw: " + e.what());
            uiProvider_->onStatus(chatId, "Encryption", std::string("Failed: ") + e.what());
        }
    });
}

void Session::sendText(ConversationId chatId, const std::string& text) {
    std::lock_guard<std::mutex> lock(conversationsMutex_);
    if (readyConversations_.count(chatId) == 0) {
        // Handshake not finished yet (or never started) - CryptoLayer's AES
        // key is still None at this point, so calling send() would not
        // fail cleanly. The UI already hides the input row until
        // onReady/setConversationReady fires, this is the backend-side
        // half of that same guard.
        uiProvider_->onStatus(chatId, "Session", "Not ready yet, please wait for the encrypted session to finish starting");
        return;
    }
    auto it = conversations_.find(chatId);
    if (it != conversations_.end()) {
        it->second->sendText(text);
    }
}

void Session::sendFile(ConversationId chatId, const std::string& filePath) {
    std::lock_guard<std::mutex> lock(conversationsMutex_);
    if (readyConversations_.count(chatId) == 0) {
        uiProvider_->onStatus(chatId, "Session", "Not ready yet, please wait for the encrypted session to finish starting");
        return;
    }
    auto it = conversations_.find(chatId);
    if (it != conversations_.end()) {
        it->second->sendFile(filePath);
    }
}

void Session::sendPlainText(ConversationId chatId, const std::string& text, std::int64_t replyToMessageId) {
    telegramClient_.sendBytes(chatId, telegram::Bytes(text.begin(), text.end()), replyToMessageId);
}

void Session::sendPlainFile(ConversationId chatId, const std::string& filePath, std::int64_t replyToMessageId) {
    telegramClient_.sendFile(chatId, filePath, replyToMessageId);
}

std::vector<PlainMessage> Session::loadCachedHistory(ConversationId chatId) const {
    return localCache_.loadHistory(static_cast<std::int64_t>(chatId));
}

void Session::cacheHistory(ConversationId chatId, const std::vector<PlainMessage>& messages) const {
    localCache_.saveHistory(static_cast<std::int64_t>(chatId), messages);
}

void Session::stop() {
    // Threads are moved out and joined WITHOUT holding conversationsMutex_:
    // an init() thread's own work (CryptoLayer callbacks calling back into
    // sendBytes/registerReceiver, see wireAndStartConversation) needs that
    // same mutex, so joining while holding it would deadlock (this thread
    // waiting on join(), that thread waiting on the lock).
    std::vector<std::thread> threadsToJoin;
    {
        std::lock_guard<std::mutex> lock(conversationsMutex_);
        for (auto& [chatId, cryptoLayer] : conversations_) {
            cryptoLayer->stop();
        }
        for (auto& [chatId, thread] : initThreads_) {
            threadsToJoin.push_back(std::move(thread));
        }
        initThreads_.clear();
    }
    telegramClient_.disconnect();
    for (auto& thread : threadsToJoin) {
        if (thread.joinable()) {
            thread.join();
        }
    }
}

}  // namespace zkgram::core
