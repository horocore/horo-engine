# Included from apps/CMakeLists.txt; source and binary directories remain scoped to apps/.
# One non-installed host token boundary; headless tests use the same parser as the real graphical root.
set(HORO_NETWORK_CONSOLE_HEADERS "${CMAKE_CURRENT_BINARY_DIR}/network_console_headers")
file(MAKE_DIRECTORY "${HORO_NETWORK_CONSOLE_HEADERS}")
configure_file(shared/console/NetworkConsoleInspection.h "${HORO_NETWORK_CONSOLE_HEADERS}/NetworkConsoleInspection.h" COPYONLY)
configure_file(shared/console/NetworkConsoleOverlay.h "${HORO_NETWORK_CONSOLE_HEADERS}/NetworkConsoleOverlay.h" COPYONLY)
add_library(HoroNetworkConsoleInspectionInternal STATIC EXCLUDE_FROM_ALL shared/console/NetworkConsoleInspection.cpp
    shared/console/NetworkConsoleOverlay.cpp)
target_compile_features(HoroNetworkConsoleInspectionInternal PUBLIC cxx_std_20)
target_include_directories(HoroNetworkConsoleInspectionInternal PUBLIC "${HORO_NETWORK_CONSOLE_HEADERS}")
target_link_libraries(HoroNetworkConsoleInspectionInternal PUBLIC HoroEngine::RuntimeConsole HoroEngine::NetworkDebuggerApplication)

set(HORO_EDITOR_CONSOLE_LAUNCH_HEADERS "${CMAKE_CURRENT_BINARY_DIR}/editor_console_launch_headers")
file(MAKE_DIRECTORY "${HORO_EDITOR_CONSOLE_LAUNCH_HEADERS}")
configure_file(HoroEditor/composition/console/EditorConsoleLaunch.h
    "${HORO_EDITOR_CONSOLE_LAUNCH_HEADERS}/EditorConsoleLaunch.h" COPYONLY)
configure_file(HoroEditor/composition/console/EditorConsoleExecution.h
    "${HORO_EDITOR_CONSOLE_LAUNCH_HEADERS}/EditorConsoleExecution.h" COPYONLY)
add_library(HoroEditorConsoleLaunchInternal STATIC EXCLUDE_FROM_ALL HoroEditor/composition/console/EditorConsoleLaunch.cpp
    HoroEditor/composition/console/EditorConsoleExecution.cpp)
target_compile_features(HoroEditorConsoleLaunchInternal PUBLIC cxx_std_20)
target_include_directories(HoroEditorConsoleLaunchInternal PUBLIC "${HORO_EDITOR_CONSOLE_LAUNCH_HEADERS}")
target_link_libraries(HoroEditorConsoleLaunchInternal PUBLIC HoroEngine::RuntimeConsoleStartup)
target_link_libraries(HoroEditorConsoleLaunchInternal PRIVATE HoroRuntimeConsoleTelemetryInternal HoroRuntimeConsoleSceneInternal HoroNetworkConsoleInspectionInternal)

if(HORO_BUILD_XR_OPENXR)
    # First-party installed-component admission belongs to the application root,
    # not XRRuntime, ExtensionHost or a concrete renderer. Only this staged
    # non-installed contract is visible to explicitly composed host consumers.
    set(HORO_OPENXR_PRODUCT_HOST_HEADERS "${CMAKE_CURRENT_BINARY_DIR}/openxr_product_host_headers")
    file(MAKE_DIRECTORY "${HORO_OPENXR_PRODUCT_HOST_HEADERS}")
    configure_file(HoroEditor/composition/xr/OpenXRComponentManifest.h
        "${HORO_OPENXR_PRODUCT_HOST_HEADERS}/OpenXRComponentManifest.h" COPYONLY)
    configure_file(HoroEditor/composition/xr/OpenXRProductDeployment.h
        "${HORO_OPENXR_PRODUCT_HOST_HEADERS}/OpenXRProductDeployment.h" COPYONLY)
    configure_file(HoroEditor/composition/xr/OpenXRLoaderLease.h
        "${HORO_OPENXR_PRODUCT_HOST_HEADERS}/OpenXRLoaderLease.h" COPYONLY)
    configure_file(HoroEditor/composition/xr/OpenXRInstalledProductSelection.h
        "${HORO_OPENXR_PRODUCT_HOST_HEADERS}/OpenXRInstalledProductSelection.h" COPYONLY)
    configure_file(HoroEditor/composition/xr/OpenXRInstalledHostCatalog.h
        "${HORO_OPENXR_PRODUCT_HOST_HEADERS}/OpenXRInstalledHostCatalog.h" COPYONLY)
    add_library(HoroOpenXRProductHost STATIC
        HoroEditor/composition/xr/OpenXRComponentManifest.cpp
        HoroEditor/composition/xr/OpenXRComponentVerification.cpp
        HoroEditor/composition/xr/OpenXRProductDeployment.cpp
        HoroEditor/composition/xr/OpenXRLoaderLease.cpp
        HoroEditor/composition/xr/OpenXRInstalledProductSelection.cpp
        HoroEditor/composition/xr/OpenXRInstalledHostCatalog.cpp)
    target_compile_features(HoroOpenXRProductHost PUBLIC cxx_std_20)
    target_include_directories(HoroOpenXRProductHost PUBLIC "${HORO_OPENXR_PRODUCT_HOST_HEADERS}")
    target_link_libraries(HoroOpenXRProductHost
        PUBLIC HoroEngine::PackageSecurity HoroEngine::XRApi HoroEngine::XRRuntime HoroEngine::Platform HoroPackageProjectHost
        PRIVATE HoroFoundationJsonInternal HoroPackageContentStagingInternal HoroHostSignatureDocuments)
endif()

if(HORO_BUILD_XR_OPENXR)
    # OS/native-time conversion depends on the selected XR owner and scheduler, not a renderer backend.
    # The application and its lifecycle tests link the same implementation; no copied test TU is compiled.
    set(HORO_OPENXR_CLOCK_HEADERS "${CMAKE_CURRENT_BINARY_DIR}/openxr_clock_headers")
    file(MAKE_DIRECTORY "${HORO_OPENXR_CLOCK_HEADERS}")
    configure_file(HoroEditor/composition/xr/OpenXRInputClock.h "${HORO_OPENXR_CLOCK_HEADERS}/OpenXRInputClock.h" COPYONLY)
    add_library(HoroOpenXRApplicationClock STATIC HoroEditor/composition/xr/OpenXRInputClock.cpp)
    target_compile_features(HoroOpenXRApplicationClock PUBLIC cxx_std_20)
    target_include_directories(HoroOpenXRApplicationClock PUBLIC "${HORO_OPENXR_CLOCK_HEADERS}")
    target_link_libraries(HoroOpenXRApplicationClock PUBLIC HoroXROpenXRHostInterface HoroEngine::Runtime
        PRIVATE HoroEngine::XROpenXR)

    # Application action composition is independently testable without selecting a concrete renderer.
    set(HORO_OPENXR_BINDING_HEADERS "${CMAKE_CURRENT_BINARY_DIR}/openxr_binding_headers")
    file(MAKE_DIRECTORY "${HORO_OPENXR_BINDING_HEADERS}")
    configure_file(HoroEditor/composition/xr/OpenXRApplicationBindings.h "${HORO_OPENXR_BINDING_HEADERS}/OpenXRApplicationBindings.h" COPYONLY)
    configure_file(HoroEditor/composition/xr/OpenXRInstalledBindings.h "${HORO_OPENXR_BINDING_HEADERS}/OpenXRInstalledBindings.h" COPYONLY)
    add_library(HoroOpenXRApplicationBindings STATIC HoroEditor/composition/xr/OpenXRApplicationBindings.cpp HoroEditor/composition/xr/OpenXRInstalledBindings.cpp)
    target_compile_features(HoroOpenXRApplicationBindings PUBLIC cxx_std_20)
    target_include_directories(HoroOpenXRApplicationBindings PUBLIC "${HORO_OPENXR_BINDING_HEADERS}")
    target_link_libraries(HoroOpenXRApplicationBindings PUBLIC HoroXROpenXRHostInterface HoroOpenXRProductHost
        PRIVATE HoroEngine::XROpenXR HoroFoundationJsonInternal)
endif()

if(HORO_BUILD_XR_OPENXR AND HORO_BUILD_RENDER_VULKAN AND TARGET HoroEditorVulkanXrHostInterface)
    set(HORO_OPENXR_APPLICATION_HEADERS "${CMAKE_CURRENT_BINARY_DIR}/openxr_application_headers")
    file(MAKE_DIRECTORY "${HORO_OPENXR_APPLICATION_HEADERS}")
    # Ownership migration: an older in-place build must not shadow the clock target's staged header.
    file(REMOVE "${HORO_OPENXR_APPLICATION_HEADERS}/OpenXRInputClock.h")
    configure_file(HoroEditor/composition/xr/OpenXRApplicationSession.h
        "${HORO_OPENXR_APPLICATION_HEADERS}/OpenXRApplicationSession.h" COPYONLY)
    configure_file(HoroEditor/composition/xr/OpenXRPlayHost.h "${HORO_OPENXR_APPLICATION_HEADERS}/OpenXRPlayHost.h" COPYONLY)
    add_library(HoroOpenXRApplicationHost STATIC HoroEditor/composition/xr/OpenXRApplicationSession.cpp
        HoroEditor/composition/xr/OpenXRApplicationInput.cpp HoroEditor/composition/xr/OpenXRApplicationProjection.cpp
        HoroEditor/composition/xr/OpenXRPlayHost.cpp HoroEditor/composition/xr/OpenXRPlayFrames.cpp HoroEditor/composition/xr/OpenXRPlayScene.cpp)
    target_compile_features(HoroOpenXRApplicationHost PUBLIC cxx_std_20)
    target_include_directories(HoroOpenXRApplicationHost PUBLIC "${HORO_OPENXR_APPLICATION_HEADERS}")
    target_link_libraries(HoroOpenXRApplicationHost
        PUBLIC HoroOpenXRApplicationClock HoroEditorPlayXrHostInterface HoroEditorXrSceneInternal HoroXRRuntimeHostInterface HoroEngine::RuntimeScene HoroOpenXRProductHost HoroOpenXRApplicationBindings HoroXROpenXRHostInterface HoroXROpenXRVulkanHostInterface HoroRenderVulkanHostInterface HoroEngine::XRRuntime HoroEngine::Runtime
        PRIVATE HoroEngine::XROpenXR HoroXROpenXRVulkan HoroEngine::RenderFrontend HoroEditorVulkanXrHostInterface)
endif()

# Pure App-owned package admission is shared by runtime loading and offline production.
set(HORO_UI_PAINT_PACKAGE_HEADERS "${CMAKE_CURRENT_BINARY_DIR}/ui_paint_package_headers")
file(MAKE_DIRECTORY "${HORO_UI_PAINT_PACKAGE_HEADERS}")
configure_file(HoroEditor/composition/ui/RuntimeUiPaintShaderPackage.h "${HORO_UI_PAINT_PACKAGE_HEADERS}/RuntimeUiPaintShaderPackage.h" COPYONLY)
add_library(HoroUiPaintShaderPackage STATIC HoroEditor/composition/ui/RuntimeUiPaintShaderPackage.cpp)
target_compile_features(HoroUiPaintShaderPackage PUBLIC cxx_std_20)
target_include_directories(HoroUiPaintShaderPackage PUBLIC "${HORO_UI_PAINT_PACKAGE_HEADERS}")
target_link_libraries(HoroUiPaintShaderPackage PUBLIC HoroEngine::RenderApi)

# Signed tool approvals remain an explicit offline host composition, never a runtime dependency.
set(HORO_EDITOR_SHADER_COOK_HOST_HEADERS "${CMAKE_CURRENT_BINARY_DIR}/editor_shader_cook_host_headers")
file(MAKE_DIRECTORY "${HORO_EDITOR_SHADER_COOK_HOST_HEADERS}")
configure_file(HoroEditor/tools/shader_cook/VerifiedShaderHostCatalog.h "${HORO_EDITOR_SHADER_COOK_HOST_HEADERS}/VerifiedShaderHostCatalog.h" COPYONLY)
configure_file(HoroEditor/tools/shader_cook/AuthenticatedMetalCookConfiguration.h
    "${HORO_EDITOR_SHADER_COOK_HOST_HEADERS}/AuthenticatedMetalCookConfiguration.h" COPYONLY)
configure_file(HoroEditor/tools/shader_cook/AuthenticatedShaderCookConfiguration.h
    "${HORO_EDITOR_SHADER_COOK_HOST_HEADERS}/AuthenticatedShaderCookConfiguration.h" COPYONLY)
add_library(HoroEditorShaderCookHostInternal STATIC
    HoroEditor/tools/shader_cook/VerifiedShaderHostCatalog.cpp
    HoroEditor/tools/shader_cook/AuthenticatedShaderCookConfiguration.cpp
    HoroEditor/tools/shader_cook/AuthenticatedMetalCookConfiguration.cpp)
target_compile_features(HoroEditorShaderCookHostInternal PUBLIC cxx_std_20)
target_include_directories(HoroEditorShaderCookHostInternal PUBLIC "${HORO_EDITOR_SHADER_COOK_HOST_HEADERS}")
target_link_libraries(HoroEditorShaderCookHostInternal PUBLIC HoroEngine::ShaderCompilerToolchain HoroEngine::Security
    PRIVATE HoroFoundationJsonInternal HoroHostSignatureDocuments)

set(HORO_ATOMIC_SHADER_COOK_HEADERS "${CMAKE_CURRENT_BINARY_DIR}/atomic_shader_cook_headers")
file(MAKE_DIRECTORY "${HORO_ATOMIC_SHADER_COOK_HEADERS}")
configure_file(HoroEditor/tools/shader_cook/AtomicShaderCookPackage.h
    "${HORO_ATOMIC_SHADER_COOK_HEADERS}/AtomicShaderCookPackage.h" COPYONLY)
add_library(HoroAtomicShaderCookPackageInternal STATIC HoroEditor/tools/shader_cook/AtomicShaderCookPackage.cpp)
target_compile_features(HoroAtomicShaderCookPackageInternal PUBLIC cxx_std_20)
target_include_directories(HoroAtomicShaderCookPackageInternal PUBLIC "${HORO_ATOMIC_SHADER_COOK_HEADERS}")
target_link_libraries(HoroAtomicShaderCookPackageInternal PUBLIC HoroEditorShaderCookHostInternal PRIVATE HoroFoundationJsonInternal)

# Fixed application UI lifecycle composition; stage only its non-installed owning contracts.
set(HORO_RUNTIME_UI_PAINT_HOST_HEADERS "${CMAKE_CURRENT_BINARY_DIR}/runtime_ui_paint_host_headers")
file(MAKE_DIRECTORY "${HORO_RUNTIME_UI_PAINT_HOST_HEADERS}")
configure_file(HoroEditor/composition/ui/RuntimeUiPaintHost.h "${HORO_RUNTIME_UI_PAINT_HOST_HEADERS}/RuntimeUiPaintHost.h" COPYONLY)
configure_file(HoroEditor/composition/ui/RuntimeUiPaintProjectSources.h "${HORO_RUNTIME_UI_PAINT_HOST_HEADERS}/RuntimeUiPaintProjectSources.h" COPYONLY)
configure_file(HoroEditor/composition/ui/RuntimeUiPaintPackagedKernel.h "${HORO_RUNTIME_UI_PAINT_HOST_HEADERS}/RuntimeUiPaintPackagedKernel.h" COPYONLY)
configure_file(HoroEditor/composition/ui/RuntimeUiPaintProjection.h "${HORO_RUNTIME_UI_PAINT_HOST_HEADERS}/RuntimeUiPaintProjection.h" COPYONLY)
add_library(HoroRuntimeUiPaintHost STATIC
    HoroEditor/composition/ui/RuntimeUiPaintHost.cpp
    HoroEditor/composition/ui/RuntimeUiPaintHostFrames.cpp
    HoroEditor/composition/ui/RuntimeUiPaintHostSource.cpp
    HoroEditor/composition/ui/RuntimeUiPaintHostOutput.cpp
    HoroEditor/composition/ui/RuntimeUiPaintHostPresentation.cpp
    HoroEditor/composition/ui/RuntimeUiPaintSceneTransaction.cpp
    HoroEditor/composition/ui/RuntimeUiPaintProjectSources.cpp
    HoroEditor/composition/ui/RuntimeUiPaintProjectKernel.cpp
    HoroEditor/composition/ui/RuntimeUiPaintPackagedKernel.cpp
    HoroEditor/composition/ui/RuntimeUiPaintProjection.cpp
    HoroEditor/composition/ui/RuntimeUiPaintProjectionFrames.cpp)
target_compile_features(HoroRuntimeUiPaintHost PUBLIC cxx_std_20)
target_include_directories(HoroRuntimeUiPaintHost PUBLIC "${HORO_RUNTIME_UI_PAINT_HOST_HEADERS}")
target_link_libraries(HoroRuntimeUiPaintHost PUBLIC HoroEngine::UiAnimationRuntimeIntegration HoroEngine::RenderFrontend
    HoroEngine::RuntimeScene HoroEngine::ShaderBuild PRIVATE HoroUiPaintShaderPackage)

set(HORO_ANIMATION_SCENE_HOST_HEADERS "${CMAKE_CURRENT_BINARY_DIR}/animation_scene_host_headers")
file(MAKE_DIRECTORY "${HORO_ANIMATION_SCENE_HOST_HEADERS}")
file(GENERATE OUTPUT "${HORO_ANIMATION_SCENE_HOST_HEADERS}/SceneAnimationHostSelection.h"
    CONTENT "#pragma once\n#include \"${CMAKE_CURRENT_SOURCE_DIR}/HoroEditor/composition/scene/SceneAnimationHostSelection.h\"\n")
add_library(HoroAnimationSceneHostInternal INTERFACE)
target_compile_features(HoroAnimationSceneHostInternal INTERFACE cxx_std_20)
target_include_directories(HoroAnimationSceneHostInternal INTERFACE "${HORO_ANIMATION_SCENE_HOST_HEADERS}")
target_link_libraries(HoroAnimationSceneHostInternal INTERFACE HoroAnimationHostIdentityInternal HoroEngine::AnimationSceneIntegration)

# Non-installed Play save composition. Forward to one canonical header identity
# so app and editor consumers cannot duplicate pragma-once declarations.
set(HORO_EDITOR_PLAY_SAVE_HEADERS "${CMAKE_CURRENT_BINARY_DIR}/editor_play_save_headers")
file(MAKE_DIRECTORY "${HORO_EDITOR_PLAY_SAVE_HEADERS}")
foreach(header EditorPlaySaveHostCapture.h EditorPlaySaveHostArchive.h
        EditorPlaySaveHostStorage.h EditorPlaySaveHostRestore.h EditorPlaySaveHost.h)
    file(GENERATE OUTPUT "${HORO_EDITOR_PLAY_SAVE_HEADERS}/${header}"
        CONTENT "#pragma once\n#include \"${CMAKE_CURRENT_SOURCE_DIR}/HoroEditor/composition/play/save/${header}\"\n")
endforeach()
add_library(HoroEditorPlaySaveHostInternal STATIC
    HoroEditor/composition/play/save/EditorPlaySaveHostCapture.cpp
    HoroEditor/composition/play/save/EditorPlaySaveHostCodec.cpp
    HoroEditor/composition/play/save/EditorPlaySaveHostArchive.cpp
    HoroEditor/composition/play/save/EditorPlaySaveHostStorage.cpp
    HoroEditor/composition/play/save/EditorPlaySaveHostRestore.cpp
    HoroEditor/composition/play/save/EditorPlaySaveHostHandoff.cpp
    HoroEditor/composition/play/save/EditorPlaySaveHost.cpp
    HoroEditor/composition/play/save/EditorPlaySaveHostRuntime.cpp
    HoroEditor/composition/play/save/EditorPlaySaveHostBackground.cpp
    HoroEditor/composition/play/save/EditorPlaySaveHostBindings.cpp
    HoroEditor/composition/play/save/EditorPlaySaveHostProjection.cpp
    HoroEditor/composition/play/save/EditorPlaySaveHostEnvironment.cpp
    HoroEditor/composition/play/save/EditorPlaySaveHostPlatform.cpp
    HoroEditor/composition/play/save/EditorPlaySaveHostInternal.h)
target_compile_features(HoroEditorPlaySaveHostInternal PUBLIC cxx_std_20)
target_sources(HoroEditorPlaySaveHostInternal PRIVATE
    HoroEditor/composition/play/save/EditorPlaySavePrefabInventory.cpp
    HoroEditor/composition/play/save/EditorPlaySavePrefabServices.cpp
    HoroEditor/composition/play/save/EditorPlaySavePrefabCapture.cpp
)
foreach(header IN ITEMS EditorPlaySaveIdentityIssuing.h EditorPlaySavePrefabInventory.h
        EditorPlaySavePrefabServices.h EditorPlaySavePrefabCapture.h)
    configure_file("HoroEditor/composition/play/save/${header}" "${HORO_EDITOR_PLAY_SAVE_HEADERS}/${header}" COPYONLY)
endforeach()
target_link_libraries(HoroEditorPlaySaveHostInternal PUBLIC HoroEngine::PrefabRuntime HoroEngine::PrefabPersistence)

target_include_directories(HoroEditorPlaySaveHostInternal PUBLIC "${HORO_EDITOR_PLAY_SAVE_HEADERS}")
target_link_libraries(HoroEditorPlaySaveHostInternal PUBLIC
    HoroEditorPlaySceneHostInternal HoroEngine::Runtime HoroEngine::RuntimeScene
    HoroEngine::SceneCook HoroEngine::GameplayRuntime HoroEngine::GameplayModuleHost HoroEngine::Security)
target_link_libraries(HoroEditorPlaySaveHostInternal PRIVATE HoroEngine::Application HoroEngine::Platform)

set(HORO_EDITOR_PROJECT_SCENE_HEADERS "${CMAKE_CURRENT_BINARY_DIR}/editor_project_scene_headers")
file(MAKE_DIRECTORY "${HORO_EDITOR_PROJECT_SCENE_HEADERS}")
configure_file(HoroEditor/composition/scene/EditorProjectSceneHost.h "${HORO_EDITOR_PROJECT_SCENE_HEADERS}/EditorProjectSceneHost.h" COPYONLY)
add_library(HoroEditorSceneProductHost STATIC HoroEditor/composition/scene/EditorProjectSceneHost.cpp)
target_link_libraries(HoroEditorSceneProductHost PUBLIC HoroEditorPlaySaveHostInternal)
target_link_libraries(HoroEditorSceneProductHost PUBLIC HoroAnimationSceneHostInternal)
target_link_libraries(HoroEditorSceneProductHost PRIVATE HoroEngine::PrefabProjectSettings HoroEngine::ProjectSurfaceSettings)
target_compile_features(HoroEditorSceneProductHost PUBLIC cxx_std_20)
target_include_directories(HoroEditorSceneProductHost PUBLIC "${HORO_EDITOR_PROJECT_SCENE_HEADERS}")
target_link_libraries(HoroEditorSceneProductHost PUBLIC HoroEditorPlayXrHostInterface HoroEditorPlaySceneHostInternal HoroScenePhysicsHost
        HoroEngine::Application HoroEngine::ProjectCollisionSettings HoroEngine::AISceneIntegration
        HoroEngine::PrefabRuntime HoroEngine::SceneRuntimeConversion HoroRuntimeUiPaintHost)

# Non-installed executable adapter shared with qualification; portable producer remains Editor/Platform-free.
add_library(HoroEditorUiStyleHostInternal STATIC HoroEditor/composition/ui_style/EditorUiStylePreparation.cpp)
target_compile_features(HoroEditorUiStyleHostInternal PUBLIC cxx_std_20)
target_include_directories(HoroEditorUiStyleHostInternal PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/HoroEditor/composition/ui_style")
target_link_libraries(HoroEditorUiStyleHostInternal PUBLIC HoroEngine::EditorServices HoroEngine::UiStyleWorkspaceApplication
        HoroEngine::PlatformImmutableFileStorage HoroEngine::Platform HoroEngine::ProjectMutationGuard)

add_library(HoroEditorTerrainHostInternal STATIC HoroEditor/composition/terrain/EditorTerrainPreparation.cpp)
target_compile_features(HoroEditorTerrainHostInternal PUBLIC cxx_std_20)
target_include_directories(HoroEditorTerrainHostInternal PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/HoroEditor/composition/terrain")
target_link_libraries(HoroEditorTerrainHostInternal PUBLIC HoroEngine::TerrainAuthoringApplication
        HoroEngine::PlatformImmutableFileStorage HoroEngine::Platform)

# Explicit development-only shader source capture; no renderer selection authority.
add_library(HoroShaderHotReloadHostInternal STATIC
    HoroEditor/composition/render/ShaderHotReloadSource.cpp
    HoroEditor/composition/render/ShaderHotReloadSourceValidation.cpp
    HoroEditor/composition/render/ShaderHotReloadHost.cpp
    HoroEditor/composition/render/EditorViewportHotReloadConfiguration.cpp)
target_compile_features(HoroShaderHotReloadHostInternal PUBLIC cxx_std_20)
target_include_directories(HoroShaderHotReloadHostInternal PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/HoroEditor/composition/render")
target_link_libraries(HoroShaderHotReloadHostInternal PUBLIC HoroEngine::ShaderHotReload HoroEngine::Assets
    PRIVATE HoroEditorViewportShaderRecipeInternal HoroEditorVulkanShaderInternal)

target_sources(HoroEditorPlaySaveHostInternal PRIVATE
    HoroEditor/composition/play/save/EditorPlaySaveHostPhysicsPreparation.cpp
    HoroEditor/composition/play/save/EditorPlaySaveHostPhysicsRestore.cpp
    HoroEditor/composition/play/save/EditorPlaySavePhysicsArchive.cpp
    HoroEditor/composition/play/save/EditorPlaySavePhysicsCapture.cpp
    HoroEditor/composition/play/save/EditorPlaySavePhysicsContent.cpp
    HoroEditor/composition/play/save/EditorPlaySavePhysicsFence.cpp
    HoroEditor/composition/play/save/EditorPlaySavePhysicsReceipt.cpp
    HoroEditor/composition/play/save/EditorPlaySavePhysicsRestore.cpp
    HoroEditor/composition/play/save/EditorPlaySavePhysicsServices.cpp)
target_link_libraries(HoroEditorPlaySaveHostInternal PRIVATE HoroScenePhysicsHost)
foreach(_horo_save_state_header IN ITEMS EditorPlaySavePhysicsArchive.h EditorPlaySavePhysicsContent.h
        EditorPlaySavePhysicsReceipt.h EditorPlaySavePhysicsServices.h)
    file(GENERATE OUTPUT "${HORO_EDITOR_PLAY_SAVE_HEADERS}/${_horo_save_state_header}"
        CONTENT "#pragma once\n#include \"${CMAKE_CURRENT_SOURCE_DIR}/HoroEditor/composition/play/save/${_horo_save_state_header}\"\n")
endforeach()

target_link_libraries(HoroEditorSceneProductHost PRIVATE HoroPhysicsStateProcessBindingInternal)
