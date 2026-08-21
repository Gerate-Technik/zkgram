cmake_minimum_required(VERSION 3.20)
# C alongside CXX: nothing in src/ is C, but the vendored lib_base generates
# D-Bus glue as .c files on Linux (gdbus-codegen, see
# third_party/desktop-app/cmake/external/glib/generate_dbus.cmake), and CMake
# refuses to generate a build for them unless C is an enabled language.
project(zkgram C CXX)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -w")
# Windows и Linux (macOS должен собираться тем же путём, что и Linux, но на
# нём не проверялся). Всё платформозависимое собрано в src/platform/paths.cpp
# и в WIN32-ветках ниже (resources/app.rc, WIN32-подсистема, флаги MSVC).
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Pin the MSVC runtime library before any vendored subdirectory can set its
# own default. TDLib and the OpenSSL-Win64 install zkgram links against were
# both built with the dynamic CRT (/MD); the vendored desktop-app cmake
# helpers (third_party/desktop-app/cmake/variables.cmake, used when
# ZKGRAM_VENDOR_LIB_UI is on) default CMAKE_MSVC_RUNTIME_LIBRARY to the
# static CRT (/MT) instead, which is correct for the real tdesktop's own
# fully-static toolchain but not for zkgram's existing dependencies. Setting
# this CACHE variable here first means the vendored tree's own
# set(... CACHE STRING "") (no FORCE) leaves it alone, so lib_ui builds
# against the same CRT as the rest of zkgram instead of imposing its own.
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL" CACHE STRING "")
endif()

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

# TDLib's own exported CMake target hardcodes an absolute dependency on
# vcpkg's x64-windows-static-md-release OpenSSL build (see
# TdStaticTargets.cmake's INTERFACE_LINK_LIBRARIES). zkgram's own code
# (src/core/local_cache.cpp) also calls OpenSSL directly, so it needs to
# resolve to that exact same build, not a separately installed OpenSSL (e.g.
# a system-wide OpenSSL-Win64) - two different physical libcrypto.lib copies
# linked into the same zkgram.exe produce duplicate-symbol (LNK2005) errors
# at link time, unrelated to and independent of the lib_ui vendoring. See
# zkgram/docs/README.md for how this was diagnosed.
if(NOT DEFINED OPENSSL_ROOT_DIR AND EXISTS "C:/vcpkg/installed/x64-windows-static-md-release")
    set(OPENSSL_ROOT_DIR "C:/vcpkg/installed/x64-windows-static-md-release")
    set(OPENSSL_USE_STATIC_LIBS ON)
endif()
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
    src/core/data_registry.cpp
    src/core/local_cache.cpp
    src/core/session.cpp
)

if(ZKGRAM_UI STREQUAL "qt")
    # Off by default: the vendored lib_ui build (see third_party/desktop-app/)
    # is Phase 1 infrastructure only - src/ui/qt/ does not use any of its
    # widgets yet (that is Phase 4-6, not done, see zkgram/.claude/UI.md
    # section 13c and prompt.md). Turning this on also requires codegen_style
    # to be able to actually run as a build-time subprocess, which is not
    # guaranteed on every machine (see docs/README.md for a known,
    # unresolved case where it fails with STATUS_DLL_NOT_FOUND for reasons
    # that needed administrator rights to diagnose further). Until Phase 4-6
    # lands, leave this OFF so normal qt builds are not blocked by it.
    option(ZKGRAM_VENDOR_LIB_UI "Build and link the vendored Telegram Desktop lib_ui (Phase 1 migration infrastructure, not yet used by src/ui/qt/)" OFF)

    if(ZKGRAM_VENDOR_LIB_UI)
        find_package(Qt6 COMPONENTS Widgets Multimedia Network Svg OpenGL OpenGLWidgets DBus REQUIRED)
    else()
        find_package(Qt6 COMPONENTS Widgets Multimedia REQUIRED)
    endif()
    set(CMAKE_AUTOMOC ON)
    set(CMAKE_AUTORCC ON)
    # This target also embeds Python.h (via crypto_layer.hpp -> pybind11), which
    # declares a struct field literally named "slots". Qt's "slots"/"signals"/
    # "emit" keyword macros collide with that, so keywords are disabled here;
    # our own code uses Q_SLOTS/Q_SIGNALS/Q_EMIT instead, see docs/README.md.
    add_compile_definitions(QT_NO_KEYWORDS)

    if(ZKGRAM_VENDOR_LIB_UI)
        # Vendored subset of Telegram Desktop's own UI toolkit (lib_ui/lib_base/
        # lib_rpl/lib_crl/codegen, from github.com/desktop-app). This link only
        # builds codegen_style and links lib_ui into zkgram; src/ui/qt/ itself
        # still uses the pre-migration hand-painted widgets for now (Phase 1 of
        # the migration - see zkgram/.claude/UI.md, section 13c, and prompt.md
        # one level above this repo for the full phase plan). GPLv3: this makes
        # the resulting zkgram.exe (qt build) subject to GPLv3 as a whole, see
        # third_party/desktop-app/CMakeLists.txt's own header comment.
        #
        # lib_base's own crash reporter needs Google Breakpad
        # (client/windows/handler/exception_handler.h), which is not
        # vendored here - zkgram has its own crash-diagnostics path already
        # (see main_qt.cpp's std::set_terminate handler), so this is not
        # needed rather than a gap to fill in later.
        set(DESKTOP_APP_DISABLE_CRASH_REPORTS ON CACHE BOOL "" FORCE)

        # Fails here, with one readable line, instead of a few hundred
        # template errors deep inside lib_ui: ui/accessible/ui_accessible_widget
        # implements QAccessibleSelectionInterface (Qt 6.5) and
        # QAccessibleAttributesInterface (Qt 6.8), so anything older cannot
        # build lib_ui at all. Notably older than what LTS distributions ship -
        # Ubuntu 24.04 is on 6.4.2 - hence the pointer to a side-by-side Qt.
        if(Qt6_VERSION VERSION_LESS 6.8)
            message(FATAL_ERROR
                "ZKGRAM_UI=qt needs Qt 6.8 or newer (found ${Qt6_VERSION}): the vendored "
                "lib_ui implements QAccessibleAttributesInterface, added in Qt 6.8. "
                "Install a newer Qt alongside the system one and point at it, e.g.\n"
                "  pip install aqtinstall\n"
                "  python3 -m aqt install-qt linux desktop 6.8.2 linux_gcc_64 -m qtmultimedia qtsvg -O /opt/Qt\n"
                "  ./build.sh --ui qt --qt-prefix /opt/Qt/6.8.2/gcc_64 ...")
        endif()

        add_subdirectory(third_party/desktop-app)
    endif()

    list(APPEND ZKGRAM_SOURCES
        src/ui/qt/main_qt.cpp
        src/ui/qt/qt_ui_provider.cpp
        src/ui/qt/main_window.cpp
        src/ui/qt/icons.cpp
        src/ui/qt/chat_background.cpp
        src/ui/qt/resources.qrc
    )
    if(ZKGRAM_VENDOR_LIB_UI)
        # crl::on_main_update_requests() is missing from upstream lib_crl -
        # see src/ui/qt/crl_on_main_qt.cpp's own header comment.
        list(APPEND ZKGRAM_SOURCES src/ui/qt/crl_on_main_qt.cpp)
        # Required Ui::Integration implementation - see
        # src/ui/qt/ui_integration.hpp's own comment for why.
        list(APPEND ZKGRAM_SOURCES src/ui/qt/ui_integration.cpp)

        # Real Telegram Desktop chat design code, copied from
        # telegram_fork/tdesktop/Telegram/SourceFiles (not referenced as a
        # submodule/library - copied and adapted directly into zkgram's own
        # src/ tree per project convention, see src/ui/chat/chat_style.cpp's
        # own header comment for what was trimmed and why). None of this
        # depends on HistoryItem/Data::Session/Main::Session - it is
        # self-contained bubble geometry/painting/theme machinery.
        list(APPEND ZKGRAM_SOURCES
            src/ui/chat/message_bubble.cpp
            src/ui/chat/chat_style.cpp
            src/ui/chat/chat_style_radius.cpp
            src/ui/chat/chat_theme.cpp
            src/ui/chat/message_bar.cpp
            src/ui/cached_round_corners.cpp
            src/ui/color_contrast.cpp
            src/ui/color_int_conversion.cpp
            src/ui/power_saving.cpp
            src/ui/text/text_options.cpp
            src/ui/grouped_layout.cpp
            src/layout/layout_selection.cpp
        )
    endif()
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
# python3XX.dll gets copied next to zkgram.exe (see the POST_BUILD step
# below) so the exe runs without a system-wide Python install, but the
# embedded interpreter's own path auto-detection looks for landmark files
# (Lib/os.py etc.) relative to wherever the DLL physically is - which finds
# nothing there, since only the bare DLL was copied, not the whole Python
# tree. Stdlib modules bundled inside the DLL itself still import fine, but
# third-party pip packages (site-packages) do not: "ModuleNotFoundError:
# No module named 'cryptography'" even though pip confirms it is installed
# - confirmed live, not a hypothetical. Baking in the real site-packages
# path (like ZKGRAM_CRYPTOLAYER_ROOT above) and appending it to sys.path
# fixes this without needing Py_SetPythonHome() (which would have to run
# before Py_Initialize, earlier than this codebase's own pybind11
# scoped_interpreter setup allows).
# Python3_SITELIB (unlike CMAKE_SOURCE_DIR-derived paths above) comes back
# with native Windows backslashes - passed raw through a compiler -D flag,
# those get mangled into no separators at all ("C:UsersUser...", confirmed
# live via a runtime sys.path dump). Converted to forward slashes first,
# same as every other baked-in path here; Windows accepts forward slashes
# in paths just fine.
string(REPLACE "\\" "/" ZKGRAM_PYTHON_SITE_PACKAGES_FIXED "${Python3_SITELIB}")
target_compile_definitions(zkgram PRIVATE ZKGRAM_PYTHON_SITE_PACKAGES="${ZKGRAM_PYTHON_SITE_PACKAGES_FIXED}")
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

# Runtime DLLs zkgram.exe needs next to it to actually launch, not just
# link: embedding Python (pybind11::embed, see python_bridge.cpp) means
# python3XX.dll is required for every build (console or qt), and MSVC's own
# vcruntime140_1.dll is sometimes missing from a machine's System32 (see the
# matching comment in third_party/desktop-app/CMakeLists.txt for the same
# issue with codegen_style/codegen_emoji) - a bare "it built" is not the same
# as "it runs", and the previous state of this project required a manual
# windeployqt + DLL-copy pass by hand after every build to actually launch
# the qt config.
if(MSVC)
    get_filename_component(zkgram_msvc_bin_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    find_file(ZKGRAM_VCRUNTIME140_1_DLL NAMES vcruntime140_1.dll PATHS "${zkgram_msvc_bin_dir}" NO_DEFAULT_PATH)
    if(NOT ZKGRAM_VCRUNTIME140_1_DLL)
        find_file(ZKGRAM_VCRUNTIME140_1_DLL NAMES vcruntime140_1.dll)
    endif()
endif()
get_filename_component(zkgram_python_bin_dir "${Python3_EXECUTABLE}" DIRECTORY)
file(GLOB zkgram_python_runtime_dll "${zkgram_python_bin_dir}/python3*.dll")
if(zkgram_python_runtime_dll OR ZKGRAM_VCRUNTIME140_1_DLL)
    add_custom_command(TARGET zkgram POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${zkgram_python_runtime_dll} ${ZKGRAM_VCRUNTIME140_1_DLL}
            $<TARGET_FILE_DIR:zkgram>
        COMMENT "Deploying Python/CRT runtime DLLs next to zkgram.exe"
    )
endif()

if(ZKGRAM_UI STREQUAL "qt")
    target_link_libraries(zkgram PRIVATE Qt6::Widgets Qt6::Multimedia)
    # windeployqt copies every Qt DLL zkgram.exe actually needs (it inspects
    # the built binary itself), including Qt6Multimedia.dll's own plugins
    # (multimedia/*.dll) that a plain DLL copy would miss.
    get_filename_component(zkgram_qt_bin_dir "${Qt6_DIR}/../../../bin" ABSOLUTE)
    find_program(ZKGRAM_WINDEPLOYQT windeployqt PATHS "${zkgram_qt_bin_dir}" NO_DEFAULT_PATH)
    if(ZKGRAM_WINDEPLOYQT)
        add_custom_command(TARGET zkgram POST_BUILD
            COMMAND "${ZKGRAM_WINDEPLOYQT}" --release "$<TARGET_FILE:zkgram>"
            COMMENT "Running windeployqt on zkgram.exe"
        )
    endif()
    if(ZKGRAM_VENDOR_LIB_UI)
        target_link_libraries(zkgram PRIVATE desktop-app::lib_ui)

        # lib_ui's own common_options (see cmake/options_win.cmake) sets
        # /DEPENDENTLOADFLAG:0x800, which restricts the loader to resolving
        # dependent DLLs from System32 only. It is PRIVATE-linked into
        # lib_ui, so it should not propagate here per ordinary CMake usage
        # requirement rules - but it demonstrably does (confirmed via
        # dumpbin /loadconfig on the built zkgram.exe: 0800 Dependent Load
        # Flag was present), so zkgram.exe itself failed to launch with
        # STATUS_DLL_NOT_FOUND for Qt6Multimedia.dll/python313.dll/etc even
        # though they were sitting right next to it - the exact same root
        # cause as codegen_style/codegen_emoji earlier, now hitting the
        # shipped app itself, not just a build-time tool. Same fix: link the
        # override after lib_ui so its 0x0 is the last (winning) occurrence.
        target_link_libraries(zkgram PRIVATE zkgram_allow_nonsystem32_deps)

        # zkgram's own .style files (Phase 3+ of the migration, see
        # zkgram/.claude/UI.md section 13c). These reuse lib_ui's already
        # generated palette (Ui::Palette, exposed transitively via
        # desktop-app::lib_ui's PUBLIC include of its own gen/ directory,
        # see third_party/desktop-app/lib_ui/cmake/generate_target.cmake) -
        # not a second, separately generated palette.
        get_filename_component(submodules_loc "third_party/desktop-app" REALPATH)
        # src_loc is now the whole src/ tree (not just src/ui/qt), because
        # ui/chat/chat.style's own "using" directives reference sibling
        # files by src-relative path (dialogs/dialogs.style,
        # ui/chat/chat.style) - see src/ui/chat/ for where those real
        # tdesktop chat-design files were copied in from.
        generate_styles(zkgram "${CMAKE_SOURCE_DIR}/src"
            "ui/qt/zkgram_icons.style;ui/menu_icons.style;ui/chat/chat.style;ui/chat/chat_style.style;ui/chat/zkgram_chat_style_stub.style;dialogs/dialogs.style"
            "")

        # codegen_style's own generated .cpp files (gen/styles/style_*.cpp)
        # only #include their own header + styles/palette.h. lib_ui's own
        # generated style files get away with this because every lib_ui.cpp
        # (generated ones included) shares lib_ui's PCH, which brings in
        # base/basic_types.h (float64 and friends, used by
        # style_core_font.h/style_core_scale.h) implicitly. zkgram has no
        # PCH of its own, so its generated style_zkgram_icons.cpp does not
        # get that for free - force-include it for every zkgram translation
        # unit instead of hand-patching a generated (overwritten-on-rebuild)
        # file.
        if(MSVC)
            target_compile_options(zkgram PRIVATE "/FI${CMAKE_SOURCE_DIR}/third_party/desktop-app/lib_base/base/basic_types.h")
        else()
            target_compile_options(zkgram PRIVATE "-include${CMAKE_SOURCE_DIR}/third_party/desktop-app/lib_base/base/basic_types.h")
        endif()
    endif()
endif()
