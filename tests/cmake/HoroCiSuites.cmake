# CI builds capabilities; the test registry owns their executable membership.
option(HORO_CI_SONAR_COVERAGE "Register the filtered Sonar UI coverage run" OFF)
option(HORO_CI_ISOLATE_TEST_EXECUTABLES "Isolate discovered executable suites for bounded CTest parallelism" OFF)

# Audited headless suites with process-local state and temporary-file fixtures.
# Explicit case resource locks still apply after private directories are assigned.
set(HORO_CI_CASE_ISOLATED_TARGETS HoroPhysicsTests)

set(HORO_CI_AUDIO_TARGETS
    HoroAudioRealtimeSafetyHarnessTests
    HoroAudioWatchdogTests
    HoroAudioNullTests
    HoroAudioDspTests
    HoroCoreAudioDspTests
    HoroAudioCommandTests
    HoroAudioMixerTests
    HoroAudioEditorPreviewTests
    HoroAudioComponentPreviewHostTests
    HoroAudioFrontendPublicHeaderConsumer
)

# The full Windows suite remains disabled. Preserve all existing focused checks
# in one headless Debug build, including their public-header compile consumers.
set(HORO_CI_WINDOWS_TARGETS
    HoroFoundationJsonTests
    HoroProjectCollisionSettingsTests
    HoroCollisionSettingsPublicConsumer
    HoroPublicationLockFoundationConsumer
    HoroProjectCollisionSettingsPublicHeaderConsumer
    HoroWorkspaceLayoutPersistenceTests
    HoroWorkspaceRecoveryPublicHeaderConsumer
    HoroNavigationProjectSettingsTests
    HoroNavigationProjectSettingsPublicConsumer
    HoroPhysicsTests
    HoroCharacterSceneProductTests
    HoroEditorPlaySessionControllerTests
    HoroProjectPhysicsCaptureBoundaryConsumer
    HoroPrefabSceneCookHostTests
    HoroPrefabAIGroupTests
    HoroPrefabBuiltinProjectionPublicConsumer
    HoroPhysicsTriggerVolumeHostContractConsumer
    HoroPhysicsTriggerVolumeHostPublicHeaderConsumer
    HoroTriggerVolumeConversionPublicHeaderConsumer
    HoroSceneCookPublicHeaderConsumer
    HoroPhysicsSceneIntegrationPublicHeaderConsumer
    HoroPrefabCookHostPublicHeaderConsumer
    HoroPhysicsPublicHeaderConsumer
    HoroGameplayPhysicsTests
    HoroGameplayRuntimeTests
    HoroGameModuleHostTests
    HoroLuaBehaviorTests
    HoroGameplayCharacterIntegrationPublicHeaderConsumer
    HoroGameplayPhysicsIntegrationPublicHeaderConsumer
    HoroGameplayModuleHostPublicHeaderConsumer
    HoroGameplayApiPublicHeaderConsumer
    HoroGameplayRuntimePublicHeaderConsumer
    HoroGameplayLuaPublicHeaderConsumer
    HoroGameplayPhysicsPublicHeaderConsumer
    HoroGameplayPhysicsSdkHostConsumer

    HoroWorldStreamingTests
    HoroWorldStreamingPublicHeaderConsumer
    HoroStreamingSourcePlannerBenchmark
    HoroDestructionSaveTests
    HoroDestructionSavePublicConsumer
    HoroDestructionSaveIntegrationPublicHeaderConsumer
    HoroDestructionApiPublicHeaderConsumer
    HoroDestructionReplicationTests
    HoroDestructionReplicationPublicHeaderConsumer
    HoroRuntimeSceneCellPayloadTests
    HoroSceneCellPayloadPublicHeaderConsumer
    HoroWorldCookIntegrationTests
    HoroWorldCookIntegrationConsumer
    HoroWorldCookIntegrationPublicHeaderConsumer
    HoroWorldStreamingPublicHeaderConsumer
    HoroWorldArtifactCodecTests
    HoroWorldArtifactCodecConsumer
    HoroAnimationApiTests
    HoroAnimationGraphPublicHeaderConsumer
    HoroLightCullingTests
    HoroLightFramePoolTests
    HoroAITaskSchedulerTests
    HoroAIFoundationQualificationTests
    HoroAITaskSchedulerPublicConsumer
    HoroGameplayAiTests
    HoroGameplayAiContractConsumer
    HoroGameplayAiIntegrationPublicHeaderConsumer
    HoroGameplayAiScriptBridgePublicHeaderConsumer
    HoroGameplayApiPublicHeaderConsumer
    HoroGameplayModuleHostPublicHeaderConsumer
    HoroExtensionsPublicHeaderConsumer
    HoroAITests
    HoroAISightTests
    HoroAIPublicHeaderConsumer
    HoroAISceneIntegrationPublicHeaderConsumer
    HoroAISightIntegrationPublicHeaderConsumer
    HoroGameplayPerceptionIntegrationPublicHeaderConsumer
    HoroMaterialBindingTests
    HoroMaterialBindingPublicHeaderConsumer
    HoroMultiviewSceneExtractionPublicHeaderConsumer
    HoroD3D12InitializationTests
    HoroD3D12ResourceTests
    HoroPlayTopologyTests
    HoroPlayTopologyPublicHeaderConsumer
    HoroNetworkDebuggerTests
    HoroNetworkDebuggerPublicHeaderConsumer
    HoroTerrainAuthoringTests
    HoroTerrainAuthoringPublicHeaderConsumer
    HoroPCGTests
    HoroPCGTerrainAdapterTests
    HoroPCGSceneAdapterTests
    HoroPCGSceneAdapterPublicHeaderConsumer
    HoroPCGSceneOutputContractConsumer
    HoroPCGPublicHeaderConsumer
    HoroPCGTerrainAdapterPublicHeaderConsumer
    HoroPCGSceneBakeTests
    HoroPCGSceneBakeContractConsumer
    HoroPCGSceneBakePublicHeaderConsumer
    HoroEditorModelPublicHeaderConsumer
    HoroWorldBakeApplicationTests
    HoroWorldBakeApplicationPublicHeaderConsumer
    HoroPartitionedWorldWorkspaceTests
    HoroInspectorBehaviorBindingTests
    HoroInspectorBehaviorPublicHeaderConsumer
    HoroPCGDocumentTests
    HoroPCGDocumentPublicHeaderConsumer
    HoroMixerDocumentTests
    ${HORO_CI_AUDIO_TARGETS}
    HoroCliCommandRegistryTests
    HoroCliOutputPublicHeaderConsumer
    HoroCliProductionOutputContract
    HoroCliMcpServeTests
    HoroPlatformTests
    HoroImmutableFileStorageTests
    HoroUpdateZipPackageProducerTests
    HoroVfxApiTests
    HoroVfxRenderExtractionTests
    HoroVfxApiPublicHeaderConsumer
    HoroRenderApiPublicHeaderConsumer
    HoroRenderFrontendPublicHeaderConsumer
    HoroRuntimeScenePublicHeaderConsumer
    HoroCinematicModelTests
    HoroFractureDocumentTests
    HoroFractureDocumentPublicHeaderConsumer
    HoroCinematicRuntimeTests
    HoroCameraCutRuntimeTests
    HoroCameraCutPublicHeaderConsumer
    HoroCinematicPropertyIntegrationTests
    HoroCinematicModelPublicHeaderConsumer
    HoroCinematicRuntimePublicHeaderConsumer
    HoroEditorServicesPublicHeaderConsumer
    HoroEditorSurfaceIdentityTests
    HoroSourceFileOpenServiceTests
    HoroPrefabTests
    HoroPrefabOverrideModelPublicHeaderConsumer
    HoroPrefabAuthoringPublicHeaderConsumer
    HoroSceneSourceModelPublicHeaderConsumer
    HoroSceneSourcePublicHeaderConsumer
    HoroSceneRuntimeConversionPublicHeaderConsumer
    HoroSceneRuntimeConversionTests
    HoroSceneDocumentTests
    HoroPrefabSceneExpansionTests
    HoroPrefabSceneExpansionContractConsumer
    HoroAssetRegistryTests
    HoroAssetCookServiceTests
    HoroAssetsPublicHeaderConsumer
    HoroInputTests
    HoroAndroidInputTests
    HoroAndroidInputIntegrationPublicHeaderConsumer
    HoroCharacterInputTests
    HoroCharacterInputPublicHeaderConsumer
    HoroInputSdlTests
    HoroRuntimeUiInputTests
    HoroRuntimeUiInputDebuggerTests
    HoroRuntimeUiInputDebuggerContractConsumer
    HoroRuntimeUiAnimationIntegrationTests
    HoroRuntimeUiAnimationIntegrationPublicHeaderConsumer
    HoroRuntimeUiHitTestingTests
    HoroRuntimeUiEventDispatchTests
    HoroRuntimeUiPointerPublicHeaderConsumer
    HoroRuntimeUiTextUnicodeTests
    HoroRuntimeUiUnicodeStartupTests
    HoroRuntimeUiUnicodeLifecycleTests
    HoroRuntimeUiTextShapingTests
    HoroRuntimeUiTextLayoutTests
    HoroRuntimeUiPublicHeaderConsumer
    HoroRuntimeUiOverlayLifecycleTests
    HoroRuntimeUiHudAssociationTests
    HoroRuntimeUiHudPublicHeaderConsumer
    HoroRuntimeUiSceneReconciliationTests
    HoroRuntimeUiSceneReconciliationPublicHeaderConsumer
    HoroGameplayRuntimeUiSdkHostConsumer
    HoroRuntimeUiCapabilityTests
    HoroRuntimeUiHotReloadTests
    HoroRuntimeUiHotReloadPublicHeaderConsumer
    HoroSceneIdentityPublicHeaderConsumer
    HoroSceneIdentityContractConsumer
    HoroRuntimeUiScreenTransitionTests
    HoroRuntimeUiScreenTransitionPublicHeaderConsumer
    HoroInputPublicHeaderConsumer
    HoroExtensionManagerTests
    HoroEditorActivityBoundaryTests

    HoroPackageLifecycleTests
    HoroPackageApplicationTests
    HoroPackageApplicationPublicHeaderConsumer
    HoroCliPackageAdaptersPublicHeaderConsumer
    HoroMcpPackageAdaptersPublicHeaderConsumer
    HoroEditorPackageAdaptersPublicHeaderConsumer
    HoroPackageLifecyclePublicHeaderConsumer
    HoroMcpSessionTests
    HoroMcpSessionPublicHeaderConsumer
    HoroProjectCodeQueryTests
    HoroProjectReadFilesTests
    HoroMcpProjectCodeQueryTests
    HoroSourceDocumentCodeQueryTests
    HoroProjectReadFilesApiPublicHeaderConsumer
    HoroPlatformProjectReadFilesPublicHeaderConsumer
    HoroProjectCodeQueryPublicHeaderConsumer
    HoroMcpProjectCodeQueryPublicHeaderConsumer
    HoroSourceDocumentCodeQueryPublicHeaderConsumer
    HoroRuntimeSaveIdentityTests
    HoroRuntimeSaveRootResolverTests
    HoroRuntimeSaveStorageQualificationTests
    HoroSaveStorageUserStateQualificationTests
    HoroRuntimeSaveFilesystemLockTests
    HoroRuntimeSaveSlotCommitTransactionTests
    HoroRuntimeSaveSlotLifecycleTests
    HoroSaveSlotLifecyclePublicHeaderConsumer
    HoroRuntimeSaveRestoreTransactionTests
    HoroSaveGameplayCheckpointPublicHeaderConsumer
    HoroRuntimeSaveEventTriggersTests
    HoroSaveEventTriggersPublicHeaderConsumer
    HoroRuntimePublicHeaderConsumer
    HoroTerrainSourceArtifactTests
    HoroTerrainSourceArtifactPublicHeaderConsumer
    HoroTerrainPayloadManifestTests
    HoroTerrainPayloadManifestPublicHeaderConsumer
    HoroTerrainProducerSnapshotTests
    HoroTerrainProducerSnapshotPublicHeaderConsumer
    HoroTerrainRenderIntegrationTests
    HoroTerrainRenderIntegrationPublicHeaderConsumer
    HoroNavigationRuntimeTests
    HoroNavigationTransportReservationTests
    HoroNavigationCharacterAdapterTests
    HoroNavigationRuntimeQualificationTests
    HoroNavigationCharacterAdapterPublicConsumer
    HoroNavigationCharacterIntegrationPublicHeaderConsumer
    HoroNavigationCoordinatorPublicConsumer
    HoroGameplayNavigationIntegrationPublicHeaderConsumer
    HoroGameplayApiPublicHeaderConsumer
    HoroGameplayRuntimePublicHeaderConsumer
    HoroGameplayModuleHostPublicHeaderConsumer
    HoroGameplayLuaPublicHeaderConsumer
    HoroNavigationRuntimePublicHeaderConsumer
    HoroGameplayNavigationTests
    HoroGameplayNavigationPublicConsumer
    HoroNavigationBakeServiceTests
)

set(HORO_CI_NAVIGATION_TARGETS
    HoroNavigationProjectSettingsTests
    HoroNavigationProjectSettingsPublicConsumer
    HoroGameplayNavigationIntegrationPublicHeaderConsumer
    HoroGameplayApiPublicHeaderConsumer
    HoroGameplayRuntimePublicHeaderConsumer
    HoroGameplayModuleHostPublicHeaderConsumer
    HoroGameplayLuaPublicHeaderConsumer
    HoroNavigationRuntimePublicHeaderConsumer
    HoroGameplayNavigationTests
    HoroGameplayNavigationPublicConsumer
    HoroNavigationRuntime
    HoroNavigationNull
    HoroNavigationRecastDetour
    HoroNavigationApiPublicHeaderConsumer
    HoroNavigationCoordinatorPublicConsumer
    HoroNavigationNullPublicHeaderConsumer
    HoroNavigationRecastDetourPublicHeaderConsumer
    HoroNavigationBakeServicePublicHeaderConsumer
    HoroNavigationBakeDiagnosticsPublicConsumer
    HoroNavigationCharacterAdapterTests
    HoroNavigationRuntimeQualificationTests
    HoroNavigationCharacterAdapterPublicConsumer
    HoroNavigationCharacterIntegrationPublicHeaderConsumer
)

set(HORO_CI_NETWORK_TARGETS
    HoroNetworkTransportGnsTests
    HoroNetworkApiPublicHeaderConsumer
)

# These editor-labelled suites were explicitly run by the coverage workflow.
set(HORO_SONAR_EDITOR_TARGETS
    HoroSourceSyntaxTests
    HoroEditorPlaySessionControllerTests
    HoroSettingsModalLifecycleTests
    HoroEditorWorkspaceControllerTests
    HoroNavigationProjectSettingsTests
    HoroNavigationProjectSettingsPublicConsumer
    HoroPackageApplicationTests
    HoroFractureDocumentTests
    HoroTerrainAuthoringTests
    HoroMixerDocumentTests
    HoroCameraCutEditorIntegrationTests
    HoroCinematicPropertyIntegrationTests
    HoroConfiguredEditorUpdateBackendTests
    HoroSourceDocumentCodeQueryTests
)

function(horo_ci_catch_labels target)
    if(target IN_LIST HORO_CI_AUDIO_TARGETS)
        list(APPEND ARG_LABELS realtime-safety)
        list(APPEND ARG_PROPERTIES TIMEOUT 90)
    endif()
    if(target IN_LIST HORO_CI_WINDOWS_TARGETS)
        list(APPEND ARG_LABELS ci-windows)
    endif()
    if(NOT "${ARG_LABELS}" MATCHES "(^|;)(gui|editor)(;|$)" OR target IN_LIST HORO_SONAR_EDITOR_TARGETS)
        list(APPEND ARG_LABELS sonar-coverage)
    endif()
    list(REMOVE_DUPLICATES ARG_LABELS)
    set(ARG_LABELS "${ARG_LABELS}" PARENT_SCOPE)
    set(ARG_PROPERTIES "${ARG_PROPERTIES}" PARENT_SCOPE)
endfunction()

function(horo_ci_test_runtime_root target output)
    set(root "${CMAKE_CURRENT_BINARY_DIR}/ci-runtime/${target}")
    if(CMAKE_HOST_WIN32 AND DEFINED ENV{RUNNER_TEMP} AND NOT "$ENV{RUNNER_TEMP}" STREQUAL "")
        # Keep classic Win32 fixture I/O below MAX_PATH without sharing namespaces
        # between build compositions. Reject a shortened-key collision explicitly.
        file(REAL_PATH "${CMAKE_CURRENT_BINARY_DIR}" build_identity)
        set(runtime_identity "${build_identity}\n${target}")
        string(SHA256 build_key "${runtime_identity}")
        string(SUBSTRING "${build_key}" 0 16 build_key)
        set(namespace "$ENV{RUNNER_TEMP}/hci/${build_key}")
        file(MAKE_DIRECTORY "${namespace}")
        set(identity_file "${namespace}/identity.txt")
        if(EXISTS "${identity_file}")
            file(READ "${identity_file}" existing_identity)
            if(NOT existing_identity STREQUAL runtime_identity)
                message(FATAL_ERROR "CI temporary build namespace collision")
            endif()
        else()
            file(WRITE "${identity_file}" "${runtime_identity}")
        endif()
        set(root "${namespace}")
    endif()
    file(MAKE_DIRECTORY "${root}")
    file(REAL_PATH "${root}" root)
    set(${output} "${root}" PARENT_SCOPE)
endfunction()

function(horo_ci_catch_properties target)
    # Apply properties after discovery: Catch flattens list-valued properties
    # into separate arguments, corrupting labels and subsequent key/value pairs.
    set(properties "LABELS [==[${ARG_LABELS}]==]")
    foreach(property IN LISTS ARG_PROPERTIES)
        string(APPEND properties " [==[${property}]==]")
    endforeach()
    set(isolation "")
    if(HORO_CI_ISOLATE_TEST_EXECUTABLES)
        horo_ci_test_runtime_root("${target}" runtime_root)
        if(target IN_LIST HORO_CI_CASE_ISOLATED_TARGETS AND NOT "${ARG_LABELS}" MATCHES "(^|;)(gpu|display|realtime-safety)(;|$)")
            set(case_properties "${CMAKE_CURRENT_BINARY_DIR}/${target}_case_isolation.cmake")
            configure_file("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/HoroCiCaseIsolation.cmake.in" "${case_properties}" @ONLY)
            string(APPEND isolation "    include([==[${case_properties}]==])\n")
        else()
            # Unaudited suites may share fixture names; keep their cases locked.
            file(MAKE_DIRECTORY "${runtime_root}/work" "${runtime_root}/tmp" "${runtime_root}/home")
            set(suite_locks "horo-suite-${target}")
            set(suite_environment "TMPDIR=set:${runtime_root}/tmp;TMP=set:${runtime_root}/tmp;TEMP=set:${runtime_root}/tmp;HOME=set:${runtime_root}/home;USERPROFILE=set:${runtime_root}/home")
            string(APPEND isolation
                "    set_tests_properties(\${${target}_TESTS} PROPERTIES WORKING_DIRECTORY [==[${runtime_root}/work]==])\n"
                "    set_tests_properties(\${${target}_TESTS} PROPERTIES RESOURCE_LOCK [==[${suite_locks}]==])\n"
                "    set_tests_properties(\${${target}_TESTS} PROPERTIES ENVIRONMENT_MODIFICATION [==[${suite_environment}]==])\n")
        endif()
        if("${ARG_LABELS}" MATCHES "(^|;)(gpu|display|realtime-safety)(;|$)")
            string(APPEND isolation "    set_tests_properties(\${${target}_TESTS} PROPERTIES RUN_SERIAL TRUE)\n")
        endif()
    endif()
    set(properties_file "${CMAKE_CURRENT_BINARY_DIR}/${target}_properties.cmake")
    file(GENERATE OUTPUT "${properties_file}" CONTENT
        "if(${target}_TESTS)\n    set_tests_properties(\${${target}_TESTS} PROPERTIES ${properties})\n${isolation}endif()\n")
    set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES "${properties_file}")
endfunction()

function(horo_finalize_ci_suites)
    add_custom_target(HoroCiAudioRealtimeChecks DEPENDS ${HORO_CI_AUDIO_TARGETS})
    if(HORO_BUILD_INPUT_SDL3)
        add_custom_target(HoroCiWindowsChecks DEPENDS ${HORO_CI_WINDOWS_TARGETS})
    endif()
    add_custom_target(HoroCiNavigationChecks DEPENDS ${HORO_CI_NAVIGATION_TARGETS})
    if(HORO_BUILD_NETWORK_GNS)
        add_custom_target(HoroCiNetworkGnsChecks DEPENDS ${HORO_CI_NETWORK_TARGETS})
    endif()

    set_property(TEST
        HoroSceneAllocationJunitPathContract
        HoroCharacterAllocationJunitPathContract
        HoroRuntimeUiSceneReconciliationPublicHeaderConsumer
        HoroRuntimeUiHotReloadPublicHeaderConsumer
        HoroSceneIdentityContractConsumer
        HoroPlayTopologyPublicHeaderConsumer
        HoroRuntimeUiHudPublicHeaderConsumer
        HoroNetworkDebuggerPublicHeaderConsumer
        HoroAudioCallbackLockPolicyTest
        HoroPrefabSceneExpansionContractConsumer
        HoroRuntimeUiScreenTransitionPublicHeaderConsumer
        HoroExtensionManagerTests
        HoroNavigationCoordinatorPublicConsumer
        HoroGameplayPhysicsPublicHeaderConsumer
        HoroGameplayPhysicsSdkContract
        HoroNavigationCharacterAdapterPublicConsumer
        HoroExtensionAbiConformanceCliSupported
        HoroExtensionAbiConformanceCliIncompatible
        HoroExtensionAbiConformanceCliRequiresModule
        HoroGameplayRuntimeUiSdkContract
        APPEND PROPERTY LABELS ci-windows)
    set_property(TEST HoroNavigationBakeDiagnosticsPublicConsumer HoroNavigationCoordinatorPublicConsumer
        APPEND PROPERTY LABELS ci-navigation)
    set_property(TEST HoroNavigationCharacterAdapterPublicConsumer APPEND PROPERTY LABELS ci-navigation)

    # Catch tests receive coverage labels during registration. Direct CTest
    # contracts use the same former gui/editor exclusion. Python tooling runs
    # once under coverage.py, rather than again inside the C++ coverage pass.
    get_property(direct_tests DIRECTORY PROPERTY TESTS)
    foreach(test IN LISTS direct_tests)
        if(HORO_CI_ISOLATE_TEST_EXECUTABLES)
            # Direct integration commands can compile SDKs or launch hosts.
            # Keep their existing process/filesystem assumptions serialized.
            set_property(TEST ${test} PROPERTY RUN_SERIAL TRUE)
        endif()
        get_test_property(${test} LABELS labels)
        if(NOT "${labels}" MATCHES "(^|;)(gui|editor)(;|$)" AND NOT test STREQUAL "HoroPythonToolingTests")
            set_property(TEST ${test} APPEND PROPERTY LABELS sonar-coverage)
        endif()
    endforeach()

    # Register the filtered run only for Sonar; regular GUI qualification uses
    # the existing discovered cases and must not execute them twice.
    if(HORO_CI_SONAR_COVERAGE AND TARGET HoroEditorUiAutomationTests)
        add_test(NAME HoroSonarEditorUiAutomationCoverage
            COMMAND HoroEditorUiAutomationTests "~[native]")
        set_tests_properties(HoroSonarEditorUiAutomationCoverage PROPERTIES
            LABELS "gui;editor;sonar-coverage" TIMEOUT 120 RUN_SERIAL TRUE)
    endif()
endfunction()
