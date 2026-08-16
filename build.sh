#!/usr/bin/env bash
# Linux/macOS аналог build.ps1: оборачивает конфигурацию и сборку через
# cmake в одну команду. Параметры повторяют build.ps1 (--ui, --config,
# --python, --td-prefix, --qt-prefix, --clean), чтобы инструкции в
# docs/README.md совпадали для обеих платформ, плюс два шага, которых на
# Windows нет: --deps (системные пакеты через пакетный менеджер) и
# автоматическая догрузка подмодулей third_party/desktop-app, без которых
# --ui qt на Linux не конфигурируется в принципе.
set -euo pipefail

UI="console"
CONFIG="Release"
PYTHON_EXE=""
TD_PREFIX=""
QT_PREFIX=""
CLEAN=0
DEPS=0
SKIP_FETCH=0

# Минимальный Qt для --ui qt. Не выдумано: вендоренный lib_ui использует
# QAccessibleSelectionInterface (появился в Qt 6.5) и
# QAccessibleAttributesInterface (появился в Qt 6.8) в
# ui/accessible/ui_accessible_widget.*, поэтому на более старом Qt сборка
# падает не на одном заголовке, а на отсутствующем классе Qt. Ubuntu 24.04
# LTS штатно даёт 6.4.2, то есть системного Qt там не хватает - см.
# сообщение в require_qt_version() ниже о том, как поставить свежий.
QT_MIN_VERSION="6.8"

usage() {
    cat <<'EOF'
Usage: ./build.sh [options]

  --ui console|qt        UI-плагин (по умолчанию console)
  --config <тип>         CMAKE_BUILD_TYPE (по умолчанию Release)
  --python <путь>        конкретный python3 для embedded-интерпретатора
  --td-prefix <путь>     префикс установки TDLib (для find_package(Td))
  --qt-prefix <путь>     префикс установки Qt6 (нужен только при --ui qt)
  --deps                 доустановить системные библиотеки и выйти
  --skip-fetch           не догружать исходники в third_party/desktop-app
  --clean                удалить build/ перед конфигурацией
  -h, --help             показать эту справку

Несколько путей в --td-prefix/--qt-prefix разделяются точкой с запятой,
как и в CMAKE_PREFIX_PATH.

Быстрый старт для GUI-сборки на чистой машине:

  sudo ./build.sh --deps
  ./installtdlib.sh
  ./build.sh --ui qt --td-prefix "$HOME/tdlib-install" --qt-prefix /opt/Qt/6.8.2/gcc_64

TDLib отдельным скриптом (installtdlib.sh), а не отсюда: это большая
сборка на десятки минут, её не нужно повторять при каждой пересборке
zkgram.
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --ui) UI="${2:?--ui требует значение}"; shift 2 ;;
        --config) CONFIG="${2:?--config требует значение}"; shift 2 ;;
        --python) PYTHON_EXE="${2:?--python требует значение}"; shift 2 ;;
        --td-prefix) TD_PREFIX="${2:?--td-prefix требует значение}"; shift 2 ;;
        --qt-prefix) QT_PREFIX="${2:?--qt-prefix требует значение}"; shift 2 ;;
        --deps) DEPS=1; shift ;;
        --skip-fetch) SKIP_FETCH=1; shift ;;
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

# ---------------------------------------------------------------- зависимости

# Пакеты для console-сборки: сам компилятор, cmake, и то, против чего
# линкуется ядро (OpenSSL/zlib нужны и TDLib, и локальному кэшу; python3-dev
# - embedded-интерпретатор через pybind11).
DEPS_COMMON_APT=(build-essential cmake ninja-build git pkg-config
                 python3-dev libssl-dev zlib1g-dev gperf)

# Дополнительно для --ui qt. Это не список зависимостей zkgram как такового,
# а список зависимостей вендоренного lib_ui/lib_base (third_party/desktop-app):
#   * libjpeg/lz4/xxhash  - прямые PRIVATE-зависимости lib_ui;
#   * glib + gobject-introspection + libgirepository - lib_base на Linux
#     генерирует D-Bus/GObject-биндинги (gdbus-codegen, g-ir-scanner, cppgir);
#   * boost + fmt        - этим собирается сам cppgir из исходников.
# Qt в этот список намеренно не входит: в репозиториях LTS-дистрибутивов он
# обычно старее, чем требует lib_ui, см. QT_MIN_VERSION выше.
DEPS_QT_APT=(libjpeg-dev liblz4-dev libxxhash-dev libglib2.0-dev
             gobject-introspection libgirepository1.0-dev
             libboost-dev libfmt-dev)

install_deps() {
    if command -v apt-get >/dev/null 2>&1; then
        local packages=("${DEPS_COMMON_APT[@]}")
        [[ "$UI" == "qt" ]] && packages+=("${DEPS_QT_APT[@]}")
        if [[ $EUID -ne 0 ]]; then
            echo "--deps ставит системные пакеты, запустите через sudo:" >&2
            echo "  sudo ./build.sh --deps${UI:+ --ui $UI}" >&2
            exit 1
        fi
        echo "Устанавливаю: ${packages[*]}"
        apt-get update
        apt-get install -y "${packages[@]}"
    elif command -v dnf >/dev/null 2>&1; then
        echo "Fedora: sudo dnf install gcc-c++ cmake ninja-build git gperf \\" >&2
        echo "    python3-devel openssl-devel zlib-devel \\" >&2
        echo "    libjpeg-turbo-devel lz4-devel xxhash-devel glib2-devel \\" >&2
        echo "    gobject-introspection-devel boost-devel fmt-devel" >&2
        exit 1
    elif command -v brew >/dev/null 2>&1; then
        echo "macOS: brew install cmake ninja gperf openssl@3 jpeg lz4 xxhash glib \\" >&2
        echo "    gobject-introspection boost fmt qt" >&2
        exit 1
    else
        echo "Неизвестный пакетный менеджер, поставьте зависимости вручную." >&2
        exit 1
    fi
}

if [[ $DEPS -eq 1 ]]; then
    install_deps
    echo
    echo "Готово. Дальше: ./installtdlib.sh, затем ./build.sh --ui $UI ..."
    exit 0
fi

# ------------------------------------------------- исходники third_party

# Подмодули, которые вендоренный third_party/desktop-app ожидает на месте,
# но которых в репозитории нет (вендоринг собирался под Windows, где ни один
# из них не участвует в сборке). Без них --ui qt на Linux не конфигурируется:
# cppgir нужен внешнему glib, kcoreaddons и xdg-desktop-portal - lib_base.
# Клонируются, а не копируются в репозиторий: это чужой GPL/LGPL-код со
# своей историей, ему место в апстриме, а не в нашем дереве.
fetch_third_party() {
    local dst url
    while read -r dst url; do
        [[ -z "$dst" ]] && continue
        if [[ -e "$dst/.git" || -n "$(ls -A "$dst" 2>/dev/null)" ]]; then
            continue
        fi
        echo "Догружаю $dst ..."
        rm -rf "$dst"
        git clone --depth 1 "$url" "$dst"
    done <<'EOF'
third_party/desktop-app/cmake/external/glib/cppgir https://gitlab.com/mnauw/cppgir.git
third_party/desktop-app/ThirdParty/kcoreaddons https://github.com/KDE/kcoreaddons.git
third_party/desktop-app/ThirdParty/xdg-desktop-portal https://github.com/flatpak/xdg-desktop-portal.git
EOF
    # Вложенный подмодуль самого cppgir - клонируется после него, поэтому
    # отдельным шагом, а не в списке выше.
    local el="third_party/desktop-app/cmake/external/glib/cppgir/expected-lite"
    if [[ -d "${el%/*}" && -z "$(ls -A "$el" 2>/dev/null)" ]]; then
        echo "Догружаю $el ..."
        rm -rf "$el"
        git clone --depth 1 https://github.com/martinmoene/expected-lite.git "$el"
    fi
}

# ------------------------------------------------------------- проверки Qt

# Ровно та же проверка, что потом сделает find_package(Qt6), но с внятным
# сообщением вместо простыни cmake-ошибок посреди сборки lib_ui.
require_qt_version() {
    local qmake="" found=""
    for candidate in "${QT_PREFIX%%;*}/bin/qmake6" "${QT_PREFIX%%;*}/bin/qmake" qmake6 qmake; do
        [[ -z "$candidate" ]] && continue
        if command -v "$candidate" >/dev/null 2>&1; then qmake="$candidate"; break; fi
    done
    [[ -z "$qmake" ]] && return 0  # пусть решает cmake, ему виднее
    found="$("$qmake" -query QT_VERSION 2>/dev/null || true)"
    [[ -z "$found" ]] && return 0
    if [[ "$(printf '%s\n%s\n' "$QT_MIN_VERSION" "$found" | sort -V | head -1)" != "$QT_MIN_VERSION" ]]; then
        cat >&2 <<EOF
Найден Qt $found, а вендоренному lib_ui нужен минимум $QT_MIN_VERSION
(ui/accessible/ui_accessible_widget использует QAccessibleAttributesInterface,
появившийся только в Qt 6.8).

В репозиториях LTS-дистрибутивов Qt обычно старее. Поставьте свежий, не
трогая системный, и укажите его через --qt-prefix:

  pip install aqtinstall
  python3 -m aqt install-qt linux desktop 6.8.2 linux_gcc_64 -m qtmultimedia qtsvg -O /opt/Qt
  ./build.sh --ui qt --qt-prefix /opt/Qt/6.8.2/gcc_64 ...
EOF
        exit 1
    fi
}

# Пересборка инкрементальна: cmake переконфигурирует только изменившееся,
# make/ninja пересобирает только затронутые .cpp. --clean нужен в редких
# случаях, когда инкрементальное состояние действительно протухло
# (переключение --ui console/qt на существующем build/, обновление самих
# TDLib/Qt - за ними cmake не следит так, как за исходниками репозитория).
if [[ $CLEAN -eq 1 && -d build ]]; then
    echo "--clean: удаляю build/ перед конфигурацией ..."
    rm -rf build
fi

if ! command -v cmake >/dev/null 2>&1; then
    echo "cmake не найден в PATH. Запустите: sudo ./build.sh --deps" >&2
    exit 1
fi

if [[ "$UI" == "qt" ]]; then
    [[ $SKIP_FETCH -eq 0 ]] && fetch_third_party
    require_qt_version
fi

CMAKE_ARGS=(-S . -B build -DPYBIND11_FINDPYTHON=ON "-DZKGRAM_UI=$UI" "-DCMAKE_BUILD_TYPE=$CONFIG")

# ZKGRAM_VENDOR_LIB_UI по умолчанию OFF (см. CMakeLists.txt), но src/ui/qt/
# безусловно включает заголовки lib_ui (ui/widgets/buttons.h,
# base/basic_types.h, styles/style_*.h), то есть с OFF qt-сборка не
# компилируется вообще ни на одной платформе. Раньше build.sh этот флаг не
# пробрасывал, поэтому ./build.sh --ui qt не мог собраться by design.
if [[ "$UI" == "qt" ]]; then
    CMAKE_ARGS+=(-DZKGRAM_VENDOR_LIB_UI=ON)
fi

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
