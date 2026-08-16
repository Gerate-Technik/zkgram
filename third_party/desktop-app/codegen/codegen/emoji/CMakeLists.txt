add_executable(codegen_emoji)
init_target(codegen_emoji "(codegen)")

get_filename_component(src_loc ../.. REALPATH)

nice_target_sources(codegen_emoji ${src_loc}
PRIVATE
    codegen/emoji/data.cpp
    codegen/emoji/data.h
    codegen/emoji/data_old.cpp
    codegen/emoji/data_old.h
    codegen/emoji/data_read.cpp
    codegen/emoji/data_read.h
    codegen/emoji/generator.cpp
    codegen/emoji/generator.h
    codegen/emoji/main.cpp
    codegen/emoji/options.cpp
    codegen/emoji/options.h
    codegen/emoji/replaces.cpp
    codegen/emoji/replaces.h
)

target_include_directories(codegen_emoji
PUBLIC
    ${src_loc}
)

target_link_libraries(codegen_emoji
PUBLIC
    desktop-app::lib_base
    desktop-app::codegen_common
    desktop-app::external_qt_static_plugins
    desktop-app::external_qt
)

# zkgram patch: see zkgram_deploy_qt_runtime() in third_party/desktop-app/CMakeLists.txt.
# codegen_emoji is invoked as a custom build step by lib_ui's generate_emoji;
# without its own Qt/CRT DLLs next to it, that step fails with
# STATUS_DLL_NOT_FOUND regardless of the caller's PATH.
zkgram_deploy_qt_runtime(codegen_emoji)

# zkgram patch: see zkgram_allow_nonsystem32_deps in
# third_party/desktop-app/CMakeLists.txt for why this is needed.
target_link_libraries(codegen_emoji PRIVATE zkgram_allow_nonsystem32_deps)
