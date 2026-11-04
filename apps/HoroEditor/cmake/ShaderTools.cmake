# Included from apps/CMakeLists.txt; source and binary directories remain scoped to apps/.
add_executable(HoroEditorUiPaintShaderCook EXCLUDE_FROM_ALL HoroEditor/tools/UiPaintCook.cpp)
target_compile_features(HoroEditorUiPaintShaderCook PRIVATE cxx_std_20)
target_link_libraries(HoroEditorUiPaintShaderCook PRIVATE HoroEditorShaderCookHostInternal HoroHostSignatureDocuments
    HoroUiPaintShaderPackage HoroEngine::Platform)

# Offline preparation tool, never linked into HoroEditor or a runtime renderer.
add_executable(HoroEditorVulkanShaderCook EXCLUDE_FROM_ALL HoroEditor/tools/VulkanViewportCook.cpp)
target_compile_features(HoroEditorVulkanShaderCook PRIVATE cxx_std_20)
target_link_libraries(HoroEditorVulkanShaderCook PRIVATE HoroEditorVulkanShaderInternal
    HoroEngine::ShaderCompilerToolchain HoroEngine::Platform)

include("${PROJECT_SOURCE_DIR}/cmake/HoroEditorClusteredLightingAssets.cmake")
horo_add_editor_clustered_lighting_assets_target()

if(TARGET HoroEditor)
    target_link_libraries(HoroEditor PRIVATE HoroEditorClusteredLightingAssetsInternal HoroEngine::EditorViewportLightingInternal)
endif()

if(TARGET HoroEditor AND TARGET HoroEditorViewportMetal)
    target_sources(HoroEditor PRIVATE HoroEditor/app/EditorClusteredLightingStartup.cpp)
endif()

# Explicit offline preparation; never linked into the running editor or runtime.
set(HORO_CLUSTERED_COOK_HEADERS "${CMAKE_CURRENT_BINARY_DIR}/clustered_cook_headers")
file(MAKE_DIRECTORY "${HORO_CLUSTERED_COOK_HEADERS}")
configure_file(HoroEditor/tools/ClusteredLightingCook.h
    "${HORO_CLUSTERED_COOK_HEADERS}/ClusteredLightingCook.h" COPYONLY)
add_library(HoroEditorClusteredLightingCookInternal STATIC
    HoroEditor/tools/ClusteredLightingCook.cpp
    HoroEditor/tools/ClusteredLightingCookPackage.cpp)
target_compile_features(HoroEditorClusteredLightingCookInternal PUBLIC cxx_std_20)
target_include_directories(HoroEditorClusteredLightingCookInternal PUBLIC "${HORO_CLUSTERED_COOK_HEADERS}")
target_link_libraries(HoroEditorClusteredLightingCookInternal PUBLIC HoroEditorShaderCookHostInternal
    PRIVATE HoroEditorClusteredLightingAssetsInternal HoroEditorViewportShaderRecipeInternal
    HoroShaderReflectionCookInternal HoroEngine::Platform HoroFoundationJsonInternal HoroAtomicShaderCookPackageInternal)
add_executable(HoroEditorClusteredLightingShaderCook EXCLUDE_FROM_ALL HoroEditor/tools/ClusteredLightingCookMain.cpp)
target_compile_features(HoroEditorClusteredLightingShaderCook PRIVATE cxx_std_20)
target_link_libraries(HoroEditorClusteredLightingShaderCook PRIVATE HoroEditorClusteredLightingCookInternal)

set(HORO_VFX_GPU_COOK_HEADERS "${CMAKE_CURRENT_BINARY_DIR}/vfx_gpu_cook_headers")
file(MAKE_DIRECTORY "${HORO_VFX_GPU_COOK_HEADERS}")
configure_file(HoroEditor/tools/VfxGpuCook.h "${HORO_VFX_GPU_COOK_HEADERS}/VfxGpuCook.h" COPYONLY)
add_library(HoroVfxGpuCookInternal STATIC HoroEditor/tools/VfxGpuCook.cpp)
target_compile_features(HoroVfxGpuCookInternal PUBLIC cxx_std_20)
target_include_directories(HoroVfxGpuCookInternal PUBLIC "${HORO_VFX_GPU_COOK_HEADERS}")
target_link_libraries(HoroVfxGpuCookInternal PUBLIC HoroEngine::RenderApi HoroEditorShaderCookHostInternal
    PRIVATE HoroAtomicShaderCookPackageInternal HoroEngine::Platform)
add_executable(HoroVfxGpuShaderCook EXCLUDE_FROM_ALL HoroEditor/tools/VfxGpuCookMain.cpp)
target_compile_features(HoroVfxGpuShaderCook PRIVATE cxx_std_20)
target_link_libraries(HoroVfxGpuShaderCook PRIVATE HoroVfxGpuCookInternal)

set(HORO_VFX_GPU_VULKAN_COOK_HEADERS "${CMAKE_CURRENT_BINARY_DIR}/vfx_gpu_vulkan_cook_headers")
file(MAKE_DIRECTORY "${HORO_VFX_GPU_VULKAN_COOK_HEADERS}")
configure_file(HoroEditor/tools/VfxGpuVulkanCook.h
    "${HORO_VFX_GPU_VULKAN_COOK_HEADERS}/VfxGpuVulkanCook.h" COPYONLY)
add_library(HoroVfxGpuVulkanCookInternal STATIC HoroEditor/tools/VfxGpuVulkanCook.cpp)
target_compile_features(HoroVfxGpuVulkanCookInternal PUBLIC cxx_std_20)
target_include_directories(HoroVfxGpuVulkanCookInternal PUBLIC "${HORO_VFX_GPU_VULKAN_COOK_HEADERS}")
target_link_libraries(HoroVfxGpuVulkanCookInternal PUBLIC HoroEngine::RenderApi HoroEditorShaderCookHostInternal
    PRIVATE HoroAtomicShaderCookPackageInternal HoroEngine::Platform)
add_executable(HoroVfxGpuVulkanShaderCook EXCLUDE_FROM_ALL HoroEditor/tools/VfxGpuVulkanCookMain.cpp)
target_compile_features(HoroVfxGpuVulkanShaderCook PRIVATE cxx_std_20)
target_link_libraries(HoroVfxGpuVulkanShaderCook PRIVATE HoroVfxGpuVulkanCookInternal)
