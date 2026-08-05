#!/usr/bin/env bash
# Linux/macOS аналог install-tdlib.ps1: собирает TDLib из исходников и
# ставит её в отдельный префикс, который потом передаётся в build.sh через
# --td-prefix. Готовых бинарников TDLib проект намеренно не тянет (см.
# README.md, раздел "Зависимости"): это криптографический компонент.
#
# Отличие от Windows-версии: здесь нет vcpkg. OpenSSL, zlib и gperf на
# Linux/macOS ставятся системным пакетным менеджером (скрипт только
# проверяет, что они есть, и печатает нужную команду, если нет), поэтому
# бутстрапить отдельный менеджер пакетов не за чем.
set -euo pipefail

SRC_DIR="${TDLIB_SRC_DIR:-$HOME/tdlib-src}"
BUILD_DIR="${TDLIB_BUILD_DIR:-$HOME/tdlib-build}"
INSTALL_DIR="${TDLIB_INSTALL_DIR:-$HOME/tdlib-install}"
# TDLib собирается только в Release: Debug-сборка с полной отладочной
# информацией легко занимает 7+ гигабайт (та же причина, что и в
# install-tdlib.ps1).
JOBS="${TDLIB_JOBS:-$( (command -v nproc >/dev/null && nproc) || sysctl -n hw.ncpu 2>/dev/null || echo 2 )}"

usage() {
    cat <<EOF
Usage: ./install-tdlib.sh [-h]

Пути и параллельность настраиваются переменными окружения:
  TDLIB_SRC_DIR      (сейчас: $SRC_DIR)
  TDLIB_BUILD_DIR    (сейчас: $BUILD_DIR)
  TDLIB_INSTALL_DIR  (сейчас: $INSTALL_DIR)
  TDLIB_JOBS         (сейчас: $JOBS)
EOF
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
    usage
    exit 0
fi

missing=()
for tool in git cmake gperf; do
    command -v "$tool" >/dev/null 2>&1 || missing+=("$tool")
done
# Заголовки, а не только рантайм-библиотеки: TDLib компилируется против
# них, поэтому нужны именно -dev пакеты.
if ! echo '#include <openssl/ssl.h>' | cc -fsyntax-only -x c - >/dev/null 2>&1; then
    missing+=("openssl-headers")
fi
if ! echo '#include <zlib.h>' | cc -fsyntax-only -x c - >/dev/null 2>&1; then
    missing+=("zlib-headers")
fi

if [[ ${#missing[@]} -gt 0 ]]; then
    echo "Не хватает зависимостей: ${missing[*]}" >&2
    echo "Debian/Ubuntu: sudo apt install build-essential cmake git gperf libssl-dev zlib1g-dev" >&2
    echo "Fedora:        sudo dnf install gcc-c++ cmake git gperf openssl-devel zlib-devel" >&2
    echo "macOS:         brew install cmake gperf openssl@3" >&2
    exit 1
fi

if [[ ! -d "$SRC_DIR/.git" ]]; then
    echo "Клонирую TDLib в $SRC_DIR ..."
    git clone https://github.com/tdlib/td.git "$SRC_DIR"
fi

echo "Конфигурирую TDLib (Release) в $BUILD_DIR ..."
cmake -S "$SRC_DIR" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$INSTALL_DIR" \
    -DTD_ENABLE_LTO=OFF

echo "Собираю и устанавливаю TDLib (-j$JOBS), это надолго ..."
cmake --build "$BUILD_DIR" --target install -j"$JOBS"

echo
echo "TDLib установлена в $INSTALL_DIR"
echo "Теперь можно собрать zkgram:"
echo "  ./build.sh --td-prefix \"$INSTALL_DIR\""
