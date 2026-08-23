#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "core/data_registry.hpp"
#include "core/local_cache.hpp"
#include "core/ui_provider.hpp"
#include "./crypto/crypto_layer.hpp"
#include "telegram/telegram_client.hpp"

// Единственный слой, которому разрешено знать и про crypto, и про telegram.
// Отвечает за жизненный цикл сессии: связывает шифрование (реальный Python
// CryptoLayer через crypto::CryptoLayer) и транспорт (TDLib через
// telegram::TelegramClient) так, чтобы они не знали друг о друге напрямую.
//
// С 2026-08-02 — менеджер НЕСКОЛЬКИХ одновременных переписок (см. TODO.md,
// "мультиюзерность"), не одна сессия на одного захардкоженного собеседника.
// Один TelegramClient (одно TDLib-соединение, один аккаунт) на весь Session,
// но под каждый чат, где пользователь явно нажал "начать зашифрованную
// переписку" (startConversation), — свой отдельный crypto::CryptoLayer
// (свой ECDH-обмен, свои подписи), все они используют один и тот же
// dataDir/password (общая локальная личность, см. TODO.md про почему это
// безопасно шарить между несколькими CryptoLayer-инстансами).
namespace zkgram::core {

class Session {
public:
    Session(std::shared_ptr<UiProvider> uiProvider, std::string dataDir, std::string password);
    ~Session();

    // Logs in to Telegram (blocks on TDLib auth, see
    // telegram::TelegramClient::connect()) and starts pushing the chat list
    // to UiProvider::onChatListUpdated(). Does not start any conversation by
    // itself - call startConversation()/startConversationWithCompanion()
    // once the user picks a chat.
    void start();
    // noexcept, and it means it - see the definition. ~Session() calls this,
    // and a destructor is noexcept by default, so an exception getting out
    // of here terminates the process instead of reporting anything. Safe to
    // call more than once (the Qt entry point calls it explicitly after the
    // event loop ends, and again via ~Session).
    void stop() noexcept;

    // Checks whether the other side of chatId is also running zkgram
    // before startConversation() is called - see
    // telegram::TelegramClient::probeZkgramPresence for how. onResult(true)
    // if they answered in time, onResult(false) on timeout/no answer.
    void probeZkgramPresence(ConversationId chatId, std::function<void(bool present)> onResult);

    // Reads a chat's existing history as plain (unencrypted) content - only
    // meaningful for a chat with no active crypto::CryptoLayer session
    // (see telegram::TelegramClient::loadMessageHistory); the caller is
    // responsible for not calling this once an encrypted session exists,
    // since that history was never CryptoLayer plaintext to begin with.
    void loadMessageHistory(ConversationId chatId, std::function<void(const std::vector<PlainMessage>&)> onResult);

    // Next page of older messages - see
    // telegram::TelegramClient::loadMoreMessageHistory().
    void loadMoreMessageHistory(ConversationId chatId, std::int64_t beforeMessageId,
                                 std::function<void(const std::vector<PlainMessage>&)> onResult);

    // There is deliberately nothing here for "the user opened/read this
    // chat" - see the note in telegram::TelegramClient for why zkgram never
    // reports that back to Telegram.

    // chatId must be one already seen via onChatListUpdated. Idempotent:
    // calling it again for an already-active conversation is a no-op
    // instead of a second handshake.
    void startConversation(ConversationId chatId);

    // For a chat not in the list yet - a Telegram username ("name"/
    // "@name") or phone number ("+1234567890"). Resolves it to a chat id
    // (see telegram::TelegramClient::resolveCompanion) and then behaves
    // like startConversation(). onResolved reports the outcome (empty
    // error string on success) since this happens asynchronously on the
    // TDLib receive-loop thread.
    void startConversationWithCompanion(const std::string& companion,
                                         std::function<void(ConversationId, const std::string& error)> onResolved);

    // Live search-as-you-type over both your existing chats and Telegram's
    // global username directory (see telegram::TelegramClient::searchChats)
    // - one-shot per call, not a standing subscription like
    // onChatListUpdated. Called on the receive-loop thread's callback, same
    // as everything else here; the caller (Qt UI) is responsible for
    // marshalling onResults back to its own thread.
    void searchChats(const std::string& query, std::function<void(const std::vector<ChatListEntry>&)> onResults);

    // See telegram::TelegramClient::fetchMyProfile's own comment.
    void fetchMyProfile(std::function<void(const std::string& name, const std::string& username)> callback);

    void sendText(ConversationId chatId, const std::string& text);
    void sendFile(ConversationId chatId, const std::string& filePath);

    // Sends a real, unencrypted Telegram message - straight to
    // telegramClient_, bypassing crypto::CryptoLayer entirely (no
    // conversations_ lookup, no readyConversations_ check). Meant for a
    // chat with no active encrypted session at all (a contact not running
    // zkgram, a group, a channel) - the same chats loadMessageHistory()
    // already reads as plain content instead of ciphertext, this is just
    // the write side of that: without it, such a chat could be read but
    // never actually written to unless the other side also ran zkgram.
    // replyToMessageId, when non-zero, links the sent message to an
    // earlier one in the same chat (see telegram::TelegramClient::
    // sendBytes/sendFile) - only meaningful for a plain chat's real TDLib
    // reply; there is no equivalent for sendText/sendFile above yet (see
    // TODO.md, threading a reply through crypto::CryptoLayer's own
    // send callback is not done in this pass).
    void sendPlainText(ConversationId chatId, const std::string& text, std::int64_t replyToMessageId = 0);
    void sendPlainFile(ConversationId chatId, const std::string& filePath, std::int64_t replyToMessageId = 0);

    // See core::LocalCache's own class comment for why this exists (not
    // just a speed optimization - CryptoLayer's forward-secret session
    // key makes old ciphertext permanently undecryptable after a
    // restart, so this is the only way encrypted history survives one at
    // all). Both are cheap no-ops if the cache directory could not be
    // resolved/created (LocalCache::available() == false) - the caller
    // does not need to check that itself first.
    std::vector<PlainMessage> loadCachedHistory(ConversationId chatId) const;
    void cacheHistory(ConversationId chatId, const std::vector<PlainMessage>& messages) const;

    // The single entity store (see historyview-port-guide.md section 4.1 /
    // core/data_registry.hpp's own comment) - populated alongside every
    // uiProvider_ call this class already makes below, not instead of it
    // yet: a UI can subscribe to this now (onItemUpdated/onPeerUpdated) to
    // start migrating off the older per-signal UiProvider callbacks, without
    // those callbacks being removed out from under it in the same change.
    DataRegistry& dataRegistry() { return dataRegistry_; }

private:
    void wireAndStartConversation(ConversationId chatId);
    // Replays pendingCiphertext_[chatId] into the session that just
    // registered its receiver - see the definition.
    void drainPendingCiphertext(ConversationId chatId);
    // Reports a handshake that is going nowhere - see the definition.
    void armHandshakeWatchdog(ConversationId chatId);

    std::shared_ptr<UiProvider> uiProvider_;
    std::string dataDir_;
    std::string password_;
    LocalCache localCache_;
    DataRegistry dataRegistry_;
    telegram::TelegramClient telegramClient_;

    std::mutex conversationsMutex_;
    // shared_ptr, not unique_ptr: sendText()/sendFile() copy a reference out
    // while holding conversationsMutex_, then release the mutex BEFORE
    // calling into the copy (CryptoLayer::sendText can block for seconds
    // deep inside the Python transport pipeline waiting on a chunk ACK -
    // see their own comments). Holding conversationsMutex_ across that call
    // deadlocked the whole app: the GUI thread would block inside it while
    // still holding the mutex, and the only thing that could ever unblock
    // it - the ACK arriving via the TDLib receive thread - needs that same
    // mutex to deliver.
    std::map<ConversationId, std::shared_ptr<crypto::CryptoLayer>> conversations_;
    // Populated only once a conversation's onReady callback actually fires
    // (handshake finished). sendText/sendFile check this before touching
    // Python: CryptoLayer's AES key is still None mid handshake, so calling
    // send()/send_file() before that is undefined behavior on the Python
    // side rather than a clean, catchable failure - see the UI-side
    // conversationReady_/conversationActive_ split in main_window.hpp for
    // the same rule at the button level.
    std::set<ConversationId> readyConversations_;
    // crypto::CryptoLayer::init() blocks (waiting on the companion's public
    // key/signature, potentially indefinitely if they never open zkgram on
    // their side) - one background thread per conversation, joined in
    // stop()/the destructor.
    std::map<ConversationId, std::thread> initThreads_;
    // Populated by each conversation's CryptoLayer::Callbacks::registerReceiver
    // (see wireAndStartConversation) - the single dispatcher registered with
    // telegramClient_ in the constructor looks a chat id up here to find
    // which conversation's ingester an incoming message/file belongs to.
    std::map<ConversationId, std::function<void(const crypto::Bytes&)>> conversationOnBytes_;
    std::map<ConversationId, std::function<void(const std::string&)>> conversationOnFile_;
    // Ciphertext that arrived for a chat before its CryptoLayer session was
    // there to take it, replayed into that session the moment it registers
    // itself (drainPendingCiphertext).
    //
    // This is what makes two people able to find each other at all. The
    // handshake opens with each side sending its node id and then waiting
    // for the other's (crypto_layer.py's node_id_exchange), and until this
    // existed anything that arrived before the local session was created was
    // simply dropped by the dispatcher above. So whoever pressed "Start
    // encrypted session" first had their node id thrown away by the other
    // side, and no matter who pressed second, one of the two waited for a
    // packet that had already come and gone - both then sat in that loop
    // forever, which is exactly what "they never find each other" was.
    //
    // Bounded per chat: this holds messages for chats that may never start a
    // session at all, and a handshake needs only a handful of packets.
    //
    // Each one is kept with the moment it arrived, because the protocol puts
    // a hard limit on how late a packet may be delivered - see
    // kCryptoLayerPacketLifetime and drainPendingCiphertext().
    struct PendingPacket {
        crypto::Bytes data;
        std::chrono::steady_clock::time_point arrived;
    };
    std::map<ConversationId, std::deque<PendingPacket>> pendingCiphertext_;
    // CryptoLayer's transport level drops any packet whose own timestamp is
    // 300 seconds or more old ("old packet. bye" in transport.py's rworker),
    // so replaying anything older than that is pointless - it would be
    // thrown away at the other end of the call. Mirrored here so the user
    // can be told what actually happened instead of watching a handshake
    // that cannot possibly finish.
    static constexpr std::chrono::seconds kCryptoLayerPacketLifetime{300};
    static constexpr std::size_t kMaxPendingCiphertext = 32;
    // Chats whose held-back packets are being replayed right now - the
    // dispatcher queues new arrivals behind them rather than letting a live
    // packet overtake the replay.
    std::set<ConversationId> drainingCiphertext_;
    std::vector<std::thread> replayThreads_;
    // One per started conversation, see armHandshakeWatchdog().
    std::vector<std::thread> watchdogThreads_;
    std::mutex shutdownMutex_;
    std::condition_variable shutdownRequested_;
    bool shuttingDown_ = false;
};

}  // namespace zkgram::core
