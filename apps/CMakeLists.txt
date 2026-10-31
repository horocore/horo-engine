# Explicit non-installed package composition contract; stage only its owning host header.
set(HORO_PACKAGE_HOST_HEADERS "${CMAKE_CURRENT_BINARY_DIR}/package_host_headers")
file(MAKE_DIRECTORY "${HORO_PACKAGE_HOST_HEADERS}")
configure_file(common/PackageProjectSession.h "${HORO_PACKAGE_HOST_HEADERS}/PackageProjectSession.h" COPYONLY)
configure_file(common/PackageProjectHost.h "${HORO_PACKAGE_HOST_HEADERS}/PackageProjectHost.h" COPYONLY)
configure_file(common/PackageHostPolicy.h "${HORO_PACKAGE_HOST_HEADERS}/PackageHostPolicy.h" COPYONLY)
add_library(HoroPackageProjectHost STATIC common/PackageProjectSession.cpp common/PackageProjectPreparation.cpp
    common/PackageProjectHost.cpp common/PackageProjectMetadata.cpp common/PackageHostPolicy.cpp)
target_compile_features(HoroPackageProjectHost PUBLIC cxx_std_20)
target_include_directories(HoroPackageProjectHost PUBLIC "${HORO_PACKAGE_HOST_HEADERS}")
target_link_libraries(HoroPackageProjectHost PUBLIC HoroEngine::PackageApplication)

# One application-owned signature document codec, shared by the package CLI and
# explicit installed-host bootstrap. The staged interface is never installed.
set(HORO_HOST_SIGNATURE_HEADERS "${CMAKE_CURRENT_BINARY_DIR}/host_signature_headers")
file(MAKE_DIRECTORY "${HORO_HOST_SIGNATURE_HEADERS}")
configure_file(common/host_security/HostSignatureDocument.h "${HORO_HOST_SIGNATURE_HEADERS}/HostSignatureDocument.h" COPYONLY)
add_library(HoroHostSignatureDocuments STATIC common/host_security/HostSignatureDocument.cpp)
target_compile_features(HoroHostSignatureDocuments PUBLIC cxx_std_20)
target_include_directories(HoroHostSignatureDocuments PUBLIC "${HORO_HOST_SIGNATURE_HEADERS}")
target_link_libraries(HoroHostSignatureDocuments PUBLIC HoroEngine::PackageSecurity PRIVATE HoroFoundationJsonInternal)

if(HORO_BUILD_XR_OPENXR)
    # First-party installed-component admission belongs to the application root,
    # not XRRuntime, ExtensionHost or a concrete renderer. Only this staged
    # non-installed contract is visible to explicitly composed host consumers.
    set(HORO_OPENXR_PRODUCT_HOST_HEADERS "${CMAKE_CURRENT_BINARY_DIR}/openxr_product_host_headers")
    file(MAKE_DIRECTORY "${HORO_OPENXR_PRODUCT_HOST_HEADERS}")
    configure_file(common/xr/OpenXRComponentManifest.h
        "${HORO_OPENXR_PRODUCT_HOST_HEADERS}/OpenXRComponentManifest.h" COPYONLY)
    configure_file(common/xr/OpenXRProductDeployment.h
        "${HORO_OPENXR_PRODUCT_HOST_HEADERS}/OpenXRProductDeployment.h" COPYONLY)
    configure_file(common/xr/OpenXRLoaderLease.h
        "${HORO_OPENXR_PRODUCT_HOST_HEADERS}/OpenXRLoaderLease.h" COPYONLY)
    configure_file(common/xr/OpenXRInstalledProductSelection.h
        "${HORO_OPENXR_PRODUCT_HOST_HEADERS}/OpenXRInstalledProductSelection.h" COPYONLY)
    configure_file(common/xr/OpenXRInstalledHostCatalog.h
        "${HORO_OPENXR_PRODUCT_HOST_HEADERS}/OpenXRInstalledHostCatalog.h" COPYONLY)
    add_library(HoroOpenXRProductHost STATIC
        common/xr/OpenXRComponentManifest.cpp
        common/xr/OpenXRComponentVerification.cpp
        common/xr/OpenXRProductDeployment.cpp
        common/xr/OpenXRLoaderLease.cpp
        common/xr/OpenXRInstalledProductSelection.cpp
        common/xr/OpenXRInstalledHostCatalog.cpp)
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
    configure_file(common/xr/OpenXRInputClock.h "${HORO_OPENXR_CLOCK_HEADERS}/OpenXRInputClock.h" COPYONLY)
    add_library(HoroOpenXRApplicationClock STATIC common/xr/OpenXRInputClock.cpp)
    target_compile_features(HoroOpenXRApplicationClock PUBLIC cxx_std_20)
    target_include_directories(HoroOpenXRApplicationClock PUBLIC "${HORO_OPENXR_CLOCK_HEADERS}")
    target_link_libraries(HoroOpenXRApplicationClock PUBLIC HoroXROpenXRHostInterface HoroEngine::Runtime
        PRIVATE HoroEngine::XROpenXR)

    # Application action composition is independently testable without selecting a concrete renderer.
    set(HORO_OPENXR_BINDING_HEADERS "${CMAKE_CURRENT_BINARY_DIR}/openxr_binding_headers")
    file(MAKE_DIRECTORY "${HORO_OPENXR_BINDING_HEADERS}")
    configure_file(common/xr/OpenXRApplicationBindings.h "${HORO_OPENXR_BINDING_HEADERS}/OpenXRApplicationBindings.h" COPYONLY)
    configure_file(common/xr/OpenXRInstalledBindings.h "${HORO_OPENXR_BINDING_HEADERS}/OpenXRInstalledBindings.h" COPYONLY)
    add_library(HoroOpenXRApplicationBindings STATIC common/xr/OpenXRApplicationBindings.cpp common/xr/OpenXRInstalledBindings.cpp)
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
    configure_file(common/xr/OpenXRApplicationSession.h
        "${HORO_OPENXR_APPLICATION_HEADERS}/OpenXRApplicationSession.h" COPYONLY)
    configure_file(common/xr/OpenXRPlayHost.h "${HORO_OPENXR_APPLICATION_HEADERS}/OpenXRPlayHost.h" COPYONLY)
    add_library(HoroOpenXRApplicationHost STATIC common/xr/OpenXRApplicationSession.cpp
        common/xr/OpenXRApplicationInput.cpp common/xr/OpenXRApplicationProjection.cpp
        common/xr/OpenXRPlayHost.cpp common/xr/OpenXRPlayFrames.cpp common/xr/OpenXRPlayScene.cpp)
    target_compile_features(HoroOpenXRApplicationHost PUBLIC cxx_std_20)
    target_include_directories(HoroOpenXRApplicationHost PUBLIC "${HORO_OPENXR_APPLICATION_HEADERS}")
    target_link_libraries(HoroOpenXRApplicationHost
        PUBLIC HoroOpenXRApplicationClock HoroEditorPlayXrHostInterface HoroEditorXrSceneInternal HoroXRRuntimeHostInterface HoroEngine::RuntimeScene HoroOpenXRProductHost HoroOpenXRApplicationBindings HoroXROpenXRHostInterface HoroXROpenXRVulkanHostInterface HoroRenderVulkanHostInterface HoroEngine::XRRuntime HoroEngine::Runtime
        PRIVATE HoroEngine::XROpenXR HoroXROpenXRVulkan HoroEngine::RenderFrontend HoroEditorVulkanXrHostInterface)
endif()

add_library(HoroHostModuleComposition STATIC
    common/HostModuleComposition.cpp
)
target_compile_features(HoroHostModuleComposition PUBLIC cxx_std_20)
target_include_directories(HoroHostModuleComposition PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/common
)
target_link_libraries(HoroHostModuleComposition PUBLIC HoroEngine::Foundation HoroEngine::PlatformServices)

# Pure App-owned package admission is shared by runtime loading and offline production.
set(HORO_UI_PAINT_PACKAGE_HEADERS "${CMAKE_CURRENT_BINARY_DIR}/ui_paint_package_headers")
file(MAKE_DIRECTORY "${HORO_UI_PAINT_PACKAGE_HEADERS}")
configure_file(common/ui/RuntimeUiPaintShaderPackage.h "${HORO_UI_PAINT_PACKAGE_HEADERS}/RuntimeUiPaintShaderPackage.h" COPYONLY)
add_library(HoroUiPaintShaderPackage STATIC common/ui/RuntimeUiPaintShaderPackage.cpp)
target_compile_features(HoroUiPaintShaderPackage PUBLIC cxx_std_20)
target_include_directories(HoroUiPaintShaderPackage PUBLIC "${HORO_UI_PAINT_PACKAGE_HEADERS}")
target_link_libraries(HoroUiPaintShaderPackage PUBLIC HoroEngine::RenderApi)

# Signed tool approvals remain an explicit offline host composition, never a runtime dependency.
set(HORO_EDITOR_SHADER_COOK_HOST_HEADERS "${CMAKE_CURRENT_BINARY_DIR}/editor_shader_cook_host_headers")
file(MAKE_DIRECTORY "${HORO_EDITOR_SHADER_COOK_HOST_HEADERS}")
configure_file(common/shader_cook/VerifiedShaderHostCatalog.h "${HORO_EDITOR_SHADER_COOK_HOST_HEADERS}/VerifiedShaderHostCatalog.h" COPYONLY)
add_library(HoroEditorShaderCookHostInternal STATIC common/shader_cook/VerifiedShaderHostCatalog.cpp)
target_compile_features(HoroEditorShaderCookHostInternal PUBLIC cxx_std_20)
target_include_directories(HoroEditorShaderCookHostInternal PUBLIC "${HORO_EDITOR_SHADER_COOK_HOST_HEADERS}")
target_link_libraries(HoroEditorShaderCookHostInternal PUBLIC HoroEngine::ShaderCompilerToolchain HoroEngine::Security
    PRIVATE HoroFoundationJsonInternal)
add_executable(HoroEditorUiPaintShaderCook EXCLUDE_FROM_ALL HoroEditor/tools/UiPaintCook.cpp)
target_compile_features(HoroEditorUiPaintShaderCook PRIVATE cxx_std_20)
target_link_libraries(HoroEditorUiPaintShaderCook PRIVATE HoroEditorShaderCookHostInternal HoroHostSignatureDocuments
    HoroUiPaintShaderPackage HoroEngine::Platform)

# Fixed application UI lifecycle composition; stage only its non-installed owning contracts.
set(HORO_RUNTIME_UI_PAINT_HOST_HEADERS "${CMAKE_CURRENT_BINARY_DIR}/runtime_ui_paint_host_headers")
file(MAKE_DIRECTORY "${HORO_RUNTIME_UI_PAINT_HOST_HEADERS}")
configure_file(common/ui/RuntimeUiPaintHost.h "${HORO_RUNTIME_UI_PAINT_HOST_HEADERS}/RuntimeUiPaintHost.h" COPYONLY)
configure_file(common/ui/RuntimeUiPaintProjectSources.h "${HORO_RUNTIME_UI_PAINT_HOST_HEADERS}/RuntimeUiPaintProjectSources.h" COPYONLY)
configure_file(common/ui/RuntimeUiPaintPackagedKernel.h "${HORO_RUNTIME_UI_PAINT_HOST_HEADERS}/RuntimeUiPaintPackagedKernel.h" COPYONLY)
configure_file(common/ui/RuntimeUiPaintProjection.h "${HORO_RUNTIME_UI_PAINT_HOST_HEADERS}/RuntimeUiPaintProjection.h" COPYONLY)
add_library(HoroRuntimeUiPaintHost STATIC
    common/ui/RuntimeUiPaintHost.cpp
    common/ui/RuntimeUiPaintHostFrames.cpp
    common/ui/RuntimeUiPaintHostSource.cpp
    common/ui/RuntimeUiPaintHostOutput.cpp
    common/ui/RuntimeUiPaintHostPresentation.cpp
    common/ui/RuntimeUiPaintSceneTransaction.cpp
    common/ui/RuntimeUiPaintProjectSources.cpp
    common/ui/RuntimeUiPaintProjectKernel.cpp
    common/ui/RuntimeUiPaintPackagedKernel.cpp
    common/ui/RuntimeUiPaintProjection.cpp
    common/ui/RuntimeUiPaintProjectionFrames.cpp)
target_compile_features(HoroRuntimeUiPaintHost PUBLIC cxx_std_20)
target_include_directories(HoroRuntimeUiPaintHost PUBLIC "${HORO_RUNTIME_UI_PAINT_HOST_HEADERS}")
target_link_libraries(HoroRuntimeUiPaintHost PUBLIC HoroEngine::UiAnimationRuntimeIntegration HoroEngine::RenderFrontend
    HoroEngine::RuntimeScene HoroEngine::ShaderBuild PRIVATE HoroUiPaintShaderPackage)

# Application-only observer; Scene remains the sole native candidate owner. Only
# this header and its declared non-installed Physics boundary reach consumers.
set(HORO_SCENE_PHYSICS_HOST_HEADERS "${CMAKE_CURRENT_BINARY_DIR}/scene_physics_host_headers")
file(MAKE_DIRECTORY "${HORO_SCENE_PHYSICS_HOST_HEADERS}")
configure_file(common/ScenePhysicsHost.h "${HORO_SCENE_PHYSICS_HOST_HEADERS}/ScenePhysicsHost.h" COPYONLY)
configure_file(common/CommittedSceneAssets.h "${HORO_SCENE_PHYSICS_HOST_HEADERS}/CommittedSceneAssets.h" COPYONLY)
add_library(HoroScenePhysicsHost STATIC common/ScenePhysicsHost.cpp common/CommittedSceneAssets.cpp)
target_compile_features(HoroScenePhysicsHost PUBLIC cxx_std_20)
target_include_directories(HoroScenePhysicsHost PUBLIC "${HORO_SCENE_PHYSICS_HOST_HEADERS}")
target_link_libraries(HoroScenePhysicsHost PUBLIC HoroPhysicsSceneHostInternal HoroEngine::SceneCook HoroEngine::Assets
        HoroEngine::ProjectPhysicsCapture)

set(HORO_EDITOR_PROJECT_SCENE_HEADERS "${CMAKE_CURRENT_BINARY_DIR}/editor_project_scene_headers")
file(MAKE_DIRECTORY "${HORO_EDITOR_PROJECT_SCENE_HEADERS}")
configure_file(common/EditorProjectSceneHost.h "${HORO_EDITOR_PROJECT_SCENE_HEADERS}/EditorProjectSceneHost.h" COPYONLY)
add_library(HoroEditorSceneProductHost STATIC common/EditorProjectSceneHost.cpp)
target_compile_features(HoroEditorSceneProductHost PUBLIC cxx_std_20)
target_include_directories(HoroEditorSceneProductHost PUBLIC "${HORO_EDITOR_PROJECT_SCENE_HEADERS}")
target_link_libraries(HoroEditorSceneProductHost PUBLIC HoroEditorPlayXrHostInterface HoroEditorPlaySceneHostInternal HoroScenePhysicsHost
        HoroEngine::Application HoroEngine::ProjectCollisionSettings HoroEngine::AISceneIntegration
        HoroEngine::PrefabRuntime HoroEngine::SceneRuntimeConversion HoroRuntimeUiPaintHost)

add_library(HoroNetworkProductHost STATIC
    common/NetworkProductHost.cpp
    common/NetworkProductSession.cpp
    common/NetworkProductTransport.cpp
    common/HeadlessNetworkServices.cpp
    common/HeadlessSceneLoadBoundary.cpp
    common/HeadlessProjectScene.cpp
    common/GameplayWorldComposition.cpp
)
target_compile_features(HoroNetworkProductHost PUBLIC cxx_std_20)
target_link_libraries(HoroNetworkProductHost PRIVATE HoroNetworkModeHostInternal)
target_include_directories(HoroNetworkProductHost PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/common)
target_link_libraries(HoroNetworkProductHost PUBLIC HoroEngine::NetworkRuntime HoroEngine::RuntimeScene
    HoroEngine::Physics HoroEngine::Runtime HoroEngine::GameplayModuleHost HoroEngine::GameplayLua
    HoroEngine::GameplayPhysicsIntegration HoroEngine::GameplayCharacterIntegration HoroEngine::PrefabRuntime HoroEngine::NetworkDebuggerApplication
    HoroEngine::ProjectCollisionSettings HoroEngine::PlatformProjectReadFiles HoroEngine::SceneRuntimeConversion HoroEngine::AISceneIntegration)
target_link_libraries(HoroNetworkProductHost PUBLIC HoroScenePhysicsHost)
if(HORO_BUILD_NETWORK_GNS)
    target_sources(HoroNetworkProductHost PRIVATE common/NetworkProductGnsSelection.cpp)
    target_link_libraries(HoroNetworkProductHost PRIVATE HoroEngine::NetworkTransportGNS HoroNetworkTransportGNSFactoryInternal)
endif()

# The terminal/headless composition remains available without GUI or GPU targets.
add_executable(horo-engine
    horo-engine/main.cpp
    horo-engine/cli_host/CliHost.cpp
    horo-engine/cli_host/CliHostErrorTranslation.cpp
    horo-engine/cli_host/CliHostInterrupts.cpp
    horo-engine/cli_host/PackageCliHost.cpp
    horo-engine/cli_host/NavigationCliHost.cpp
    horo-engine/cli_host/CliHostCommands.cpp
    horo-engine/cli_host/McpServe.cpp
    horo-engine/cli_host/McpStdio.cpp
    horo-engine/NetworkProductLaunch.cpp
    horo-engine/NetworkProductLaunchOptions.cpp
)
target_compile_features(horo-engine PRIVATE cxx_std_20)
# Only identity consumers depend on the changing Git revision. Keep unrelated
# application units reusable by compiler and analysis caches across commits.
set_property(SOURCE horo-engine/main.cpp horo-engine/NetworkProductLaunch.cpp
    APPEND PROPERTY COMPILE_DEFINITIONS HORO_SOURCE_REVISION="${HORO_SOURCE_REVISION}")

# Conservative provider closure: any first-party navigation implementation or
# public contract change invalidates native bake compatibility. Configure again
# when contents change, including uncommitted edits; a Git revision is insufficient.
file(GLOB_RECURSE horo_navigation_identity_inputs CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/include/Horo/*.h"
    "${PROJECT_SOURCE_DIR}/src/runtime/navigation/*.cpp"
    "${PROJECT_SOURCE_DIR}/src/runtime/navigation/*.h"
    "${PROJECT_SOURCE_DIR}/src/application/navigation/*.cpp"
    "${PROJECT_SOURCE_DIR}/src/application/navigation/*.h"
    "${PROJECT_SOURCE_DIR}/src/foundation/math/*.cpp"
    "${PROJECT_SOURCE_DIR}/src/foundation/math/*.h")
list(SORT horo_navigation_identity_inputs)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${horo_navigation_identity_inputs})
set(horo_navigation_identity_material "recast=${HORO_RECAST_NAVIGATION_REVISION};enabled=${HORO_BUILD_NAVIGATION_RECAST_DETOUR};compiler=${CMAKE_CXX_COMPILER_ID}/${CMAKE_CXX_COMPILER_VERSION};target=${CMAKE_CXX_COMPILER_TARGET};system=${CMAKE_SYSTEM_NAME}/${CMAKE_SYSTEM_PROCESSOR};architectures=${CMAKE_OSX_ARCHITECTURES};pointer=${CMAKE_SIZEOF_VOID_P};flags=${CMAKE_CXX_FLAGS};debug=${CMAKE_CXX_FLAGS_DEBUG};release=${CMAKE_CXX_FLAGS_RELEASE};relwithdebinfo=${CMAKE_CXX_FLAGS_RELWITHDEBINFO};minsize=${CMAKE_CXX_FLAGS_MINSIZEREL};definitions=DT_POLYREF64,RC_DISABLE_ASSERTS")
foreach(horo_navigation_identity_input IN LISTS horo_navigation_identity_inputs)
    file(SHA256 "${horo_navigation_identity_input}" horo_navigation_identity_digest)
    file(RELATIVE_PATH horo_navigation_identity_path "${PROJECT_SOURCE_DIR}" "${horo_navigation_identity_input}")
    string(APPEND horo_navigation_identity_material ";${horo_navigation_identity_path}=${horo_navigation_identity_digest}")
endforeach()
string(SHA256 HORO_NAVIGATION_BUILD_FINGERPRINT "${horo_navigation_identity_material}")

# One non-installed native composition authority for editor, CLI and MCP bake adapters.
set(HORO_NAVIGATION_HOST_HEADERS "${CMAKE_CURRENT_BINARY_DIR}/navigation_host_headers")
file(MAKE_DIRECTORY "${HORO_NAVIGATION_HOST_HEADERS}")
configure_file(common/NativeNavigationBakeComposition.h "${HORO_NAVIGATION_HOST_HEADERS}/NativeNavigationBakeComposition.h" COPYONLY)
add_library(HoroNativeNavigationBakeHost STATIC common/NativeNavigationBakeComposition.cpp)
target_compile_features(HoroNativeNavigationBakeHost PUBLIC cxx_std_20)
target_include_directories(HoroNativeNavigationBakeHost PUBLIC "${HORO_NAVIGATION_HOST_HEADERS}")
target_compile_definitions(HoroNativeNavigationBakeHost PRIVATE
        HORO_NAVIGATION_BUILD_FINGERPRINT="${HORO_NAVIGATION_BUILD_FINGERPRINT}-$<CONFIG>")
target_link_libraries(HoroNativeNavigationBakeHost PUBLIC HoroEngine::NavigationBakeAutomation
        PRIVATE HoroEngine::NavigationRecastDetour HoroEngine::PlatformProjectIo HoroEngine::Platform)
target_link_libraries(horo-engine PRIVATE HoroNativeNavigationBakeHost)

if(MINGW)
    target_link_options(horo-engine PRIVATE -municode)
endif()
target_compile_definitions(horo-engine PRIVATE
    HORO_ENGINE_VERSION_STRING="${PROJECT_VERSION}"
    HORO_BUILD_CONFIGURATION="$<CONFIG>"
    HORO_PRODUCT_HAS_GNS=$<BOOL:${HORO_BUILD_NETWORK_GNS}>
    HORO_NAVIGATION_BUILD_FINGERPRINT="${HORO_NAVIGATION_BUILD_FINGERPRINT}-$<CONFIG>"
)
target_link_libraries(horo-engine PRIVATE HoroEngine::PlatformProjectIo HoroEngine::ProjectCollisionSettings HoroEngine::PlatformProjectReadFiles HoroEngine::PlatformImmutableFileStorage
    HoroEngine::Application HoroEngine::Extensions HoroHostModuleComposition
    HoroNetworkProductHost HoroPackageProjectHost HoroEngine::CliPackageAdapters HoroEngine::McpPackageAdapters
    HoroEngine::CliNavigationAdapters HoroEngine::McpNavigationAdapters HoroEngine::NavigationRecastDetour
    HoroEngine::Runtime
    HoroEngine::CliHost
    HoroEngine::McpController
    HoroEngine::Platform
)
if(HORO_BUILD_NETWORK_GNS)
    target_link_libraries(horo-engine PRIVATE HoroEngine::NetworkTransportGNS)
endif()
# Validate the actual transitive link graph, including configurations that build GUI peers.
function(horo_verify_terminal_link_boundary)
    set(pending horo-engine)
    set(visited)
    while(pending)
        list(POP_FRONT pending current)
        if(NOT TARGET "${current}" OR current IN_LIST visited)
            continue()
        endif()
        list(APPEND visited "${current}")
        if(current MATCHES "(HoroGui|ImGui|HoroEditorViewport|HoroRender(OpenGL|Metal|Vulkan|D3D12)|HoroInputSdl|SDL)")
            message(FATAL_ERROR "horo-engine headless link boundary reaches ${current}")
        endif()
        get_target_property(alias "${current}" ALIASED_TARGET)
        if(alias)
            list(APPEND pending "${alias}")
        endif()
        foreach(property LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
            get_target_property(links "${current}" "${property}")
            if(links)
                foreach(link IN LISTS links)
                    string(REGEX REPLACE "^\\$<LINK_ONLY:([^>]+)>$" "\\1" link "${link}")
                    if(TARGET "${link}")
                        list(APPEND pending "${link}")
                    endif()
                endforeach()
            endif()
        endforeach()
    endwhile()
endfunction()
horo_verify_terminal_link_boundary()

install(TARGETS horo-engine RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT Game)

add_executable(horo-extension-validate
    horo-extension-validate/main.cpp
)
target_compile_features(horo-extension-validate PRIVATE cxx_std_20)
target_link_libraries(horo-extension-validate PRIVATE HoroEngine::Extensions)
set(HORO_EXTENSION_VALIDATOR_SDK_PATH
    "${HORO_EXTENSION_SDK_PACKAGE_DIR}/bin/horo-extension-validate${CMAKE_EXECUTABLE_SUFFIX}")
add_custom_command(
    OUTPUT "${HORO_EXTENSION_VALIDATOR_SDK_PATH}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "$<TARGET_FILE:horo-extension-validate>"
        "${HORO_EXTENSION_VALIDATOR_SDK_PATH}"
    DEPENDS horo-extension-validate
    COMMENT "Staging the extension manifest validator in the SDK"
    VERBATIM)
add_custom_target(HoroExtensionSdkValidatorStage ALL
    DEPENDS "${HORO_EXTENSION_VALIDATOR_SDK_PATH}")

add_executable(horo-extension-conformance
    horo-extension-conformance/main.cpp
)
target_compile_features(horo-extension-conformance PRIVATE cxx_std_20)
target_link_libraries(horo-extension-conformance PRIVATE HoroEngine::Extensions)
set(HORO_EXTENSION_CONFORMANCE_SDK_PATH
    "${HORO_EXTENSION_SDK_PACKAGE_DIR}/bin/horo-extension-conformance${CMAKE_EXECUTABLE_SUFFIX}")
add_custom_command(
    OUTPUT "${HORO_EXTENSION_CONFORMANCE_SDK_PATH}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "$<TARGET_FILE:horo-extension-conformance>"
        "${HORO_EXTENSION_CONFORMANCE_SDK_PATH}"
    DEPENDS horo-extension-conformance
    COMMENT "Staging the extension ABI conformance harness in the SDK"
    VERBATIM)
add_custom_target(HoroExtensionSdkConformanceStage ALL
    DEPENDS "${HORO_EXTENSION_CONFORMANCE_SDK_PATH}")

add_executable(horo-package
    horo-package/main.cpp
    horo-package/PackageIo.cpp
    horo-package/Pack.cpp
    horo-package/SignatureDocument.cpp
    horo-package/Sign.cpp
    horo-package/Verify.cpp
)
target_compile_features(horo-package PRIVATE cxx_std_20)
target_link_libraries(horo-package PRIVATE HoroEngine::PackageSecurity HoroEngine::Packages HoroEngine::Security
    HoroHostSignatureDocuments miniz nlohmann_json::nlohmann_json MbedTLS::mbedcrypto)
set(HORO_PACKAGE_SDK_PATH
    "${HORO_EXTENSION_SDK_PACKAGE_DIR}/bin/horo-package${CMAKE_EXECUTABLE_SUFFIX}")
add_custom_command(
    OUTPUT "${HORO_PACKAGE_SDK_PATH}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "$<TARGET_FILE:horo-package>"
        "${HORO_PACKAGE_SDK_PATH}"
    DEPENDS horo-package
    COMMENT "Staging the package authoring commands in the versioned SDK"
    VERBATIM)
add_custom_target(HoroExtensionSdkPackageStage ALL DEPENDS "${HORO_PACKAGE_SDK_PATH}")

# Focused SDK-author qualification can stage its three source-free tools without
# building unrelated applications.
add_custom_target(HoroExtensionAuthorCiStage DEPENDS
    HoroExtensionSdkValidatorStage
    HoroExtensionSdkConformanceStage
    HoroExtensionSdkPackageStage)

# Non-installed executable adapter shared with qualification; portable producer remains Editor/Platform-free.
add_library(HoroEditorUiStyleHostInternal STATIC common/ui_style/EditorUiStylePreparation.cpp)
target_compile_features(HoroEditorUiStyleHostInternal PUBLIC cxx_std_20)
target_include_directories(HoroEditorUiStyleHostInternal PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/common/ui_style")
target_link_libraries(HoroEditorUiStyleHostInternal PUBLIC HoroEngine::EditorServices HoroEngine::UiStyleWorkspaceApplication
        HoroEngine::PlatformImmutableFileStorage HoroEngine::Platform HoroEngine::ProjectMutationGuard)

# The graphical executable is an optional composition root. Foundation and
# headless hosts must remain buildable when the GUI adapter is intentionally off.
if(HORO_BUILD_EDITOR_GUI)
    set(HORO_EDITOR_APP_ID "dev.horo.editor")
    set(HORO_EDITOR_ASSET_INSTALL_DIR "${CMAKE_INSTALL_DATADIR}/horo-engine/assets")

    add_executable(HoroEditor
        HoroEditor/main.cpp
        HoroEditor/app/HoroEditorApp.cpp
        HoroEditor/app/EditorNavigationBakeHost.cpp
        HoroEditor/app/EditorWorkerShutdown.cpp
        HoroEditor/app/EditorAudioPreviewHost.cpp
        HoroEditor/app/EditorAudioPreviewDecode.cpp
        HoroEditor/app/EditorAudioPreviewPreparation.cpp
        HoroEditor/app/RenderGraphInspectionPane.cpp
        HoroEditor/app/EditorUserStateMigration.cpp
        HoroEditor/app/ConfiguredEditorUpdateBackend.cpp
        HoroEditor/app/ConfiguredEditorUpdateManifestSource.cpp
    )

    target_link_libraries(HoroEditor PRIVATE HoroEngine::PlatformProjectReadFiles)

    if(APPLE)
        # SDL_GetBasePath resolves to Contents/Resources for an application bundle.
        set(HORO_EDITOR_PACKAGED_ASSET_ROOT_RELATIVE "assets")
        set(HORO_EDITOR_BUILD_ASSET_ROOT "$<TARGET_BUNDLE_DIR:HoroEditor>/Contents/Resources/assets")
    else()
        file(RELATIVE_PATH HORO_EDITOR_PACKAGED_ASSET_ROOT_RELATIVE
            "${CMAKE_INSTALL_FULL_BINDIR}"
            "${CMAKE_INSTALL_FULL_DATADIR}/horo-engine/assets"
        )
        set(HORO_EDITOR_BUILD_ASSET_ROOT "$<TARGET_FILE_DIR:HoroEditor>/${HORO_EDITOR_PACKAGED_ASSET_ROOT_RELATIVE}")
    endif()

    # Offline qualification stages generated cook output beside the same assets consumed by the real host.
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/HoroEditorAssetRoot-$<CONFIG>.txt"
        CONTENT "${HORO_EDITOR_BUILD_ASSET_ROOT}\n")

    add_custom_command(TARGET HoroEditor POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${PROJECT_SOURCE_DIR}/assets"
            "${HORO_EDITOR_BUILD_ASSET_ROOT}"
        COMMENT "Staging Horo Editor runtime assets"
        VERBATIM
    )

    if(APPLE)
        set(HORO_EDITOR_APP_ICON "${CMAKE_CURRENT_SOURCE_DIR}/HoroEditor/resources/HoroEditor.icns")
        set_source_files_properties("${HORO_EDITOR_APP_ICON}" PROPERTIES
            MACOSX_PACKAGE_LOCATION "Resources"
        )
        target_sources(HoroEditor PRIVATE "${HORO_EDITOR_APP_ICON}")
        set_target_properties(HoroEditor PROPERTIES
            MACOSX_BUNDLE TRUE
            MACOSX_BUNDLE_BUNDLE_NAME "Horo Editor"
            MACOSX_BUNDLE_GUI_IDENTIFIER "${HORO_EDITOR_APP_ID}"
            MACOSX_BUNDLE_ICON_FILE "HoroEditor.icns"
            MACOSX_BUNDLE_LONG_VERSION_STRING "${PROJECT_VERSION}"
            MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
            MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        )
        install(TARGETS HoroEditor
            BUNDLE DESTINATION "."
            COMPONENT Editor
        )
        install(DIRECTORY "${PROJECT_SOURCE_DIR}/assets/"
            DESTINATION "HoroEditor.app/Contents/Resources/assets"
            COMPONENT Editor
        )
    else()
        install(TARGETS HoroEditor
            RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
            COMPONENT Editor
        )
        install(DIRECTORY "${PROJECT_SOURCE_DIR}/assets/"
            DESTINATION "${HORO_EDITOR_ASSET_INSTALL_DIR}"
            COMPONENT Editor
        )
    endif()

    if(WIN32)
        set(HORO_EDITOR_WINDOWS_ICON "${CMAKE_CURRENT_SOURCE_DIR}/HoroEditor/resources/windows/HoroEditor.ico")
        configure_file(
            "${CMAKE_CURRENT_SOURCE_DIR}/HoroEditor/resources/windows/HoroEditor.rc.in"
            "${CMAKE_CURRENT_BINARY_DIR}/HoroEditor.rc"
            @ONLY
        )
        target_sources(HoroEditor PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/HoroEditor.rc")
    endif()

    if(UNIX AND NOT APPLE)
        set(HORO_EDITOR_DESKTOP_FILE "${CMAKE_CURRENT_BINARY_DIR}/${HORO_EDITOR_APP_ID}.desktop")
        configure_file(
            "${CMAKE_CURRENT_SOURCE_DIR}/HoroEditor/resources/linux/HoroEditor.desktop.in"
            "${HORO_EDITOR_DESKTOP_FILE}"
            @ONLY
        )
        install(FILES "${PROJECT_SOURCE_DIR}/assets/launcher/logo.png"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/1024x1024/apps"
            RENAME "${HORO_EDITOR_APP_ID}.png"
            COMPONENT Editor
        )
        install(FILES "${HORO_EDITOR_DESKTOP_FILE}"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/applications"
            COMPONENT Editor
        )
    endif()

    target_compile_features(HoroEditor PRIVATE cxx_std_20)
    set_property(SOURCE HoroEditor/app/HoroEditorApp.cpp
        APPEND PROPERTY COMPILE_DEFINITIONS HORO_SOURCE_REVISION="${HORO_SOURCE_REVISION}")
    target_link_libraries(HoroEditor PRIVATE HoroNativeNavigationBakeHost HoroEngine::PlatformProjectIo)

    if(BUILD_TESTING AND HORO_ENABLE_GPU_SMOKE_TESTS AND HORO_BUILD_RENDER_OPENGL)
        target_sources(HoroEditor PRIVATE HoroEditor/app/EditorSurfaceLifecycleQualification.cpp)
        target_compile_definitions(HoroEditor PRIVATE HORO_EDITOR_SURFACE_QUALIFICATION=1)
    endif()

    target_include_directories(HoroEditor
        PRIVATE
            ${PROJECT_SOURCE_DIR}/apps/HoroEditor/app
            ${PROJECT_SOURCE_DIR}/src
            ${PROJECT_SOURCE_DIR}/vendor/stb
            ${PROJECT_SOURCE_DIR}/vendor/pfd
            ${HORO_GENERATED_DIR}
    )

    target_compile_definitions(HoroEditor
        PRIVATE
            GL_SILENCE_DEPRECATION
            HORO_EDITOR_APP_ID="${HORO_EDITOR_APP_ID}"
            HORO_EDITOR_PACKAGED_ASSET_ROOT_RELATIVE="${HORO_EDITOR_PACKAGED_ASSET_ROOT_RELATIVE}"
            HORO_ENGINE_VERSION_STRING="${PROJECT_VERSION}"
            HORO_BUILD_CONFIGURATION="$<CONFIG>"
            HORO_GAMEPLAY_SDK_PACKAGE_DIR="${HORO_GAMEPLAY_SDK_PACKAGE_DIR}"
            HORO_GAMEPLAY_CXX_COMPILER="${CMAKE_CXX_COMPILER}"
    )

    target_link_libraries(HoroEditor
        PRIVATE
            HoroPackageProjectHost
            HoroRuntimeUiPaintHost
            HoroEditorUiStyleHostInternal
            HoroEngine::ShaderBuild
            HoroEngine::Gui
            HoroEngine::EditorServices
            HoroEngine::EditorRenderExtraction
            HoroEngine::RenderFrontend
            HoroEditorPresentationSurfaceInternal
            HoroEngine::Runtime
            HoroEngine::RuntimeScene
            HoroEngine::PhysicsSceneIntegration
            HoroEditorSceneProductHost
            HoroEditorPlaySceneHostInternal
            HoroScenePhysicsHost
            HoroEngine::Extensions
            HoroEngine::Platform
            HoroEngine::ProjectMigrations
            HoroEngine::UserStateMigration
            HoroEngine::InputSdl
            HoroEngine::UpdateDiscovery
            HoroEngine::UpdateDownload
            CURL::libcurl
            HoroHostModuleComposition
            HoroThirdParty::ImGui
            nlohmann_json::nlohmann_json
    )

    target_link_libraries(HoroEditor PRIVATE HoroAudioFrontendComposition HoroEngine::AudioImport HoroEngine::AudioNull)
    if(HORO_BUILD_XR_OPENXR)
        set(HORO_EDITOR_XR_BOOTSTRAP_HEADERS "${CMAKE_CURRENT_BINARY_DIR}/editor_xr_bootstrap_headers")
        file(MAKE_DIRECTORY "${HORO_EDITOR_XR_BOOTSTRAP_HEADERS}")
        foreach(header EditorOpenXRBootstrap.h EditorOpenXRHostServices.h)
            configure_file("HoroEditor/app/${header}" "${HORO_EDITOR_XR_BOOTSTRAP_HEADERS}/${header}" COPYONLY)
        endforeach()
        add_library(HoroEditorOpenXRBootstrap STATIC HoroEditor/app/EditorOpenXRBootstrap.cpp)
        target_compile_features(HoroEditorOpenXRBootstrap PUBLIC cxx_std_20)
        target_include_directories(HoroEditorOpenXRBootstrap PUBLIC "${HORO_EDITOR_XR_BOOTSTRAP_HEADERS}")
        target_link_libraries(HoroEditorOpenXRBootstrap
            PUBLIC HoroEngine::Foundation HoroEngine::RuntimeScene HoroOpenXRProductHost
            PRIVATE HoroOpenXRApplicationBindings HoroHostSignatureDocuments HoroEngine::Security HoroEngine::Platform HoroEngine::XRRuntime)
        target_compile_definitions(HoroEditor PRIVATE HORO_HAS_OPENXR_HOST=1)
        target_link_libraries(HoroEditor PRIVATE HoroEngine::XRRuntime HoroOpenXRProductHost HoroEditorOpenXRBootstrap)
        if(TARGET HoroOpenXRApplicationHost)
            target_compile_definitions(HoroEditor PRIVATE HORO_HAS_OPENXR_VULKAN_HOST=1)
            target_link_libraries(HoroEditor PRIVATE HoroOpenXRApplicationHost)
        endif()
    endif()
    if(TARGET HoroAudioSdl3)
        target_link_libraries(HoroEditor PRIVATE HoroEngine::AudioSdl3)
        target_compile_definitions(HoroEditor PRIVATE HORO_EDITOR_AUDIO_SDL3=1)
    else()
        target_compile_definitions(HoroEditor PRIVATE HORO_EDITOR_AUDIO_SDL3=0)
    endif()

    if(HORO_ENABLE_OPENTELEMETRY)
        target_compile_definitions(HoroEditor PRIVATE HORO_HAS_OPENTELEMETRY=1)
        target_link_libraries(HoroEditor PRIVATE HoroEngine::OpenTelemetry)
    endif()

    # Development composition: concrete backends are linked explicitly at the
    # executable boundary until RenderModuleHost owns installed-product loading.
    if(HORO_BUILD_RENDER_OPENGL)
        target_compile_definitions(HoroEditor PRIVATE HORO_HAS_RENDER_OPENGL=1)
        target_link_libraries(HoroEditor PRIVATE HoroEngine::EditorViewportOpenGL)
    endif()

    if(HORO_BUILD_RENDER_VULKAN)
        target_compile_definitions(HoroEditor PRIVATE HORO_HAS_RENDER_VULKAN=1)
        target_sources(HoroEditor PRIVATE HoroEditor/app/EditorVulkanShaderAssets.cpp)
        target_link_libraries(HoroEditor PRIVATE HoroEditorVulkanGuiInternal)
    endif()

    if(HORO_BUILD_RENDER_METAL)
        target_compile_definitions(HoroEditor PRIVATE HORO_HAS_RENDER_METAL=1)
        target_link_libraries(HoroEditor PRIVATE HoroEngine::EditorViewportMetal)
    endif()
endif()

# Offline preparation tool, never linked into HoroEditor or a runtime renderer.
add_executable(HoroEditorVulkanShaderCook EXCLUDE_FROM_ALL HoroEditor/tools/VulkanViewportCook.cpp)
target_compile_features(HoroEditorVulkanShaderCook PRIVATE cxx_std_20)
target_link_libraries(HoroEditorVulkanShaderCook PRIVATE HoroEditorVulkanShaderInternal
    HoroEngine::ShaderCompilerToolchain HoroEngine::Platform)
