#!/usr/bin/env bash
# Linux/macOS аналог build.ps1: оборачивает конфигурацию и сборку через
# cmake в одну команду. Параметры повторяют build.ps1 (--ui, --config,
# --python, --td-prefix, --qt-prefix, --clean), чтобы инструкции в
# docs/README.md совпадали для обеих платформ.
set -euo pipefail

UI="console"
CONFIG="Release"
PYTHON_EXE=""
TD_PREFIX=""
QT_PREFIX=""
CLEAN=0

usage() {
    cat <<'EOF'
Usage: ./build.sh [options]

  --ui console|qt        UI-плагин (по умолчанию console)
  --config <тип>         CMAKE_BUILD_TYPE (по умолчанию Release)
  --python <путь>        конкретный python3 для embedded-интерпретатора
  --td-prefix <путь>     префикс установки TDLib (для find_package(Td))
  --qt-prefix <путь>     префикс установки Qt6 (нужен только при --ui qt)
  --clean                удалить build/ перед конфигурацией
  -h, --help             показать эту справку

Несколько путей в --td-prefix/--qt-prefix разделяются точкой с запятой,
как и в CMAKE_PREFIX_PATH.
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --ui) UI="${2:?--ui требует значение}"; shift 2 ;;
        --config) CONFIG="${2:?--config требует значение}"; shift 2 ;;
        --python) PYTHON_EXE="${2:?--python требует значение}"; shift 2 ;;
        --td-prefix) TD_PREFIX="${2:?--td-prefix требует значение}"; shift 2 ;;
        --qt-prefix) QT_PREFIX="${2:?--qt-prefix требует значение}"; shift 2 ;;
        --clean) CLEAN=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Неизвестный параметр: $1" >&2; usage >&2; exit 1 ;;
    esac
done

if [[ "$UI" != "console" && "$UI" != "qt" ]]; then
    echo "--ui должен быть console или qt, получено: $UI" >&2
    exit 1
fi

# Скрипт можно запускать из любой директории: пути ниже относительные к
# репозиторию, а не к рабочей папке (та же причина, по которой пути внутри
# самой программы считаются от каталога exe, см. src/platform/paths.hpp).
cd "$(dirname "$(readlink -f "$0")")"

if ! command -v cmake >/dev/null 2>&1; then
    echo "cmake не найден в PATH. Установите его (apt install cmake / brew install cmake)." >&2
    exit 1
fi

# Пересборка инкрементальна: cmake переконфигурирует только изменившееся,
# make/ninja пересобирает только затронутые .cpp. --clean нужен в редких
# случаях, когда инкрементальное состояние действительно протухло
# (переключение --ui console/qt на существующем build/, обновление самих
# TDLib/Qt - за ними cmake не следит так, как за исходниками репозитория).
if [[ $CLEAN -eq 1 && -d build ]]; then
    echo "--clean: удаляю build/ перед конфигурацией ..."
    rm -rf build
fi

CMAKE_ARGS=(-S . -B build -DPYBIND11_FINDPYTHON=ON "-DZKGRAM_UI=$UI" "-DCMAKE_BUILD_TYPE=$CONFIG")
if [[ -n "$PYTHON_EXE" ]]; then
    CMAKE_ARGS+=("-DPython3_EXECUTABLE=$PYTHON_EXE")
fi

PREFIXES=()
[[ -n "$TD_PREFIX" ]] && PREFIXES+=("$TD_PREFIX")
[[ -n "$QT_PREFIX" ]] && PREFIXES+=("$QT_PREFIX")
if [[ ${#PREFIXES[@]} -gt 0 ]]; then
    CMAKE_ARGS+=("-DCMAKE_PREFIX_PATH=$(IFS=';'; echo "${PREFIXES[*]}")")
fi

cmake "${CMAKE_ARGS[@]}"

# --config для одноконфигурационных генераторов (Makefiles/Ninja, обычные
# на Linux/macOS) не значит ничего, тип сборки там задан на этапе
# конфигурации через CMAKE_BUILD_TYPE выше. Передаём его всё равно: если
# пользователь выбрал многоконфигурационный генератор (Ninja Multi-Config,
# Xcode) через CMAKE_GENERATOR, без него собрался бы Debug.
cmake --build build --config "$CONFIG" --parallel

# Одноконфигурационные генераторы кладут бинарник прямо в build/,
# многоконфигурационные - в build/<Config>/.
if [[ -x "build/$CONFIG/zkgram" ]]; then
    echo "Сборка завершена. Запуск: ./build/$CONFIG/zkgram"
else
    echo "Сборка завершена. Запуск: ./build/zkgram"
fi
