cmake_minimum_required(VERSION 3.20)
project(zkgram CXX)

# Windows и Linux (macOS должен собираться тем же путём, что и Linux, но на
# нём не проверялся). Всё платформозависимое собрано в src/platform/paths.cpp
# и в WIN32-ветках ниже (resources/app.rc, WIN32-подсистема, флаги MSVC).
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# UI plugin selected at configure time (see docs/README.md, UI plugins section).
set(ZKGRAM_UI "console" CACHE STRING "UI plugin to build: console or qt")
set_property(CACHE ZKGRAM_UI PROPERTY STRINGS console qt)

# Directory containing the sibling cryptolayer/cryptolayer-module-interface
# repos (see src/crypto/python_bridge.cpp). Resolved to an ABSOLUTE path at
# configure time (CMAKE_SOURCE_DIR/.. - the layout where zkgram,
# cryptolayer, and cryptolayer-module-interface are checked out next to each
# other), not a relative "..": a relative path here is resolved against the
# process's working directory at *runtime*, which is only "the zkgram/ repo
# root" when launched from a terminal cd'd there - launching the built exe
# normally (double-click, shortcut, Start-Process without an explicit
# -WorkingDirectory) sets the working directory to the exe's own folder
# instead, so "../cryptolayer/src" silently pointed at the wrong place and
# `import crypto_layer` failed. This crashed the whole app on startup
# (CryptoLayer's constructor is called directly from main(), uncaught) -
# see TODO.md, "ввел номер потом пароль и потом приложение вылетело".
# Override only if your checkout is laid out differently.
get_filename_component(ZKGRAM_CRYPTOLAYER_ROOT_DEFAULT "${CMAKE_SOURCE_DIR}/.." ABSOLUTE)
set(ZKGRAM_CRYPTOLAYER_ROOT "${ZKGRAM_CRYPTOLAYER_ROOT_DEFAULT}" CACHE PATH "Directory containing cryptolayer/ and cryptolayer-module-interface/")

# Absolute path to this repo itself, for python/ (see python_bridge.cpp) and
# python/wordcoder_dict.json (see crypto_layer.cpp) - same working-directory
# problem as ZKGRAM_CRYPTOLAYER_ROOT above, fixed the same way.
set(ZKGRAM_ROOT "${CMAKE_SOURCE_DIR}")

find_package(Python3 COMPONENTS Interpreter Development REQUIRED)

include(FetchContent)
FetchContent_Declare(
    pybind11
    GIT_REPOSITORY https://github.com/pybind/pybind11.git
    GIT_TAG        v2.13.6
)
FetchContent_MakeAvailable(pybind11)

# TDLib: build from source or install via vcpkg ("vcpkg install tdlib"), then
# point CMAKE_PREFIX_PATH / Td_DIR at it. See docs/README.md, build section.
find_package(Td REQUIRED)
find_package(OpenSSL REQUIRED)

# На Windows потоки тянет сам компилятор, на Linux/macOS -pthread нужно
# указывать явно. Проект заводит свои потоки напрямую (фоновый receive()-луп
# в telegram_client.cpp, стартовый поток в main_qt.cpp), поэтому не полагаемся
# на то, протащит ли pthread транзитивно импортированная цель TDLib.
find_package(Threads REQUIRED)

# Some TDLib installs (some builds/versions of TdStaticTargets.cmake) do not
# export a Td::TdStatic aggregate target, only the individual component
# libraries (Td::tdclient, Td::tdcore, ...). fix-tdlib-static.ps1 detects
# that and generates this file with a fallback Td::TdStatic INTERFACE target
# in that case. A no-op if the file was never generated (normal case).
if(EXISTS "${CMAKE_SOURCE_DIR}/cmake/td_static_fallback.cmake")
    include("${CMAKE_SOURCE_DIR}/cmake/td_static_fallback.cmake")
endif()

set(ZKGRAM_SOURCES
    src/platform/paths.cpp
    src/crypto/python_bridge.cpp
    src/crypto/crypto_layer.cpp
    src/telegram/telegram_client.cpp
    src/core/session.cpp
)

if(ZKGRAM_UI STREQUAL "qt")
    find_package(Qt6 COMPONENTS Widgets Multimedia REQUIRED)
    set(CMAKE_AUTOMOC ON)
    set(CMAKE_AUTORCC ON)
    # This target also embeds Python.h (via crypto_layer.hpp -> pybind11), which
    # declares a struct field literally named "slots". Qt's "slots"/"signals"/
    # "emit" keyword macros collide with that, so keywords are disabled here;
    # our own code uses Q_SLOTS/Q_SIGNALS/Q_EMIT instead, see docs/README.md.
    add_compile_definitions(QT_NO_KEYWORDS)
    list(APPEND ZKGRAM_SOURCES
        src/ui/qt/main_qt.cpp
        src/ui/qt/qt_ui_provider.cpp
        src/ui/qt/main_window.cpp
        src/ui/qt/resources.qrc
    )
elseif(ZKGRAM_UI STREQUAL "console")
    list(APPEND ZKGRAM_SOURCES src/main.cpp)
else()
    message(FATAL_ERROR "Unknown ZKGRAM_UI value: ${ZKGRAM_UI} (expected console or qt)")
endif()

# Кастомная иконка приложения (Windows): положите resources/app.ico — см. resources/README.md
if(WIN32 AND EXISTS "${CMAKE_SOURCE_DIR}/resources/app.ico")
    list(APPEND ZKGRAM_SOURCES resources/app.rc)
endif()

# WIN32 (no console window) only for the Qt build - the console UI is meant
# to run in a terminal (reads stdin via std::getline) and needs one.
if(ZKGRAM_UI STREQUAL "qt" AND WIN32)
    add_executable(zkgram WIN32 ${ZKGRAM_SOURCES})
else()
    add_executable(zkgram ${ZKGRAM_SOURCES})
endif()

target_include_directories(zkgram PRIVATE src)
target_compile_definitions(zkgram PRIVATE ZKGRAM_CRYPTOLAYER_ROOT="${ZKGRAM_CRYPTOLAYER_ROOT}" ZKGRAM_ROOT="${ZKGRAM_ROOT}")
target_link_libraries(zkgram PRIVATE pybind11::embed Td::TdStatic OpenSSL::Crypto Threads::Threads)

# ELF-платформы: нативные модули CPython (например _cffi_backend, который
# тянет cryptography, а её тянет cryptolayer) грузятся через dlopen и
# ожидают найти символы Python уже загруженными в процессе. Когда Python
# подключён динамически (libpython3.x.so, обычный случай для системного
# Python и то, на чём проект проверялся), они находятся в самой .so и
# этого флага не требуется. Он нужен для сборок с Py_ENABLE_SHARED=0, где
# интерпретатор влинкован статически (libpython3.x.a): там символы Python
# оказываются внутри самого exe и по умолчанию не попадают в динамическую
# таблицу символов, из-за чего импорт нативного модуля падает уже в
# рантайме с "undefined symbol: PyExc_TypeError". То же самое делает и сам
# CPython, когда собирает свой бинарник. На Windows такой проблемы нет.
if(UNIX)
    target_link_options(zkgram PRIVATE -rdynamic)
endif()

# MSVC's default Release flags (/O2 /DNDEBUG) emit no .pdb at all, so a
# crash dump only ever shows raw "zkgram+0xNNNNNN" offsets, not function
# names - useless once the binary has been rebuilt even once since the
# crash (see TODO.md, "Найден и исправлен реальный краш"). /Zi (debug info
# in the .obj) + /DEBUG (linker emits the .pdb) keep full optimization but
# always produce a usable .pdb next to the .exe.
if(MSVC)
    target_compile_options(zkgram PRIVATE /Zi)
    target_link_options(zkgram PRIVATE /DEBUG)
else()
    # То же самое для GCC/Clang: -O2 в Release без -g даёт бэктрейс из
    # голых адресов. Отладочная информация не мешает оптимизации и не
    # попадает в память при запуске, при желании её всегда можно снять
    # через strip.
    target_compile_options(zkgram PRIVATE -g)
endif()

if(ZKGRAM_UI STREQUAL "qt")
    target_link_libraries(zkgram PRIVATE Qt6::Widgets Qt6::Multimedia)
endif()
