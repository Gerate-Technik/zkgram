#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// Transport layer (TDLib wrapper).
//
// IMPORTANT: this header and its implementation must not include or know
// anything about src/crypto. Only already-encrypted bytes/file paths cross
// this boundary, never keys or plaintext (see docs/README.md).
//
// TDLib types are kept out of this header on purpose (pimpl): callers of
// TelegramClient never need <td/telegram/...> on their include path, only
// telegram_client.cpp does.
namespace zkgram::telegram {

using Bytes = std::vector<std::uint8_t>;
using ChatId = std::int64_t;

// A snapshot of one chat for the chat list UI - not everything TDLib knows
// about a chat, just what a sidebar entry needs.
struct ChatSummary {
    ChatId id = 0;
    std::string title;
    std::string lastMessagePreview;
    int unreadCount = 0;
    // True for a broadcast channel (td_api::chatTypeSupergroup with
    // is_channel_ = true) - encrypted sessions only make sense for a 1:1
    // conversation with one other CryptoLayer node, not a channel's
    // one-to-many audience, so the UI must not offer to start one here.
    bool isChannel = false;
    // Local filesystem path to the chat's small profile photo, once
    // downloaded - empty until then (or if the chat has no photo at all),
    // see TelegramClient's avatar download tracking.
    std::string photoPath;
};

using MessageId = std::int64_t;

// One historical (already-sent) message, for chats read as plain Telegram
// content rather than through crypto::CryptoLayer - see
// TelegramClient::loadMessageHistory(). Deliberately not the same shape as
// an encrypted conversation's live text (there is no chat id here, the
// caller already knows it).
struct PlainMessage {
    MessageId id = 0;
    bool isOutgoing = false;
    std::string text;  // for a text message, or a placeholder label ("Document") for non-photo media
    std::int64_t date = 0;  // unix timestamp
    // Non-empty once a photo is available locally, either already cached
    // by TDLib or freshly downloaded - see TelegramClient::onHistoryPhotoReady()
    // for how a message that had no photo yet at load time gets one later.
    std::string photoPath;
    // Display name of whoever sent this (incoming messages only - always
    // empty for an outgoing one, that is always "you"). Resolving a TDLib
    // user id to a name needs its own getUser() round trip, so this starts
    // empty and is filled in later via onHistorySenderNameReady() the same
    // way photoPath is, not returned synchronously here - without a name a
    // group chat with several other people in it reads as one anonymous
    // stream of messages, which looked indistinguishable from "messages
    // from other users are not loading at all".
    std::string senderName;
};

class TelegramClient {
public:
    // dataDir is where TDLib keeps its own local database (separate from
    // crypto's). Unlike the original single-companion design, no companion
    // is pinned at construction time - this client can talk to any number
    // of chats at once, see resolveCompanion()/sendBytes(ChatId, ...) below
    // (see TODO.md, "мультиюзерность").
    explicit TelegramClient(std::string dataDir);
    ~TelegramClient();

    TelegramClient(const TelegramClient&) = delete;
    TelegramClient& operator=(const TelegramClient&) = delete;

    // Lets this layer ask for phone number / login code / 2FA password
    // during authorization without knowing anything about UiProvider;
    // core::Session wires this to UiProvider::requestCredential. The
    // provider is expected to block until the user answers.
    void setAuthInputProvider(std::function<std::string(const std::string& prompt)> provider);

    // Starts TDLib's receive loop on a background thread and logs in (using
    // the auth input provider for interactive steps). Blocks until the
    // client is ready or authorization fails (throws std::runtime_error on
    // failure). Does NOT resolve any particular chat - call
    // resolveCompanion() or loadChatList() afterwards for that.
    void connect();

    // Requests the chat list to start loading (TDLib's own async pattern:
    // this call itself does not return the list). onUpdate is invoked on
    // the receive-loop thread every time TDLib pushes a change (new chat,
    // reordering, last message changed, ...) with the current known
    // snapshot, most-recent-first. Safe to call once; TDLib keeps the list
    // updated afterwards without needing to call this again.
    void loadChatList(std::function<void(const std::vector<ChatSummary>&)> onUpdate);

    // Live search-as-you-type (same idea as tdesktop's own dialogs search
    // box): looks up query against both your existing chats/contacts and
    // Telegram's global public username directory, and returns whatever
    // TDLib already knows about the matches (title, chat id) - same
    // ChatSummary shape as loadChatList, so the UI can render results with
    // the exact same row widget. onResults is called once per call to
    // searchChats (not a standing subscription like loadChatList's
    // onUpdate) - call again on every keystroke, debounced by the caller.
    void searchChats(const std::string& query, std::function<void(const std::vector<ChatSummary>&)> onResults);

    // Reads a chat's existing message history as plain Telegram content
    // (up to the 50 most recent messages, oldest first) - for a chat that
    // has no crypto::CryptoLayer session (a channel, a group, or a contact
    // not running zkgram): those messages were never CryptoLayer
    // ciphertext to begin with, so there is nothing to decrypt, they can
    // just be shown as-is like any normal Telegram client would. Not
    // called for a chat with an active encrypted session - see
    // core::Session/the Qt UI for that split.
    void loadMessageHistory(ChatId chatId, std::function<void(const std::vector<PlainMessage>&)> onResult);

    // Fetches the next page of older messages, anchored just before
    // beforeMessageId (the oldest message id already loaded) - same
    // "load more on scroll up" idea as tdesktop's History::loadBack(), see
    // TelegramClient::fetchMessageHistory() in telegram_client.cpp for
    // exactly how the anchor works.
    void loadMoreMessageHistory(ChatId chatId, MessageId beforeMessageId,
                                 std::function<void(const std::vector<PlainMessage>&)> onResult);

    // Fires once for each history photo that was not yet downloaded at the
    // time loadMessageHistory() returned that message (see PlainMessage::
    // photoPath) - one process-wide callback, same pattern as
    // onBytesReceived/onFileReceived below, since a single-shot
    // loadMessageHistory() result has no room for a live per-call
    // subscription of its own.
    void onHistoryPhotoReady(std::function<void(ChatId, MessageId, const std::string& path)> callback);

    // Fires once for each incoming history message whose sender name was
    // not yet resolved at the time loadMessageHistory()/
    // loadMoreMessageHistory() returned it (see PlainMessage::senderName).
    void onHistorySenderNameReady(std::function<void(ChatId, MessageId, const std::string& name)> callback);

    // Resolves an arbitrary Telegram username ("name"/"@name") or phone
    // number ("+1234567890", international format) to a chat id, creating
    // a private chat if needed - the on-demand equivalent of what used to
    // happen once at startup. onResolved(id, error) - error is empty on
    // success, id is 0 on failure.
    void resolveCompanion(const std::string& companion,
                           std::function<void(ChatId, const std::string& error)> onResolved);

    // Checks whether the other side of chatId is also running zkgram,
    // before offering to start an encrypted session with them - a plain
    // Telegram contact has no way to answer a CryptoLayer handshake at all.
    // Sends a short, self-explanatory plain-text probe message (not run
    // through crypto::CryptoLayer - this happens before any handshake, so
    // there is nothing to encrypt with yet) and waits up to a few seconds
    // for a matching auto-reply; a zkgram peer's TelegramClient recognizes
    // and answers the probe automatically (see onNewMessage), a normal
    // Telegram client or person just sees (and can ignore/delete) an
    // ordinary message. onResult(true) if a reply arrived in time,
    // onResult(false) on timeout. Only one probe may be in flight per
    // chatId at a time; a new call replaces an older still-pending one.
    void probeZkgramPresence(ChatId chatId, std::function<void(bool present)> onResult);

    // Sends already-encrypted bytes as a plain text message to chatId. The
    // bytes must already be a valid UTF-8 encoding (crypto::CryptoLayer
    // produces a text-safe ciphertext representation before this is called).
    void sendBytes(ChatId chatId, const Bytes& data);

    // Sends an already-encrypted local file as a document to chatId.
    void sendFile(ChatId chatId, const std::string& filePath);

    // One process-wide callback each (not per-chat) - the chat id comes
    // through as the first callback argument, dispatching to the right
    // CryptoLayer conversation is core::Session's job now, not this
    // layer's (see core/session.hpp).
    void onBytesReceived(std::function<void(ChatId, const Bytes&)> callback);
    void onFileReceived(std::function<void(ChatId, const std::string&)> callback);

    void disconnect();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace zkgram::telegram
