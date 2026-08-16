add_executable(codegen_style)
init_target(codegen_style "(codegen)")

get_filename_component(src_loc ../.. REALPATH)

nice_target_sources(codegen_style ${src_loc}
PRIVATE
    codegen/style/generator.cpp
    codegen/style/generator.h
    codegen/style/main.cpp
    codegen/style/module.cpp
    codegen/style/module.h
    codegen/style/options.cpp
    codegen/style/options.h
    codegen/style/parsed_file.cpp
    codegen/style/parsed_file.h
    codegen/style/processor.cpp
    codegen/style/processor.h
    codegen/style/render_svg.cpp
    codegen/style/render_svg.h
    codegen/style/structure_types.cpp
    codegen/style/structure_types.h
)

target_include_directories(codegen_style
PUBLIC
    ${src_loc}
)

target_link_libraries(codegen_style
PUBLIC
    desktop-app::codegen_common
    desktop-app::lib_base
    desktop-app::lib_crl
)

# zkgram patch: see zkgram_deploy_qt_runtime() in third_party/desktop-app/CMakeLists.txt.
# codegen_style is invoked as a custom build step by lib_ui's generate_styles/
# generate_palette; without its own Qt/CRT DLLs next to it, that step fails
# with STATUS_DLL_NOT_FOUND regardless of the caller's PATH.
zkgram_deploy_qt_runtime(codegen_style)

# zkgram patch: see zkgram_allow_nonsystem32_deps in
# third_party/desktop-app/CMakeLists.txt for why this is needed.
target_link_libraries(codegen_style PRIVATE zkgram_allow_nonsystem32_deps)
