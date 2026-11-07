# Non-installed engine-development GUI service. The executable supplies its exact build inputs.
add_library(HoroEditorGuiLiveReloadHost STATIC
    "${PROJECT_SOURCE_DIR}/src/editor/development/live_reload/EditorGuiLiveReload.cpp")
target_compile_features(HoroEditorGuiLiveReloadHost PUBLIC cxx_std_20)
target_include_directories(HoroEditorGuiLiveReloadHost PUBLIC
    "${PROJECT_SOURCE_DIR}/src/editor/development/live_reload")
target_link_libraries(HoroEditorGuiLiveReloadHost PUBLIC HoroEngine::EditorServices)

function(horo_attach_gui_live_host target)
    target_link_libraries(${target} PRIVATE HoroEditorGuiLiveReloadHost)
    set(config_dir "${PROJECT_BINARY_DIR}/gui-live/${target}-config")
    target_include_directories(${target} PRIVATE "${config_dir}")
    if(MSVC)
        target_compile_options(${target} PRIVATE /FC /utf-8)
    endif()
    if(NOT HORO_EDITOR_LIVE_UI)
        file(GENERATE OUTPUT "${config_dir}/EditorGuiLiveReloadBuildConfig.h" CONTENT
            "#pragma once\n#include \"EditorGuiLiveReload.h\"\nnamespace Horo::Editor { inline EditorGuiLiveReloadConfiguration BuildEditorGuiLiveReloadConfiguration() { return {}; } }\n")
        return()
    endif()
    set(HORO_GUI_LIVE_DIR "${PROJECT_BINARY_DIR}/gui-live")
    if(APPLE)
        # Preserve host definitions for bundle imports, including inline address getters.
        target_link_options(${target} PRIVATE "LINKER:-export_dynamic"
            "LINKER:-force_load,$<TARGET_FILE:HoroGui>")
        # Every registered draw must resolve against this host, even if static fallback
        # callers did not otherwise pull its object or service dependencies from the archive.
        add_custom_command(OUTPUT "${HORO_GUI_LIVE_DIR}/${target}.rsp"
            COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/scripts/dev.py" gui-live-bridge
                --compile-commands "${PROJECT_BINARY_DIR}/compile_commands.json"
                --output "${HORO_GUI_LIVE_DIR}/${target}.rsp" --image "$<TARGET_FILE_NAME:${target}>"
                --platform darwin
            DEPENDS "${PROJECT_SOURCE_DIR}/scripts/dev.py" "${PROJECT_BINARY_DIR}/compile_commands.json"
            VERBATIM)
        target_sources(${target} PRIVATE "${HORO_GUI_LIVE_DIR}/${target}.rsp")
    else()
        set(exports "${HORO_GUI_LIVE_DIR}/${target}.def")
        add_custom_command(OUTPUT "${exports}" "${HORO_GUI_LIVE_DIR}/${target}.rsp"
            COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/scripts/dev.py" gui-live-bridge
                --compile-commands "${PROJECT_BINARY_DIR}/compile_commands.json"
                --output "${exports}" --image "$<TARGET_FILE_NAME:${target}>"
            DEPENDS HoroGui "${PROJECT_SOURCE_DIR}/scripts/dev.py"
                "${PROJECT_BINARY_DIR}/compile_commands.json"
            VERBATIM)
        target_sources(${target} PRIVATE "${exports}")
    endif()
    set_target_properties(${target} PROPERTIES ENABLE_EXPORTS ON)
    if(APPLE)
        set(compiler "${CMAKE_CXX_COMPILER}")
        set(link_options "-bundle_loader \"$<TARGET_FILE:${target}>\"")
    else()
        # RCC++'s Windows transport discovers the matching MSVC developer environment.
        set(compiler "")
        set(link_options "\"$<TARGET_LINKER_FILE:${target}>\"")
    endif()
    file(GENERATE OUTPUT "${config_dir}/EditorGuiLiveReloadBuildConfig.h" CONTENT "#pragma once
#include \"EditorGuiLiveReload.h\"
namespace Horo::Editor {
inline EditorGuiLiveReloadConfiguration BuildEditorGuiLiveReloadConfiguration() {
    return {
        .hostImage = u8R\"horo($<TARGET_FILE:${target}>)horo\",
        .intermediateRoot = u8R\"horo(${CMAKE_CURRENT_BINARY_DIR}/live-ui/${target})horo\",
        .includeManifest = u8R\"horo(${HORO_GUI_LIVE_DIR}/includes.txt)horo\",
        .compileOptions = R\"horo(@\"${HORO_GUI_LIVE_DIR}/${target}.rsp\")horo\",
        .linkOptions = R\"horo(${link_options})horo\",
        .compiler = u8R\"horo(${compiler})horo\"
    };
}
}")
endfunction()
