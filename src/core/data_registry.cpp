#include "core/data_registry.hpp"

#include <limits>

namespace zkgram::core {

MessageData* DataRegistry::upsertMessage(MessageData data) {
    if (data.id == 0) {
        data.id = --nextLocalId_[data.chatId];
    }
    Key key{data.chatId, data.id};
    auto it = messages_.find(key);
    if (it == messages_.end()) {
        auto owned = std::make_unique<MessageData>(std::move(data));
        MessageData* ptr = owned.get();
        messages_.emplace(key, std::move(owned));
        itemUpdated_.emit(ptr);
        return ptr;
    }
    *it->second = std::move(data);
    itemUpdated_.emit(it->second.get());
    return it->second.get();
}

void DataRegistry::notifyUpdated(MessageData* data) const {
    itemUpdated_.emit(data);
}

MessageData* DataRegistry::message(ConversationId chatId, MessageId id) const {
    auto it = messages_.find(Key{chatId, id});
    return it != messages_.end() ? it->second.get() : nullptr;
}

void DataRegistry::removeMessage(ConversationId chatId, MessageId id) {
    Key key{chatId, id};
    if (messages_.erase(key) != 0) {
        itemRemoved_.emit(chatId, id);
    }
}

std::vector<MessageData*> DataRegistry::messagesFor(ConversationId chatId) const {
    std::vector<MessageData*> result;
    // messages_ is ordered by (chatId, id) - lower_bound/loop over the
    // contiguous run for this chatId is cheaper than scanning the whole
    // map and filtering, and avoids depending on a separate per-chat index.
    for (auto it = messages_.lower_bound(Key{chatId, std::numeric_limits<MessageId>::min()});
         it != messages_.end() && it->first.chatId == chatId; ++it) {
        result.push_back(it->second.get());
    }
    return result;
}

PeerData* DataRegistry::upsertPeer(PeerData data) {
    ConversationId id = data.id;
    auto it = peers_.find(id);
    if (it == peers_.end()) {
        auto owned = std::make_unique<PeerData>(std::move(data));
        PeerData* ptr = owned.get();
        peers_.emplace(id, std::move(owned));
        peerUpdated_.emit(ptr);
        return ptr;
    }
    *it->second = std::move(data);
    peerUpdated_.emit(it->second.get());
    return it->second.get();
}

PeerData* DataRegistry::peer(ConversationId id) const {
    auto it = peers_.find(id);
    return it != peers_.end() ? it->second.get() : nullptr;
}

std::vector<PeerData*> DataRegistry::allPeers() const {
    std::vector<PeerData*> result;
    result.reserve(peers_.size());
    for (const auto& [id, peer] : peers_) {
        result.push_back(peer.get());
    }
    return result;
}

}  // namespace zkgram::core
