#include "crypto/python_bridge.hpp"

#include <filesystem>
#include <optional>

#include "platform/paths.hpp"

// ZKGRAM_CRYPTOLAYER_ROOT/ZKGRAM_ROOT are absolute paths baked in at compile
// time (CMakeLists.txt, derived from CMAKE_SOURCE_DIR) - they only make
// sense on the machine that built the exe, where cryptolayer/ and
// cryptolayer-module-interface/ actually sit next to zkgram/ on disk. Copy
// the exe to another PC and these paths point at a directory that never
// existed there, so `import bridge`/`import crypto_layer` fails with
// "No module named bridge" (reported by a beta tester whose machine does
// not have C:/Users/User/cryptolayer/... at all).
//
// To make a distributed build work, build-all.ps1 additionally copies the
// three things Python needs (cryptolayer/src, cryptolayer-module-interface,
// python/bridge.py) into a "python-deps" folder next to zkgram.exe in
// dist/. At startup we check for that folder relative to the exe's own
// location (not the working directory - see the CMAKE_SOURCE_DIR comment
// this replaces, launching by double-click sets the working directory to
// the exe's folder anyway, but resolving explicitly is not more code) and
// prefer it over the compile-time dev paths when present.
#ifndef ZKGRAM_CRYPTOLAYER_ROOT
#define ZKGRAM_CRYPTOLAYER_ROOT ".."
#endif
#ifndef ZKGRAM_ROOT
#define ZKGRAM_ROOT "."
#endif

namespace zkgram::crypto {

namespace {

// Один embedded-интерпретатор на весь процесс - см. заголовок про то,
// почему он больше не поле PythonBridge.
class EmbeddedInterpreter {
public:
    EmbeddedInterpreter() {
        configureSysPath();
        configureLogging();

        // ГЛАВНОЕ: отдать GIL. Py_Initialize (внутри scoped_interpreter)
        // оставляет GIL захваченным потоком, который его вызвал, и сам
        // никогда не отпускает. Пока этого release не было, поток,
        // создавший CryptoLayer (у Qt-плагина это GUI-поток), держал GIL
        // до конца жизни процесса, а Session::wireAndStartConversation
        // запускает CryptoLayer::init() на ОТДЕЛЬНОМ потоке - и тот
        // намертво вставал на своём py::gil_scoped_acquire, ещё не начав
        // рукопожатие. Снаружи это выглядело как "Starting encrypted
        // session..." навсегда и повисший интерфейс, потому что GUI-поток
        // тоже оказывался заблокирован на следующем же обращении к Python.
        //
        // std::optional, а не просто поле: gil_scoped_release должен
        // сработать ПОСЛЕ настройки sys.path выше (она сама требует GIL),
        // а поле конструировалось бы до тела конструктора.
        gilRelease_.emplace();
    }

private:
    // Раскладка ожидается такой: cryptolayer/ и
    // cryptolayer-module-interface/ рядом с zkgram/ (ZKGRAM_CRYPTOLAYER_ROOT),
    // либо всё вместе в python-deps/ рядом с exe у переносимой сборки.
    static void configureSysPath() {
        py::module_ sys = py::module_::import("sys");
        py::list path = sys.attr("path");

        std::string cryptolayerRoot = ZKGRAM_CRYPTOLAYER_ROOT;
        std::string zkgramRoot = ZKGRAM_ROOT;

        std::string portableDeps = zkgram::platform::executableDir() + "/python-deps";
        if (std::filesystem::exists(portableDeps + "/cryptolayer/src")) {
            cryptolayerRoot = portableDeps;
            zkgramRoot = portableDeps;
        }

        path.append(cryptolayerRoot + "/cryptolayer/src");
        path.append(cryptolayerRoot + "/cryptolayer-module-interface");
        path.append(zkgramRoot + "/python");
    }

    // cryptolayer/src/crypto_layer.py and levels/base.py both do
    // logging.getLogger(...).error/info(...) throughout the entire
    // handshake and send/receive protocol (signature mismatches, AES-GCM
    // decrypt failures, brotli decompress failures, WordCoder decode
    // errors - exactly the kind of failure that would explain a
    // handshake silently getting stuck) but neither this file nor
    // cryptolayer itself ever called logging.basicConfig()/addHandler()
    // anywhere. Without a configured handler, Python's logging module
    // falls back to its "handler of last resort", which only prints
    // WARNING-and-above to sys.stderr - and this Qt build links as a
    // WIN32 (no console) subsystem app, so stderr goes nowhere anyone
    // can see. Every one of those error() calls was real diagnostic
    // information that no one, including this codebase's own debugging
    // sessions, could ever actually see. Routed into the same
    // zkgram_debug.log the C++ side's own logDebug() helpers
    // (session.cpp/telegram_client.cpp/crypto_layer.cpp) already write
    // to, with a distinguishing "[python]" prefix, rather than a second
    // separate log file.
    static void configureLogging() {
        std::string logPath = zkgram::platform::executableDir();
        logPath = (logPath.empty() ? std::string(".") : logPath) + "/zkgram_debug.log";

        py::module_ logging = py::module_::import("logging");
        py::object handler = logging.attr("FileHandler")(logPath, "a", "utf-8");
        handler.attr("setFormatter")(
            logging.attr("Formatter")("%(asctime)s [python] %(name)s %(levelname)s: %(message)s"));
        py::object root = logging.attr("getLogger")();
        root.attr("addHandler")(handler);
        root.attr("setLevel")(logging.attr("INFO"));
    }

    py::scoped_interpreter interpreter_;
    // Порядок уничтожения обратен порядку объявления: gilRelease_ первым
    // забирает GIL обратно, и только потом interpreter_ финализирует
    // Python. Наоборот финализация шла бы без GIL.
    std::optional<py::gil_scoped_release> gilRelease_;
};

}  // namespace

PythonBridge::PythonBridge() {
    // Инициализация function-local static потокобезопасна (C++11), так что
    // две переписки, стартующие одновременно, не поднимут два интерпретатора.
    static EmbeddedInterpreter interpreter;
    (void)interpreter;
}

PythonBridge::~PythonBridge() = default;

py::module_ PythonBridge::import(const std::string& moduleName) {
    return py::module_::import(moduleName.c_str());
}

}  // namespace zkgram::crypto
