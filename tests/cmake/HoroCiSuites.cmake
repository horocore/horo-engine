# CI builds capabilities; the test registry owns their executable membership.
option(HORO_CI_SONAR_COVERAGE "Register the filtered Sonar UI coverage run" OFF)

set(HORO_CI_AUDIO_TARGETS
    HoroAudioRealtimeSafetyHarnessTests
    HoroAudioWatchdogTests
    HoroAudioNullTests
    HoroAudioDspTests
    HoroCoreAudioDspTests
    HoroAudioCommandTests
    HoroAudioMixerTests
)

# The full Windows suite remains disabled. Preserve all existing focused checks
# in one headless Debug build, including their public-header compile consumers.
set(HORO_CI_WINDOWS_TARGETS
    HoroD3D12InitializationTests
    HoroNetworkDebuggerTests
    HoroNetworkDebuggerPublicHeaderConsumer
    HoroTerrainAuthoringTests
    HoroTerrainAuthoringPublicHeaderConsumer
    HoroMixerDocumentTests
    ${HORO_CI_AUDIO_TARGETS}
    HoroCliCommandRegistryTests
    HoroCliOutputPublicHeaderConsumer
    HoroCliProductionOutputContract
    HoroCliMcpServeTests
    HoroPlatformTests
    HoroUpdateZipPackageProducerTests
    HoroVfxApiTests
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
    HoroPrefabTests
    HoroPrefabSceneExpansionTests
    HoroPrefabSceneExpansionContractConsumer
    HoroAssetRegistryTests
    HoroInputTests
    HoroInputSdlTests
    HoroRuntimeUiInputTests
    HoroRuntimeUiTextUnicodeTests
    HoroRuntimeUiUnicodeStartupTests
    HoroRuntimeUiUnicodeLifecycleTests
    HoroRuntimeUiTextShapingTests
    HoroRuntimeUiTextLayoutTests
    HoroRuntimeUiPublicHeaderConsumer
    HoroRuntimeUiOverlayLifecycleTests
    HoroInputPublicHeaderConsumer
    HoroExtensionManagerTests
    HoroEditorActivityBoundaryTests
    HoroExtensionsPublicHeaderConsumer
    HoroMcpSessionTests
    HoroMcpSessionPublicHeaderConsumer
    HoroRuntimeSaveRootResolverTests
    HoroRuntimeSaveFilesystemLockTests
    HoroRuntimeSaveSlotCommitTransactionTests
    HoroRuntimeSaveEventTriggersTests
    HoroSaveEventTriggersPublicHeaderConsumer
    HoroRuntimePublicHeaderConsumer
    HoroTerrainSourceArtifactTests
    HoroTerrainSourceArtifactPublicHeaderConsumer
    HoroTerrainPayloadManifestTests
    HoroTerrainPayloadManifestPublicHeaderConsumer
    HoroTerrainProducerSnapshotTests
    HoroTerrainProducerSnapshotPublicHeaderConsumer
)

set(HORO_CI_NAVIGATION_TARGETS
    HoroNavigationRuntime
    HoroNavigationNull
    HoroNavigationRecastDetour
    HoroNavigationApiPublicHeaderConsumer
    HoroNavigationRuntimePublicHeaderConsumer
    HoroNavigationNullPublicHeaderConsumer
    HoroNavigationRecastDetourPublicHeaderConsumer
    HoroNavigationBakeServicePublicHeaderConsumer
    HoroNavigationBakeDiagnosticsPublicConsumer
)

set(HORO_CI_NETWORK_TARGETS
    HoroNetworkTransportGnsTests
    HoroNetworkApiPublicHeaderConsumer
)

# These editor-labelled suites were explicitly run by the coverage workflow.
set(HORO_SONAR_EDITOR_TARGETS
    HoroFractureDocumentTests
    HoroTerrainAuthoringTests
    HoroMixerDocumentTests
    HoroCameraCutEditorIntegrationTests
    HoroCinematicPropertyIntegrationTests
    HoroConfiguredEditorUpdateBackendTests
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

function(horo_ci_catch_properties target)
    # Apply properties after discovery: Catch flattens list-valued properties
    # into separate arguments, corrupting labels and subsequent key/value pairs.
    set(properties "LABELS [==[${ARG_LABELS}]==]")
    foreach(property IN LISTS ARG_PROPERTIES)
        string(APPEND properties " [==[${property}]==]")
    endforeach()
    set(properties_file "${CMAKE_CURRENT_BINARY_DIR}/${target}_properties.cmake")
    file(GENERATE OUTPUT "${properties_file}" CONTENT
        "if(${target}_TESTS)\n    set_tests_properties(\${${target}_TESTS} PROPERTIES ${properties})\nendif()\n")
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
        HoroNetworkDebuggerPublicHeaderConsumer
        HoroAudioCallbackLockPolicyTest
        HoroPrefabSceneExpansionContractConsumer
        HoroExtensionManagerTests
        HoroExtensionAbiConformanceCliSupported
        HoroExtensionAbiConformanceCliIncompatible
        HoroExtensionAbiConformanceCliRequiresModule
        APPEND PROPERTY LABELS ci-windows)
    set_property(TEST HoroNavigationBakeDiagnosticsPublicConsumer APPEND PROPERTY LABELS ci-navigation)

    # Catch tests receive coverage labels during registration. Direct CTest
    # contracts use the same former gui/editor exclusion. Python tooling runs
    # once under coverage.py, rather than again inside the C++ coverage pass.
    get_property(direct_tests DIRECTORY PROPERTY TESTS)
    foreach(test IN LISTS direct_tests)
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
            LABELS "gui;editor;sonar-coverage" TIMEOUT 120)
    endif()
endfunction()
