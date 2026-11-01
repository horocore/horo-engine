include_guard(GLOBAL)

# This app adapter is also compiled by headless admission tests without enabling the GUI host.
function(horo_add_editor_clustered_lighting_assets_target)
    if(TARGET HoroEditorClusteredLightingAssetsInternal)
        return()
    endif()
    set(clustered_headers "${PROJECT_BINARY_DIR}/editor_clustered_asset_headers")
    file(MAKE_DIRECTORY "${clustered_headers}")
    configure_file("${PROJECT_SOURCE_DIR}/apps/HoroEditor/app/EditorClusteredLightingAssets.h"
        "${clustered_headers}/EditorClusteredLightingAssets.h" COPYONLY)
    add_library(HoroEditorClusteredLightingAssetsInternal STATIC
        "${PROJECT_SOURCE_DIR}/apps/HoroEditor/app/EditorClusteredLightingAssets.cpp")
    target_compile_features(HoroEditorClusteredLightingAssetsInternal PUBLIC cxx_std_20)
    target_include_directories(HoroEditorClusteredLightingAssetsInternal PUBLIC "${clustered_headers}")
    target_link_libraries(HoroEditorClusteredLightingAssetsInternal PUBLIC HoroEngine::RenderApi
        PRIVATE HoroEditorViewportShaderRecipeInternal)
endfunction()
