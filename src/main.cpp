#include <exception>
#include <filesystem>
#include <future>
#include <iostream>
#include <memory>

#include "core/session.hpp"
#include "platform/paths.hpp"

namespace {

// Next to the exe, not a bare "data" resolved against the current working
// directory - see main_qt.cpp for the full explanation (a relative "data"
// silently pointed at a different folder depending on how the exe was
// launched, so the local identity/TDLib session never seemed to persist).
std::string dataDirNextToExe() {
    std::string dir = zkgram::platform::executableDir();
    if (dir.empty()) {
        return "data";
    }
    return (std::filesystem::path(dir) / "data").string();
}

class ConsoleUiProvider : public zkgram::core::UiProvider {
public:
    void onChatListUpdated(const std::vector<zkgram::core::ChatListEntry>& chats) override {
        std::cout << "[chats] " << chats.size() << " chat(s):\n";
        for (const auto& chat : chats) {
            std::cout << "  " << chat.id << "  " << chat.title
                       << (chat.hasActiveConversation ? "  [encrypted session active]" : "") << "\n";
        }
    }

    void onStatus(zkgram::core::ConversationId conversation, const std::string& stage,
                  const std::string& message) override {
        std::cout << "[" << conversation << "][" << stage << "] " << message << "\n";
    }

    void onTextReceived(zkgram::core::ConversationId conversation, const std::string& text) override {
        std::cout << "[" << conversation << "] peer: " << text << "\n";
    }

    void onFileReceived(zkgram::core::ConversationId conversation, const std::string& filePath) override {
        std::cout << "[" << conversation << "] peer sent a file: " << filePath << "\n";
    }

    void onReady(zkgram::core::ConversationId conversation) override {
        std::cout << "[" << conversation << "] ready\n";
    }

    void onPingTimeout(zkgram::core::ConversationId conversation) override {
        std::cout << "[" << conversation << "] ping timeout\n";
    }

    void onDisconnect(zkgram::core::ConversationId conversation) override {
        std::cout << "[" << conversation << "] disconnected\n";
    }

    bool confirmSignatures(zkgram::core::ConversationId conversation, const std::string& mySign,
                            const std::string& companionSign) override {
        std::cout << "[" << conversation << "] My signature: " << mySign << "\n";
        std::cout << "[" << conversation << "] Companion signature: " << companionSign << "\n";
        std::cout << "Confirm match? [y/N]: ";
        std::string answer;
        std::getline(std::cin, answer);
        return answer == "y" || answer == "Y";
    }

    std::string requestCredential(const std::string& prompt, const std::string& dataType) override {
        std::cout << prompt << " (" << dataType << "): ";
        std::string value;
        std::getline(std::cin, value);
        return value;
    }
};

}  // namespace

int main() {
    std::cout << "Local password: ";
    std::string password;
    std::getline(std::cin, password);

    auto ui = std::make_shared<ConsoleUiProvider>();
    std::unique_ptr<zkgram::core::Session> session;
    try {
        session = std::make_unique<zkgram::core::Session>(ui, dataDirNextToExe(), password);
    } catch (const std::exception& e) {
        std::cout << "Failed to start: " << e.what() << "\n";
        return 1;
    }

    try {
        session->start();
    } catch (const std::exception& e) {
        std::cout << "Connection failed: " << e.what() << "\n";
        return 1;
    }

    std::cout << "Logged in. Companion to start an encrypted conversation with (Telegram username or "
                 "+phone number): ";
    std::string companionId;
    std::getline(std::cin, companionId);

    std::promise<std::pair<zkgram::core::ConversationId, std::string>> resolved;
    auto resolvedFuture = resolved.get_future();
    session->startConversationWithCompanion(
        companionId, [&resolved](zkgram::core::ConversationId chatId, const std::string& error) {
            resolved.set_value({chatId, error});
        });
    auto [chatId, error] = resolvedFuture.get();
    if (!error.empty()) {
        std::cout << "Could not resolve companion: " << error << "\n";
        session->stop();
        return 1;
    }

    std::cout << "Starting encrypted session with chat " << chatId
              << ". Type a message and press Enter to send it, /file <path> to send a file, /quit to exit.\n";
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line == "/quit") {
            break;
        }
        try {
            if (line.rfind("/file ", 0) == 0) {
                session->sendFile(chatId, line.substr(6));
            } else if (!line.empty()) {
                session->sendText(chatId, line);
            }
        } catch (const std::exception& e) {
            std::cout << "Send failed: " << e.what() << "\n";
        }
    }

    session->stop();
    return 0;
}
