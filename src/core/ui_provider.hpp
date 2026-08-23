#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Full UI extension point. core::Session forwards every UI signal from
// crypto::CryptoLayer::Callbacks here; an implementation must not know
// anything about crypto or telegram internals.
namespace zkgram::core {

// Same underlying type as telegram::ChatId, deliberately not that type
// itself: this header must not include telegram_client.hpp (see the file
// comment above and docs/README.md on layering).
using ConversationId = std::int64_t;

// One chat-list sidebar entry - a copy of telegram::ChatSummary's fields,
// not that type itself, for the same layering reason as ConversationId.
// core::Session does the field-by-field conversion.
struct ChatListEntry {
    ConversationId id = 0;
    std::string title;
    std::string lastMessagePreview;
    int unreadCount = 0;
    // True once a crypto::CryptoLayer conversation exists for this chat
    // (see core::Session::startConversation) - lets the UI show "Open" vs
    // "Start encrypted session" per entry.
    bool hasActiveConversation = false;
    // True for a broadcast channel - encryption is a 1:1 protocol, a
    // channel's one-to-many audience cannot use it, so the UI must not
    // offer to start a session here (see telegram::ChatSummary::isChannel).
    bool isChannel = false;
    // Local filesystem path to the chat's profile photo, empty until
    // downloaded (see telegram::ChatSummary::photoPath).
    std::string photoPath;
    // See telegram::ChatSummary::lastMessageDate's own comment.
    std::int64_t lastMessageDate = 0;
};

// One historical (already-sent) message from a chat with no
// crypto::CryptoLayer session - a copy of telegram::PlainMessage's fields,
// not that type itself, for the same layering reason as ChatListEntry. See
// core::Session::loadMessageHistory().
struct PlainMessage {
    std::int64_t id = 0;
    bool isOutgoing = false;
    std::string text;
    std::int64_t date = 0;
    // Non-empty once a photo is available locally - see
    // telegram::PlainMessage::photoPath and
    // UiProvider::onHistoryPhotoReady() for a photo that was still
    // downloading when the message list this struct came from was
    // returned.
    std::string photoPath;
    // Non-empty once resolved - see telegram::PlainMessage::senderName and
    // UiProvider::onHistorySenderNameReady() for a name not yet known when
    // this struct was returned.
    std::string senderName;
    // See telegram::PlainMessage::mediaAlbumId's own comment.
    std::int64_t mediaAlbumId = 0;
    // See telegram::PlainMessage::replyToMessageId's own comment.
    std::int64_t replyToMessageId = 0;
    // See telegram::PlainMessage::isVoiceNote/voiceNoteDuration/
    // voiceNotePath's own comments.
    bool isVoiceNote = false;
    std::int32_t voiceNoteDuration = 0;
    std::string voiceNotePath;
    std::vector<std::uint8_t> voiceWaveform;
};

class UiProvider {
public:
    virtual ~UiProvider() = default;

    // Called once before Session::start(), before connecting.
    virtual void onInit() {}

    // Pushed every time TelegramClient's chat list changes (new chat,
    // reordering, last message, unread count) - see
    // telegram::TelegramClient::loadChatList(). Not pure virtual: a UI
    // that only ever talks to one pinned companion (e.g. a future
    // scripted/headless plugin) is not required to implement a chat list.
    virtual void onChatListUpdated(const std::vector<ChatListEntry>& chats) {
        (void)chats;
    }

    // A photo belonging to a message previously returned by
    // Session::loadMessageHistory() finished downloading after the fact
    // (see PlainMessage::photoPath) - lets the UI swap that one message's
    // placeholder for the real image instead of requiring a full reload.
    // Not pure virtual for the same reason as onChatListUpdated above.
    virtual void onHistoryPhotoReady(ConversationId conversation, std::int64_t messageId, const std::string& path) {
        (void)conversation;
        (void)messageId;
        (void)path;
    }

    // A history message's sender name resolved after the fact - see
    // PlainMessage::senderName. Same not-pure-virtual reasoning.
    virtual void onHistorySenderNameReady(ConversationId conversation, std::int64_t messageId,
                                           const std::string& name) {
        (void)conversation;
        (void)messageId;
        (void)name;
    }

    // The other side has read every outgoing message up to and including
    // messageId in this conversation - drives the single/double tick, see
    // telegram::TelegramClient::onOutgoingReadUpTo() for where the value
    // comes from and why it says nothing about what we have read. Same
    // not-pure-virtual reasoning as the callbacks above.
    virtual void onOutgoingReadUpTo(ConversationId conversation, std::int64_t messageId) {
        (void)conversation;
        (void)messageId;
    }

    // A voice note's audio finished downloading after the fact - see
    // PlainMessage::voiceNotePath. Same not-pure-virtual reasoning.
    virtual void onHistoryVoiceReady(ConversationId conversation, std::int64_t messageId, const std::string& path) {
        (void)conversation;
        (void)messageId;
        (void)path;
    }

    // Every one of these is scoped to a single conversation (one companion
    // chat with its own crypto::CryptoLayer handshake/session, see
    // core::Session::startConversation) - a user can have several running
    // at once, unlike the original one-companion-per-process design (see
    // TODO.md, "мультиюзерность").
    // A live incoming message in a chat with no CryptoLayer session -
    // ordinary Telegram content, nothing to decrypt, the same shape
    // loadMessageHistory() returns. Distinct from onTextReceived below,
    // which is only ever CryptoLayer plaintext: this one carries a real
    // Telegram message id, so a UI can tell it apart from the copy of the
    // same message it may also get from history. Not pure virtual, for the
    // same reason as onChatListUpdated above.
    virtual void onPlainMessageReceived(ConversationId conversation, const PlainMessage& message) {
        (void)conversation;
        (void)message;
    }

    virtual void onStatus(ConversationId conversation, const std::string& stage, const std::string& message) = 0;
    virtual void onTextReceived(ConversationId conversation, const std::string& text) = 0;
    virtual void onFileReceived(ConversationId conversation, const std::string& filePath) = 0;

    virtual void onReady(ConversationId conversation) = 0;
    virtual void onPingTimeout(ConversationId conversation) = 0;
    virtual void onDisconnect(ConversationId conversation) = 0;

    // Show mySign/companionSign to the user and return their decision.
    // Called synchronously from the Python/GIL callback chain (see
    // docs/README.md), so blocking is allowed only while waiting for
    // explicit user input, never for network I/O.
    virtual bool confirmSignatures(ConversationId conversation, const std::string& mySign,
                                    const std::string& companionSign) = 0;

    // Not conversation-scoped: this is the TDLib login flow (phone number,
    // SMS code, 2FA password), which happens once per process, before any
    // conversation exists - see telegram::TelegramClient::setAuthInputProvider.
    // Closes the request_data gap on the cryptolayer.UIProvider side too
    // (see cryptolayer/src/UIProvider.py: request_data(prompt, data_type)),
    // though CryptoLayer itself never actually calls it in practice.
    // dataType is a Python type name (e.g. "str"); an empty string means
    // the user cancelled the request.
    virtual std::string requestCredential(const std::string& prompt, const std::string& dataType) = 0;
};

}  // namespace zkgram::core
