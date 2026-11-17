include_guard(GLOBAL)

function(horo_add_editor_baseline_lighting_assets_target)
    if(TARGET HoroEditorBaselineLightingAssetsInternal)
        return()
    endif()
    set(headers "${PROJECT_BINARY_DIR}/editor_baseline_asset_headers")
    file(MAKE_DIRECTORY "${headers}")
    configure_file("${PROJECT_SOURCE_DIR}/apps/HoroEditor/app/EditorBaselineLightingAssets.h"
        "${headers}/EditorBaselineLightingAssets.h" COPYONLY)
    add_library(HoroEditorBaselineLightingAssetsInternal STATIC
        "${PROJECT_SOURCE_DIR}/apps/HoroEditor/app/EditorBaselineLightingAssets.cpp")
    target_compile_features(HoroEditorBaselineLightingAssetsInternal PUBLIC cxx_std_20)
    target_include_directories(HoroEditorBaselineLightingAssetsInternal PUBLIC "${headers}")
    target_link_libraries(HoroEditorBaselineLightingAssetsInternal PUBLIC HoroEngine::RenderApi
        PRIVATE HoroEditorViewportShaderRecipeInternal)
endfunction()
