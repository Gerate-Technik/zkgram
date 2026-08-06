#include "telegram/telegram_client.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include <td/telegram/Client.h>
#include <td/telegram/td_api.h>

#include "config.hpp"
#include "platform/paths.hpp"

// TDLib's generated API lives in td::td_api, not a global td_api namespace;
// this alias matches the pattern TDLib's own example/cpp/td_example.cpp
// uses so the rest of this file can refer to it unqualified.
namespace td_api = td::td_api;

// Compiled and runs against a real TDLib build as of 2026-08-02 (see
// zkgram/TODO.md) - follows the standard td::ClientManager pattern from
// TDLib's own example/cpp/td_example.cpp, adapted to this project's
// callback-based transport contract. Authorization, chat resolution and
// sending/receiving text have all been exercised live against a real
// account. Multi-chat support (loadChatList/resolveCompanion taking a
// ChatId instead of one pinned companion) added 2026-08-02, not yet
// exercised live - see TODO.md, "мультиюзерность".
namespace zkgram::telegram {

namespace {

// Next to the exe (see session.cpp's identical helper) - not a hardcoded
// dev-machine path, so beta testers' copies actually produce a log file.
std::string logFilePath() {
    std::string dir = zkgram::platform::executableDir();
    if (dir.empty()) {
        return "zkgram_debug.log";
    }
    return (std::filesystem::path(dir) / "zkgram_debug.log").string();
}

// Temporary diagnostic-only logging (same purpose/removal plan as
// main_window.cpp's logDebug() - see TODO.md, the Next-button investigation).
// Plain std::ofstream, not Qt: this file has no Qt dependency and should
// not gain one just for this.
void logDebug(const std::string& message) {
    std::ofstream file(logFilePath(), std::ios::app);
    if (file) {
        file << message << "\n";
    }
}

std::string stripLeadingAt(const std::string& username) {
    return (!username.empty() && username.front() == '@') ? username.substr(1) : username;
}

bool looksLikePhoneNumber(const std::string& companion) {
    return !companion.empty() && companion.front() == '+';
}

// Ошибки авторизации, означающие "пользователь ввёл не то": на них есть
// смысл переспросить тот же шаг, а не рвать весь вход. Раньше любая ошибка
// шла в markFailed(), поэтому одна опечатка в коде из SMS убивала сессию
// целиком и приходилось перезапускать приложение.
//
// PHONE_CODE_EXPIRED сюда сознательно не входит: на нём TDLib сам
// возвращается в authorizationStateWaitPhoneNumber, повторять шаг кода
// бессмысленно.
bool isRetryableAuthError(const std::string& message) {
    static const char* kRetryable[] = {"PHONE_CODE_INVALID", "PHONE_CODE_EMPTY", "PASSWORD_HASH_INVALID",
                                        "PHONE_NUMBER_INVALID"};
    for (const char* candidate : kRetryable) {
        if (message.find(candidate) != std::string::npos) {
            return true;
        }
    }
    return false;
}

// Промпт, уходящий в UiProvider::requestCredential, может нести причину
// предыдущей неудачи: первая строка - сам шаг, всё после перевода строки -
// текст ошибки. Консольный UI печатает промпт целиком, Qt-плагин разбирает
// его в showRequestCredentialDialog() и показывает вторую часть под полем
// ввода. Без этого повторный запрос выглядел бы как экран, который
// беспричинно спрашивает одно и то же.
std::string promptWithError(const std::string& basePrompt, const std::string& error) {
    return error.empty() ? basePrompt : basePrompt + "\n" + error;
}

Bytes stringToBytes(const std::string& text) {
    return Bytes(text.begin(), text.end());
}

// Extension-based only, no content sniffing: good enough to decide between
// TDLib's inputMessagePhoto (inline preview on both ends) and the generic
// inputMessageDocument fallback for everything else.
bool looksLikeImagePath(const std::string& filePath) {
    auto dot = filePath.find_last_of('.');
    if (dot == std::string::npos) {
        return false;
    }
    std::string ext = filePath.substr(dot + 1);
    for (char& c : ext) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return ext == "jpg" || ext == "jpeg" || ext == "png" || ext == "gif" || ext == "webp" || ext == "bmp";
}

// Short preview text for the chat list sidebar - only messageText gets a
// real preview (its content is plaintext-shaped already, whether or not it
// is actually a zkgram ciphertext blob); everything else just names the
// kind, matching how a normal Telegram client's chat list shows "Photo"/
// "Document" instead of trying to render non-text content inline.
std::string previewFor(const td_api::message& message) {
    if (message.content_ == nullptr) {
        return "";
    }
    switch (message.content_->get_id()) {
        case td_api::messageText::ID: {
            auto* content = static_cast<const td_api::messageText*>(message.content_.get());
            return content->text_ != nullptr ? content->text_->text_ : "";
        }
        case td_api::messageDocument::ID:
            return "Document";
        case td_api::messagePhoto::ID:
            return "Photo";
        default:
            return "";
    }
}

}  // namespace

struct TelegramClient::Impl {
    explicit Impl(std::string dataDir) : dataDir_(std::move(dataDir)) {}

    ~Impl() {
        disconnect();
    }

    // ---- query plumbing -------------------------------------------------

    using ObjectPtr = td_api::object_ptr<td_api::Object>;
    using Handler = std::function<void(ObjectPtr)>;

    std::uint64_t nextQueryId() {
        return ++lastQueryId_;
    }

    // Fire-and-forget: TDLib is told (via request_id 0) that the response
    // is not needed. Safe to call from any thread.
    void sendNotify(td_api::object_ptr<td_api::Function> function) {
        clientManager_.send(clientId_, 0, std::move(function));
    }

    // Sends a request and runs `handler` on the receive-loop thread once
    // the response arrives. Safe to call from any thread; the handler
    // itself always runs on the background thread started by connect().
    void sendWithHandler(td_api::object_ptr<td_api::Function> function, Handler handler) {
        std::uint64_t queryId = nextQueryId();
        {
            std::lock_guard<std::mutex> lock(handlersMutex_);
            handlers_[queryId] = std::move(handler);
        }
        clientManager_.send(clientId_, queryId, std::move(function));
    }

    // ---- lifecycle --------------------------------------------------------

    void connect() {
        running_ = true;
        receiverThread_ = std::thread([this] { loop(); });

        std::unique_lock<std::mutex> lock(readyMutex_);
        readyCv_.wait(lock, [this] { return ready_ || failed_; });
        if (failed_) {
            throw std::runtime_error("TelegramClient: authorization failed: " + failureReason_);
        }
    }

    void disconnect() {
        if (running_) {
            if (clientId_ != 0) {
                sendNotify(td_api::make_object<td_api::close>());
            }
            running_ = false;
        }

        // Join outside the running_ guard above: running_ means "the receive
        // loop should keep going", not "there is a thread to clean up", and
        // markFailed() (any authorization failure - empty phone number,
        // rejected code, ...) already sets it to false without joining
        // anything. Gating the join on it therefore skipped the join
        // entirely on exactly the path where connect() throws, leaving
        // receiverThread_ joinable for ~Impl to destroy - and destroying a
        // joinable std::thread calls std::terminate. The CLI showed this as
        // "terminate called without an active exception" instead of the
        // "Connection failed: ..." message main.cpp was trying to print
        // (the message was lost because abort() does not flush std::cout).
        // Same discipline as probeTimeoutThreads_ below.
        if (receiverThread_.joinable()) {
            receiverThread_.join();
        }

        {
            // If connect() is still blocked waiting for authorizationStateReady
            // (e.g. disconnect() was called because the UI was closed mid-login),
            // unblock it instead of hanging forever.
            std::lock_guard<std::mutex> lock(readyMutex_);
            if (!ready_ && !failed_) {
                failed_ = true;
                failureReason_ = "disconnected before authorization completed";
                readyCv_.notify_all();
            }
        }
        // Outside the running_ guard above (not skipped on a second
        // disconnect() call): each probeZkgramPresence() timeout thread
        // sleeps at most 6 seconds before exiting on its own, and must be
        // joined before this Impl can be destroyed, or a thread could touch
        // a dangling `this` after destruction (same discipline as
        // receiverThread_).
        std::lock_guard<std::mutex> lock(probeTimeoutThreadsMutex_);
        for (auto& thread : probeTimeoutThreads_) {
            if (thread.joinable()) {
                thread.join();
            }
        }
        probeTimeoutThreads_.clear();
    }

    // ZKGRAM_PROXY_SERVER/ZKGRAM_PROXY_PORT/ZKGRAM_PROXY_SECRET configure an
    // MTProto proxy (td_api::proxyTypeMtproto) - not a generic SOCKS5/HTTP
    // proxy. Some networks pass ordinary HTTPS through a VPN/tunnel just
    // fine (e.g. https://api.telegram.org loads) while specifically
    // fingerprinting and dropping TDLib's actual MTProto connections to
    // Telegram's DCs, which then hang forever with no error (see
    // TODO.md, "не запускается ни через какой VPN"; a plain SOCKS5/HTTP
    // proxy would not help here, only an MTProto proxy's obfuscation
    // does). Sent once at the start of the receive loop, before any
    // connection attempt, via sendNotify (fire-and-forget, matching
    // setTdlibParameters below) - if it fails, TDLib just falls back to a
    // direct connection with the same behavior as not setting this at all.
    void configureProxyIfSet() {
        const char* server = std::getenv("ZKGRAM_PROXY_SERVER");
        const char* portStr = std::getenv("ZKGRAM_PROXY_PORT");
        const char* secret = std::getenv("ZKGRAM_PROXY_SECRET");
        if (!server || !portStr || !secret) {
            return;
        }
        auto type = td_api::make_object<td_api::proxyTypeMtproto>(secret);
        auto proxy = td_api::make_object<td_api::proxy>(server, std::atoi(portStr), std::move(type));
        sendNotify(td_api::make_object<td_api::addProxy>(std::move(proxy), /*enable=*/true, /*comment=*/""));
    }

    void loop() {
        // ZKGRAM_TG_LOG_VERBOSITY (0-10, TDLib default is 3) - raise to 5+
        // to see actual per-connection network events (which DC/IP TDLib
        // is dialing, connect/timeout per socket) instead of only the
        // high-level receive-loop polling noise default verbosity shows.
        // Diagnostic-only, not needed for normal operation - see TODO.md,
        // "MTProto висит без ошибок даже через VPN".
        if (const char* verbosity = std::getenv("ZKGRAM_TG_LOG_VERBOSITY")) {
            td::ClientManager::execute(td_api::make_object<td_api::setLogVerbosityLevel>(std::atoi(verbosity)));
        }
        clientId_ = clientManager_.create_client_id();
        configureProxyIfSet();
        // Wakes up the client so the first authorizationStateWaitTdlibParameters
        // update is produced without waiting for an unrelated request.
        sendNotify(td_api::make_object<td_api::getOption>("version"));

        while (running_) {
            auto response = clientManager_.receive(1.0);
            if (!response.object) {
                continue;
            }
            // processUpdate/handler eventually call into onBytesReceived_/
            // onFileReceived_ (see below), which core::Session wires to
            // CryptoLayer's Python-side message ingestion - std::thread's
            // entry function has no implicit exception handling, so an
            // exception thrown anywhere in that chain (a malformed
            // ciphertext, a Python-side decode error, etc.) used to escape
            // this thread uncaught and call std::terminate(), crashing the
            // whole process with no diagnostic (matches the crash dumps
            // analyzed after "entered phone, then password" - see TODO.md).
            // One bad update must not take down an otherwise-working
            // connection, so it is swallowed here instead.
            try {
                if (response.request_id == 0) {
                    processUpdate(std::move(response.object));
                    continue;
                }
                Handler handler;
                {
                    std::lock_guard<std::mutex> lock(handlersMutex_);
                    auto it = handlers_.find(response.request_id);
                    if (it != handlers_.end()) {
                        handler = std::move(it->second);
                        handlers_.erase(it);
                    }
                }
                if (handler) {
                    handler(std::move(response.object));
                }
            } catch (const std::exception&) {
                // No logger at this layer (see docs/README.md on layering) -
                // nothing to attach the message to yet. Swallowing beats
                // crashing; making this observable in the UI is tracked in
                // TODO.md.
            } catch (...) {
            }
        }
    }

    void markReady() {
        {
            std::lock_guard<std::mutex> lock(readyMutex_);
            ready_ = true;
        }
        readyCv_.notify_all();
    }

    void markFailed(std::string reason) {
        {
            std::lock_guard<std::mutex> lock(readyMutex_);
            failed_ = true;
            failureReason_ = std::move(reason);
        }
        readyCv_.notify_all();
        running_ = false;
    }

    std::string requestAuthInput(const std::string& prompt) {
        logDebug("requestAuthInput: entered, prompt=" + prompt);
        if (!authInputProvider_) {
            markFailed("no auth input provider registered for prompt: " + prompt);
            return {};
        }
        std::string result = authInputProvider_(prompt);
        logDebug("requestAuthInput: authInputProvider_ returned, length=" + std::to_string(result.size()));
        return result;
    }

    // ---- update dispatch ----------------------------------------------

    void processUpdate(ObjectPtr object) {
        switch (object->get_id()) {
            case td_api::updateAuthorizationState::ID: {
                auto* update = static_cast<td_api::updateAuthorizationState*>(object.get());
                onAuthorizationStateUpdate(std::move(update->authorization_state_));
                break;
            }
            case td_api::updateNewMessage::ID: {
                auto* update = static_cast<td_api::updateNewMessage*>(object.get());
                onNewMessage(*update->message_);
                break;
            }
            case td_api::updateFile::ID: {
                auto* update = static_cast<td_api::updateFile*>(object.get());
                onFileUpdate(*update->file_);
                break;
            }
            case td_api::updateNewChat::ID: {
                auto* update = static_cast<td_api::updateNewChat*>(object.get());
                onChatUpsert(*update->chat_);
                break;
            }
            case td_api::updateChatPosition::ID: {
                auto* update = static_cast<td_api::updateChatPosition*>(object.get());
                onChatPositionUpdate(update->chat_id_, update->position_.get());
                break;
            }
            case td_api::updateChatLastMessage::ID: {
                auto* update = static_cast<td_api::updateChatLastMessage*>(object.get());
                onChatLastMessageUpdate(update->chat_id_, update->last_message_.get());
                for (auto& position : update->positions_) {
                    onChatPositionUpdate(update->chat_id_, position.get());
                }
                break;
            }
            case td_api::updateChatReadInbox::ID: {
                auto* update = static_cast<td_api::updateChatReadInbox*>(object.get());
                onChatUnreadCountUpdate(update->chat_id_, update->unread_count_);
                break;
            }
            case td_api::updateChatPhoto::ID: {
                // Not always known yet by the time updateNewChat first
                // fires (see onChatUpsert) - TDLib pushes it separately,
                // sometimes noticeably later, once the photo is actually
                // available. Missing this case was why avatars only ever
                // showed up for chats whose photo happened to already be
                // attached at chat-creation time.
                auto* update = static_cast<td_api::updateChatPhoto*>(object.get());
                auto it = chats_.find(update->chat_id_);
                if (it != chats_.end()) {
                    requestChatPhoto(update->chat_id_, it->second, update->photo_.get());
                    publishChatList();
                }
                break;
            }
            default:
                break;
        }
    }

    // TDLib's own example/cpp/td_example.cpp always sends authorization-flow
    // requests (setTdlibParameters, setAuthenticationPhoneNumber, etc.) with
    // a real response handler, never fire-and-forget (request_id 0, see
    // sendNotify) - unlike every other request in this file. Matching that:
    // besides surfacing real errors (wrong phone format, flood wait, etc.)
    // via markFailed instead of silently hanging forever with no
    // diagnostic, sending these with request_id 0 was the likely cause of
    // TDLib never advancing past authorizationStateWaitTdlibParameters at
    // all - see TODO.md, "зависает после пароля, ни ошибки, ни прогресса".
    void sendAuthRequest(td_api::object_ptr<td_api::Function> function) {
        logDebug("sendAuthRequest: sending request");
        sendWithHandler(std::move(function), [this](ObjectPtr object) {
            logDebug("sendAuthRequest: response received, id=" + std::to_string(object->get_id()));
            if (object->get_id() == td_api::error::ID) {
                auto* error = static_cast<td_api::error*>(object.get());
                logDebug("sendAuthRequest: error response: " + error->message_);
                markFailed("TDLib: " + error->message_);
            }
        });
    }

    // Собирает запрос из введённого пользователем значения и, если TDLib
    // отвечает "введено не то" (isRetryableAuthError), переспрашивает тот же
    // шаг вместо markFailed(). Повтор приходится инициировать самим: на таких
    // ошибках TDLib остаётся в прежнем authorizationState и нового update не
    // присылает, поэтому onAuthorizationStateUpdate второй раз не позовут.
    //
    // Рекурсия идёт через обработчик ответа, то есть выполняется на потоке
    // receive-лупа - там же, где и onAuthorizationStateUpdate, который и так
    // блокируется на requestAuthInput, пока пользователь печатает. Стек не
    // растёт: каждый следующий запрос уходит из обработчика предыдущего,
    // а не изнутри вложенного вызова.
    //
    // Пустой ввод трактуется как отмена (закрыли окно - см.
    // AuthSlide::cancelPending, или EOF на stdin в консольном UI). Без этой
    // проверки повтор был бы бесконечным: пустое значение TDLib отклонит
    // снова, и мы бы спрашивали заново уже без участия пользователя.
    using AuthRequestFactory = std::function<td_api::object_ptr<td_api::Function>(const std::string&)>;

    void sendAuthRequestWithRetry(AuthRequestFactory makeRequest, std::string basePrompt,
                                   const std::string& lastError = {}) {
        std::string value = requestAuthInput(promptWithError(basePrompt, lastError));
        if (value.empty()) {
            markFailed("authorization cancelled");
            return;
        }
        logDebug("sendAuthRequestWithRetry: sending request for step " + basePrompt);
        sendWithHandler(makeRequest(value), [this, makeRequest, basePrompt](ObjectPtr object) {
            logDebug("sendAuthRequestWithRetry: response received, id=" + std::to_string(object->get_id()));
            if (object->get_id() != td_api::error::ID) {
                return;
            }
            auto* error = static_cast<td_api::error*>(object.get());
            logDebug("sendAuthRequestWithRetry: error response: " + error->message_);
            if (isRetryableAuthError(error->message_)) {
                sendAuthRequestWithRetry(makeRequest, basePrompt, error->message_);
                return;
            }
            markFailed("TDLib: " + error->message_);
        });
    }

    void onAuthorizationStateUpdate(td_api::object_ptr<td_api::AuthorizationState> state) {
        logDebug("onAuthorizationStateUpdate: state id=" + std::to_string(state->get_id()));
        switch (state->get_id()) {
            case td_api::authorizationStateWaitTdlibParameters::ID: {
                // TDLib creates only the last path component itself, not
                // parent directories (std::filesystem::create_directory
                // semantics, not create_directories) - if dataDir_ itself
                // does not exist yet (a fresh install, first run), TDLib
                // fails with "Can't create directory .../tdlib: path not
                // found" and never sends another authorization state
                // update. That failure used to be invisible: it was
                // reported on the setTdlibParameters response, which
                // sendNotify (request_id 0) discards - the whole client
                // just sat there polling forever with no error and no
                // progress (see TODO.md, "зависает после пароля, ни
                // ошибки, ни прогресса" - the real cause, not a VPN/proxy
                // issue as first suspected). create_directories here
                // covers both the fresh-install case and any deeper
                // missing parents; sendAuthRequest below (not sendNotify)
                // is what actually surfaces this class of error now.
                std::error_code ec;
                std::filesystem::create_directories(dataDir_ + "/tdlib", ec);

                auto parameters = td_api::make_object<td_api::setTdlibParameters>();
                parameters->database_directory_ = dataDir_ + "/tdlib";
                parameters->use_message_database_ = true;
                parameters->use_secret_chats_ = false;
                // ZKGRAM_TG_API_ID/ZKGRAM_TG_API_HASH override the built-in
                // (public Telegram Desktop) credentials without a rebuild -
                // Telegram's servers can delay/withhold the login code for
                // sign-ins from a shared/well-known api_id, and a personal
                // pair from my.telegram.org is the standard fix (see
                // zkgram/TODO.md, "Коды авторизации не приходили").
                const char* envApiId = std::getenv("ZKGRAM_TG_API_ID");
                const char* envApiHash = std::getenv("ZKGRAM_TG_API_HASH");
                parameters->api_id_ = envApiId ? std::atoi(envApiId) : config::kTelegramApiId;
                parameters->api_hash_ = envApiHash ? envApiHash : config::kTelegramApiHash;
                parameters->system_language_code_ = "en";
                parameters->device_model_ = "zkgram";
                parameters->application_version_ = "0.1";
                sendAuthRequest(std::move(parameters));
                break;
            }
            case td_api::authorizationStateWaitPhoneNumber::ID: {
                sendAuthRequestWithRetry(
                    [](const std::string& value) -> td_api::object_ptr<td_api::Function> {
                        return td_api::make_object<td_api::setAuthenticationPhoneNumber>(value, nullptr);
                    },
                    "Phone number");
                break;
            }
            case td_api::authorizationStateWaitCode::ID: {
                sendAuthRequestWithRetry(
                    [](const std::string& value) -> td_api::object_ptr<td_api::Function> {
                        return td_api::make_object<td_api::checkAuthenticationCode>(value);
                    },
                    "Login code");
                break;
            }
            case td_api::authorizationStateWaitPassword::ID: {
                sendAuthRequestWithRetry(
                    [](const std::string& value) -> td_api::object_ptr<td_api::Function> {
                        return td_api::make_object<td_api::checkAuthenticationPassword>(value);
                    },
                    "Two-factor password");
                break;
            }
            case td_api::authorizationStateReady::ID: {
                // No single companion to resolve anymore (see TODO.md,
                // "мультиюзерность") - core::Session decides what to do
                // next (load the chat list, resolve a specific companion,
                // ...) once it sees onReady, via TelegramClient's public
                // API, not automatically here.
                markReady();
                break;
            }
            case td_api::authorizationStateClosed::ID: {
                running_ = false;
                break;
            }
            default:
                // authorizationStateWaitEmailAddress/Code, logOut, etc. are not
                // supported by this minimal personal-account client.
                break;
        }
    }

    // ---- chat list --------------------------------------------------------

    // TDLib pushes chat list membership/ordering as a set of
    // per-ChatList positions on each chat (a chat can be in several lists
    // - archive, main, folders); order_ == 0 means "not in this list".
    // Only chatListMain is tracked here - no archive/folder support, not
    // needed for a personal E2E companion picker.
    bool isMainListOrder(const td_api::chatPosition* position) const {
        return position != nullptr && position->list_ != nullptr && position->list_->get_id() == td_api::chatListMain::ID;
    }

    // Forward-declared: used by requestChatPhoto()'s signature below,
    // defined in full further down next to chats_ (see the member list).
    struct ChatEntry;

    void onChatUpsert(const td_api::chat& chat) {
        ChatEntry& entry = chats_[chat.id_];
        entry.title = chat.title_;
        entry.unreadCount = chat.unread_count_;
        if (chat.last_message_ != nullptr) {
            entry.lastMessagePreview = previewFor(*chat.last_message_);
        }
        for (auto& position : chat.positions_) {
            if (isMainListOrder(position.get())) {
                entry.order = position->order_;
            }
        }
        // Only a broadcast channel is excluded from encryption - CryptoLayer
        // is a 1:1 handshake between two nodes, it has no group protocol, so
        // offering "Start encrypted session" on a channel's one-to-many feed
        // does not make sense (see chatTypeSupergroup::is_channel_).
        if (chat.type_ != nullptr && chat.type_->get_id() == td_api::chatTypeSupergroup::ID) {
            entry.isChannel = static_cast<const td_api::chatTypeSupergroup*>(chat.type_.get())->is_channel_;
        }
        requestChatPhoto(chat.id_, entry, chat.photo_.get());
        publishChatList();
    }

    // Kicks off (or finishes, if TDLib already has it cached locally) a
    // download of the chat's small profile photo - same downloadFile
    // mechanism as an incoming message attachment, but tracked in its own
    // pendingAvatarDownloads_ map: unlike a received file, a finished
    // avatar download must update chats_/publishChatList(), not flow into
    // crypto::CryptoLayer's decryption pipeline (see onFileUpdate).
    void requestChatPhoto(ChatId chatId, ChatEntry& entry, const td_api::chatPhotoInfo* photo) {
        if (photo == nullptr || photo->small_ == nullptr) {
            logDebug("requestChatPhoto: chat " + std::to_string(chatId) + " has no photo");
            return;
        }
        const td_api::file& file = *photo->small_;
        if (file.local_ != nullptr && file.local_->is_downloading_completed_) {
            entry.photoPath = file.local_->path_;
            logDebug("requestChatPhoto: chat " + std::to_string(chatId) + " already local at " + entry.photoPath);
            return;
        }
        logDebug("requestChatPhoto: chat " + std::to_string(chatId) + " requesting download of file " +
                  std::to_string(file.id_));
        {
            std::lock_guard<std::mutex> lock(pendingAvatarsMutex_);
            pendingAvatarDownloads_[file.id_] = chatId;
        }
        // sendWithHandler (a real request_id), not sendNotify (request_id
        // 0) - td/telegram/Client.h documents request_id 0 as reserved for
        // TDLib's own incoming updates, "incoming requests are thus not
        // allowed to have request_id == 0". Every avatar requested via
        // sendNotify (as this used to do, same as the rest of this file's
        // fire-and-forget calls) never produced a single updateFile push,
        // at any priority - logging the synchronous response here as well,
        // to see immediately whether TDLib considers the download
        // started/already local/failed outright, rather than only finding
        // out from a later push that may never come.
        sendWithHandler(td_api::make_object<td_api::downloadFile>(file.id_, 32, 0, 0, false),
                         [this, chatId](ObjectPtr object) {
                             if (object->get_id() == td_api::error::ID) {
                                 auto* error = static_cast<td_api::error*>(object.get());
                                 logDebug("requestChatPhoto: downloadFile for chat " + std::to_string(chatId) +
                                           " failed: " + error->message_);
                                 return;
                             }
                             if (object->get_id() != td_api::file::ID) {
                                 return;
                             }
                             auto* file = static_cast<td_api::file*>(object.get());
                             bool completed = file->local_ != nullptr && file->local_->is_downloading_completed_;
                             bool active = file->local_ != nullptr && file->local_->is_downloading_active_;
                             logDebug("requestChatPhoto: downloadFile response for chat " + std::to_string(chatId) +
                                       " completed=" + std::to_string(completed) + " active=" + std::to_string(active));
                             if (completed) {
                                 auto it = chats_.find(chatId);
                                 if (it != chats_.end()) {
                                     it->second.photoPath = file->local_->path_;
                                     publishChatList();
                                 }
                             }
                             // If not completed yet, onFileUpdate() picks it
                             // up from here via pendingAvatarDownloads_ once
                             // TDLib pushes updateFile for this file id.
                         });
    }

    void onChatPositionUpdate(ChatId chatId, const td_api::chatPosition* position) {
        auto it = chats_.find(chatId);
        if (it == chats_.end() || !isMainListOrder(position)) {
            return;
        }
        it->second.order = position->order_;
        publishChatList();
    }

    void onChatLastMessageUpdate(ChatId chatId, const td_api::message* lastMessage) {
        auto it = chats_.find(chatId);
        if (it == chats_.end()) {
            return;
        }
        it->second.lastMessagePreview = lastMessage != nullptr ? previewFor(*lastMessage) : "";
        publishChatList();
    }

    void onChatUnreadCountUpdate(ChatId chatId, int unreadCount) {
        auto it = chats_.find(chatId);
        if (it == chats_.end()) {
            return;
        }
        it->second.unreadCount = unreadCount;
        publishChatList();
    }

    // order_ == 0 means "not (or no longer) in this list" per TDLib's own
    // convention - filtered out rather than shown as a zero-order entry.
    // Higher order sorts first (TDLib's ordering key, not a timestamp).
    void publishChatList() {
        if (!chatListCallback_) {
            return;
        }
        std::vector<ChatSummary> summaries;
        summaries.reserve(chats_.size());
        for (auto& [id, entry] : chats_) {
            if (entry.order == 0) {
                continue;
            }
            summaries.push_back(
                ChatSummary{id, entry.title, entry.lastMessagePreview, entry.unreadCount, entry.isChannel, entry.photoPath});
        }
        std::sort(summaries.begin(), summaries.end(),
                  [this](const ChatSummary& a, const ChatSummary& b) { return chats_[a.id].order > chats_[b.id].order; });
        chatListCallback_(summaries);
    }

    void loadChatList(std::function<void(const std::vector<ChatSummary>&)> onUpdate) {
        chatListCallback_ = std::move(onUpdate);
        requestMoreChats();
    }

    // td_api::loadChats only ever loads roughly one page per call (however
    // many chats TDLib's local database/network fetch happens to return
    // that round, not the full `limit_` requested) and is documented to
    // need repeated calls until it returns an error meaning the list is
    // exhausted - this was previously called exactly once, fire-and-forget
    // (sendNotify, the same request_id-0 pattern already found unreliable
    // for avatar downloads), so only whatever small first batch TDLib
    // happened to return ever showed up: most chats, and therefore their
    // avatars and their ability to load message history, never existed in
    // chats_ at all. Chains itself via sendWithHandler's real response
    // instead: each call's completion immediately requests the next page,
    // as fast as TDLib can serve them, until the "no more chats" error
    // response ends the chain. kMaxRounds is a sanity cap, not a normal
    // stopping point - an ordinary account exhausts the list in a handful
    // of rounds.
    void requestMoreChats(int round = 0) {
        constexpr int kMaxRounds = 40;
        if (round >= kMaxRounds) {
            logDebug("requestMoreChats: stopping after " + std::to_string(kMaxRounds) +
                      " rounds (safety cap, not exhaustion)");
            return;
        }
        sendWithHandler(td_api::make_object<td_api::loadChats>(td_api::make_object<td_api::chatListMain>(), 200),
                         [this, round](ObjectPtr object) {
                             if (object->get_id() == td_api::error::ID) {
                                 auto* error = static_cast<td_api::error*>(object.get());
                                 logDebug("requestMoreChats: round " + std::to_string(round) + " stopped: " +
                                           std::to_string(error->code_) + " " + error->message_ + " (" +
                                           std::to_string(chats_.size()) + " chats loaded)");
                                 return;
                             }
                             logDebug("requestMoreChats: round " + std::to_string(round) + " ok, " +
                                       std::to_string(chats_.size()) + " chats loaded so far, requesting more");
                             requestMoreChats(round + 1);
                         });
    }

    void loadMessageHistory(ChatId chatId, std::function<void(const std::vector<PlainMessage>&)> onResult) {
        fetchMessageHistory(chatId, 0, std::move(onResult));
    }

    // beforeMessageId 0 means "from the newest" (the initial load); a
    // non-zero id (the oldest message id already shown) fetches the next
    // older page - same idea as tdesktop's History::loadBack() calling
    // Data::Histories::requestHistory() again with an offset anchored to
    // the oldest currently-loaded message (see
    // Telegram/SourceFiles/history/history.cpp/data/data_histories.cpp in
    // telegram_fork/tdesktop), just without tdesktop's local on-disk
    // message cache - every page here is a fresh TDLib request each time,
    // gap-based pagination via MTProto being wholly TDLib's job under the
    // hood, not something reimplemented here. offset_ 0 with a non-zero
    // from_message_id_ returns the messages strictly older than that
    // anchor, not including it, so callers never need to de-duplicate the
    // anchor message themselves.
    void loadMoreMessageHistory(ChatId chatId, MessageId beforeMessageId,
                                 std::function<void(const std::vector<PlainMessage>&)> onResult) {
        fetchMessageHistory(chatId, beforeMessageId, std::move(onResult));
    }

    void fetchMessageHistory(ChatId chatId, MessageId beforeMessageId,
                              std::function<void(const std::vector<PlainMessage>&)> onResult) {
        sendWithHandler(td_api::make_object<td_api::getChatHistory>(chatId, beforeMessageId, 0, 50, false),
                         [this, chatId, onResult](ObjectPtr object) {
            std::vector<PlainMessage> result;
            // A getChatHistory failure (e.g. "Group chat is deactivated",
            // or any other permission/rate-limit error) used to fall
            // through silently here and just return an empty result,
            // indistinguishable from "this chat genuinely has no history
            // yet" - the caller (maybeLoadPlainHistory in main_window.cpp)
            // marks a chat's history as loaded the moment it asks, so a
            // silent failure meant that chat would never show any history
            // and would never retry, with nothing in the log to explain
            // why. Logging the real error at least makes that diagnosable.
            if (object->get_id() == td_api::error::ID) {
                auto* error = static_cast<td_api::error*>(object.get());
                logDebug("fetchMessageHistory: chat " + std::to_string(chatId) + " getChatHistory failed: " +
                          std::to_string(error->code_) + " " + error->message_);
            }
            if (object->get_id() == td_api::messages::ID) {
                auto* messages = static_cast<td_api::messages*>(object.get());
                for (auto& messagePtr : messages->messages_) {
                    if (messagePtr == nullptr) {
                        continue;
                    }
                    PlainMessage plain;
                    plain.id = messagePtr->id_;
                    plain.isOutgoing = messagePtr->is_outgoing_;
                    plain.text = previewFor(*messagePtr);
                    plain.date = messagePtr->date_;
                    // Only incoming messages need a sender name resolved -
                    // an outgoing one is always "you". messageSenderChat
                    // (an anonymous admin post, or a channel/group posting
                    // as itself) is left unresolved for now - a chat name
                    // would need the same getChat() round trip as
                    // requestSenderName's getUser(), just not implemented
                    // here since anonymous-admin messages are a minority
                    // case, not the "messages from other users don't show
                    // up" gap this is fixing.
                    if (!plain.isOutgoing && messagePtr->sender_id_ != nullptr &&
                        messagePtr->sender_id_->get_id() == td_api::messageSenderUser::ID) {
                        auto* sender = static_cast<td_api::messageSenderUser*>(messagePtr->sender_id_.get());
                        requestSenderName(chatId, plain.id, sender->user_id_);
                    }
                    // A photo message's actual image, not just the "Photo"
                    // placeholder text from previewFor() - matches how a
                    // real Telegram client shows the picture itself, not a
                    // label, in message history. photo_->sizes_ is ordered
                    // smallest to largest; take the largest, same choice
                    // downloadIncomingFile() makes for live messages.
                    if (messagePtr->content_ != nullptr && messagePtr->content_->get_id() == td_api::messagePhoto::ID) {
                        auto* content = static_cast<td_api::messagePhoto*>(messagePtr->content_.get());
                        if (content->photo_ != nullptr && !content->photo_->sizes_.empty()) {
                            auto& largest = content->photo_->sizes_.back();
                            if (largest->photo_ != nullptr) {
                                requestHistoryPhoto(chatId, plain.id, *largest->photo_);
                                if (largest->photo_->local_ != nullptr && largest->photo_->local_->is_downloading_completed_) {
                                    plain.photoPath = largest->photo_->local_->path_;
                                }
                            }
                        }
                    }
                    result.push_back(std::move(plain));
                }
            }
            // getChatHistory returns newest-first; reverse for chronological
            // (oldest-first) display, matching how a normal chat reads.
            std::reverse(result.begin(), result.end());
            onResult(result);
        });
    }

    // getUser()'s response is tied directly to this one call via the
    // sendWithHandler closure - unlike requestHistoryPhoto()/onFileUpdate(),
    // there is no need for a separate pending-request map here, since
    // TDLib does not multiplex several in-flight file downloads onto a
    // single push the way updateFile does.
    void requestSenderName(ChatId chatId, MessageId messageId, std::int64_t userId) {
        sendWithHandler(td_api::make_object<td_api::getUser>(userId), [this, chatId, messageId](ObjectPtr object) {
            if (object->get_id() != td_api::user::ID) {
                return;
            }
            auto* user = static_cast<td_api::user*>(object.get());
            std::string name = user->first_name_;
            if (!user->last_name_.empty()) {
                name += name.empty() ? user->last_name_ : (" " + user->last_name_);
            }
            if (name.empty()) {
                return;
            }
            if (onHistorySenderName_) {
                onHistorySenderName_(chatId, messageId, name);
            }
        });
    }

    // Same idea as requestChatPhoto() (a real request_id via
    // sendWithHandler, not the request_id-0 sendNotify that was confirmed
    // to silently never produce a download for avatars - see
    // requestChatPhoto's own comment) but tracked in pendingHistoryPhotos_
    // and reported through onHistoryPhoto_ instead of publishChatList(),
    // since a history photo belongs to one specific message, not a chat's
    // sidebar entry.
    void requestHistoryPhoto(ChatId chatId, MessageId messageId, const td_api::file& file) {
        if (file.local_ != nullptr && file.local_->is_downloading_completed_) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(pendingHistoryPhotosMutex_);
            pendingHistoryPhotos_[file.id_] = {chatId, messageId};
        }
        sendWithHandler(td_api::make_object<td_api::downloadFile>(file.id_, 32, 0, 0, false),
                         [this, chatId, messageId](ObjectPtr object) {
                             if (object->get_id() != td_api::file::ID) {
                                 return;
                             }
                             auto* file = static_cast<td_api::file*>(object.get());
                             if (file->local_ != nullptr && file->local_->is_downloading_completed_ && onHistoryPhoto_) {
                                 std::lock_guard<std::mutex> lock(pendingHistoryPhotosMutex_);
                                 pendingHistoryPhotos_.erase(file->id_);
                                 onHistoryPhoto_(chatId, messageId, file->local_->path_);
                             }
                             // If not completed yet, onFileUpdate() picks it
                             // up from here via pendingHistoryPhotos_ once
                             // TDLib pushes updateFile for this file id.
                         });
    }

    // ---- live search --------------------------------------------------

    static std::vector<ChatId> chatIdsFrom(const ObjectPtr& object) {
        if (object->get_id() != td_api::chats::ID) {
            return {};
        }
        auto* chats = static_cast<const td_api::chats*>(object.get());
        return std::vector<ChatId>(chats->chat_ids_.begin(), chats->chat_ids_.end());
    }

    void searchChats(const std::string& query, std::function<void(const std::vector<ChatSummary>&)> onResults) {
        if (query.empty()) {
            onResults({});
            return;
        }

        // Two lookups combined, same idea as tdesktop's own dialogs search:
        // searchChatsOnServer covers existing chats/contacts by name,
        // searchPublicChats covers exact-username global matches for
        // someone never messaged before. TDLib guarantees an updateNewChat
        // for every chat id a search returns before the search response
        // itself arrives, so by the time both callbacks below fire, chats_
        // already has an entry for each id - no extra getChat round trip.
        auto combined = std::make_shared<std::vector<ChatId>>();
        auto pending = std::make_shared<int>(2);
        auto onOnePartDone = [this, combined, pending, onResults](std::vector<ChatId> ids) {
            for (ChatId id : ids) {
                if (std::find(combined->begin(), combined->end(), id) == combined->end()) {
                    combined->push_back(id);
                }
            }
            if (--(*pending) > 0) {
                return;  // still waiting on the other half
            }
            std::vector<ChatSummary> summaries;
            summaries.reserve(combined->size());
            for (ChatId id : *combined) {
                auto it = chats_.find(id);
                if (it != chats_.end()) {
                    summaries.push_back(ChatSummary{id, it->second.title, it->second.lastMessagePreview,
                                                     it->second.unreadCount, it->second.isChannel, it->second.photoPath});
                }
            }
            onResults(summaries);
        };

        sendWithHandler(td_api::make_object<td_api::searchChatsOnServer>(query, nullptr, 20),
                        [onOnePartDone](ObjectPtr object) { onOnePartDone(chatIdsFrom(object)); });
        sendWithHandler(td_api::make_object<td_api::searchPublicChats>(query, nullptr),
                        [onOnePartDone](ObjectPtr object) { onOnePartDone(chatIdsFrom(object)); });
    }

    // ---- on-demand companion resolution ------------------------------------

    void resolveCompanion(const std::string& companion, std::function<void(ChatId, const std::string&)> onResolved) {
        logDebug("resolveCompanion: entered, companion=" + companion);
        if (looksLikePhoneNumber(companion)) {
            sendWithHandler(
                td_api::make_object<td_api::searchUserByPhoneNumber>(companion, false),
                [this, companion, onResolved](ObjectPtr object) {
                    if (object->get_id() != td_api::user::ID) {
                        onResolved(0, "companion phone number not found on Telegram");
                        return;
                    }
                    auto* user = static_cast<td_api::user*>(object.get());
                    sendWithHandler(td_api::make_object<td_api::createPrivateChat>(user->id_, false),
                                    [onResolved](ObjectPtr chatObject) { finishResolve(std::move(chatObject), onResolved); });
                });
        } else {
            sendWithHandler(td_api::make_object<td_api::searchPublicChat>(stripLeadingAt(companion)),
                             [onResolved](ObjectPtr chatObject) { finishResolve(std::move(chatObject), onResolved); });
        }
    }

    static void finishResolve(ObjectPtr object, const std::function<void(ChatId, const std::string&)>& onResolved) {
        if (object->get_id() != td_api::chat::ID) {
            std::string message = "could not resolve chat";
            if (object->get_id() == td_api::error::ID) {
                message = static_cast<td_api::error*>(object.get())->message_;
            }
            onResolved(0, message);
            return;
        }
        auto* chat = static_cast<td_api::chat*>(object.get());
        onResolved(chat->id_, "");
    }

    // ---- incoming -----------------------------------------------------

    // ---- zkgram presence probe ------------------------------------------
    //
    // A leading zero-width space plus a fixed tag makes the marker byte-
    // exact and unambiguous to detect, while the readable sentence after it
    // means a person who is not running zkgram (or is looking at this chat
    // in an ordinary Telegram client) sees a self-explanatory message
    // instead of raw protocol gibberish - not hidden from them, just not
    // something a normal client would ever generate or react to on its own.
    // "\xE2\x80\x8B" is the raw UTF-8 encoding of U+200B (zero-width
    // space), spelled out as explicit bytes rather than typed literally -
    // MSVC's handling of a literal non-ASCII character in a narrow string
    // literal depends on the source/execution charset, which is not worth
    // relying on for a single invisible character.
    static constexpr const char* kProbePingPrefix = "\xE2\x80\x8Bzkgram-probe-ping";
    static constexpr const char* kProbePongPrefix = "\xE2\x80\x8Bzkgram-probe-pong";

    static bool startsWith(const std::string& text, const char* prefix) {
        return text.rfind(prefix, 0) == 0;
    }

    // Returns true if `text` was a probe ping/pong and has been fully
    // handled here (auto-reply sent, or a waiting probeZkgramPresence()
    // caller resolved) - the caller must not also treat it as a normal
    // incoming message/ciphertext in that case.
    bool handlePotentialProbe(ChatId chatId, const std::string& text) {
        if (startsWith(text, kProbePingPrefix)) {
            sendPlainText(chatId, std::string(kProbePongPrefix) +
                                       "\nzkgram: yes, this contact uses zkgram too - you can start an encrypted "
                                       "session with them. Automatic message, safe to ignore or delete.");
            return true;
        }
        if (startsWith(text, kProbePongPrefix)) {
            std::function<void(bool)> callback;
            {
                std::lock_guard<std::mutex> lock(probesMutex_);
                auto it = pendingProbes_.find(chatId);
                if (it != pendingProbes_.end()) {
                    callback = std::move(it->second);
                    pendingProbes_.erase(it);
                }
            }
            if (callback) {
                callback(true);
            }
            return true;
        }
        return false;
    }

    void probeZkgramPresence(ChatId chatId, std::function<void(bool)> onResult) {
        {
            std::lock_guard<std::mutex> lock(probesMutex_);
            pendingProbes_[chatId] = onResult;
        }
        sendPlainText(chatId, std::string(kProbePingPrefix) +
                                   "\nzkgram: checking whether this contact uses zkgram to enable an encrypted "
                                   "session. Automatic message, safe to ignore or delete.");

        // No reply within the timeout means either they are not running
        // zkgram, or their zkgram is not currently running/connected -
        // either way, report "not present" rather than waiting forever.
        // Joined in disconnect(), same discipline as every other
        // background thread this class starts.
        std::lock_guard<std::mutex> lock(probeTimeoutThreadsMutex_);
        probeTimeoutThreads_.emplace_back([this, chatId] {
            std::this_thread::sleep_for(std::chrono::seconds(6));
            std::function<void(bool)> callback;
            {
                std::lock_guard<std::mutex> probeLock(probesMutex_);
                auto it = pendingProbes_.find(chatId);
                if (it != pendingProbes_.end()) {
                    callback = std::move(it->second);
                    pendingProbes_.erase(it);
                }
            }
            if (callback) {
                callback(false);
            }
        });
    }

    void onNewMessage(td_api::message& message) {
        if (message.is_outgoing_ || message.content_ == nullptr) {
            return;
        }
        ChatId chatId = message.chat_id_;
        switch (message.content_->get_id()) {
            case td_api::messageText::ID: {
                auto* content = static_cast<td_api::messageText*>(message.content_.get());
                if (content->text_ == nullptr || handlePotentialProbe(chatId, content->text_->text_)) {
                    break;
                }
                if (onBytes_) {
                    onBytes_(chatId, stringToBytes(content->text_->text_));
                }
                // The same message a second time, but as plain readable
                // Telegram content instead of bytes to decrypt. Which of the
                // two a given chat actually wants is core::Session's call -
                // only it knows which chats have a CryptoLayer session - so
                // both are offered and it picks; see its onBytesReceived()/
                // onPlainMessageReceived() dispatchers. Before this existed
                // only the bytes went out, and for a chat with no encrypted
                // session they were dropped on the floor there, which is why
                // a message arriving in an open plain chat never appeared
                // until the chat was reopened and its history refetched.
                if (onPlainMessage_) {
                    PlainMessage plain;
                    plain.id = message.id_;
                    plain.isOutgoing = false;
                    plain.text = content->text_->text_;
                    plain.date = message.date_;
                    onPlainMessage_(chatId, plain);
                    // Resolved asynchronously and delivered through
                    // onHistorySenderName_, exactly as for a history message
                    // (see fetchMessageHistory) - the UI matches it up by
                    // message id, which is why plain.id is a real one here.
                    if (message.sender_id_ != nullptr &&
                        message.sender_id_->get_id() == td_api::messageSenderUser::ID) {
                        auto* sender = static_cast<td_api::messageSenderUser*>(message.sender_id_.get());
                        requestSenderName(chatId, plain.id, sender->user_id_);
                    }
                }
                break;
            }
            case td_api::messageDocument::ID: {
                auto* content = static_cast<td_api::messageDocument*>(message.content_.get());
                if (content->document_ != nullptr && content->document_->document_ != nullptr) {
                    downloadIncomingFile(chatId, content->document_->document_->id_);
                }
                break;
            }
            case td_api::messagePhoto::ID: {
                // photo_->sizes_ is ordered smallest to largest; take the
                // largest available size, same as a real Telegram client's
                // "open in full size" would.
                auto* content = static_cast<td_api::messagePhoto*>(message.content_.get());
                if (content->photo_ != nullptr && !content->photo_->sizes_.empty()) {
                    auto& largest = content->photo_->sizes_.back();
                    if (largest->photo_ != nullptr) {
                        downloadIncomingFile(chatId, largest->photo_->id_);
                    }
                }
                break;
            }
            default:
                break;
        }
    }

    void downloadIncomingFile(ChatId chatId, std::int32_t fileId) {
        {
            std::lock_guard<std::mutex> lock(pendingFilesMutex_);
            pendingFiles_[fileId] = chatId;
        }
        // sendWithHandler (a real request_id) and priority 32 (max), not
        // sendNotify (request_id 0) at priority 1 (min) - the exact same
        // bug already found and fixed for avatar downloads (see
        // requestChatPhoto's comment): request_id 0 is documented in
        // td/telegram/Client.h as reserved for TDLib's own incoming
        // updates, and every download sent that way was confirmed to
        // never produce a single updateFile push. This is the live
        // equivalent for an incoming photo/document attachment inside an
        // active encrypted conversation - the same failure mode reads as
        // "messages in the chat don't load" when it is really just their
        // attachment never arriving, since the surrounding text still
        // comes through fine via CryptoLayer's own onTextReceived, which
        // does not go through this path at all.
        sendWithHandler(td_api::make_object<td_api::downloadFile>(fileId, 32, 0, 0, false), [chatId, fileId](ObjectPtr object) {
            if (object->get_id() == td_api::error::ID) {
                auto* error = static_cast<td_api::error*>(object.get());
                logDebug("downloadIncomingFile: chat " + std::to_string(chatId) + " file " + std::to_string(fileId) +
                          " failed: " + std::to_string(error->code_) + " " + error->message_);
            }
            // If it succeeded but is not yet complete, onFileUpdate() picks
            // it up from here via pendingFiles_ once TDLib pushes
            // updateFile for this file id, same as every other download in
            // this file.
        });
    }

    void onFileUpdate(const td_api::file& file) {
        // Temporary: logs every push regardless of completion, to tell
        // apart "TDLib never even tried" from "download in progress but
        // never finishes" while chasing the avatars-never-appear report.
        bool completed = file.local_ != nullptr && file.local_->is_downloading_completed_;
        bool active = file.local_ != nullptr && file.local_->is_downloading_active_;
        std::int64_t downloadedSize = file.local_ != nullptr ? file.local_->downloaded_size_ : -1;
        logDebug("onFileUpdate: file " + std::to_string(file.id_) + " completed=" + std::to_string(completed) +
                  " active=" + std::to_string(active) + " downloadedSize=" + std::to_string(downloadedSize) +
                  " expectedSize=" + std::to_string(file.expected_size_));
        if (file.local_ == nullptr || !file.local_->is_downloading_completed_) {
            return;
        }

        {
            ChatId avatarChatId = 0;
            bool isAvatar = false;
            {
                std::lock_guard<std::mutex> lock(pendingAvatarsMutex_);
                auto it = pendingAvatarDownloads_.find(file.id_);
                isAvatar = it != pendingAvatarDownloads_.end();
                if (isAvatar) {
                    avatarChatId = it->second;
                    pendingAvatarDownloads_.erase(it);
                }
            }
            if (isAvatar) {
                auto chatIt = chats_.find(avatarChatId);
                if (chatIt != chats_.end()) {
                    chatIt->second.photoPath = file.local_->path_;
                    logDebug("onFileUpdate: avatar for chat " + std::to_string(avatarChatId) + " downloaded to " +
                              file.local_->path_);
                    publishChatList();
                } else {
                    logDebug("onFileUpdate: avatar file " + std::to_string(file.id_) +
                              " completed but chat " + std::to_string(avatarChatId) + " is no longer known");
                }
                return;
            }
        }

        {
            PendingHistoryPhoto pending{};
            bool isHistoryPhoto = false;
            {
                std::lock_guard<std::mutex> lock(pendingHistoryPhotosMutex_);
                auto it = pendingHistoryPhotos_.find(file.id_);
                isHistoryPhoto = it != pendingHistoryPhotos_.end();
                if (isHistoryPhoto) {
                    pending = it->second;
                    pendingHistoryPhotos_.erase(it);
                }
            }
            if (isHistoryPhoto) {
                logDebug("onFileUpdate: history photo for chat " + std::to_string(pending.chatId) + " message " +
                          std::to_string(pending.messageId) + " downloaded to " + file.local_->path_);
                if (onHistoryPhoto_) {
                    onHistoryPhoto_(pending.chatId, pending.messageId, file.local_->path_);
                }
                return;
            }
        }

        ChatId chatId = 0;
        bool isPending = false;
        {
            std::lock_guard<std::mutex> lock(pendingFilesMutex_);
            auto it = pendingFiles_.find(file.id_);
            isPending = it != pendingFiles_.end();
            if (isPending) {
                chatId = it->second;
            }
        }
        if (!isPending) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(pendingFilesMutex_);
            pendingFiles_.erase(file.id_);
        }
        if (onFile_) {
            onFile_(chatId, file.local_->path_);
        }
    }

    // ---- outgoing ---------------------------------------------------------

    // A real request id (sendWithHandler), not the request_id-0 sendNotify
    // every sendMessage call used before - same bug class as
    // requestChatPhoto/downloadIncomingFile (see their own comments):
    // a failed send (blocked by the recipient, chat restrictions, etc.)
    // used to fail completely silently, with nothing logged and no way to
    // tell a genuine delivery failure apart from network lag.
    void sendMessageContent(ChatId chatId, td_api::object_ptr<td_api::InputMessageContent> content) {
        sendWithHandler(
            td_api::make_object<td_api::sendMessage>(chatId, nullptr, nullptr, nullptr, nullptr, std::move(content)),
            [chatId](ObjectPtr object) {
                if (object->get_id() == td_api::error::ID) {
                    auto* error = static_cast<td_api::error*>(object.get());
                    logDebug("sendMessageContent: chat " + std::to_string(chatId) +
                              " failed: " + std::to_string(error->code_) + " " + error->message_);
                }
            });
    }

    void sendPlainText(ChatId chatId, const std::string& text) {
        auto content = td_api::make_object<td_api::inputMessageText>();
        content->text_ = td_api::make_object<td_api::formattedText>(text, std::vector<td_api::object_ptr<td_api::textEntity>>());
        sendMessageContent(chatId, std::move(content));
    }

    void sendBytes(ChatId chatId, const Bytes& data) {
        sendPlainText(chatId, std::string(data.begin(), data.end()));
    }

    void sendFile(ChatId chatId, const std::string& filePath) {
        if (looksLikeImagePath(filePath)) {
            // inputMessagePhoto gives an inline preview on both ends instead
            // of a generic document icon; width_/height_ are left at 0
            // (unknown), which TDLib accepts and fills in server-side.
            // photo_ takes an inputPhoto wrapper (file + thumbnail + video +
            // sticker ids + size), not a raw InputFile - see td_api.h.
            auto photo = td_api::make_object<td_api::inputPhoto>();
            photo->photo_ = td_api::make_object<td_api::inputFileLocal>(filePath);
            photo->width_ = 0;
            photo->height_ = 0;
            auto content = td_api::make_object<td_api::inputMessagePhoto>();
            content->photo_ = std::move(photo);
            sendMessageContent(chatId, std::move(content));
            return;
        }
        // document_ takes an inputDocument wrapper (file + thumbnail +
        // disable_content_type_detection), not a raw InputFile - see td_api.h.
        auto document = td_api::make_object<td_api::inputDocument>();
        document->document_ = td_api::make_object<td_api::inputFileLocal>(filePath);
        document->disable_content_type_detection_ = true;
        auto content = td_api::make_object<td_api::inputMessageDocument>();
        content->document_ = std::move(document);
        sendMessageContent(chatId, std::move(content));
    }

    std::string dataDir_;
    td::ClientManager clientManager_;
    std::int32_t clientId_ = 0;

    std::atomic<bool> running_{false};
    std::thread receiverThread_;

    std::mutex readyMutex_;
    std::condition_variable readyCv_;
    bool ready_ = false;
    bool failed_ = false;
    std::string failureReason_;

    std::uint64_t lastQueryId_ = 0;
    std::mutex handlersMutex_;
    std::map<std::uint64_t, Handler> handlers_;

    std::mutex pendingFilesMutex_;
    std::map<std::int32_t, ChatId> pendingFiles_;

    struct PendingHistoryPhoto {
        ChatId chatId = 0;
        MessageId messageId = 0;
    };
    std::mutex pendingHistoryPhotosMutex_;
    std::map<std::int32_t, PendingHistoryPhoto> pendingHistoryPhotos_;
    std::function<void(ChatId, MessageId, const std::string&)> onHistoryPhoto_;
    std::function<void(ChatId, MessageId, const std::string&)> onHistorySenderName_;

    // ---- zkgram presence probe state ------------------------------------
    std::mutex probesMutex_;
    std::map<ChatId, std::function<void(bool)>> pendingProbes_;
    std::mutex probeTimeoutThreadsMutex_;
    std::vector<std::thread> probeTimeoutThreads_;

    struct ChatEntry {
        std::string title;
        std::string lastMessagePreview;
        int unreadCount = 0;
        std::int64_t order = 0;
        bool isChannel = false;
        std::string photoPath;
    };
    std::map<ChatId, ChatEntry> chats_;
    std::function<void(const std::vector<ChatSummary>&)> chatListCallback_;
    // fileId -> chatId for avatar downloads in flight - separate from
    // pendingFiles_ (incoming message attachments), see requestChatPhoto()/
    // onFileUpdate().
    std::mutex pendingAvatarsMutex_;
    std::map<std::int32_t, ChatId> pendingAvatarDownloads_;

    std::function<std::string(const std::string&)> authInputProvider_;
    std::function<void(ChatId, const Bytes&)> onBytes_;
    std::function<void(ChatId, const std::string&)> onFile_;
    std::function<void(ChatId, const PlainMessage&)> onPlainMessage_;
};

TelegramClient::TelegramClient(std::string dataDir) : impl_(std::make_unique<Impl>(std::move(dataDir))) {}

TelegramClient::~TelegramClient() = default;

void TelegramClient::setAuthInputProvider(std::function<std::string(const std::string& prompt)> provider) {
    impl_->authInputProvider_ = std::move(provider);
}

void TelegramClient::connect() {
    impl_->connect();
}

void TelegramClient::loadChatList(std::function<void(const std::vector<ChatSummary>&)> onUpdate) {
    impl_->loadChatList(std::move(onUpdate));
}

void TelegramClient::loadMessageHistory(ChatId chatId, std::function<void(const std::vector<PlainMessage>&)> onResult) {
    impl_->loadMessageHistory(chatId, std::move(onResult));
}

void TelegramClient::loadMoreMessageHistory(ChatId chatId, MessageId beforeMessageId,
                                             std::function<void(const std::vector<PlainMessage>&)> onResult) {
    impl_->loadMoreMessageHistory(chatId, beforeMessageId, std::move(onResult));
}

void TelegramClient::searchChats(const std::string& query, std::function<void(const std::vector<ChatSummary>&)> onResults) {
    impl_->searchChats(query, std::move(onResults));
}

void TelegramClient::resolveCompanion(const std::string& companion,
                                       std::function<void(ChatId, const std::string&)> onResolved) {
    impl_->resolveCompanion(companion, std::move(onResolved));
}

void TelegramClient::probeZkgramPresence(ChatId chatId, std::function<void(bool)> onResult) {
    impl_->probeZkgramPresence(chatId, std::move(onResult));
}

void TelegramClient::sendBytes(ChatId chatId, const Bytes& data) {
    impl_->sendBytes(chatId, data);
}

void TelegramClient::sendFile(ChatId chatId, const std::string& filePath) {
    impl_->sendFile(chatId, filePath);
}

void TelegramClient::onBytesReceived(std::function<void(ChatId, const Bytes&)> callback) {
    impl_->onBytes_ = std::move(callback);
}

void TelegramClient::onPlainMessageReceived(std::function<void(ChatId, const PlainMessage&)> callback) {
    impl_->onPlainMessage_ = std::move(callback);
}

void TelegramClient::onFileReceived(std::function<void(ChatId, const std::string&)> callback) {
    impl_->onFile_ = std::move(callback);
}

void TelegramClient::onHistoryPhotoReady(std::function<void(ChatId, MessageId, const std::string&)> callback) {
    impl_->onHistoryPhoto_ = std::move(callback);
}

void TelegramClient::onHistorySenderNameReady(std::function<void(ChatId, MessageId, const std::string&)> callback) {
    impl_->onHistorySenderName_ = std::move(callback);
}

void TelegramClient::disconnect() {
    impl_->disconnect();
}

}  // namespace zkgram::telegram
