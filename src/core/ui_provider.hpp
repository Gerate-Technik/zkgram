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

    // Every one of these is scoped to a single conversation (one companion
    // chat with its own crypto::CryptoLayer handshake/session, see
    // core::Session::startConversation) - a user can have several running
    // at once, unlike the original one-companion-per-process design (see
    // TODO.md, "мультиюзерность").
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
