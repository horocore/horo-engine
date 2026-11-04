# Included from apps/CMakeLists.txt; source and binary directories remain scoped to apps/.
include(HoroModuleScriptPackage)
horo_add_module_script_package_target()

add_library(HoroNetworkProductHost STATIC
    horo-engine/composition/network/NetworkProductHost.cpp
    horo-engine/composition/network/NetworkProductSession.cpp
    horo-engine/composition/network/NetworkProductTransport.cpp
    horo-engine/composition/network/HeadlessNetworkServices.cpp
    horo-engine/composition/network/HeadlessAnimationFixed.h
    horo-engine/composition/network/NetworkProductComposition.h
    horo-engine/composition/network/HeadlessSceneLoadBoundary.cpp
    horo-engine/composition/network/HeadlessProjectScene.cpp
    horo-engine/composition/network/GameplayWorldComposition.cpp
)
target_compile_features(HoroNetworkProductHost PUBLIC cxx_std_20)
target_link_libraries(HoroNetworkProductHost PRIVATE HoroNetworkModeHostInternal)
target_link_libraries(HoroNetworkProductHost PRIVATE HoroEngine::PrefabProjectSettings HoroEngine::ProjectSurfaceSettings)
target_include_directories(HoroNetworkProductHost PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/horo-engine/composition/network)
target_link_libraries(HoroNetworkProductHost PUBLIC HoroEngine::NetworkRuntime HoroEngine::RuntimeScene
    HoroEngine::Physics HoroEngine::Runtime HoroEngine::GameplayModuleHost HoroEngine::GameplayLua
    HoroEngine::GameplayPhysicsIntegration HoroEngine::GameplayCharacterIntegration HoroEngine::PrefabRuntime HoroEngine::NetworkDebuggerApplication
    HoroEngine::ProjectCollisionSettings HoroEngine::PlatformProjectReadFiles HoroEngine::SceneRuntimeConversion HoroEngine::AISceneIntegration)
target_link_libraries(HoroNetworkProductHost PUBLIC HoroScenePhysicsHost)
target_link_libraries(HoroNetworkProductHost PUBLIC HoroEngine::AnimationSceneIntegration HoroAnimationHostIdentityInternal)
if(HORO_BUILD_NETWORK_GNS)
    target_sources(HoroNetworkProductHost PRIVATE horo-engine/composition/network/NetworkProductGnsSelection.cpp)
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
target_sources(horo-engine PRIVATE
    horo-engine/cli_host/ExtensionScriptCliAuthentication.cpp
    horo-engine/cli_host/ExtensionScriptCliSnapshot.cpp
    horo-engine/cli_host/ExtensionScriptCliOptions.cpp
    horo-engine/cli_host/ExtensionScriptCliHost.cpp
    horo-engine/cli_host/ExtensionScriptCliCommand.cpp)
target_link_libraries(horo-engine PRIVATE HoroEngine::ExtensionLuaBindings HoroHostSignatureDocuments)
target_link_libraries(horo-engine PRIVATE HoroEngine::AnimationCookedAssets)
target_sources(horo-engine PRIVATE horo-engine/cli_host/ModuleScriptCliExecution.cpp)
target_link_libraries(horo-engine PRIVATE HoroScriptModuleHostInternal HoroModuleScriptPackageInternal)
target_compile_features(horo-engine PRIVATE cxx_std_20)
# Only identity consumers depend on the changing Git revision. Keep unrelated
# application units reusable by compiler and analysis caches across commits.
set_property(SOURCE horo-engine/main.cpp horo-engine/NetworkProductLaunch.cpp
    APPEND PROPERTY COMPILE_DEFINITIONS HORO_SOURCE_REVISION="${HORO_SOURCE_REVISION}")

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

target_link_libraries(HoroNetworkProductHost PRIVATE HoroPhysicsStateProcessBindingInternal)
