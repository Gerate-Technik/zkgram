#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <pybind11/pybind11.h>

#include "crypto/python_bridge.hpp"

// Обёртка над настоящим Python cryptolayer.CryptoLayer (см.
// ../cryptolayer/src/crypto_layer.py). Держит внутри embedded-интерпретатор
// и py::object с реальным движком (ключи, AES-GCM, ECDSA/ECDH, WordCoder).
//
// ВАЖНО (инвариант изоляции, см. docs/README.md §1): этот файл не
// #include-ит ничего из src/telegram и не знает про TDLib — транспорт
// подключается только через Callbacks, которые связывает src/core/session.*.
namespace zkgram::crypto {

using Bytes = std::vector<std::uint8_t>;

class CryptoLayer {
public:
    struct Callbacks {
        // Отправка уже зашифрованных данных наружу (реализует telegram::TelegramClient).
        std::function<void(const Bytes&)> sendBytes;
        std::function<void(const std::string& filePath)> sendFile;

        // Регистрация обработчиков входящих данных: вызывающая сторона (src/core)
        // должна связать их с telegram::TelegramClient::onBytesReceived/onFileReceived.
        std::function<void(std::function<void(const Bytes&)> onBytes,
                            std::function<void(const std::string&)> onFile)>
            registerReceiver;

        // UI-колбэки (см. zkgram::core::UiProvider) — связывает src/core.
        std::function<void(const std::string& stage, const std::string& message, const std::string& statusType)>
            updateStatus;
        std::function<void(long timestamp, const std::string& text)> onTextReceived;
        std::function<void(long timestamp, const std::string& filePath, const std::string& originalName)>
            onFileReceived;
        std::function<bool(const std::string& mySign, const std::string& companionSign)> checkSignatures;
        std::function<void()> onReady;
        std::function<void()> onPingTimeout;
        std::function<void()> onDisconnect;

        // Requests data from the user (password, etc), see cryptolayer.UIProvider.request_data.
        // dataType is a Python type name (for example "str").
        std::function<std::string(const std::string& prompt, const std::string& dataType)> requestData;
    };

    CryptoLayer(std::string dataDir, std::string password, std::string companionId, Callbacks callbacks);
    ~CryptoLayer();

    CryptoLayer(const CryptoLayer&) = delete;
    CryptoLayer& operator=(const CryptoLayer&) = delete;

    // Запускает полный цикл инициализации CryptoLayer (обмен ID/подписями/ключами).
    void init();

    void sendText(const std::string& text);
    void sendFile(const std::string& filePath);
    void stop(bool sendDisconnect = true);

private:
    PythonBridge bridge_;
    pybind11::object cryptoLayerObj_;  // экземпляр Python cryptolayer.CryptoLayer
};

}  // namespace zkgram::crypto
