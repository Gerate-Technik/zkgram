#include "crypto/crypto_layer.hpp"

#include <pybind11/functional.h>
#include <pybind11/stl.h>

#include <filesystem>
#include <fstream>
#include <stdexcept>

#include <windows.h>

// Same reasoning as python_bridge.cpp: an absolute path baked in at
// configure time, not a path resolved against the process's working
// directory at runtime.
#ifndef ZKGRAM_ROOT
#define ZKGRAM_ROOT "."
#endif

namespace py = pybind11;

namespace zkgram::crypto {

namespace {

// Converts a raw Python traceback into a plain C++ exception so nothing
// outside this file (session.cpp, main.cpp, the UI plugins) needs to know
// about pybind11/py::error_already_set - they only ever see std::runtime_error.
// Without this, an uncaught py::error_already_set escaping a callback (e.g.
// a KeyError from CryptoLayer.send()) reaches main() unhandled and crashes
// the whole process via std::terminate instead of being reportable to the
// user through UiProvider.
[[noreturn]] void rethrowAsRuntimeError(const py::error_already_set& error) {
    throw std::runtime_error(std::string("CryptoLayer (Python): ") + error.what());
}

// Next to the exe, same pattern as session.cpp/telegram_client.cpp's own
// logDebug() (not shared with them - this is a different translation
// unit). Added because this whole file previously had zero diagnostic
// output: an encrypted-session start hanging or failing here (the Python
// handshake in cryptolayer/src/crypto_layer.py) left no trace anywhere,
// only session.cpp's own wireAndStartConversation() logging (also added
// alongside this) - see TODO.md, the encrypted-session-start investigation.
std::string logFilePath() {
    wchar_t buffer[MAX_PATH];
    DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (length == 0 || length == MAX_PATH) {
        return "zkgram_debug.log";
    }
    return (std::filesystem::path(buffer).parent_path() / "zkgram_debug.log").string();
}

void logDebug(const std::string& message) {
    std::ofstream file(logFilePath(), std::ios::app);
    if (file) {
        file << "[crypto_layer] " << message << "\n";
    }
}

// Оборачивает Bytes <-> py::bytes на границе с Python, чтобы Callbacks
// оставались чисто C++-типами (std::function<void(const Bytes&)>).
std::function<void(py::bytes)> wrapBytesCallback(std::function<void(const Bytes&)> fn) {
    return [fn = std::move(fn)](py::bytes data) {
        std::string s = data;  // py::bytes конвертируется в std::string по значению
        fn(Bytes(s.begin(), s.end()));
    };
}

// WordCoder.encode() (см. cryptolayer/src/wordcoder.py) индексируется по каждому
// байту шифротекста, поэтому пустой словарь роняет любую отправку KeyError-ом.
// python/wordcoder_dict.json - копия cryptolayer-cli/src/dicts/short_words_RU.json;
// путь абсолютный (ZKGRAM_ROOT), не зависит от рабочей папки процесса,
// см. python_bridge.cpp.
py::dict loadWordcoderDict() {
    py::object file = py::module_::import("builtins").attr("open")(std::string(ZKGRAM_ROOT) + "/python/wordcoder_dict.json", "r",
                                                                     py::arg("encoding") = "utf-8");
    py::dict dict = py::module_::import("json").attr("load")(file);
    file.attr("close")();
    return dict;
}

}  // namespace

CryptoLayer::CryptoLayer(std::string dataDir, std::string password, std::string companionId, Callbacks callbacks) {
    logDebug("CryptoLayer(): constructing for companionId=" + companionId);
    py::gil_scoped_acquire gil;

    try {
        py::module_ bridgeModule = bridge_.import("bridge");

        py::dict uiCallbacks;
        uiCallbacks["request_data"] = callbacks.requestData;
        uiCallbacks["update_status"] = callbacks.updateStatus;
        uiCallbacks["on_text_received"] = callbacks.onTextReceived;
        uiCallbacks["on_file_received"] = callbacks.onFileReceived;
        uiCallbacks["check_signatures"] = callbacks.checkSignatures;
        uiCallbacks["on_ready"] = callbacks.onReady;
        uiCallbacks["on_ping_timeout"] = callbacks.onPingTimeout;
        uiCallbacks["on_disconnect"] = callbacks.onDisconnect;

        py::object uiProvider = bridgeModule.attr("CppUiProvider")(uiCallbacks);

        py::dict moduleCallbacks;
        moduleCallbacks["send_bytes"] = wrapBytesCallback(callbacks.sendBytes);
        moduleCallbacks["send_file"] = callbacks.sendFile;

        // Python (bridge.CppModule.Listener) вызывает register_receiver со своими
        // self._on_bytes/self._on_file — конвертируем их в C++ std::function и
        // отдаём наружу через Callbacks::registerReceiver, которую связывает
        // src/core/session.cpp с telegram::TelegramClient::onBytesReceived/onFileReceived.
        moduleCallbacks["register_receiver"] =
            std::function<void(std::function<void(py::bytes)>, std::function<void(std::string)>)>(
                [registerReceiver = callbacks.registerReceiver](std::function<void(py::bytes)> pyOnBytes,
                                                                  std::function<void(std::string)> pyOnFile) {
                    registerReceiver(
                        [pyOnBytes](const Bytes& data) {
                            py::gil_scoped_acquire gil;  // вызов из потока TDLib
                            pyOnBytes(py::bytes(reinterpret_cast<const char*>(data.data()), data.size()));
                        },
                        [pyOnFile](const std::string& filePath) {
                            py::gil_scoped_acquire gil;  // вызов из потока TDLib
                            pyOnFile(filePath);
                        });
                });

        py::object module = bridgeModule.attr("CppModule")(moduleCallbacks);
        module.attr("init")(py::make_tuple("noop"), companionId);

        py::module_ cryptoLayerModule = bridge_.import("crypto_layer");
        py::object CryptoLayerClass = cryptoLayerModule.attr("CryptoLayer");

        cryptoLayerObj_ = CryptoLayerClass(uiProvider, dataDir, module, password, loadWordcoderDict());
        logDebug("CryptoLayer(): construction succeeded for companionId=" + companionId);
    } catch (const py::error_already_set& error) {
        logDebug("CryptoLayer(): construction threw for companionId=" + companionId + ": " + error.what());
        rethrowAsRuntimeError(error);
    }
}

CryptoLayer::~CryptoLayer() {
    py::gil_scoped_acquire gil;
    try {
        // py::object(), а не py::none(): нужно оставить поле ПУСТЫМ, а не
        // держащим None. Поля уничтожаются после тела деструктора, когда
        // gil выше уже отпущен, и py::object::~object() зовёт dec_ref() -
        // для None это обращение к счётчику ссылок без GIL, на котором
        // pybind11 срабатывает своей проверкой и убивает процесс
        // ("dec_ref() is being called while the GIL is either not held or
        // invalid"). У пустого объекта m_ptr == nullptr, и dec_ref для
        // него не делает ничего - см. pytypes.h, там стоит явная проверка
        // m_ptr != nullptr перед assert'ом.
        //
        // Раньше этого не было видно: интерпретатор навсегда оставлял GIL
        // захваченным (см. python_bridge.cpp), поэтому dec_ref всегда
        // случайно оказывался под GIL. Как только GIL стали честно
        // отдавать, закрытие переписки начало ронять процесс.
        cryptoLayerObj_ = py::object();
    } catch (const py::error_already_set&) {
        // Destructors must not throw; there is nothing more to clean up.
    }
}

void CryptoLayer::init() {
    logDebug("init(): calling Python CryptoLayer.init() (blocks on the handshake)");
    py::gil_scoped_acquire gil;
    try {
        cryptoLayerObj_.attr("init")();
        logDebug("init(): Python CryptoLayer.init() returned normally");
    } catch (const py::error_already_set& error) {
        logDebug(std::string("init(): Python CryptoLayer.init() raised: ") + error.what());
        rethrowAsRuntimeError(error);
    }
}

void CryptoLayer::sendText(const std::string& text) {
    py::gil_scoped_acquire gil;
    try {
        cryptoLayerObj_.attr("send")(text);
    } catch (const py::error_already_set& error) {
        rethrowAsRuntimeError(error);
    }
}

void CryptoLayer::sendFile(const std::string& filePath) {
    py::gil_scoped_acquire gil;
    try {
        cryptoLayerObj_.attr("send_file")(filePath);
    } catch (const py::error_already_set& error) {
        rethrowAsRuntimeError(error);
    }
}

void CryptoLayer::stop(bool sendDisconnect) {
    py::gil_scoped_acquire gil;
    try {
        cryptoLayerObj_.attr("stop")(sendDisconnect);
    } catch (const py::error_already_set& error) {
        rethrowAsRuntimeError(error);
    }
}

}  // namespace zkgram::crypto
