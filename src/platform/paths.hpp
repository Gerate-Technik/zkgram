#pragma once

#include <string>

// Платформенные примитивы, не относящиеся ни к одному из трёх слоёв из
// docs/README.md §1 (crypto/telegram/core). Сюда можно класть только то,
// что не знает ни про ключи, ни про сеть, ни про UI, иначе это нарушение
// изоляции слоёв: инвариант "crypto не включает telegram и наоборот"
// сохраняется, потому что включать этот заголовок можно откуда угодно, а
// он сам не включает ничего из проекта.
namespace zkgram::platform {

// Абсолютный путь к каталогу, в котором лежит запущенный исполняемый файл.
//
// Именно каталог exe, а не рабочая директория процесса: рабочая директория
// зависит от способа запуска (двойной клик, ярлык, Start-Process, systemd,
// .desktop-файл), поэтому относительные "data"/"zkgram_debug.log" каждый
// раз указывали в разное место - см. комментарии в main_qt.cpp и
// session.cpp про "пароль не сохраняется" и про пропавший лог у бета
// тестера.
//
// Реализация зависит от ОС (GetModuleFileNameW / /proc/self/exe /
// _NSGetExecutablePath), поэтому она собрана в одном месте, а не
// продублирована в пяти файлах, как было раньше, когда проект собирался
// только под Windows.
//
// Возвращает пустую строку, если путь получить не удалось; вызывающая
// сторона сама решает, чем это заменить (обычно относительным путём).
std::string executableDir();

// Per-user application data directory for zkgram's own local cache (see
// core::LocalCache) - "%APPDATA%\zkgram" on Windows (same
// %APPDATA%\<app name>\ scheme real Telegram Desktop uses for its own
// "tdata" folder, "%APPDATA%\Telegram Desktop\tdata" - see
// telegram_fork/tdesktop's storage/storage_account.cpp, BaseGlobalPath()
// - but zkgram's own separate folder, not literally inside Telegram
// Desktop's data directory), "~/.local/share/zkgram" on Linux via
// XDG_DATA_HOME, "~/Library/Application Support/zkgram" on macOS.
// Distinct from executableDir(): this directory must survive a
// reinstall/portable-copy-replacement (the whole point of a persistent
// cache), whereas executableDir() is tied to wherever this particular
// copy of the exe happens to sit.
//
// Returns an empty string if the platform's own directory could not be
// resolved; the caller decides the fallback (usually skip caching rather
// than write somewhere unpredictable).
std::string appDataDir();

}  // namespace zkgram::platform
