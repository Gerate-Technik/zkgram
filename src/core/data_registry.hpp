#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

// Single registry for every message/peer entity this process knows about -
// see historyview-port-guide.md section 4.1 ("Data::Session-эквивалент").
// Replaces the previous pattern of a value-copied PlainMessage plus N
// side-channel callbacks keyed by (chatId, messageId) - see
// core::UiProvider::onHistoryPhotoReady/onHistorySenderNameReady before
// this existed: a photo finishing download or a sender name resolving used
// to thread a bespoke (chatId, messageId, newValue) callback all the way
// out to the UI, which then had to re-locate and patch its own separate
// copy of the same message. Here it just mutates the MessageData already
// stored and fires one itemUpdated() signal; every subscriber (there may
// be several, e.g. a message list and a chat-list preview) sees the same
// update through the same channel, and a message's photo/sender name/album
// id are never out of sync with each other the way three independent
// callbacks racing on the same message id could get.
//
// Deliberately not lib_rpl: this header is compiled into both the qt and
// console UI variants (core/session.cpp is a common source, see
// CMakeLists.txt), and lib_rpl is only vendored/linked under
// ZKGRAM_UI=qt -DZKGRAM_VENDOR_LIB_UI=ON. Signal below is the minimal
// subscribe/emit this needs, nothing console does not already get for
// free from the standard library.
namespace zkgram::core {

using ConversationId = std::int64_t;
using MessageId = std::int64_t;

// What produced this message - determines which parts of the app are
// allowed to render it (see MainWindow's secretModeOn_ split, which this
// mirrors): a real TDLib message read/sent as plain Telegram content, one
// that passed through crypto::CryptoLayer, or a local status/system line
// with no Telegram counterpart at all.
enum class MessageOrigin { Plain, Secret, System };

struct MessageData {
    ConversationId chatId = 0;
    // A real TDLib message id for Plain messages; DataRegistry assigns a
    // synthetic negative id (see upsertMessage) for Secret/System messages,
    // which have none - callers never need to invent their own.
    MessageId id = 0;
    MessageOrigin origin = MessageOrigin::Plain;
    bool isOutgoing = false;
    std::string text;
    std::string photoPath;
    std::string senderName;
    std::int64_t date = 0;
    std::int64_t mediaAlbumId = 0;
    MessageId replyToMessageId = 0;
};

struct PeerData {
    ConversationId id = 0;
    std::string title;
    std::string lastMessagePreview;
    int unreadCount = 0;
    bool hasActiveConversation = false;
    bool isChannel = false;
    std::string photoPath;
};

// Minimal in-process pub/sub. A Subscription unsubscribes on destruction
// (RAII) - hold on to it for as long as the callback should stay live,
// same lifetime discipline as lib_rpl's own subscriptions, just without
// the dependency.
template <typename... Args>
class Signal {
public:
    using Slot = std::function<void(Args...)>;

    class Subscription {
    public:
        Subscription() = default;
        Subscription(Signal* owner, std::size_t id) : owner_(owner), id_(id) {}
        Subscription(Subscription&& other) noexcept
            : owner_(other.owner_), id_(other.id_) {
            other.owner_ = nullptr;
        }
        Subscription& operator=(Subscription&& other) noexcept {
            if (this != &other) {
                reset();
                owner_ = other.owner_;
                id_ = other.id_;
                other.owner_ = nullptr;
            }
            return *this;
        }
        Subscription(const Subscription&) = delete;
        Subscription& operator=(const Subscription&) = delete;
        ~Subscription() { reset(); }

        void reset() {
            if (owner_ != nullptr) {
                owner_->unsubscribe(id_);
                owner_ = nullptr;
            }
        }

    private:
        Signal* owner_ = nullptr;
        std::size_t id_ = 0;
    };

    Subscription subscribe(Slot slot) {
        std::size_t id = nextId_++;
        slots_[id] = std::move(slot);
        return Subscription(this, id);
    }

    void emit(Args... args) const {
        // Copies the slot list before calling out: a slot may subscribe or
        // unsubscribe (e.g. a widget destroyed mid-callback) during emit,
        // which would otherwise invalidate slots_ while this loop iterates
        // it.
        std::vector<Slot> toCall;
        toCall.reserve(slots_.size());
        for (const auto& [id, slot] : slots_) {
            toCall.push_back(slot);
        }
        for (const auto& slot : toCall) {
            slot(args...);
        }
    }

private:
    friend class Subscription;
    void unsubscribe(std::size_t id) { slots_.erase(id); }

    std::size_t nextId_ = 0;
    std::map<std::size_t, Slot> slots_;
};

class DataRegistry {
public:
    // Inserts on first sight (data.id == 0 gets a synthetic negative id
    // assigned here, see MessageData::id's own comment), otherwise
    // overwrites the existing entry in place - pointer identity is
    // preserved across an update, so a caller that cached the returned
    // MessageData* earlier keeps a valid, now-updated view. Fires
    // itemUpdated() either way.
    MessageData* upsertMessage(MessageData data);
    // For mutating a MessageData* already obtained from upsertMessage/
    // message() (e.g. a photo path arriving asynchronously after the
    // message itself was created) without needing its (chatId, id) again -
    // mutate the pointed-to struct directly, then call this to fire
    // itemUpdated() for it.
    void notifyUpdated(MessageData* data) const;
    MessageData* message(ConversationId chatId, MessageId id) const;
    void removeMessage(ConversationId chatId, MessageId id);
    // All messages for one chat, oldest-first (insertion order) - the
    // shape a message list actually renders from.
    std::vector<MessageData*> messagesFor(ConversationId chatId) const;

    PeerData* upsertPeer(PeerData data);
    PeerData* peer(ConversationId id) const;
    std::vector<PeerData*> allPeers() const;

    Signal<MessageData*>::Subscription onItemUpdated(Signal<MessageData*>::Slot slot) {
        return itemUpdated_.subscribe(std::move(slot));
    }
    Signal<ConversationId, MessageId>::Subscription onItemRemoved(
        Signal<ConversationId, MessageId>::Slot slot) {
        return itemRemoved_.subscribe(std::move(slot));
    }
    Signal<PeerData*>::Subscription onPeerUpdated(Signal<PeerData*>::Slot slot) {
        return peerUpdated_.subscribe(std::move(slot));
    }

private:
    struct Key {
        ConversationId chatId;
        MessageId id;
        bool operator<(const Key& other) const {
            return chatId != other.chatId ? chatId < other.chatId : id < other.id;
        }
    };
    // std::map, not unordered_map: messagesFor() needs insertion-adjacent
    // ordering for the (chatId, id) range it scans, and a real TDLib id is
    // monotonically increasing within a chat, so ordering by Key already
    // gives chronological order for Plain messages. Secret/System messages
    // (synthetic negative ids, see upsertMessage) sort before every Plain
    // one in the same chat as a result - callers needing true chronological
    // order across origins (mixed secret/plain history) must still sort by
    // MessageData::date themselves; this registry only guarantees identity
    // and update delivery, not a display order.
    std::map<Key, std::unique_ptr<MessageData>> messages_;
    std::map<ConversationId, std::unique_ptr<PeerData>> peers_;
    // Per-chat counter, not a single global one: keeps ids stable and
    // small even if only a few chats ever accumulate Secret/System
    // messages, and a collision is impossible either way since Key
    // already scopes by chatId.
    std::map<ConversationId, MessageId> nextLocalId_;

    Signal<MessageData*> itemUpdated_;
    Signal<ConversationId, MessageId> itemRemoved_;
    Signal<PeerData*> peerUpdated_;
};

}  // namespace zkgram::core
