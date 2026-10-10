include_guard(GLOBAL)

# This manifest is intentionally explicit: changing a first-party link edge must
# update the architecture policy in the same review.
horo_allow_target_dependencies(TARGET HoroFoundation)
horo_allow_target_dependencies(TARGET HoroHostErrors DEPENDENCIES HoroFoundation)
horo_allow_target_dependencies(TARGET HoroSecurity DEPENDENCIES HoroFoundation)
horo_allow_target_dependencies(TARGET HoroCliHost DEPENDENCIES HoroFoundation HoroHostErrors)
horo_allow_target_dependencies(TARGET HoroMcpSession DEPENDENCIES HoroFoundation HoroSecurity)
horo_allow_target_dependencies(TARGET HoroMcpRegistry DEPENDENCIES HoroMcpSession)
horo_allow_target_dependencies(TARGET HoroMcpController DEPENDENCIES HoroMcpRegistry)
horo_allow_target_dependencies(TARGET HoroModelProvider)
horo_allow_target_dependencies(TARGET HoroModelProviderAdapters DEPENDENCIES HoroModelProvider)
horo_allow_target_dependencies(TARGET HoroOpenTelemetry DEPENDENCIES HoroFoundation)
horo_allow_target_dependencies(TARGET HoroPlatform DEPENDENCIES HoroFoundation HoroSecurity)
horo_allow_target_dependencies(TARGET HoroPlatformServices DEPENDENCIES HoroFoundation HoroPlatform)
horo_allow_target_dependencies(TARGET HoroPlatformServicesExtension DEPENDENCIES HoroPlatformServices HoroExtensions)
horo_allow_target_dependencies(TARGET HoroPackages DEPENDENCIES HoroFoundation)
horo_allow_target_dependencies(TARGET HoroPackageSecurity DEPENDENCIES HoroFoundation HoroPackages HoroSecurity)
horo_allow_target_dependencies(TARGET HoroApplication DEPENDENCIES HoroFoundation)
horo_allow_target_dependencies(TARGET HoroProjectSettings DEPENDENCIES HoroApplication HoroNetworkApi)
horo_allow_target_dependencies(TARGET HoroReleaseProcess DEPENDENCIES HoroApplication HoroPlatform)
horo_allow_target_dependencies(TARGET HoroReleaseService DEPENDENCIES HoroApplication HoroFoundation)
horo_allow_target_dependencies(TARGET HoroReleaseGitHub DEPENDENCIES HoroApplication)
horo_allow_target_dependencies(TARGET HoroReleaseGitHubCli DEPENDENCIES HoroReleaseGitHub HoroPlatform)
horo_allow_target_dependencies(TARGET HoroUpdateManifest DEPENDENCIES HoroApplication HoroSecurity)
horo_allow_target_dependencies(TARGET HoroUpdateDiscovery DEPENDENCIES HoroUpdateManifest)
horo_allow_target_dependencies(TARGET HoroUpdateTransfer DEPENDENCIES HoroUpdateManifest)
horo_allow_target_dependencies(TARGET HoroUpdateDownload DEPENDENCIES HoroUpdateTransfer HoroPlatform)
horo_allow_target_dependencies(TARGET HoroUpdateOfflineSource DEPENDENCIES HoroUpdateDiscovery HoroUpdateDownload HoroPlatform)
horo_allow_target_dependencies(TARGET HoroUpdateActivation DEPENDENCIES HoroUpdateDownload HoroPlatform)
horo_allow_target_dependencies(TARGET HoroUserStateMigration DEPENDENCIES HoroPlatform)
horo_allow_target_dependencies(TARGET HoroProjectMigrations DEPENDENCIES HoroApplication HoroProjectSettings HoroNavigationApi HoroAssets)
horo_allow_target_dependencies(TARGET HoroSaveApi DEPENDENCIES HoroFoundation)
horo_allow_target_dependencies(TARGET HoroRuntimeFrameInternal DEPENDENCIES HoroFoundation)
horo_allow_target_dependencies(TARGET HoroRationalDurationInternal DEPENDENCIES HoroFoundation)
horo_allow_target_dependencies(TARGET HoroRuntime DEPENDENCIES HoroFoundation HoroSaveApi HoroRationalDurationInternal)
horo_allow_target_dependencies(TARGET HoroUiAnimationIntegrationInternal DEPENDENCIES HoroRuntime HoroRuntimeUi)
horo_allow_target_dependencies(TARGET HoroRuntimeUi DEPENDENCIES HoroFoundation HoroAssets HoroRationalDurationInternal)
horo_allow_target_dependencies(TARGET HoroUiTemplateGraph DEPENDENCIES HoroFoundation HoroAssets HoroPackages)
horo_allow_target_dependencies(TARGET HoroRuntimeUiInput DEPENDENCIES HoroInput HoroRuntimeUi)
horo_allow_target_dependencies(TARGET HoroAssets DEPENDENCIES HoroFoundation)
horo_allow_target_dependencies(TARGET HoroNetworkApi DEPENDENCIES HoroFoundation)
horo_allow_target_dependencies(TARGET HoroNetworkDebuggerApplication DEPENDENCIES HoroNetworkApi)
horo_allow_target_dependencies(TARGET HoroPlayTopologyApplication DEPENDENCIES HoroFoundation HoroNetworkApi HoroPlatform)
horo_allow_target_dependencies(TARGET HoroNetworkRuntime DEPENDENCIES HoroNetworkApi HoroRuntimeScene HoroRuntime)
horo_allow_target_dependencies(TARGET HoroNetworkTransportNull DEPENDENCIES HoroNetworkApi)
if(HORO_BUILD_NETWORK_GNS)
    horo_allow_target_dependencies(TARGET HoroNetworkTransportGNSFactoryInternal DEPENDENCIES HoroNetworkApi)
    horo_allow_target_dependencies(TARGET HoroNetworkTransportGNS DEPENDENCIES HoroNetworkTransportGNSFactoryInternal)
endif()
horo_allow_target_dependencies(TARGET HoroAudioApi DEPENDENCIES HoroFoundation HoroAssets)
horo_allow_target_dependencies(TARGET HoroAudioImport DEPENDENCIES HoroAudioApi)
horo_allow_target_dependencies(TARGET HoroAudioCook DEPENDENCIES HoroAudioImport HoroAssets)
horo_allow_target_dependencies(TARGET HoroAudioDsp DEPENDENCIES HoroAudioApi)
horo_allow_target_dependencies(TARGET HoroAudioPlayback DEPENDENCIES HoroAudioDsp HoroAudioApi HoroAudioCommands)
horo_allow_target_dependencies(TARGET HoroAudioMetrics DEPENDENCIES HoroAudioCommands HoroFoundation)
horo_allow_target_dependencies(TARGET HoroAudioSafetyHooks)
horo_allow_target_dependencies(TARGET HoroAudioMemory DEPENDENCIES HoroAudioApi HoroAudioSafetyHooks)
horo_allow_target_dependencies(TARGET HoroAudioCommands DEPENDENCIES HoroAudioMemory HoroAudioSafetyHooks)
horo_allow_target_dependencies(TARGET HoroAudioMixer DEPENDENCIES HoroAudioDsp HoroAudioCommands)
horo_allow_target_dependencies(TARGET HoroAudioVoiceRender DEPENDENCIES HoroAudioPlayback HoroAudioMixer)
horo_allow_target_dependencies(TARGET HoroAudioFrontend
    DEPENDENCIES HoroAudioCommands HoroAudioVoiceRender HoroAudioBackendContract)
horo_allow_target_dependencies(TARGET HoroAudioFrontendComposition
    DEPENDENCIES HoroAudioFrontend HoroAudioVoiceRender HoroAudioBackendContract)
horo_allow_target_dependencies(TARGET HoroAudioBackendContract DEPENDENCIES HoroAudioApi)
horo_allow_target_dependencies(TARGET HoroAudioWatchdog DEPENDENCIES HoroAudioBackendContract HoroAudioSafetyHooks)
horo_allow_target_dependencies(TARGET HoroAudioNull DEPENDENCIES HoroAudioBackendContract HoroAudioCommands HoroAudioWatchdog)
if(TARGET HoroAudioSdl3)
    horo_allow_target_dependencies(TARGET HoroAudioSdl3 DEPENDENCIES HoroAudioBackendContract HoroAudioCommands HoroAudioWatchdog)
endif()
horo_allow_target_dependencies(TARGET HoroPhysicsModel DEPENDENCIES HoroFoundation HoroAssets HoroRuntime)
horo_allow_target_dependencies(TARGET HoroPhysics DEPENDENCIES HoroFoundation HoroAssets HoroPhysicsModel)
horo_allow_target_dependencies(TARGET HoroPhysicsSceneIntegration DEPENDENCIES HoroPhysics HoroRuntimeScene HoroSceneCellPayload)
horo_allow_target_dependencies(TARGET HoroGameplayPhysicsIntegration DEPENDENCIES HoroGameplayApi HoroPhysics)
horo_allow_target_dependencies(TARGET HoroAI DEPENDENCIES HoroFoundation)
horo_allow_target_dependencies(TARGET HoroAISceneIntegration DEPENDENCIES HoroAI HoroRuntimeScene)
horo_allow_target_dependencies(TARGET HoroAISightIntegration DEPENDENCIES HoroAISceneIntegration HoroPhysics)
horo_allow_target_dependencies(TARGET HoroGameplayPerceptionIntegration DEPENDENCIES HoroAISceneIntegration HoroNetworkRuntime)
horo_allow_target_dependencies(TARGET HoroAnimationApi DEPENDENCIES HoroFoundation HoroAssets)
horo_allow_target_dependencies(TARGET HoroPCG DEPENDENCIES HoroFoundation)
horo_allow_target_dependencies(TARGET HoroPCGTerrainAdapter DEPENDENCIES HoroPCG HoroTerrainApi)
horo_allow_target_dependencies(TARGET HoroVfxApi DEPENDENCIES HoroFoundation HoroAssets)
horo_allow_target_dependencies(TARGET HoroDestructionApi DEPENDENCIES HoroFoundation HoroAssets)
horo_allow_target_dependencies(TARGET HoroDestructionCook DEPENDENCIES HoroDestructionApi HoroAssets)
horo_allow_target_dependencies(TARGET HoroDestructionCollisionArtifacts DEPENDENCIES HoroDestructionApi HoroPhysics)
horo_allow_target_dependencies(TARGET HoroDestructionPhysicsCook DEPENDENCIES HoroDestructionCook HoroDestructionCollisionArtifacts)
horo_allow_target_dependencies(TARGET HoroDestructionRuntime DEPENDENCIES HoroDestructionApi)
horo_allow_target_dependencies(TARGET HoroDestructionApplication DEPENDENCIES HoroDestructionRuntime)
horo_allow_target_dependencies(TARGET HoroDestructionReplication DEPENDENCIES HoroDestructionApi HoroNetworkApi)
horo_allow_target_dependencies(TARGET HoroFractureDocument DEPENDENCIES HoroDestructionApi)
horo_allow_target_dependencies(TARGET HoroCinematicModel DEPENDENCIES HoroFoundation HoroRuntime HoroAssets HoroSceneModel)
horo_allow_target_dependencies(TARGET HoroCinematicRuntime DEPENDENCIES HoroCinematicModel)
horo_allow_target_dependencies(TARGET HoroCameraRuntime DEPENDENCIES HoroRuntimeScene HoroRenderApi)
horo_allow_target_dependencies(TARGET HoroCinematicCameraRuntime DEPENDENCIES HoroCinematicRuntime HoroCameraRuntime)
horo_allow_target_dependencies(TARGET HoroCinematicScriptBridge DEPENDENCIES HoroCinematicRuntime HoroExtensions HoroGameplayModuleHost)
horo_allow_target_dependencies(TARGET HoroNavigationApi DEPENDENCIES HoroFoundation)
horo_allow_target_dependencies(TARGET HoroNavigationRuntime DEPENDENCIES HoroNavigationApi)
horo_allow_target_dependencies(TARGET HoroNavigationBakeService DEPENDENCIES HoroNavigationRuntime HoroAssets HoroPlatform HoroRuntime)
horo_allow_target_dependencies(TARGET HoroNavigationSceneIntegration DEPENDENCIES HoroNavigationRuntime HoroRuntimeScene)
horo_allow_target_dependencies(TARGET HoroNavigationAssetSceneIntegration DEPENDENCIES HoroNavigationSceneIntegration HoroAssets)
horo_allow_target_dependencies(TARGET HoroNavigationContentIntegration DEPENDENCIES HoroApplication HoroNavigationAssetSceneIntegration)
horo_allow_target_dependencies(TARGET HoroXRApi DEPENDENCIES HoroFoundation HoroRenderApi)
horo_allow_target_dependencies(TARGET HoroXRRuntime DEPENDENCIES HoroXRApi)
horo_allow_target_dependencies(TARGET HoroXRInputBindings DEPENDENCIES HoroXRApi HoroInput)
if(HORO_BUILD_XR_OPENXR)
    horo_allow_target_dependencies(TARGET HoroXROpenXRHostInterface DEPENDENCIES HoroXRRuntime HoroXRInputBindings HoroPlatform)
    horo_allow_target_dependencies(TARGET HoroXROpenXR DEPENDENCIES HoroXROpenXRHostInterface)
endif()

option(HORO_VERIFY_XR_DISABLED "Verify that the optional native XR graph and SDK population are absent" OFF)
if(HORO_VERIFY_XR_DISABLED)
    if(HORO_BUILD_XR_OPENXR)
        message(FATAL_ERROR "XR-disabled verification requires HORO_BUILD_XR_OPENXR=OFF")
    endif()
    foreach(target HoroXROpenXR HoroXROpenXRHostInterface HoroOpenXRHeaders)
        if(TARGET ${target})
            message(FATAL_ERROR "Native XR target is present in the disabled graph: ${target}")
        endif()
    endforeach()
    FetchContent_GetProperties(horo_openxr_sdk POPULATED horo_xr_sdk_populated)
    if(horo_xr_sdk_populated)
        message(FATAL_ERROR "OpenXR SDK was populated by the disabled composition")
    endif()
    message(STATUS "XR disabled: native targets and SDK population are absent; cached sources may remain")
endif()
horo_allow_target_dependencies(TARGET HoroTerrainApi DEPENDENCIES HoroFoundation)
horo_allow_target_dependencies(TARGET HoroTerrainRender DEPENDENCIES HoroTerrainApi HoroRenderApi)
horo_allow_target_dependencies(TARGET HoroTerrainImport DEPENDENCIES HoroTerrainApi HoroAssets)
horo_allow_target_dependencies(TARGET HoroTerrainAuthoring DEPENDENCIES HoroTerrainImport)
horo_allow_target_dependencies(TARGET HoroTerrainCook DEPENDENCIES HoroTerrainImport)
horo_allow_target_dependencies(TARGET HoroTerrainProducerIntegration DEPENDENCIES HoroTerrainCook HoroWorldStreaming)
horo_allow_target_dependencies(TARGET HoroTerrainStreaming DEPENDENCIES HoroTerrainApi HoroWorldStreaming)
horo_allow_target_dependencies(TARGET HoroTerrainRuntime DEPENDENCIES HoroTerrainApi HoroFoundation)
horo_allow_target_dependencies(TARGET HoroNavigationNull DEPENDENCIES HoroNavigationApi)
horo_allow_target_dependencies(TARGET HoroNavigationRecastDetour DEPENDENCIES HoroNavigationApi)
horo_allow_target_dependencies(TARGET HoroNavigationCrowdDetour DEPENDENCIES HoroNavigationRuntime)
horo_allow_target_dependencies(TARGET HoroWorldStreaming DEPENDENCIES HoroFoundation HoroAssets)
horo_allow_target_dependencies(TARGET HoroSceneCellPayload DEPENDENCIES HoroRuntimeScene HoroWorldStreaming)
horo_allow_target_dependencies(TARGET HoroPrefab DEPENDENCIES HoroFoundation HoroAssets HoroGameplayApi)
horo_allow_target_dependencies(TARGET HoroPrefabRuntime DEPENDENCIES HoroPrefab HoroRuntimeScene)
horo_allow_target_dependencies(TARGET HoroPrefabAuthoring DEPENDENCIES HoroPrefab HoroApplication)
horo_allow_target_dependencies(TARGET HoroPrefabSceneExpansion DEPENDENCIES HoroPrefabAuthoring HoroRuntimeScene)
horo_allow_target_dependencies(TARGET HoroInput DEPENDENCIES HoroFoundation)
horo_allow_target_dependencies(TARGET HoroInputSdl DEPENDENCIES HoroInput)

horo_allow_target_dependencies(TARGET HoroGameplayApi DEPENDENCIES HoroFoundation HoroNetworkApi HoroSaveApi)
horo_allow_target_dependencies(TARGET HoroRuntimeScene
    DEPENDENCIES HoroFoundation HoroRuntime HoroAssets HoroGameplayApi HoroNavigationApi HoroPhysicsModel HoroSceneModel HoroRuntimeUi HoroAI HoroAudioApi)
horo_allow_target_dependencies(TARGET HoroGameplayRuntime
    DEPENDENCIES HoroGameplayApi HoroRuntimeScene HoroRuntime HoroGameplayPhysicsIntegration HoroPrefabRuntime)
horo_allow_target_dependencies(TARGET HoroGameplayModuleHost
    DEPENDENCIES HoroGameplayRuntime HoroPlatform HoroGameplayPhysicsIntegration)
horo_allow_target_dependencies(TARGET HoroGameplayBuild
    DEPENDENCIES HoroFoundation HoroPlatform HoroGameplayModuleHost)
horo_allow_target_dependencies(TARGET HoroGameplayLua DEPENDENCIES HoroGameplayRuntime HoroGameplayPhysicsIntegration)

horo_allow_target_dependencies(TARGET HoroRenderApi DEPENDENCIES HoroFoundation)
horo_allow_target_dependencies(TARGET HoroSceneRenderExtraction DEPENDENCIES HoroRuntimeScene HoroRenderApi)
horo_allow_target_dependencies(TARGET HoroShaderCompilerToolchain DEPENDENCIES HoroRenderApi HoroPlatform)
horo_allow_target_dependencies(TARGET HoroShaderBuild DEPENDENCIES HoroFoundation HoroRenderApi)
horo_allow_target_dependencies(TARGET HoroRenderBackendRegistry DEPENDENCIES HoroRenderApi)
horo_allow_target_dependencies(TARGET HoroRenderFrontend
    DEPENDENCIES HoroRenderApi HoroRenderBackendRegistry HoroRuntimeUi)
horo_allow_target_dependencies(TARGET HoroSceneModel DEPENDENCIES HoroFoundation HoroGameplayApi HoroRuntimeUi HoroAudioApi)
horo_allow_target_dependencies(TARGET HoroRenderNull DEPENDENCIES HoroRenderApi)
horo_allow_target_dependencies(TARGET HoroRenderOpenGL)
horo_allow_target_dependencies(TARGET HoroRenderMetal)
horo_allow_target_dependencies(TARGET HoroRenderVulkan)
horo_allow_target_dependencies(TARGET HoroRenderD3D12 DEPENDENCIES HoroRenderApi)

horo_allow_target_dependencies(TARGET HoroSceneSourceModel
    DEPENDENCIES HoroFoundation HoroAI HoroPrefab HoroRuntimeScene)
horo_allow_target_dependencies(TARGET HoroSceneSource DEPENDENCIES HoroSceneSourceModel)
horo_allow_target_dependencies(TARGET HoroSceneRuntimeConversion
    DEPENDENCIES HoroSceneSource HoroPrefabSceneExpansion HoroRuntimeScene HoroSceneSourceCodecInternal)
horo_allow_target_dependencies(TARGET HoroSceneCook DEPENDENCIES HoroRuntimeScene HoroSceneSourceCodecInternal)
horo_allow_target_dependencies(TARGET HoroPrefabCookHost
    DEPENDENCIES HoroApplication HoroAssets HoroEditorServices HoroPackageSecurity HoroPrefabAuthoring HoroSceneCook HoroSceneRuntimeConversion)
horo_allow_target_dependencies(TARGET HoroSceneSourceCodecInternal DEPENDENCIES HoroSceneSource)
horo_allow_target_dependencies(TARGET HoroEditorModel
    DEPENDENCIES HoroFoundation HoroAI HoroPrefab HoroPrefabAuthoring HoroPrefabSceneExpansion HoroSceneModel HoroRuntimeScene HoroSceneSourceModel HoroSceneSourceCodecInternal HoroSceneRuntimeConversion)
horo_allow_target_dependencies(TARGET HoroEditorViewportScene DEPENDENCIES HoroEditorModel)
horo_allow_target_dependencies(TARGET HoroEditorViewportResources
    DEPENDENCIES HoroEditorViewportScene HoroRenderFrontend)
horo_allow_target_dependencies(TARGET HoroEditorRenderExtraction
    DEPENDENCIES HoroEditorModel HoroEditorViewportScene)
horo_allow_target_dependencies(TARGET HoroEditorServices
    DEPENDENCIES HoroAudioApi HoroAudioFrontend
        HoroFoundation HoroHostErrors HoroCinematicRuntime
        HoroNetworkApi
        HoroNetworkDebuggerApplication
        HoroApplication
        HoroPlatform
        HoroRuntimeUi
        HoroEditorModel
        HoroGameplayModuleHost
        HoroGameplayBuild
        HoroInput
        HoroProjectMigrations
        HoroSceneSourceCodecInternal
        HoroAssets)
horo_allow_target_dependencies(TARGET HoroEditorViewportOpenGL
    DEPENDENCIES HoroEditorViewportScene HoroEditorViewportResources HoroRenderOpenGL HoroRenderFrontend)
horo_allow_target_dependencies(TARGET HoroEditorViewportMetal
    DEPENDENCIES HoroEditorViewportScene HoroEditorViewportResources HoroRenderMetal HoroRenderFrontend)
horo_allow_target_dependencies(TARGET HoroGui
    DEPENDENCIES HoroPlayTopologyApplication HoroEditorServices HoroCinematicCameraRuntime HoroFoundation HoroEditorRenderExtraction HoroExtensions)
horo_allow_target_dependencies(TARGET HoroExtensions
    DEPENDENCIES HoroFoundation HoroPlatform HoroAssets HoroSecurity)

# Executables are composition roots and may select any production module.
horo_allow_target_dependencies(TARGET HoroHostModuleComposition DEPENDENCIES HoroFoundation HoroPlatformServices)
horo_allow_target_dependencies(TARGET HoroNetworkProductHost
    DEPENDENCIES HoroNetworkRuntime HoroRuntimeScene HoroPhysics HoroRuntime HoroGameplayModuleHost HoroGameplayLua HoroGameplayPhysicsIntegration HoroPrefabRuntime HoroNetworkDebuggerApplication)
horo_allow_target_dependencies(TARGET HoroUiAnimationRuntimeIntegration DEPENDENCIES HoroRuntime HoroRuntimeUi)
horo_allow_target_dependencies(TARGET horo-engine
    DEPENDENCIES HoroApplication HoroExtensions HoroHostModuleComposition HoroNetworkProductHost HoroNetworkTransportGNS HoroCliHost HoroRuntime HoroMcpController HoroPlatform)
horo_allow_target_dependencies(TARGET horo-extension-validate DEPENDENCIES HoroExtensions)
horo_allow_target_dependencies(TARGET HoroExtensionSdkValidatorStage DEPENDENCIES horo-extension-validate)
horo_allow_target_dependencies(TARGET horo-extension-conformance DEPENDENCIES HoroExtensions)
horo_allow_target_dependencies(TARGET HoroExtensionSdkConformanceStage DEPENDENCIES horo-extension-conformance)
horo_allow_target_dependencies(TARGET horo-package DEPENDENCIES HoroPackageSecurity HoroPackages HoroSecurity)
horo_allow_target_dependencies(TARGET HoroExtensionSdkPackageStage DEPENDENCIES horo-package)
horo_allow_target_dependencies(TARGET HoroExtensionAuthorCiStage
    DEPENDENCIES HoroExtensionSdkValidatorStage HoroExtensionSdkConformanceStage HoroExtensionSdkPackageStage)
horo_allow_target_dependencies(TARGET HoroEditor
    DEPENDENCIES
        HoroShaderBuild
        HoroGui
        HoroEditorServices
        HoroEditorRenderExtraction
        HoroRenderFrontend
        HoroRuntime
        HoroRuntimeScene
        HoroPhysicsSceneIntegration
        HoroExtensions
        HoroPlatform
        HoroProjectMigrations
        HoroUserStateMigration
        HoroInputSdl
        HoroUpdateDiscovery
        HoroUpdateDownload
        HoroOpenTelemetry
        HoroEditorViewportOpenGL
        HoroEditorViewportMetal
        HoroHostModuleComposition)

# These edges predate the target-level architecture. Each exception is tied to
# an active roadmap owner and must disappear when that ticket resolves the
# corresponding boundary.
horo_allow_temporary_dependency_exception(
    TARGET HoroSceneModel
    DEPENDENCY HoroRenderApi
    OWNER "Rendering"
    REMOVAL_TICKET "#275"
    REASON "Primitive mesh contracts still reuse renderer mesh types")
horo_allow_temporary_dependency_exception(
    TARGET HoroEditorServices
    DEPENDENCY HoroGameplayLua
    OWNER "Gameplay"
    REMOVAL_TICKET "#61"
    REASON "Editor services still select the concrete Lua gameplay adapter")
horo_allow_temporary_dependency_exception(
    TARGET HoroRenderNull
    DEPENDENCY HoroRenderBackendRegistry
    OWNER "Rendering"
    REMOVAL_TICKET "#62"
    REASON "Static backend registration predates the renderer module host")
horo_allow_temporary_dependency_exception(
    TARGET HoroRenderOpenGL
    DEPENDENCY HoroRenderBackendRegistry
    OWNER "Rendering"
    REMOVAL_TICKET "#62"
    REASON "Static backend registration predates the renderer module host")
horo_allow_temporary_dependency_exception(
    TARGET HoroRenderMetal
    DEPENDENCY HoroRenderBackendRegistry
    OWNER "Rendering"
    REMOVAL_TICKET "#62"
    REASON "Static backend registration predates the renderer module host")
horo_allow_temporary_dependency_exception(
    TARGET HoroRenderVulkan
    DEPENDENCY HoroRenderBackendRegistry
    OWNER "Rendering"
    REMOVAL_TICKET "#62"
    REASON "Static backend registration predates the renderer module host")

option(HORO_VERIFY_NETWORK_DISABLED "Verify that no production network dependency is populated or configured" OFF)
if(HORO_VERIFY_NETWORK_DISABLED)
    if(HORO_BUILD_NETWORK_GNS)
        message(FATAL_ERROR "Network-disabled verification requires HORO_BUILD_NETWORK_GNS=OFF")
    endif()
    foreach(target HoroNetworkTransportGNS GameNetworkingSockets_s libprotobuf protoc c-ares::cares)
        if(TARGET ${target})
            message(FATAL_ERROR "Production network target is present in the disabled graph: ${target}")
        endif()
    endforeach()
    # Cached source directories may exist from the ON build. The current
    # configure must not populate them or add their targets when networking is OFF.
    foreach(dependency horo_gns horo_protobuf horo_cares)
        FetchContent_GetProperties(${dependency} POPULATED populated)
        if(populated)
            message(FATAL_ERROR "Production network dependency was populated while disabled: ${dependency}")
        endif()
    endforeach()
    message(STATUS "Network disabled: production targets and FetchContent dependencies are absent")
endif()
