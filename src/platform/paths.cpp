#include "platform/paths.hpp"

#include <cstdlib>
#include <filesystem>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <cstdint>

#include <mach-o/dyld.h>
#endif

namespace zkgram::platform {

std::string executableDir() {
#if defined(_WIN32)
    // Растущий буфер, а не фиксированный MAX_PATH: путь длиннее MAX_PATH
    // (long paths включены в системе, или exe лежит очень глубоко)
    // GetModuleFileNameW обрезает и возвращает ровно размер буфера с
    // ERROR_INSUFFICIENT_BUFFER, то есть прежний код с `length == MAX_PATH`
    // молча уходил в фолбэк вместо реального пути.
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
        DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return {};
        }
        if (length < buffer.size()) {
            return std::filesystem::path(std::wstring(buffer.data(), length)).parent_path().string();
        }
        if (buffer.size() >= 32768) {  // Потолок длины пути в Windows.
            return {};
        }
        buffer.resize(buffer.size() * 2);
    }
#elif defined(__APPLE__)
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);  // Только чтобы узнать нужный размер.
    std::vector<char> buffer(size ? size : 1);
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
        return {};
    }
    std::error_code ec;
    // _NSGetExecutablePath может вернуть путь с симлинками и "..", отсюда
    // weakly_canonical - без него parent_path() указывал бы на каталог
    // симлинка, а не на каталог настоящего exe.
    std::filesystem::path exePath = std::filesystem::weakly_canonical(std::filesystem::path(buffer.data()), ec);
    if (ec) {
        exePath = std::filesystem::path(buffer.data());
    }
    return exePath.parent_path().string();
#else
    // Linux и другие системы с /proc: /proc/self/exe это симлинк на сам
    // бинарник. read_symlink с std::error_code, а не бросающая версия:
    // помимо экзотических систем без procfs, executableDir() вызывается в
    // том числе из обработчика std::terminate в main_qt.cpp, где бросать
    // новое исключение нельзя.
    std::error_code ec;
    std::filesystem::path exePath = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (ec || exePath.empty()) {
        return {};
    }
    return exePath.parent_path().string();
#endif
}

std::string appDataDir() {
#if defined(_WIN32)
    // Same growing-buffer pattern as executableDir() above -
    // GetEnvironmentVariableW returns the required size (including the
    // null terminator) when the supplied buffer is too small, rather
    // than silently truncating.
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
        DWORD length = GetEnvironmentVariableW(L"APPDATA", buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return {};  // Variable not set - no sensible fallback on Windows.
        }
        if (length < buffer.size()) {
            return (std::filesystem::path(std::wstring(buffer.data(), length)) / "zkgram").string();
        }
        buffer.resize(length);
    }
#elif defined(__APPLE__)
    const char* home = std::getenv("HOME");
    if (home == nullptr || *home == '\0') {
        return {};
    }
    return (std::filesystem::path(home) / "Library" / "Application Support" / "zkgram").string();
#else
    // XDG Base Directory spec - most Linux desktop environments set
    // XDG_DATA_HOME already; ~/.local/share is the spec's own documented
    // default when it is not set, not a guess.
    const char* xdgDataHome = std::getenv("XDG_DATA_HOME");
    if (xdgDataHome != nullptr && *xdgDataHome != '\0') {
        return (std::filesystem::path(xdgDataHome) / "zkgram").string();
    }
    const char* home = std::getenv("HOME");
    if (home == nullptr || *home == '\0') {
        return {};
    }
    return (std::filesystem::path(home) / ".local" / "share" / "zkgram").string();
#endif
}

}  // namespace zkgram::platform
