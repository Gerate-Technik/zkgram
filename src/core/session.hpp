#pragma once

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>

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
    void stop();

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
    void sendPlainText(ConversationId chatId, const std::string& text);
    void sendPlainFile(ConversationId chatId, const std::string& filePath);

private:
    void wireAndStartConversation(ConversationId chatId);

    std::shared_ptr<UiProvider> uiProvider_;
    std::string dataDir_;
    std::string password_;
    telegram::TelegramClient telegramClient_;

    std::mutex conversationsMutex_;
    std::map<ConversationId, std::unique_ptr<crypto::CryptoLayer>> conversations_;
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
};

}  // namespace zkgram::core
