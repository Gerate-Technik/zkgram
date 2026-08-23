#include "core/session.hpp"

#include <cctype>
#include <chrono>
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
// Does this message look like CryptoLayer traffic rather than somebody's
// ordinary Telegram text? Everything the crypto stack sends leaves as a
// space-joined run of WordCoder dictionary words - one short word per
// ciphertext byte (the transitional level's sworker(), in cryptolayer) - and
// every packet carries an ECDSA signature ahead of its payload, so even the
// smallest one runs to some seventy words. Judged by shape rather than
// against the dictionary itself, which keeps the dictionary file out of this
// layer; being wrong is cheap either way, since a plain message mistaken for
// ciphertext is dropped by the transitional level's own decode (it logs the
// failure and returns) and a packet mistaken for plain text only leaves us
// where we were before any of this buffering existed.
bool looksLikeCryptoLayerTraffic(const crypto::Bytes& data) {
    if (data.size() < 64) {
        return false;
    }
    int tokens = 1;
    std::size_t tokenBytes = 0;
    for (std::uint8_t byte : data) {
        if (byte == ' ') {
            if (tokenBytes == 0) {
                return false;  // a run of spaces: WordCoder joins with exactly one
            }
            ++tokens;
            tokenBytes = 0;
            continue;
        }
        // No control characters, no digits, no punctuation - the dictionary
        // is words only. Bytes >= 0x80 are the continuation bytes of the
        // dictionary's Cyrillic letters and are left alone.
        if (byte < 0x80 && std::isalpha(static_cast<unsigned char>(byte)) == 0) {
            return false;
        }
        ++tokenBytes;
        // The dictionary refuses words longer than 10 characters (WordCoder's
        // own _check_dict), which is at most 20 bytes of UTF-8 Cyrillic.
        if (tokenBytes > 20) {
            return false;
        }
    }
    return tokenBytes > 0 && tokens >= 16;
}

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
            bool draining = drainingCiphertext_.count(chatId) != 0;
            if (it != conversationOnBytes_.end() && !draining) {
                onBytes = it->second;
            } else if (looksLikeCryptoLayerTraffic(data)) {
                // Nothing to hand this to yet (or a replay of earlier
                // packets is still in flight, and these must not overtake
                // it) - hold on to it, see pendingCiphertext_'s own comment
                // for why dropping it broke the handshake outright.
                std::deque<PendingPacket>& pending = pendingCiphertext_[chatId];
                pending.push_back(PendingPacket{data, std::chrono::steady_clock::now()});
                while (pending.size() > kMaxPendingCiphertext) {
                    pending.pop_front();
                }
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
        plain.isVoiceNote = message.isVoiceNote;
        plain.voiceNoteDuration = message.voiceNoteDuration;
        plain.voiceNotePath = message.voiceNotePath;
        plain.voiceWaveform = message.voiceWaveform;
        MessageData data{chatId, message.id, MessageOrigin::Plain, message.isOutgoing,
                          message.text, message.photoPath, message.senderName, message.date,
                          message.mediaAlbumId, message.replyToMessageId};
        data.isVoiceNote = message.isVoiceNote;
        data.voiceNoteDuration = message.voiceNoteDuration;
        data.voiceNotePath = message.voiceNotePath;
        dataRegistry_.upsertMessage(std::move(data));
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
    telegramClient_.onOutgoingReadUpTo([this](telegram::ChatId chatId, telegram::MessageId messageId) {
        uiProvider_->onOutgoingReadUpTo(chatId, messageId);
    });
    telegramClient_.onHistoryVoiceReady(
        [this](telegram::ChatId chatId, telegram::MessageId messageId, const std::string& path) {
            if (MessageData* existing = dataRegistry_.message(chatId, messageId)) {
                existing->voiceNotePath = path;
                dataRegistry_.notifyUpdated(existing);
            }
            uiProvider_->onHistoryVoiceReady(chatId, messageId, path);
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
            entry.lastMessageDate = chat.lastMessageDate;
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
            entry.lastMessageDate = chat.lastMessageDate;
            entries.push_back(std::move(entry));
        }
        onResults(entries);
    });
}

void Session::fetchMyProfile(std::function<void(const std::string&, const std::string&)> callback) {
    telegramClient_.fetchMyProfile(std::move(callback));
}

namespace {
std::vector<PlainMessage> convertHistory(const std::vector<telegram::PlainMessage>& messages) {
    std::vector<PlainMessage> converted;
    converted.reserve(messages.size());
    for (const auto& message : messages) {
        converted.push_back(PlainMessage{message.id, message.isOutgoing, message.text, message.date, message.photoPath,
                                          message.senderName, message.mediaAlbumId, message.replyToMessageId,
                                          message.isVoiceNote, message.voiceNoteDuration, message.voiceNotePath,
                                          message.voiceWaveform});
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
        {
            std::lock_guard<std::mutex> lock(conversationsMutex_);
            conversationOnBytes_[chatId] = std::move(onBytes);
            conversationOnFile_[chatId] = std::move(onFile);
        }
        drainPendingCiphertext(chatId);
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
        std::make_shared<crypto::CryptoLayer>(dataDir_, password_, std::to_string(chatId), std::move(callbacks));
    crypto::CryptoLayer* cryptoLayerPtr = cryptoLayer.get();
    {
        std::lock_guard<std::mutex> lock(conversationsMutex_);
        conversations_[chatId] = std::move(cryptoLayer);
    }

    armHandshakeWatchdog(chatId);

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

// Feeds a chat's held-back ciphertext into the session that just registered
// itself, oldest first, then lets live traffic through again.
//
// On its own thread, not on the caller's: registerReceiver() is called from
// inside CryptoLayer's own create_session(), i.e. from Python, on the init
// thread that is about to start the handshake - pushing packets back into
// the stack from there would re-enter it before it finished setting itself
// up. The thread is joined in stop(); it does not outlive the Session.
//
// While it runs, drainingCiphertext_ makes the dispatcher queue new arrivals
// instead of delivering them, so a packet arriving mid-replay cannot
// overtake the ones already waiting.
void Session::drainPendingCiphertext(ConversationId chatId) {
    std::lock_guard<std::mutex> lock(conversationsMutex_);
    if (pendingCiphertext_[chatId].empty()) {
        pendingCiphertext_.erase(chatId);
        return;
    }
    drainingCiphertext_.insert(chatId);
    replayThreads_.emplace_back([this, chatId] {
        // A moment's grace before the first packet goes in. registerReceiver
        // is called from CryptoLayer's create_session(), and the transitional
        // level it hands us only gets its own upper/lower levels wired by the
        // update_levels() call right after that returns - a packet arriving
        // in between would land in a half-built stack. That window is
        // microseconds wide on the init thread; this waits far longer than it
        // needs to, and gives it up immediately if the app is closing.
        {
            std::unique_lock<std::mutex> lock(shutdownMutex_);
            if (shutdownRequested_.wait_for(lock, std::chrono::milliseconds(250),
                                             [this] { return shuttingDown_; })) {
                std::lock_guard<std::mutex> conversationsLock(conversationsMutex_);
                drainingCiphertext_.erase(chatId);
                return;
            }
        }
        bool droppedStale = false;
        for (;;) {
            PendingPacket packet;
            std::function<void(const crypto::Bytes&)> onBytes;
            {
                std::lock_guard<std::mutex> lock(conversationsMutex_);
                auto pending = pendingCiphertext_.find(chatId);
                auto receiver = conversationOnBytes_.find(chatId);
                if (pending == pendingCiphertext_.end() || pending->second.empty() ||
                    receiver == conversationOnBytes_.end()) {
                    if (pending != pendingCiphertext_.end()) {
                        pendingCiphertext_.erase(pending);
                    }
                    drainingCiphertext_.erase(chatId);
                    break;
                }
                packet = std::move(pending->second.front());
                pending->second.pop_front();
                onBytes = receiver->second;
            }
            // Older than the protocol accepts: handing it over would only
            // have it dropped one level down (transport.py's rworker checks
            // the packet's own timestamp), so it is dropped here instead and
            // reported once the queue is done - see below for why this is
            // worth saying out loud rather than silently ignoring.
            if (std::chrono::steady_clock::now() - packet.arrived >= kCryptoLayerPacketLifetime) {
                logDebug("drainPendingCiphertext: chatId=" + std::to_string(chatId) + " dropping a packet held for " +
                          std::to_string(std::chrono::duration_cast<std::chrono::seconds>(
                                              std::chrono::steady_clock::now() - packet.arrived)
                                              .count()) +
                          "s - past the protocol's own 300s limit");
                droppedStale = true;
                continue;
            }
            logDebug("drainPendingCiphertext: chatId=" + std::to_string(chatId) + " replaying " +
                      std::to_string(packet.data.size()) + " bytes held from before the session existed");
            onBytes(packet.data);
        }
        if (!droppedStale) {
            return;
        }
        // The companion started their side more than five minutes ago, and
        // the protocol cannot recover from that on its own. CryptoLayer
        // re-sends an unacknowledged packet every 30 seconds forever, but it
        // re-sends the SAME bytes, timestamp included
        // (send_with_pending_acknowledgment in transport.py), while the
        // receiving end throws away anything stamped 300 seconds ago or more
        // (rworker, same file). So once that window has passed, the first
        // side's node id can never be accepted again, no matter how long
        // either of them waits - the handshake is unrecoverable until one of
        // them starts over. Nothing on this side can repair it: the
        // timestamp sits inside the signed payload, so it cannot be rewritten
        // in passing. All that is left is to say exactly that, instead of
        // leaving two people staring at a progress line forever.
        uiProvider_->onStatus(chatId, "Encryption",
                               "Your companion started their encrypted session more than 5 minutes ago, and the "
                               "protocol refuses packets that old - this handshake cannot complete. Ask them to "
                               "press \"Start encrypted session\" again, and press it on both sides within about "
                               "five minutes of each other.");
    });
}

// Says out loud that a handshake is not going anywhere.
//
// CryptoLayer waits for the companion's node id, signature and public key in
// three plain `while not ...: sleep(0.1)` loops with no timeout of their own
// (crypto_layer.py), so a companion who never presses "Start encrypted
// session" leaves the session waiting silently for as long as the app runs.
// The status lines it does emit ("Waiting for companion node id...") never
// change, and there is nothing in them to tell a slow companion from one who
// is not running zkgram at all. This turns that silence into one explicit
// message; it deliberately does not cancel anything, since a companion who
// starts ten minutes later still completes the handshake normally.
void Session::armHandshakeWatchdog(ConversationId chatId) {
    watchdogThreads_.emplace_back([this, chatId] {
        std::unique_lock<std::mutex> lock(shutdownMutex_);
        if (shutdownRequested_.wait_for(lock, std::chrono::seconds(45), [this] { return shuttingDown_; })) {
            return;  // the app is closing, nothing to report
        }
        lock.unlock();
        {
            std::lock_guard<std::mutex> conversationsLock(conversationsMutex_);
            if (readyConversations_.count(chatId) != 0) {
                return;  // handshake finished on its own, as it usually does
            }
        }
        logDebug("armHandshakeWatchdog: chatId=" + std::to_string(chatId) + " still not ready after 45s");
        uiProvider_->onStatus(chatId, "Encryption",
                               "Still waiting for the companion. An encrypted session needs BOTH sides to press "
                               "\"Start encrypted session\" in this chat, and within about five minutes of each "
                               "other - the protocol drops handshake packets older than that (see "
                               "drainPendingCiphertext).");
    });
}

void Session::sendText(ConversationId chatId, const std::string& text) {
    // conversationsMutex_ used to be held across the CryptoLayer::sendText()
    // call below (which can block for real seconds deep inside the Python
    // transport pipeline, waiting on a chunk ACK - see levels/transport.py's
    // send_with_pending_acknowledgment). Called from the GUI thread, that
    // deadlocked the whole app: the only thing that could ever deliver the
    // ACK and unblock it is the TDLib receive thread, which needs this same
    // mutex to store incoming bytes (see the onBytesReceived handler above).
    // Fixed by copying the shared_ptr out and releasing the mutex first -
    // conversations_ had to become shared_ptr (was unique_ptr) so this copy
    // is possible; the CryptoLayer object stays alive via this local copy
    // even if conversations_ itself is ever modified concurrently.
    std::shared_ptr<crypto::CryptoLayer> layer;
    {
        std::lock_guard<std::mutex> lock(conversationsMutex_);
        if (readyConversations_.count(chatId) == 0) {
            // Handshake not finished yet (or never started) - CryptoLayer's
            // AES key is still None at this point, so calling send() would
            // not fail cleanly. The UI already hides the input row until
            // onReady/setConversationReady fires, this is the backend-side
            // half of that same guard.
            uiProvider_->onStatus(chatId, "Session", "Not ready yet, please wait for the encrypted session to finish starting");
            return;
        }
        auto it = conversations_.find(chatId);
        if (it != conversations_.end()) {
            layer = it->second;  // just a refcount bump, cheap
        }
    }
    if (layer) {
        layer->sendText(text);
    }
}

void Session::sendFile(ConversationId chatId, const std::string& filePath) {
    // Same fix as sendText() above: copy the shared_ptr out and release
    // conversationsMutex_ before calling into it - see that function's own
    // comment for why holding the mutex across this call deadlocked the app.
    std::shared_ptr<crypto::CryptoLayer> layer;
    {
        std::lock_guard<std::mutex> lock(conversationsMutex_);
        if (readyConversations_.count(chatId) == 0) {
            uiProvider_->onStatus(chatId, "Session", "Not ready yet, please wait for the encrypted session to finish starting");
            return;
        }
        auto it = conversations_.find(chatId);
        if (it != conversations_.end()) {
            layer = it->second;
        }
    }
    if (layer) {
        layer->sendFile(filePath);
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

// Deliberately noexcept, and every step below is individually guarded.
//
// This is the shutdown path, and ~Session() calls it, where a destructor is
// noexcept by default - so an exception escaping here does not surface as a
// failure, it calls std::terminate() and aborts the process. That was the
// crash on closing the window: CryptoLayer::stop() forwards any Python-side
// exception as a std::runtime_error (see rethrowAsRuntimeError in
// crypto_layer.cpp), nothing here caught it, and stopping a conversation
// whose handshake never finished raises on the Python side rather than
// failing quietly - the same "AES key is still None" state that
// MainWindow::updateConversationControlsVisibility() already hides the
// composer for.
//
// One conversation failing to shut down cleanly must also not skip the
// rest, so each is stopped in its own try block instead of one around the
// loop: whatever happens, the TDLib client still gets disconnected and every
// init thread still gets joined - and an unjoined init thread would be its
// own std::terminate() when the map is destroyed.
void Session::stop() noexcept {
    // Woken first, before anything blocking below: a watchdog is otherwise
    // asleep for up to 45 seconds and would hold shutdown for that long, and
    // it has nothing useful to say once the app is closing anyway.
    {
        std::lock_guard<std::mutex> lock(shutdownMutex_);
        shuttingDown_ = true;
    }
    shutdownRequested_.notify_all();
    // Threads are moved out and joined WITHOUT holding conversationsMutex_:
    // an init() thread's own work (CryptoLayer callbacks calling back into
    // sendBytes/registerReceiver, see wireAndStartConversation) needs that
    // same mutex, so joining while holding it would deadlock (this thread
    // waiting on join(), that thread waiting on the lock).
    std::vector<std::thread> threadsToJoin;
    {
        std::lock_guard<std::mutex> lock(conversationsMutex_);
        for (auto& [chatId, cryptoLayer] : conversations_) {
            try {
                cryptoLayer->stop();
            } catch (const std::exception& error) {
                logDebug("stop: chatId=" + std::to_string(chatId) +
                          " CryptoLayer::stop() failed, continuing shutdown: " + error.what());
            } catch (...) {
                logDebug("stop: chatId=" + std::to_string(chatId) +
                          " CryptoLayer::stop() failed with a non-standard exception, continuing shutdown");
            }
        }
        for (auto& [chatId, thread] : initThreads_) {
            threadsToJoin.push_back(std::move(thread));
        }
        initThreads_.clear();
        for (auto& thread : replayThreads_) {
            threadsToJoin.push_back(std::move(thread));
        }
        replayThreads_.clear();
        for (auto& thread : watchdogThreads_) {
            threadsToJoin.push_back(std::move(thread));
        }
        watchdogThreads_.clear();
    }
    try {
        telegramClient_.disconnect();
    } catch (const std::exception& error) {
        logDebug(std::string("stop: TelegramClient::disconnect() failed: ") + error.what());
    } catch (...) {
        logDebug("stop: TelegramClient::disconnect() failed with a non-standard exception");
    }
    for (auto& thread : threadsToJoin) {
        if (thread.joinable()) {
            thread.join();
        }
    }
}

}  // namespace zkgram::core
