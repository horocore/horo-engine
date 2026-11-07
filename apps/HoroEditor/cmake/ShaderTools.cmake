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
include(HoroEditorBaselineLightingAssets)
horo_add_editor_baseline_lighting_assets_target()

if(TARGET HoroEditor)
    target_link_libraries(HoroEditor PRIVATE HoroEditorClusteredLightingAssetsInternal
        HoroEditorBaselineLightingAssetsInternal HoroEngine::EditorViewportLightingInternal)
endif()

if(TARGET HoroEditor AND TARGET HoroEditorViewportMetal)
    target_sources(HoroEditor PRIVATE HoroEditor/app/EditorClusteredLightingStartup.cpp)
    target_sources(HoroEditor PRIVATE HoroEditor/app/EditorColorPipelineStartup.cpp)
    target_link_libraries(HoroEditor PRIVATE HoroColorOutputPackageInternal HoroEditorViewportColorInternal)
    include(HoroEditorColorPipelineAssets)
    horo_stage_editor_color_pipeline()
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

# Forward rendering consumes its own complete generic raster family, with no compute artifact.
set(HORO_BASELINE_COOK_HEADERS "${CMAKE_CURRENT_BINARY_DIR}/baseline_cook_headers")
file(MAKE_DIRECTORY "${HORO_BASELINE_COOK_HEADERS}")
configure_file(HoroEditor/tools/BaselineLightingCook.h
    "${HORO_BASELINE_COOK_HEADERS}/BaselineLightingCook.h" COPYONLY)
add_library(HoroEditorBaselineLightingCookInternal STATIC
    HoroEditor/tools/BaselineLightingCook.cpp
    HoroEditor/tools/BaselineLightingCookPackage.cpp)
target_compile_features(HoroEditorBaselineLightingCookInternal PUBLIC cxx_std_20)
target_include_directories(HoroEditorBaselineLightingCookInternal PUBLIC "${HORO_BASELINE_COOK_HEADERS}")
target_link_libraries(HoroEditorBaselineLightingCookInternal PUBLIC HoroEditorShaderCookHostInternal
    PRIVATE HoroEditorBaselineLightingAssetsInternal HoroEditorViewportShaderRecipeInternal
    HoroShaderReflectionCookInternal HoroEngine::Platform HoroAtomicShaderCookPackageInternal)
add_executable(HoroEditorBaselineLightingShaderCook EXCLUDE_FROM_ALL HoroEditor/tools/BaselineLightingCookMain.cpp)
target_compile_features(HoroEditorBaselineLightingShaderCook PRIVATE cxx_std_20)
target_link_libraries(HoroEditorBaselineLightingShaderCook PRIVATE HoroEditorBaselineLightingCookInternal)

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

# Offline exposure production reuses authenticated tools and atomic publication.
set(HORO_EXPOSURE_GPU_COOK_HEADERS "${CMAKE_CURRENT_BINARY_DIR}/exposure_gpu_cook_headers")
file(MAKE_DIRECTORY "${HORO_EXPOSURE_GPU_COOK_HEADERS}")
configure_file(HoroEditor/tools/ExposureGpuCook.h "${HORO_EXPOSURE_GPU_COOK_HEADERS}/ExposureGpuCook.h" COPYONLY)
add_library(HoroExposureGpuCookInternal STATIC HoroEditor/tools/ExposureGpuCook.cpp)
target_compile_features(HoroExposureGpuCookInternal PUBLIC cxx_std_20)
target_include_directories(HoroExposureGpuCookInternal PUBLIC "${HORO_EXPOSURE_GPU_COOK_HEADERS}")
target_link_libraries(HoroExposureGpuCookInternal PUBLIC HoroEngine::RenderApi HoroEditorShaderCookHostInternal HoroColorOutputPackageInternal
    PRIVATE HoroShaderReflectionCookInternal HoroAtomicShaderCookPackageInternal HoroEngine::Platform)
add_executable(HoroExposureGpuShaderCook EXCLUDE_FROM_ALL HoroEditor/tools/ExposureGpuCookMain.cpp)
target_compile_features(HoroExposureGpuShaderCook PRIVATE cxx_std_20)
target_link_libraries(HoroExposureGpuShaderCook PRIVATE HoroExposureGpuCookInternal)

# Product color cook keeps analytic source/tables and real reflected native artifacts in one generation.
set(HORO_COLOR_OUTPUT_COOK_HEADERS "${CMAKE_CURRENT_BINARY_DIR}/color_output_cook_headers")
file(MAKE_DIRECTORY "${HORO_COLOR_OUTPUT_COOK_HEADERS}")
configure_file(HoroEditor/tools/ColorOutputCook.h "${HORO_COLOR_OUTPUT_COOK_HEADERS}/ColorOutputCook.h" COPYONLY)
add_library(HoroColorOutputCookInternal STATIC
    HoroEditor/tools/ColorOutputCook.cpp
    HoroEditor/tools/ColorOutputCookBundle.cpp
    HoroEditor/tools/ColorOutputCookPackage.cpp)
target_compile_features(HoroColorOutputCookInternal PUBLIC cxx_std_20)
target_include_directories(HoroColorOutputCookInternal PUBLIC "${HORO_COLOR_OUTPUT_COOK_HEADERS}")
target_link_libraries(HoroColorOutputCookInternal PUBLIC HoroEngine::RenderApi HoroEditorShaderCookHostInternal HoroColorOutputPackageInternal
    PRIVATE HoroColorOutputShaderRecipeInternal HoroShaderReflectionCookInternal
    HoroAtomicShaderCookPackageInternal HoroEngine::Platform HoroFoundationJsonInternal)
add_executable(HoroColorOutputShaderCook EXCLUDE_FROM_ALL HoroEditor/tools/ColorOutputCookMain.cpp)
target_compile_features(HoroColorOutputShaderCook PRIVATE cxx_std_20)
target_link_libraries(HoroColorOutputShaderCook PRIVATE HoroColorOutputCookInternal)

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

# Ordinary and masked indexed-particle shaders share one authenticated offline producer.
set(HORO_VFX_INSTANCED_COOK_HEADERS "${CMAKE_CURRENT_BINARY_DIR}/vfx_instanced_cook_headers")
file(MAKE_DIRECTORY "${HORO_VFX_INSTANCED_COOK_HEADERS}")
configure_file(HoroEditor/tools/VfxInstancedCook.h
    "${HORO_VFX_INSTANCED_COOK_HEADERS}/VfxInstancedCook.h" COPYONLY)
add_library(HoroVfxInstancedCookInternal STATIC HoroEditor/tools/VfxInstancedCook.cpp)
target_compile_features(HoroVfxInstancedCookInternal PUBLIC cxx_std_20)
target_include_directories(HoroVfxInstancedCookInternal PUBLIC "${HORO_VFX_INSTANCED_COOK_HEADERS}")
target_link_libraries(HoroVfxInstancedCookInternal PUBLIC HoroEngine::RenderApi HoroEditorShaderCookHostInternal
    PRIVATE HoroAtomicShaderCookPackageInternal HoroEngine::Platform)
add_executable(HoroVfxInstancedShaderCook EXCLUDE_FROM_ALL HoroEditor/tools/VfxInstancedCookMain.cpp)
target_compile_features(HoroVfxInstancedShaderCook PRIVATE cxx_std_20)
target_link_libraries(HoroVfxInstancedShaderCook PRIVATE HoroVfxInstancedCookInternal)
