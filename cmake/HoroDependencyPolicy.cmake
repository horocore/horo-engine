include_guard(GLOBAL)

# This manifest is intentionally explicit: changing a first-party link edge must
# update the architecture policy in the same review.
horo_allow_target_dependencies(TARGET HoroFoundation)
horo_allow_target_dependencies(TARGET HoroSecurity DEPENDENCIES HoroFoundation)
horo_allow_target_dependencies(TARGET HoroCliHost DEPENDENCIES HoroFoundation)
horo_allow_target_dependencies(TARGET HoroOpenTelemetry DEPENDENCIES HoroFoundation)
horo_allow_target_dependencies(TARGET HoroPlatform DEPENDENCIES HoroFoundation HoroSecurity)
horo_allow_target_dependencies(TARGET HoroPlatformServices DEPENDENCIES HoroFoundation)
horo_allow_target_dependencies(TARGET HoroPackages DEPENDENCIES HoroFoundation)
horo_allow_target_dependencies(TARGET HoroApplication DEPENDENCIES HoroFoundation)
horo_allow_target_dependencies(TARGET HoroProjectMigrations DEPENDENCIES HoroApplication)
horo_allow_target_dependencies(TARGET HoroRuntime DEPENDENCIES HoroFoundation)
horo_allow_target_dependencies(TARGET HoroRuntimeUi DEPENDENCIES HoroFoundation HoroAssets)
horo_allow_target_dependencies(TARGET HoroAssets DEPENDENCIES HoroFoundation)
horo_allow_target_dependencies(TARGET HoroNetworkApi DEPENDENCIES HoroFoundation)
horo_allow_target_dependencies(TARGET HoroNetworkRuntime DEPENDENCIES HoroNetworkApi HoroRuntimeScene)
horo_allow_target_dependencies(TARGET HoroNetworkTransportNull DEPENDENCIES HoroNetworkApi)
horo_allow_target_dependencies(TARGET HoroAudioApi DEPENDENCIES HoroFoundation HoroAssets)
horo_allow_target_dependencies(TARGET HoroAudioDsp DEPENDENCIES HoroAudioApi)
horo_allow_target_dependencies(TARGET HoroAudioMemory DEPENDENCIES HoroAudioApi)
horo_allow_target_dependencies(TARGET HoroAudioCommands DEPENDENCIES HoroAudioMemory)
horo_allow_target_dependencies(TARGET HoroAudioBackendContract DEPENDENCIES HoroAudioApi)
horo_allow_target_dependencies(TARGET HoroAudioNull DEPENDENCIES HoroAudioBackendContract HoroAudioCommands)
horo_allow_target_dependencies(TARGET HoroPhysics DEPENDENCIES HoroFoundation HoroAssets)
horo_allow_target_dependencies(TARGET HoroPhysicsSceneIntegration DEPENDENCIES HoroPhysics HoroRuntimeScene)
horo_allow_target_dependencies(TARGET HoroAI DEPENDENCIES HoroFoundation)
horo_allow_target_dependencies(TARGET HoroAnimationApi DEPENDENCIES HoroFoundation HoroAssets)
horo_allow_target_dependencies(TARGET HoroPCG DEPENDENCIES HoroFoundation)
horo_allow_target_dependencies(TARGET HoroVfxApi DEPENDENCIES HoroFoundation HoroAssets)
horo_allow_target_dependencies(TARGET HoroDestructionApi DEPENDENCIES HoroFoundation HoroAssets)
horo_allow_target_dependencies(TARGET HoroCinematicModel DEPENDENCIES HoroFoundation HoroAssets)
horo_allow_target_dependencies(TARGET HoroCinematicRuntime DEPENDENCIES HoroCinematicModel)
horo_allow_target_dependencies(TARGET HoroNavigationApi DEPENDENCIES HoroFoundation)
horo_allow_target_dependencies(TARGET HoroNavigationRuntime DEPENDENCIES HoroNavigationApi)
horo_allow_target_dependencies(TARGET HoroXRApi DEPENDENCIES HoroFoundation HoroRenderApi)
horo_allow_target_dependencies(TARGET HoroTerrainApi DEPENDENCIES HoroFoundation)
horo_allow_target_dependencies(TARGET HoroNavigationNull DEPENDENCIES HoroNavigationApi)
horo_allow_target_dependencies(TARGET HoroNavigationRecastDetour DEPENDENCIES HoroNavigationApi)
horo_allow_target_dependencies(TARGET HoroWorldStreaming DEPENDENCIES HoroFoundation HoroAssets)
horo_allow_target_dependencies(TARGET HoroPrefab DEPENDENCIES HoroFoundation HoroAssets HoroGameplayApi)
horo_allow_target_dependencies(TARGET HoroPrefabAuthoring DEPENDENCIES HoroPrefab HoroApplication)
horo_allow_target_dependencies(TARGET HoroInput DEPENDENCIES HoroFoundation)
horo_allow_target_dependencies(TARGET HoroInputSdl DEPENDENCIES HoroInput)

horo_allow_target_dependencies(TARGET HoroGameplayApi DEPENDENCIES HoroFoundation)
horo_allow_target_dependencies(TARGET HoroRuntimeScene
    DEPENDENCIES HoroFoundation HoroRuntime HoroAssets HoroGameplayApi HoroNavigationApi HoroSceneModel HoroRuntimeUi)
horo_allow_target_dependencies(TARGET HoroGameplayRuntime
    DEPENDENCIES HoroGameplayApi HoroRuntimeScene)
horo_allow_target_dependencies(TARGET HoroGameplayModuleHost
    DEPENDENCIES HoroGameplayRuntime HoroPlatform)
horo_allow_target_dependencies(TARGET HoroGameplayBuild
    DEPENDENCIES HoroFoundation HoroPlatform HoroGameplayModuleHost)
horo_allow_target_dependencies(TARGET HoroGameplayLua DEPENDENCIES HoroGameplayRuntime)

horo_allow_target_dependencies(TARGET HoroRenderApi DEPENDENCIES HoroFoundation)
horo_allow_target_dependencies(TARGET HoroShaderCompilerToolchain DEPENDENCIES HoroRenderApi HoroPlatform)
horo_allow_target_dependencies(TARGET HoroRenderBackendRegistry DEPENDENCIES HoroRenderApi)
horo_allow_target_dependencies(TARGET HoroRenderFrontend
    DEPENDENCIES HoroRenderApi HoroRenderBackendRegistry HoroRuntimeUi)
horo_allow_target_dependencies(TARGET HoroSceneModel DEPENDENCIES HoroFoundation HoroRuntimeUi)
horo_allow_target_dependencies(TARGET HoroRenderNull DEPENDENCIES HoroRenderApi)
horo_allow_target_dependencies(TARGET HoroRenderOpenGL)
horo_allow_target_dependencies(TARGET HoroRenderMetal)

horo_allow_target_dependencies(TARGET HoroEditorModel
    DEPENDENCIES HoroFoundation HoroPrefab HoroSceneModel HoroRuntimeScene)
horo_allow_target_dependencies(TARGET HoroEditorViewportScene DEPENDENCIES HoroEditorModel)
horo_allow_target_dependencies(TARGET HoroEditorViewportResources
    DEPENDENCIES HoroEditorViewportScene HoroRenderFrontend)
horo_allow_target_dependencies(TARGET HoroEditorRenderExtraction
    DEPENDENCIES HoroEditorModel HoroEditorViewportScene)
horo_allow_target_dependencies(TARGET HoroEditorServices
    DEPENDENCIES
        HoroFoundation
        HoroApplication
        HoroPlatform
        HoroEditorModel
        HoroGameplayModuleHost
        HoroGameplayBuild
        HoroInput
        HoroProjectMigrations
        HoroAssets)
horo_allow_target_dependencies(TARGET HoroEditorViewportOpenGL
    DEPENDENCIES HoroEditorViewportScene HoroEditorViewportResources HoroRenderOpenGL HoroRenderFrontend)
horo_allow_target_dependencies(TARGET HoroEditorViewportMetal
    DEPENDENCIES HoroEditorViewportScene HoroEditorViewportResources HoroRenderMetal HoroRenderFrontend)
horo_allow_target_dependencies(TARGET HoroGui
    DEPENDENCIES HoroEditorServices HoroFoundation HoroEditorRenderExtraction HoroExtensions)
horo_allow_target_dependencies(TARGET HoroExtensions
    DEPENDENCIES HoroFoundation HoroPlatform HoroAssets HoroSecurity)

# Executables are composition roots and may select any production module.
horo_allow_target_dependencies(TARGET HoroHostModuleComposition DEPENDENCIES HoroFoundation)
horo_allow_target_dependencies(TARGET horo-engine DEPENDENCIES HoroApplication HoroHostModuleComposition)
horo_allow_target_dependencies(TARGET horo-extension-validate DEPENDENCIES HoroExtensions)
horo_allow_target_dependencies(TARGET HoroExtensionSdkValidatorStage DEPENDENCIES horo-extension-validate)
horo_allow_target_dependencies(TARGET HoroEditor
    DEPENDENCIES
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
        HoroInputSdl
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
