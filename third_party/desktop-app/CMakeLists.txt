# Vendored subset of the desktop-app (Telegram Desktop) toolkit:
# cmake (cmake_helpers), lib_rpl, lib_crl, lib_base, codegen, lib_ui, plus the
# small ThirdParty/ headers they need (GSL, range-v3, expected, xxHash).
#
# This mirrors the validated Phase 0 scratch build (see zkgram/docs/README.md,
# "lib_ui/RpWidget: статус переноса") rather than the full upstream
# cmake/external tree: only the pieces codegen_style/lib_ui actually need are
# wired up, not the full desktop-app dependency graph (ffmpeg, webrtc, ANGLE,
# etc. are not needed by zkgram and are intentionally not vendored).
#
# GPLv3: lib_ui/lib_base/lib_rpl/lib_crl/codegen/cmake are GNU GPLv3 (see
# https://github.com/desktop-app/legal/blob/master/LEGAL). Anything statically
# linked against this directory makes the resulting binary subject to GPLv3.

set(DESKTOP_APP_USE_PACKAGED ON CACHE BOOL "" FORCE)

get_filename_component(submodules_loc "." REALPATH)
get_filename_component(cmake_loc "cmake" REALPATH)
get_filename_component(third_party_loc "ThirdParty" REALPATH)
# Upstream's own cmake_helpers sets this; the vendored subset never did, so
# every include(${cmake_helpers_loc}/...) inside the copied cmake/ tree
# resolved to an absolute "/external/..." path and failed. Only ever hit on
# Linux, because the block in lib_base that uses it (D-Bus codegen) is
# guarded by if (LINUX) - which is why the Windows build never noticed.
get_filename_component(cmake_helpers_loc "cmake" REALPATH)

include(cmake/variables.cmake)
include(cmake/nice_target_sources.cmake)
include(cmake/target_compile_options_if_exists.cmake)
include(cmake/target_link_frameworks.cmake)
include(cmake/target_link_options_if_exists.cmake)
include(cmake/init_target.cmake)
include(cmake/generate_target.cmake)
include(cmake/options.cmake)
include(cmake/target_prepare_qrc.cmake)

macro(add_checked_subdirectory name)
    if (NOT DEFINED desktop_app_skip_libs OR NOT ${name} IN_LIST desktop_app_skip_libs)
        add_subdirectory(${name})
    endif()
endmacro()

add_subdirectory(cmake/external/gsl)

# external_qt: linked directly against the same Qt6 target zkgram's own
# CMakeLists.txt already found (find_package(Qt6 ...) in ZKGRAM_UI=qt branch),
# instead of upstream's cmake/external/qt, which unconditionally pulls the
# kimageformats submodule (AVIF/JXL static plugins) that zkgram does not need.
add_library(external_qt INTERFACE IMPORTED GLOBAL)
add_library(desktop-app::external_qt ALIAS external_qt)
target_link_libraries(external_qt INTERFACE Qt::Core Qt::Gui Qt::Widgets Qt::Network Qt::Svg Qt::OpenGL Qt::OpenGLWidgets)

if (LINUX)
    # Qt::DBus on top: lib_base's Linux platform layer and the bundled
    # kcoreaddons sources (kurlmimedata.cpp -> dbustypes_p.h) include
    # QtDBus headers directly. Not needed on Windows, which is why the
    # vendored list above never had it.
    target_link_libraries(external_qt INTERFACE Qt::DBus)
endif()

# lib_ui includes some Qt private headers directly (e.g. QtGui/private/qfixed_p.h
# in ui/style/style_core_font.h) - normal upstream tdesktop practice, these
# headers ship with Qt itself under a versioned private/ subfolder, just not
# on the public include path by default.
get_target_property(zkgram_qt_core_includes Qt6::Core INTERFACE_INCLUDE_DIRECTORIES)
list(GET zkgram_qt_core_includes 0 zkgram_qt_core_include_dir)
get_target_property(zkgram_qt_gui_includes Qt6::Gui INTERFACE_INCLUDE_DIRECTORIES)
list(GET zkgram_qt_gui_includes 0 zkgram_qt_gui_include_dir)
get_target_property(zkgram_qt_widgets_includes Qt6::Widgets INTERFACE_INCLUDE_DIRECTORIES)
list(GET zkgram_qt_widgets_includes 0 zkgram_qt_widgets_include_dir)
target_include_directories(external_qt INTERFACE
    "${zkgram_qt_core_include_dir}/${Qt6Core_VERSION}"
    "${zkgram_qt_core_include_dir}/${Qt6Core_VERSION}/QtCore"
    "${zkgram_qt_gui_include_dir}/${Qt6Gui_VERSION}"
    "${zkgram_qt_gui_include_dir}/${Qt6Gui_VERSION}/QtGui"
    "${zkgram_qt_widgets_include_dir}/${Qt6Widgets_VERSION}"
    "${zkgram_qt_widgets_include_dir}/${Qt6Widgets_VERSION}/QtWidgets"
)

# external_openssl: reuses the OpenSSL::* targets zkgram's root CMakeLists.txt
# already resolves via find_package(OpenSSL REQUIRED), instead of upstream's
# cmake/external/openssl, which always expects a hand-built OpenSSL tree.
add_library(external_openssl INTERFACE IMPORTED GLOBAL)
add_library(desktop-app::external_openssl ALIAS external_openssl)
target_link_libraries(external_openssl INTERFACE OpenSSL::SSL OpenSSL::Crypto)

add_library(external_crash_reports INTERFACE IMPORTED GLOBAL)
add_library(desktop-app::external_crash_reports ALIAS external_crash_reports)

# Stub: real cmake/external/qt.cmake (AVIF/JXL static plugins via the
# kimageformats submodule) is intentionally not vendored, see the header
# comment above. codegen_emoji links against this target but zkgram does not
# need the extra image format plugins it would normally provide.
add_library(external_qt_static_plugins INTERFACE IMPORTED GLOBAL)
add_library(desktop-app::external_qt_static_plugins ALIAS external_qt_static_plugins)

# vcpkg is deliberately NOT on the global CMAKE_PREFIX_PATH (passed on the
# command line) - it must never be visible to zkgram's own root
# find_package(OpenSSL REQUIRED), which needs to keep resolving the
# dedicated OpenSSL-Win64 (MD runtime) install the rest of zkgram (and
# TDLib) already links against. Linking vcpkg's own OpenSSL in *alongside*
# it previously caused duplicate-symbol link errors (two different
# libcrypto.lib copies in the same binary) - see zkgram/docs/README.md.
# Scoped here, as a plain (non-cache) variable, so it only affects the
# find_package() calls below and everything add_subdirectory()-ed afterward
# (codegen, lib_ui), not anything already resolved in the parent scope.
if (WIN32)
    list(APPEND CMAKE_PREFIX_PATH "C:/vcpkg/installed/x64-windows")
endif()

# external_jpeg: required by lib_ui itself (not by codegen_style).
# "vcpkg install libjpeg-turbo:x64-windows" provides the JPEG::JPEG target.
find_package(JPEG REQUIRED)
add_library(external_jpeg INTERFACE IMPORTED GLOBAL)
add_library(desktop-app::external_jpeg ALIAS external_jpeg)
target_link_libraries(external_jpeg INTERFACE JPEG::JPEG)

# external_zlib / external_lz4: lib_ui's remaining PRIVATE dependencies
# (alongside external_jpeg and external_xxhash above).
find_package(ZLIB REQUIRED)
add_library(external_zlib INTERFACE IMPORTED GLOBAL)
add_library(desktop-app::external_zlib ALIAS external_zlib)
target_link_libraries(external_zlib INTERFACE ZLIB::ZLIB)

# "vcpkg install lz4:x64-windows" provides the lz4::lz4 target. Distro
# packages generally do not: Debian/Ubuntu's liblz4-dev ships pkg-config
# metadata and no CMake config package at all, so the CONFIG-mode lookup
# below finds nothing and the whole configure fails. Fall back to
# pkg-config in that case and build the same lz4::lz4 target by hand.
find_package(lz4 CONFIG QUIET)
if (NOT TARGET lz4::lz4)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(DESKTOP_APP_LZ4 REQUIRED IMPORTED_TARGET liblz4)
    add_library(lz4::lz4 INTERFACE IMPORTED)
    target_link_libraries(lz4::lz4 INTERFACE PkgConfig::DESKTOP_APP_LZ4)
endif()
add_library(external_lz4 INTERFACE IMPORTED GLOBAL)
add_library(desktop-app::external_lz4 ALIAS external_lz4)
target_link_libraries(external_lz4 INTERFACE lz4::lz4)

add_subdirectory(cmake/external/ranges)
add_subdirectory(cmake/external/expected)
add_subdirectory(cmake/external/xxhash)

if (LINUX)
    # lib_base's Linux half is not optional: base/platform/linux/*.cpp are in
    # its source list unconditionally, and they #include the D-Bus/GObject
    # bindings that external_glib generates plus KShell/KUser/KSandbox from
    # external_kcoreaddons. Neither external was in the vendored subset (it
    # was assembled against Windows, where none of this compiles in), so
    # ZKGRAM_UI=qt could never configure on Linux at all.
    #
    # external_glib additionally pulls cmake/external/glib/cppgir, which
    # upstream carries as a git submodule - see the shim CMakeLists.txt there
    # for how it binds to the distro's cppgir package instead.
    add_subdirectory(cmake/external/glib)
    add_subdirectory(cmake/external/kcoreaddons)
endif()

# codegen_style/codegen_emoji are host tools CMake invokes as custom build
# steps (to generate style headers / emoji data) using whatever PATH the
# invoking build system (MSBuild) has, which does not necessarily include Qt's
# bin directory. Without their Qt/CRT DLLs sitting right next to them, the
# custom build step fails with STATUS_DLL_NOT_FOUND even though the tool
# built successfully (see zkgram/docs/README.md for how this was diagnosed).
# Copying the DLLs next to the .exe makes them resolvable regardless of the
# caller's PATH, since "search the executable's own directory first" is
# always in effect.
# vcruntime140_1.dll specifically (as opposed to vcruntime140.dll/msvcp140*)
# is sometimes missing from System32 on machines that only have the Build
# Tools installed, not the full VC++ Redistributable. It ships inside the
# compiler's own toolset directory regardless, so it is found from
# CMAKE_CXX_COMPILER's location rather than assuming a system install.
get_filename_component(zkgram_msvc_bin_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
find_file(ZKGRAM_VCRUNTIME140_1_DLL
    NAMES vcruntime140_1.dll
    PATHS "${zkgram_msvc_bin_dir}"
    NO_DEFAULT_PATH
)
if(NOT ZKGRAM_VCRUNTIME140_1_DLL)
    # Fall back to a full search (e.g. onecore/x64 redist layout).
    find_file(ZKGRAM_VCRUNTIME140_1_DLL NAMES vcruntime140_1.dll)
endif()

# add_custom_command(TARGET ... POST_BUILD) must be called from the same
# CMakeLists.txt directory that created the target, so this cannot be a
# function invoked from here for codegen_style/codegen_emoji (they are
# created inside codegen/codegen/style and codegen/codegen/emoji). Instead,
# zkgram_deploy_qt_runtime() is defined here (before those subdirectories are
# added, so they inherit it) and called from a small, deliberate patch at the
# bottom of each of those two vendored CMakeLists.txt files - see the comment
# there for why.
set(ZKGRAM_QT_RUNTIME_FILES $<TARGET_FILE:Qt::Core> $<TARGET_FILE:Qt::Gui> $<TARGET_FILE:Qt::Svg>)
if(ZKGRAM_VCRUNTIME140_1_DLL)
    list(APPEND ZKGRAM_QT_RUNTIME_FILES "${ZKGRAM_VCRUNTIME140_1_DLL}")
endif()

function(zkgram_deploy_qt_runtime target_name)
    if(NOT WIN32)
        return()
    endif()
    add_custom_command(TARGET ${target_name} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${ZKGRAM_QT_RUNTIME_FILES} $<TARGET_FILE_DIR:${target_name}>
        COMMENT "Deploying Qt/CRT runtime DLLs next to ${target_name}"
    )
endfunction()

# common_options (cmake/options_win.cmake, linked into every target by
# init_target()) sets /DEPENDENTLOADFLAG:0x800, which restricts the loader to
# resolving dependent DLLs from System32 only. That is correct DLL-hijacking
# hardening for the shipped app (lib_ui/zkgram.exe), but it is exactly what
# was causing codegen_style/codegen_emoji (build-time host tools that must
# load Qt's DLLs from a non-System32 location) to fail to launch at all with
# STATUS_DLL_NOT_FOUND - not missing DLLs, not antivirus, not Control Flow
# Guard, all of which were checked and ruled out first. common_options is a
# single shared INTERFACE library (init_target links every target against
# it), so its own flags cannot be made conditional per-consumer; instead,
# this target is linked after a consuming target's other dependencies so its
# /DEPENDENTLOADFLAG:0x0 is the last one on the link line (MSVC uses the last
# occurrence of a repeated switch).
add_library(zkgram_allow_nonsystem32_deps INTERFACE)
if (MSVC)
    # MSVC-only switch. Unguarded, it reached GNU ld on Linux, which read it
    # as an input filename ("cannot find /DEPENDENTLOADFLAG:0x0") and failed
    # every link that pulls this target in - codegen_style, codegen_emoji and
    # zkgram itself. The target stays defined unconditionally so the three
    # target_link_libraries() calls referring to it need no guards of their own.
    target_link_options(zkgram_allow_nonsystem32_deps INTERFACE /DEPENDENTLOADFLAG:0x0)
endif()

add_subdirectory(lib_rpl)
add_subdirectory(lib_crl)
add_subdirectory(lib_base)
add_subdirectory(codegen/codegen/common)
add_subdirectory(codegen/codegen/style)
add_subdirectory(codegen/codegen/emoji)
add_subdirectory(lib_ui)
