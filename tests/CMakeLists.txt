add_test(NAME HoroSaveObservabilityDependencyPolicyTest
    COMMAND "${CMAKE_COMMAND}" "-DHORO_ENGINE_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
    "-DHORO_TEST_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/save-observability-dependency"
    -P "${PROJECT_SOURCE_DIR}/tests/cmake/RunSaveObservabilityDependencyPolicyTest.cmake")
set_tests_properties(HoroSaveObservabilityDependencyPolicyTest PROPERTIES LABELS "unit;save;architecture")
include(Catch)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/HoroCiSuites.cmake")

add_test(NAME HoroDependencyDirectionNegativeConfigureTest
    COMMAND "${CMAKE_COMMAND}"
        "-DHORO_ENGINE_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
        "-DHORO_TEST_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/dependency-direction-negative"
        -P "${PROJECT_SOURCE_DIR}/tests/cmake/RunDependencyDirectionNegativeTest.cmake")
set_tests_properties(HoroDependencyDirectionNegativeConfigureTest PROPERTIES
    LABELS "architecture;cmake")

add_test(NAME HoroAudioDependencyPolicyTest
    COMMAND "${CMAKE_COMMAND}"
        "-DHORO_ENGINE_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
        -P "${PROJECT_SOURCE_DIR}/tests/cmake/VerifyAudioDependencyPolicy.cmake")
set_tests_properties(HoroAudioDependencyPolicyTest PROPERTIES
    LABELS "architecture;cmake;audio")

add_test(NAME HoroAudioCallbackLockPolicyTest
    COMMAND "${CMAKE_COMMAND}"
        "-DHORO_ENGINE_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
        -P "${PROJECT_SOURCE_DIR}/tests/cmake/VerifyAudioCallbackLockPolicy.cmake")
set_tests_properties(HoroAudioCallbackLockPolicyTest PROPERTIES
    LABELS "architecture;cmake;audio;realtime-safety")

add_test(NAME HoroMcpBoundaryNegativeConfigureTest
    COMMAND "${CMAKE_COMMAND}"
        "-DHORO_ENGINE_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
        "-DHORO_TEST_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/mcp-boundary-negative"
        -P "${PROJECT_SOURCE_DIR}/tests/cmake/RunMcpBoundaryNegativeTest.cmake")
set_tests_properties(HoroMcpBoundaryNegativeConfigureTest PROPERTIES
    LABELS "architecture;cmake;mcp")

add_test(NAME HoroGameplayManifestRevisionValidationTest
    COMMAND "${CMAKE_COMMAND}"
        "-DHORO_ENGINE_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
        "-DHORO_TEST_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/gameplay-manifest-validation"
        -P "${PROJECT_SOURCE_DIR}/tests/cmake/RunGameplayManifestRevisionValidationTest.cmake")
set_tests_properties(HoroGameplayManifestRevisionValidationTest PROPERTIES
    LABELS "gameplay;cmake")

add_test(NAME HoroExtensionSdkExternalConsumerTest
    COMMAND "${CMAKE_COMMAND}"
        "-DHORO_EXTENSION_SDK_PACKAGE_DIR=${HORO_EXTENSION_SDK_PACKAGE_DIR}"
        "-DHORO_EXTENSION_SDK_VERSION=${HORO_EXTENSION_SDK_VERSION}"
        "-DHORO_ENGINE_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
        "-DHORO_TEST_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/extension-sdk-consumer"
        "-DCMAKE_GENERATOR=${CMAKE_GENERATOR}"
        "-DCMAKE_GENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}"
        "-DCMAKE_GENERATOR_TOOLSET=${CMAKE_GENERATOR_TOOLSET}"
        "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}"
        "-DCMAKE_LINKER=${CMAKE_LINKER}"
        "-DCMAKE_OSX_SYSROOT=${CMAKE_OSX_SYSROOT}"
        "-DCMAKE_OSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}"
        -P "${PROJECT_SOURCE_DIR}/tests/cmake/RunExtensionSdkConsumerTest.cmake")
set_tests_properties(HoroExtensionSdkExternalConsumerTest PROPERTIES
    LABELS "extensions;sdk;cmake")

add_test(NAME HoroExtensionSdkReconfigureTest
    COMMAND "${CMAKE_COMMAND}"
        "-DHORO_ENGINE_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
        "-DHORO_TEST_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/extension-sdk-reconfigure"
        "-DCMAKE_GENERATOR=${CMAKE_GENERATOR}"
        "-DCMAKE_GENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}"
        "-DCMAKE_GENERATOR_TOOLSET=${CMAKE_GENERATOR_TOOLSET}"
        -P "${PROJECT_SOURCE_DIR}/tests/cmake/RunExtensionSdkReconfigureTest.cmake")
set_tests_properties(HoroExtensionSdkReconfigureTest PROPERTIES
    LABELS "extensions;sdk;cmake")

add_test(NAME HoroExtensionScaffoldsTest
    COMMAND "${CMAKE_COMMAND}"
        "-DHORO_EXTENSION_SDK_PACKAGE_DIR=${HORO_EXTENSION_SDK_PACKAGE_DIR}"
        "-DHORO_EXTENSION_SCAFFOLDER=${HORO_EXTENSION_SDK_PACKAGE_DIR}/bin/horo-scaffold-extension.py"
        "-DHORO_EXTENSION_ACTIVATION_TEST=$<TARGET_FILE:HoroPackagedExtensionActivation>"
        "-DHORO_TEST_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/extension-scaffolds"
        "-DPYTHON_EXECUTABLE=${Python3_EXECUTABLE}"
        "-DCMAKE_GENERATOR=${CMAKE_GENERATOR}"
        "-DCMAKE_GENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}"
        "-DCMAKE_GENERATOR_TOOLSET=${CMAKE_GENERATOR_TOOLSET}"
        -P "${PROJECT_SOURCE_DIR}/tests/cmake/RunExtensionScaffoldsTest.cmake")
set_tests_properties(HoroExtensionScaffoldsTest PROPERTIES
    LABELS "extensions;sdk;cmake")

add_test(NAME HoroExtensionAuthorCiTest
    COMMAND "${CMAKE_COMMAND}"
        "-DHORO_EXTENSION_SDK_PACKAGE_DIR=${HORO_EXTENSION_SDK_PACKAGE_DIR}"
        "-DPYTHON_EXECUTABLE=${Python3_EXECUTABLE}"
        "-DHORO_TEST_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/extension-author-ci"
        -P "${PROJECT_SOURCE_DIR}/tests/cmake/RunExtensionAuthorCiTest.cmake")
set_tests_properties(HoroExtensionAuthorCiTest PROPERTIES LABELS "extensions;sdk;cmake")

# One translation unit per registered public header verifies that consumers can
# compile the contract using only its owning target's usage requirements.
horo_add_public_header_consumer_targets()
target_sources(HoroGameplayApiPublicHeaderConsumer PRIVATE support/PersistenceInstallationPublicHeaderConsumer.cpp)
target_sources(HoroRuntimePublicHeaderConsumer PRIVATE support/SaveSceneCanonicalPublicHeaderConsumer.cpp)
target_sources(HoroRuntimeScenePublicHeaderConsumer PRIVATE support/SaveContentWorldPublicHeaderConsumer.cpp)
target_sources(HoroWorldStreamingPublicHeaderConsumer PRIVATE support/StreamingOwnerFramePublicContract.cpp)

execute_process(
        COMMAND "${Python3_EXECUTABLE}" -c "import pytest; print(pytest.__version__)"
        RESULT_VARIABLE HORO_PYTEST_IMPORT_RESULT
        OUTPUT_VARIABLE HORO_PYTEST_VERSION
        ERROR_VARIABLE HORO_PYTEST_IMPORT_ERROR
        OUTPUT_STRIP_TRAILING_WHITESPACE
)
if (NOT HORO_PYTEST_IMPORT_RESULT EQUAL 0)
    message(WARNING
            "BUILD_TESTING: pytest is not available; skipping repository Python tests."
    )
else ()
    message(STATUS "Using pytest ${HORO_PYTEST_VERSION} with ${Python3_EXECUTABLE}")

    add_test(NAME HoroPythonToolingTests
            COMMAND "${Python3_EXECUTABLE}" -m pytest
            "${PROJECT_SOURCE_DIR}/tests/python"
            --quiet
            --basetemp "${CMAKE_CURRENT_BINARY_DIR}/pytest-temp"
    )
    set_tests_properties(HoroPythonToolingTests PROPERTIES
            LABELS "python;tooling"
            WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
            ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1;PYTHONUTF8=1"
    )
endif ()

add_compile_definitions(HORO_PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}")

function(horo_register_catch_test target)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "" "" "LABELS;EXTRA_ARGS;PROPERTIES")
    horo_ci_catch_labels(${target})
    target_compile_features(${target} PRIVATE cxx_std_20)
    target_compile_definitions(${target} PRIVATE HORO_PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    target_link_libraries(${target} PRIVATE Catch2::Catch2WithMain)
    catch_discover_tests(${target}
            TEST_PREFIX "${target}::"
            EXTRA_ARGS ${ARG_EXTRA_ARGS}
    )
    horo_ci_catch_properties(${target})
endfunction()

add_executable(HoroModelProviderTests unit/editor/ModelProviderContractTests.cpp)
target_compile_features(HoroModelProviderTests PRIVATE cxx_std_20)
target_link_libraries(HoroModelProviderTests PRIVATE HoroEngine::ModelProviderAdapters Catch2::Catch2WithMain)
add_test(NAME HoroModelProviderContractTests
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/python/model_provider_http_fixture.py"
            "$<TARGET_FILE:HoroModelProviderTests>")
set_tests_properties(HoroModelProviderContractTests PROPERTIES LABELS "unit;agent;headless" TIMEOUT 60)

add_executable(HoroPackageValidationTests
    unit/packages/PackageInstallTests.cpp
    unit/packages/PackageDependencyResolverTests.cpp
    unit/packages/PackageLockfileTests.cpp
    unit/packages/PackageRequestTests.cpp
    unit/packages/PackagePathTests.cpp
    unit/packages/PackageArchiveTests.cpp
    unit/packages/PackageCacheTests.cpp
    unit/packages/PackageFileManifestTests.cpp
    unit/packages/PackageRestoreTests.cpp
    unit/packages/PackagePublisherVerificationTests.cpp)
target_link_libraries(HoroPackageValidationTests PRIVATE HoroEngine::Packages HoroEngine::PackageSecurity HoroEngine::Platform nlohmann_json::nlohmann_json miniz)
horo_register_catch_test(HoroPackageValidationTests LABELS "packages;headless")

if (HORO_ENABLE_IMGUI_UI_TESTS)
    if (NOT TARGET HoroThirdPartyImGui)
        message(FATAL_ERROR "HoroEditorUiAutomationTests requires instrumented HoroThirdPartyImGui")
    endif ()
    get_target_property(HORO_IMGUI_TEST_ENGINE_DEFINITIONS HoroThirdPartyImGui COMPILE_DEFINITIONS)
    if (NOT "IMGUI_ENABLE_TEST_ENGINE" IN_LIST HORO_IMGUI_TEST_ENGINE_DEFINITIONS)
        message(FATAL_ERROR "HoroEditorUiAutomationTests requires IMGUI_ENABLE_TEST_ENGINE")
    endif ()

    add_executable(HoroEditorUiAutomationTests
            gui/EditorUiAutomationHarnessTests.cpp
            gui/FullEditorUiAutomationScenarios.cpp
            helpers/editor_ui/EditorUiTestHarness.cpp
            helpers/editor_ui/EditorUiTestSurface.cpp
            helpers/editor_ui/InteractiveEditorUiTestRenderer.cpp
            helpers/editor_ui/FullEditorUiTestActions.cpp
            helpers/editor_ui/FullEditorUiTestHost.cpp
            helpers/editor_ui/FullEditorUiTestSetups.cpp
    )
    target_include_directories(HoroEditorUiAutomationTests PRIVATE
            ${PROJECT_SOURCE_DIR}/tests/helpers/editor_ui
            ${PROJECT_SOURCE_DIR}/src
            ${PROJECT_SOURCE_DIR}/src/editor/screens/workspace
            ${imgui_test_engine_SOURCE_DIR}
            ${imgui_test_engine_SOURCE_DIR}/imgui_test_engine
    )
    target_compile_definitions(HoroEditorUiAutomationTests PRIVATE
            HORO_PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
            HORO_GAMEPLAY_SDK_PACKAGE_DIR="${HORO_GAMEPLAY_SDK_PACKAGE_DIR}"
            HORO_GAMEPLAY_CXX_COMPILER="${CMAKE_CXX_COMPILER}"
    )
    target_link_libraries(HoroEditorUiAutomationTests PRIVATE
            HoroEngine::Gui
            HoroEngine::ProjectMigrations
            HoroEngine::RenderFrontend
            HoroThirdParty::ImGui
            nlohmann_json::nlohmann_json
    )
    if (HORO_BUILD_RENDER_OPENGL)
        target_sources(HoroEditorUiAutomationTests PRIVATE
                helpers/editor_ui/InteractiveOpenGlEditorUiTestSurface.cpp
        )
        target_compile_definitions(HoroEditorUiAutomationTests PRIVATE
                HORO_UI_TEST_HAS_INTERACTIVE_OPENGL=1
        )
        target_link_libraries(HoroEditorUiAutomationTests PRIVATE
                HoroEngine::EditorViewportOpenGL
        )
    endif ()
    if (HORO_BUILD_RENDER_METAL)
        target_sources(HoroEditorUiAutomationTests PRIVATE
                helpers/editor_ui/InteractiveMetalEditorUiTestSurface.mm
        )
        target_compile_definitions(HoroEditorUiAutomationTests PRIVATE
                HORO_UI_TEST_HAS_INTERACTIVE_METAL=1
        )
        target_link_libraries(HoroEditorUiAutomationTests PRIVATE
                HoroEngine::EditorViewportMetal
        )
    endif ()
    horo_register_catch_test(HoroEditorUiAutomationTests
            LABELS "gui;headless;imgui"
            PROPERTIES TIMEOUT 120
    )
endif ()

add_executable(HoroInputTests
        unit/runtime/input/GameplayInputFrameTests.cpp
        unit/runtime/input/InputTests.cpp
        unit/runtime/input/InputContextTests.cpp
        unit/runtime/input/TextInputTests.cpp
)
target_compile_features(HoroInputTests PRIVATE cxx_std_20)
target_link_libraries(HoroInputTests PRIVATE HoroEngine::Input)
add_executable(HoroRuntimeUiInputTests
        unit/runtime/input/RuntimeUiTextInputTests.cpp
        unit/runtime/input/RuntimeUiNavigationTests.cpp
        support/AllocationProbe.cpp
)
target_compile_features(HoroRuntimeUiInputTests PRIVATE cxx_std_20)
target_include_directories(HoroRuntimeUiInputTests PRIVATE ${PROJECT_SOURCE_DIR}/src/runtime/ui/input)
target_include_directories(HoroRuntimeUiInputTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(HoroRuntimeUiInputTests PRIVATE HoroEngine::RuntimeUiInput)
horo_register_catch_test(HoroRuntimeUiInputTests LABELS "unit;input;runtime_ui;headless")
add_executable(HoroRuntimeUiNavigationPublicHeaderConsumer support/RuntimeUiNavigationPublicHeaderConsumer.cpp)
target_compile_features(HoroRuntimeUiNavigationPublicHeaderConsumer PRIVATE cxx_std_20)
target_link_libraries(HoroRuntimeUiNavigationPublicHeaderConsumer PRIVATE HoroEngine::RuntimeUiInput)
add_test(NAME HoroRuntimeUiNavigationPublicHeaderConsumer COMMAND HoroRuntimeUiNavigationPublicHeaderConsumer)
set_tests_properties(HoroRuntimeUiNavigationPublicHeaderConsumer PROPERTIES LABELS "unit;input;runtime_ui;headless")
if (HORO_BUILD_INPUT_SDL3)
    add_executable(HoroInputSdlTests
            unit/runtime/input/SdlTextInputTests.cpp
    )
    target_compile_features(HoroInputSdlTests PRIVATE cxx_std_20)
    target_include_directories(HoroInputSdlTests PRIVATE ${PROJECT_SOURCE_DIR}/src/runtime/input/sdl)
    target_link_libraries(HoroInputSdlTests PRIVATE HoroEngine::InputSdl ${HORO_SDL3_TARGET})
    horo_register_catch_test(HoroInputSdlTests LABELS "unit;input;sdl;headless")
endif ()
add_executable(HoroNetworkApiTests
        unit/runtime/networking/MessageEnvelopeTests.cpp
        unit/runtime/networking/MessageDeliveryGateTests.cpp
        unit/runtime/networking/NetworkAddressTests.cpp
        unit/runtime/networking/NetworkHandleTests.cpp
        unit/runtime/networking/NetworkFailureTests.cpp
        unit/runtime/networking/NetworkIoServiceTests.cpp
        unit/runtime/networking/NetworkLifecycleTests.cpp
        unit/runtime/networking/NetworkMetricsTests.cpp
        unit/runtime/networking/NetworkProjectSettingsTests.cpp
        unit/runtime/networking/NetworkTargetCapabilitiesTests.cpp
        unit/runtime/networking/PacketQueueTests.cpp
        unit/runtime/networking/ProtocolIdentityTests.cpp
        unit/runtime/networking/ReplicationDescriptorTests.cpp
        unit/runtime/networking/ReplicationDeclarationPolicyTests.cpp
        unit/runtime/networking/NetworkReplicationInventoryTests.cpp
        unit/runtime/networking/ReplicationDescriptorRegistryTests.cpp
        unit/runtime/networking/ReplicationSerializerTests.cpp
        unit/runtime/networking/ReplicationRolesTests.cpp
        unit/runtime/networking/RpcDescriptorTests.cpp
        unit/runtime/networking/TransportCapabilitiesTests.cpp
        unit/runtime/networking/TransportBudgetTests.cpp
)
target_compile_features(HoroNetworkApiTests PRIVATE cxx_std_20)
target_link_libraries(HoroNetworkApiTests PRIVATE HoroEngine::NetworkApi)
target_include_directories(HoroNetworkApiTests PRIVATE $<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>)
horo_register_catch_test(HoroNetworkApiTests LABELS "unit;network;headless")
add_executable(HoroReplicationDeclarationPublicHeaderConsumer support/ReplicationDeclarationPublicHeaderConsumer.cpp)
target_compile_features(HoroReplicationDeclarationPublicHeaderConsumer PRIVATE cxx_std_20)
target_link_libraries(HoroReplicationDeclarationPublicHeaderConsumer PRIVATE HoroEngine::NetworkApi)
add_test(NAME HoroReplicationDeclarationPublicHeaderConsumer COMMAND HoroReplicationDeclarationPublicHeaderConsumer)
set_tests_properties(HoroReplicationDeclarationPublicHeaderConsumer PROPERTIES LABELS "unit;network;headless;public_headers")
add_executable(HoroNetworkTransportNullTests
        support/AllocationProbe.cpp
        unit/runtime/networking/DeterministicTransportTests.cpp
        unit/runtime/networking/DeterministicTransportQualificationTests.cpp
)
target_compile_features(HoroNetworkTransportNullTests PRIVATE cxx_std_20)
target_include_directories(HoroNetworkTransportNullTests PRIVATE ${PROJECT_SOURCE_DIR}/tests/support)
target_link_libraries(HoroNetworkTransportNullTests PRIVATE HoroEngine::NetworkTransportNull)
horo_register_catch_test(HoroNetworkTransportNullTests LABELS "unit;network;headless")
if(HORO_BUILD_NETWORK_GNS)
    add_executable(HoroNetworkTransportGnsTests
            support/AllocationProbe.cpp
            unit/runtime/networking/GnsTransportTests.cpp
            unit/runtime/networking/GnsTransportQualificationTests.cpp
    )
    target_compile_features(HoroNetworkTransportGnsTests PRIVATE cxx_std_20)
    target_include_directories(HoroNetworkTransportGnsTests PRIVATE ${PROJECT_SOURCE_DIR}/src/runtime/networking/gns ${PROJECT_SOURCE_DIR}/tests/support)
    target_compile_definitions(HoroNetworkTransportGnsTests PRIVATE $<$<PLATFORM_ID:Windows>:NOMINMAX>)
    target_link_libraries(HoroNetworkTransportGnsTests PRIVATE GameNetworkingSockets::static c-ares::cares)
    target_link_libraries(HoroNetworkTransportGnsTests PRIVATE HoroEngine::NetworkTransportGNS)
    horo_register_catch_test(HoroNetworkTransportGnsTests LABELS "unit;network;gns;socket")
endif()
add_executable(HoroNetworkRuntimeTests
        unit/runtime/networking/AdmissionProtectionTests.cpp
        unit/runtime/networking/InboundMessageDispatcherTests.cpp
        unit/runtime/networking/AuthenticationSessionAdapterTests.cpp
        unit/runtime/networking/NetworkModeCompositionTests.cpp
        unit/runtime/networking/NetworkDiagnosticsTests.cpp
        unit/runtime/networking/HandshakeNegotiationTests.cpp
        unit/runtime/networking/NetworkObjectMappingTests.cpp
        unit/runtime/networking/NetworkTickAlignmentTests.cpp
        unit/runtime/networking/PeerSessionLifecycleTests.cpp
        unit/runtime/networking/ReplicationWorldLifecycleTests.cpp
        unit/runtime/networking/RpcGameplayDispatchTests.cpp
        unit/runtime/networking/RpcGameplayDispatchLifecycleTests.cpp
        unit/runtime/networking/RpcAuthorizationPolicyTests.cpp
        unit/runtime/networking/RpcParameterPolicyTests.cpp
)
target_compile_features(HoroNetworkRuntimeTests PRIVATE cxx_std_20)
target_link_libraries(HoroNetworkRuntimeTests PRIVATE HoroEngine::NetworkRuntime HoroEngine::NetworkTransportNull)
horo_register_catch_test(HoroNetworkRuntimeTests LABELS "unit;network;headless")
add_executable(HoroReplicationStateCaptureTests
        support/AllocationProbe.cpp
        unit/runtime/networking/ReplicationStateCaptureTests.cpp
        unit/runtime/networking/ReplicationCaptureContainerTests.cpp
        unit/runtime/networking/ReplicationCaptureSchedulingTests.cpp
        unit/runtime/networking/SceneReplicationCommitSourceTests.cpp
)
target_compile_features(HoroReplicationStateCaptureTests PRIVATE cxx_std_20)
target_include_directories(HoroReplicationStateCaptureTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/support)
target_link_libraries(HoroReplicationStateCaptureTests PRIVATE HoroEngine::NetworkRuntime)
horo_register_catch_test(HoroReplicationStateCaptureTests LABELS "unit;network;headless;replication_capture")
add_executable(HoroReplicationStateCodecTests
        unit/runtime/networking/ReplicationStateCodecTests.cpp
        unit/runtime/networking/ReplicationStateCodecLifecycleTests.cpp
        unit/runtime/networking/ReplicationStateCodecProjectionTests.cpp
        unit/runtime/networking/ReplicationStateCodecSceneTests.cpp
        unit/runtime/networking/ReplicationStateCodecFaultTests.cpp
)
target_compile_features(HoroReplicationStateCodecTests PRIVATE cxx_std_20)
target_link_libraries(HoroReplicationStateCodecTests PRIVATE HoroEngine::NetworkRuntime)
horo_register_catch_test(HoroReplicationStateCodecTests LABELS "unit;network;headless;replication_codec")
add_executable(HoroNetworkModePublicHeaderConsumer support/NetworkModePublicHeaderConsumer.cpp)
target_compile_features(HoroNetworkModePublicHeaderConsumer PRIVATE cxx_std_20)
target_link_libraries(HoroNetworkModePublicHeaderConsumer PRIVATE HoroEngine::NetworkRuntime)
add_test(NAME HoroNetworkModePublicHeaderConsumer COMMAND HoroNetworkModePublicHeaderConsumer)
set_tests_properties(HoroNetworkModePublicHeaderConsumer PROPERTIES LABELS "unit;network;headless")
add_executable(HoroNetworkModeHostIntegrationTests
        integration/network/NetworkModeHostIntegrationTests.cpp
)
target_compile_features(HoroNetworkModeHostIntegrationTests PRIVATE cxx_std_20)
target_compile_definitions(HoroNetworkModeHostIntegrationTests PRIVATE HORO_TEST_PHYSICS_NATIVE=$<BOOL:${HORO_BUILD_PHYSICS_NATIVE}>)
target_include_directories(HoroNetworkModeHostIntegrationTests PRIVATE ${PROJECT_SOURCE_DIR}/tests/unit/runtime/networking)
target_link_libraries(HoroNetworkModeHostIntegrationTests PRIVATE HoroNetworkProductHost HoroEngine::NetworkTransportNull
    HoroEngine::Platform)
horo_register_catch_test(HoroNetworkModeHostIntegrationTests LABELS "integration;network;headless")
add_test(NAME HoroNetworkStandaloneProductLaunch COMMAND $<TARGET_FILE:horo-engine> --run-network-product standalone 2)
set_tests_properties(HoroNetworkStandaloneProductLaunch PROPERTIES LABELS "integration;network;headless;product")
if(HORO_BUILD_NETWORK_GNS)
    add_test(NAME HoroNetworkDedicatedProductLaunch COMMAND $<TARGET_FILE:horo-engine> --run-network-product dedicated 1 127.0.0.1:38981)
    set_tests_properties(HoroNetworkDedicatedProductLaunch PROPERTIES LABELS "integration;network;headless;product")
    add_test(NAME HoroNetworkClientProductLaunch COMMAND $<TARGET_FILE:horo-engine> --run-network-product client 2 127.0.0.1:38982)
    set_tests_properties(HoroNetworkClientProductLaunch PROPERTIES LABELS "integration;network;headless;product")
    add_test(NAME HoroNetworkListenProductLaunch COMMAND $<TARGET_FILE:horo-engine> --run-network-product listen 2 127.0.0.1:38983 127.0.0.1:38983)
    set_tests_properties(HoroNetworkListenProductLaunch PROPERTIES LABELS "integration;network;headless;product")
else()
    add_test(NAME HoroNetworkUnsupportedProductMode COMMAND $<TARGET_FILE:horo-engine> --run-network-product dedicated 1 127.0.0.1:38981)
    set_tests_properties(HoroNetworkUnsupportedProductMode PROPERTIES WILL_FAIL TRUE LABELS "integration;network;headless;product")
endif()
add_test(NAME HoroNetworkInvalidProductMode COMMAND $<TARGET_FILE:horo-engine> --run-network-product invalid 1)
set_tests_properties(HoroNetworkInvalidProductMode PROPERTIES WILL_FAIL TRUE LABELS "integration;network;headless;product")
add_test(NAME HoroNetworkInvalidProductFrames COMMAND $<TARGET_FILE:horo-engine> --run-network-product standalone 10001)
set_tests_properties(HoroNetworkInvalidProductFrames PROPERTIES WILL_FAIL TRUE LABELS "integration;network;headless;product")
add_test(NAME HoroNetworkInvalidProductEndpoint COMMAND $<TARGET_FILE:horo-engine> --run-network-product dedicated 1 invalid-endpoint)
set_tests_properties(HoroNetworkInvalidProductEndpoint PROPERTIES WILL_FAIL TRUE LABELS "integration;network;headless;product")
include(${PROJECT_SOURCE_DIR}/cmake/HoroNetworkTransportComposition.cmake)
add_executable(HoroNetworkDisabledLinkProbe support/NetworkTransportLinkProbe.cpp)
horo_configure_network_transport_profile(HoroNetworkDisabledLinkProbe DISABLED)
add_executable(HoroNetworkNullLinkProbe support/NetworkTransportLinkProbe.cpp)
horo_configure_network_transport_profile(HoroNetworkNullLinkProbe HEADLESS_NULL)
foreach(_profile IN ITEMS Disabled Null)
    file(GENERATE
            OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/HoroNetwork${_profile}LinkProbe-$<CONFIG>.links"
            CONTENT "$<TARGET_PROPERTY:HoroNetwork${_profile}LinkProbe,LINK_LIBRARIES>")
    add_test(NAME HoroNetwork${_profile}LinkageContract
            COMMAND ${CMAKE_COMMAND}
                    -DPROFILE=${_profile}
                    -DMANIFEST=${CMAKE_CURRENT_BINARY_DIR}/HoroNetwork${_profile}LinkProbe-$<CONFIG>.links
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/support/AssertNetworkTransportLinkage.cmake)
    set_tests_properties(HoroNetwork${_profile}LinkageContract PROPERTIES LABELS "unit;network;headless")
endforeach()
add_executable(HoroNetworkTransportCompositionTests
        unit/runtime/networking/TransportBackendCompositionTests.cpp
)
target_compile_features(HoroNetworkTransportCompositionTests PRIVATE cxx_std_20)
target_link_libraries(HoroNetworkTransportCompositionTests PRIVATE HoroEngine::NetworkRuntime HoroEngine::NetworkTransportNull)
add_dependencies(HoroNetworkTransportCompositionTests HoroNetworkDisabledLinkProbe HoroNetworkNullLinkProbe)
horo_register_catch_test(HoroNetworkTransportCompositionTests LABELS "unit;network;headless")
target_sources(HoroAudioApiPublicHeaderConsumer PRIVATE support/AudioStreamingFactoryPublicContract.cpp support/AudioSpatialPublicContract.cpp)
add_executable(HoroAudioApiTests
        support/AllocationProbe.cpp
        unit/runtime/audio/AudioAcousticQueryTests.cpp
        unit/runtime/audio/AudioAssetSchemaTests.cpp
        unit/runtime/audio/MixerAssetSchemaTests.cpp
        unit/runtime/audio/AudioSoundReferenceTests.cpp
        unit/runtime/audio/AudioStreamDecoderTests.cpp
        unit/runtime/audio/AudioStreamingServiceTests.cpp
        unit/runtime/audio/AudioStreamingCancellationTests.cpp
        unit/runtime/audio/AudioStreamingControlTests.cpp
        unit/runtime/audio/AudioBackendCapabilitiesTests.cpp
        unit/runtime/audio/AudioCallbackEventsTests.cpp
        unit/runtime/audio/AudioDeviceDiscoveryTests.cpp
        unit/runtime/audio/AudioDeviceNegotiationTests.cpp
        unit/runtime/audio/AudioDeviceTimingTests.cpp
        unit/runtime/audio/AudioFormatTests.cpp
        unit/runtime/audio/AudioFailureRecoveryTests.cpp
        unit/runtime/audio/AudioMediaFormatRegistryTests.cpp
        unit/runtime/audio/AudioIdentityTests.cpp
        unit/runtime/audio/AudioResamplerPlanTests.cpp
        unit/runtime/audio/AudioPlaybackRequestTests.cpp
        unit/runtime/audio/AudioPlanarBlockTests.cpp
        unit/runtime/audio/AudioConcurrencyGroupTests.cpp
        unit/runtime/audio/AudioVoiceStateMachineTests.cpp
)
target_include_directories(HoroAudioApiTests PRIVATE
        ${PROJECT_SOURCE_DIR}/src/audio/api
        ${PROJECT_SOURCE_DIR}/tests/support
)
target_compile_features(HoroAudioApiTests PRIVATE cxx_std_20)
target_link_libraries(HoroAudioApiTests PRIVATE HoroEngine::AudioApi)
horo_register_catch_test(HoroAudioApiTests LABELS "unit;audio;headless")

add_executable(HoroAudioImportTests
        unit/runtime/audio/AudioSourceImporterTests.cpp
)
target_compile_features(HoroAudioImportTests PRIVATE cxx_std_20)
target_link_libraries(HoroAudioImportTests PRIVATE HoroEngine::AudioImport)
horo_register_catch_test(HoroAudioImportTests LABELS "unit;audio;headless")
add_executable(HoroAudioCookTests
        unit/runtime/audio/AudioCookerTests.cpp
        unit/runtime/audio/AudioCookedStreamContextTests.cpp
)
target_compile_features(HoroAudioCookTests PRIVATE cxx_std_20)
target_link_libraries(HoroAudioCookTests PRIVATE HoroEngine::AudioCook HoroEngine::Platform)
target_include_directories(HoroAudioCookTests PRIVATE ${PROJECT_SOURCE_DIR}/tests/support)
horo_register_catch_test(HoroAudioCookTests LABELS "unit;audio;headless")
add_executable(HoroAudioBackendContractTests
        unit/runtime/audio/AudioBackendContractTests.cpp
)
target_compile_features(HoroAudioBackendContractTests PRIVATE cxx_std_20)
target_link_libraries(HoroAudioBackendContractTests PRIVATE HoroAudioBackendContract)
horo_register_catch_test(HoroAudioBackendContractTests LABELS "unit;audio;headless")
add_executable(HoroAudioWatchdogTests
        unit/runtime/audio/AudioCallbackWatchdogTests.cpp
)
target_compile_features(HoroAudioWatchdogTests PRIVATE cxx_std_20)
target_link_libraries(HoroAudioWatchdogTests PRIVATE HoroAudioWatchdog)
horo_register_catch_test(HoroAudioWatchdogTests LABELS "unit;audio;headless")
add_executable(HoroAudioRealtimeSafetyHarnessTests
        support/AllocationProbe.cpp
        unit/runtime/audio/AudioRealtimeSafetyHarnessTests.cpp
)
target_include_directories(HoroAudioRealtimeSafetyHarnessTests PRIVATE ${PROJECT_SOURCE_DIR}/tests/support)
target_compile_features(HoroAudioRealtimeSafetyHarnessTests PRIVATE cxx_std_20)
target_link_libraries(HoroAudioRealtimeSafetyHarnessTests PRIVATE
        HoroAudioWatchdog HoroEngine::AudioCommands HoroEngine::AudioDsp)
horo_register_catch_test(HoroAudioRealtimeSafetyHarnessTests LABELS "unit;audio;headless;realtime-safety")
add_executable(HoroAudioNullTests
        unit/runtime/audio/NullAudioBackendTests.cpp
)
target_compile_features(HoroAudioNullTests PRIVATE cxx_std_20)
target_link_libraries(HoroAudioNullTests PRIVATE HoroEngine::AudioNull)
horo_register_catch_test(HoroAudioNullTests LABELS "unit;audio;headless")
if(HORO_BUILD_AUDIO_SDL3)
    add_executable(HoroAudioSdl3Tests
            unit/runtime/audio/Sdl3AudioBackendTests.cpp
    )
    target_compile_features(HoroAudioSdl3Tests PRIVATE cxx_std_20)
    target_link_libraries(HoroAudioSdl3Tests PRIVATE HoroEngine::AudioSdl3)
    horo_register_catch_test(HoroAudioSdl3Tests
            LABELS "unit;audio;backend"
            PROPERTIES ENVIRONMENT "SDL_AUDIODRIVER=dummy")
endif()
add_executable(HoroAudioDspTests
    unit/runtime/audio/AudioDSPNodeTests.cpp
    unit/runtime/audio/ResamplerKernelTests.cpp
    unit/runtime/audio/AudioResamplerTests.cpp
)
target_include_directories(HoroAudioDspTests PRIVATE ${PROJECT_SOURCE_DIR}/src/audio/resampling)
target_compile_features(HoroAudioDspTests PRIVATE cxx_std_20)
target_link_libraries(HoroAudioDspTests PRIVATE HoroEngine::AudioDsp)
horo_register_catch_test(HoroAudioDspTests LABELS "unit;audio;headless")
add_executable(HoroAudioPlaybackTests
    support/AllocationProbe.cpp
    unit/runtime/audio/AudioVoicePlaybackTests.cpp
    unit/runtime/audio/AudioRepeatedPlaybackTests.cpp
    unit/runtime/audio/AudioRepeatedPlaybackTestFixture.h
    unit/runtime/audio/AudioPlaybackTestSupport.h
)
target_include_directories(HoroAudioPlaybackTests PRIVATE ${PROJECT_SOURCE_DIR}/tests/support)
target_compile_features(HoroAudioPlaybackTests PRIVATE cxx_std_20)
target_link_libraries(HoroAudioPlaybackTests PRIVATE HoroEngine::AudioPlayback HoroEngine::AudioCommands HoroEngine::AudioNull)
horo_register_catch_test(HoroAudioPlaybackTests LABELS "unit;audio;headless")
target_sources(HoroAudioDspPublicHeaderConsumer PRIVATE support/CoreStereoSpatialPublicContract.cpp)
add_executable(HoroCoreAudioDspTests
    support/AllocationProbe.cpp
    unit/runtime/audio/CoreAudioDSPNodeTests.cpp
    unit/runtime/audio/CoreStereoSpatialRendererTests.cpp
)
target_include_directories(HoroCoreAudioDspTests PRIVATE ${PROJECT_SOURCE_DIR}/tests/support)
target_compile_features(HoroCoreAudioDspTests PRIVATE cxx_std_20)
target_link_libraries(HoroCoreAudioDspTests PRIVATE HoroEngine::AudioDsp)
horo_register_catch_test(HoroCoreAudioDspTests LABELS "unit;audio;headless")
add_executable(HoroMixerAllocationFailureWorker
        unit/runtime/audio/MixerAllocationFailureWorker.cpp
        support/AllocationProbe.cpp)
target_include_directories(HoroMixerAllocationFailureWorker PRIVATE ${PROJECT_SOURCE_DIR}/tests/support)
target_compile_features(HoroMixerAllocationFailureWorker PRIVATE cxx_std_20)
target_link_libraries(HoroMixerAllocationFailureWorker PRIVATE HoroEngine::AudioMixer Catch2::Catch2)
if(MSVC)
    target_compile_definitions(HoroMixerAllocationFailureWorker PRIVATE NOMINMAX)
    target_link_libraries(HoroMixerAllocationFailureWorker PRIVATE Dbghelp)
endif()
target_sources(HoroAudioMixerPublicHeaderConsumer PRIVATE support/MixerSnapshotPublicContract.cpp)
add_executable(HoroAudioMixerTests
        unit/runtime/audio/MixerSnapshotRenderTests.cpp
        unit/runtime/audio/MixerSnapshotTests.cpp
        unit/runtime/audio/MixerGraphCompilerTests.cpp
        unit/runtime/audio/MixerGraphRuntimeTests.cpp
        unit/runtime/audio/MixerRenderPlanTests.cpp
        unit/runtime/audio/MixerTestFixture.h
        support/AllocationProbe.cpp)
target_include_directories(HoroAudioMixerTests PRIVATE ${PROJECT_SOURCE_DIR}/tests/support)
target_compile_features(HoroAudioMixerTests PRIVATE cxx_std_20)
target_link_libraries(HoroAudioMixerTests PRIVATE HoroEngine::AudioMixer HoroEngine::Platform Threads::Threads)
target_compile_definitions(HoroAudioMixerTests PRIVATE
        HORO_MIXER_ALLOCATION_WORKER="$<TARGET_FILE:HoroMixerAllocationFailureWorker>")
add_dependencies(HoroAudioMixerTests HoroMixerAllocationFailureWorker)
horo_register_catch_test(HoroAudioMixerTests LABELS "unit;audio;headless;realtime-safety")
add_executable(HoroAudioResamplerBenchmark EXCLUDE_FROM_ALL benchmarks/audio/ResamplerBenchmark.cpp)
target_compile_features(HoroAudioResamplerBenchmark PRIVATE cxx_std_20)
target_link_libraries(HoroAudioResamplerBenchmark PRIVATE HoroEngine::AudioDsp)

add_executable(HoroAudioMemoryTests unit/runtime/audio/AudioMemoryTests.cpp)
target_compile_features(HoroAudioMemoryTests PRIVATE cxx_std_20)
target_link_libraries(HoroAudioMemoryTests PRIVATE HoroEngine::AudioMemory)
horo_register_catch_test(HoroAudioMemoryTests LABELS "unit;audio;headless")
add_executable(HoroAudioCommandTests
    unit/runtime/audio/AudioClockTests.cpp
    unit/runtime/audio/AudioParameterAutomationTests.cpp
    unit/runtime/audio/AudioCommandTests.cpp
    unit/runtime/audio/AudioCommandBufferTests.cpp
    unit/runtime/audio/AudioCommandStagingTests.cpp
    unit/runtime/audio/AudioEventQueueTests.cpp
    unit/runtime/audio/AudioFocusPolicyTests.cpp
    unit/runtime/audio/AudioLifecycleReconcilerTests.cpp
    unit/runtime/audio/ScheduledAudioCommandBatchTests.cpp
)
target_compile_features(HoroAudioCommandTests PRIVATE cxx_std_20)
target_link_libraries(HoroAudioCommandTests PRIVATE HoroEngine::AudioCommands)
horo_register_catch_test(HoroAudioCommandTests LABELS "unit;audio;headless")
add_executable(HoroAudioMetricsTests
    support/AllocationProbe.cpp
    unit/runtime/audio/AudioMetricExtractionTests.cpp
    unit/runtime/audio/AudioMetricsTests.cpp
)
target_include_directories(HoroAudioMetricsTests PRIVATE ${PROJECT_SOURCE_DIR}/tests/support)
target_compile_features(HoroAudioMetricsTests PRIVATE cxx_std_20)
target_link_libraries(HoroAudioMetricsTests PRIVATE HoroEngine::AudioMetrics)
horo_register_catch_test(HoroAudioMetricsTests LABELS "unit;audio;headless")

add_executable(HoroCharacterSurfacePublicHeaderConsumer support/CharacterSurfacePublicHeaderConsumer.cpp)
target_compile_features(HoroCharacterSurfacePublicHeaderConsumer PRIVATE cxx_std_20)
target_link_libraries(HoroCharacterSurfacePublicHeaderConsumer PRIVATE HoroEngine::Physics)
add_test(NAME HoroCharacterSurfacePublicHeaderConsumer COMMAND HoroCharacterSurfacePublicHeaderConsumer)
set_tests_properties(HoroCharacterSurfacePublicHeaderConsumer PROPERTIES LABELS "unit;physics;headless")

add_executable(HoroPhysicsTests
        unit/runtime/physics/PhysicsCellAttachmentsTests.cpp
        support/AllocationProbe.cpp
        unit/runtime/physics/CharacterFoundationQualificationTests.cpp
        unit/runtime/physics/CharacterControllerContractsTests.cpp
        unit/runtime/physics/CharacterDiagnosticsTests.cpp
        unit/runtime/physics/CharacterFastPathStorageTests.cpp
        unit/runtime/physics/CharacterGroundingTests.cpp
        unit/runtime/physics/CharacterCollisionFilteringTests.cpp
        unit/runtime/physics/CharacterSlopeTests.cpp
        unit/runtime/physics/CharacterShapeChangeTests.cpp
        unit/runtime/physics/CharacterMetricsTests.cpp
        unit/runtime/physics/CharacterWorldSettingsTests.cpp
        unit/runtime/physics/CharacterWorldTests.cpp
        unit/runtime/physics/CharacterWorldCommandTests.cpp
        unit/runtime/physics/CanonicalSolverTests.cpp
        unit/runtime/physics/PhysicsActivationTests.cpp
        unit/runtime/physics/PhysicsBodyDescriptorTests.cpp
        unit/runtime/physics/PhysicsBodyDynamicsTests.cpp
        unit/runtime/physics/PhysicsCollisionSchemaTests.cpp
        unit/runtime/physics/PhysicsConstraintDescriptorTests.cpp
        unit/runtime/physics/PhysicsCompoundCookTests.cpp
        unit/runtime/physics/PhysicsConvexHullCookTests.cpp
        unit/runtime/physics/PhysicsCollisionCookerTests.cpp
        unit/runtime/physics/PhysicsTriangleMeshCookTests.cpp
        unit/runtime/physics/PhysicsHeightFieldCookTests.cpp
        unit/runtime/physics/PhysicsCookedShapeCacheTests.cpp
        unit/runtime/physics/PhysicsCookedShapeDescriptorTests.cpp
        unit/runtime/physics/PhysicsDeterminismPolicyTests.cpp
        unit/runtime/physics/PhysicsEventProjectionTests.cpp
        unit/runtime/physics/PhysicsDiagnosticsTests.cpp
        unit/runtime/physics/PhysicsDebugSnapshotTests.cpp
        unit/runtime/physics/PhysicsFoundationQualificationTests.cpp
        unit/runtime/physics/PhysicsHandleRegistryTests.cpp
        unit/runtime/physics/PhysicsIdentityTests.cpp
        unit/runtime/physics/PhysicsMetricsTests.cpp
        unit/runtime/physics/PhysicsMaterialAssetTests.cpp
        unit/runtime/physics/PhysicsMassPropertiesTests.cpp
        unit/runtime/physics/PhysicsPoseTests.cpp
        unit/runtime/physics/PhysicsPropertyFuzzTests.cpp
        unit/runtime/physics/PhysicsQueryTests.cpp
        unit/runtime/physics/PhysicsQueryEventCapabilityTests.cpp
        unit/runtime/physics/PhysicsReferenceSceneCorpusTests.cpp
        unit/runtime/physics/PhysicsSceneActivationTests.cpp
        unit/runtime/physics/PrefabPhysicsStructuralTests.cpp
        unit/runtime/physics/PhysicsSceneContainmentTests.cpp
        unit/runtime/physics/PhysicsShapeDescriptorTests.cpp
        unit/runtime/physics/PhysicsStepPolicyTests.cpp
        unit/runtime/physics/PhysicsTransformAuthorityTests.cpp
        unit/runtime/physics/PhysicsWorldBudgetsTests.cpp
        unit/runtime/physics/PhysicsWorldConstraintMutationTests.cpp
        unit/runtime/physics/PhysicsWorldContainmentTests.cpp
        unit/runtime/physics/PhysicsWorldDescriptorTests.cpp
        unit/runtime/physics/PhysicsWorldLifecycleTests.cpp
        unit/runtime/physics/PhysicsWorldSettingsTests.cpp
        unit/runtime/physics/PhysicsWorldSolverJobTests.cpp
        unit/runtime/physics/PhysicsWorldTests.cpp
)
target_compile_features(HoroPhysicsTests PRIVATE cxx_std_20)
target_include_directories(HoroPhysicsTests PRIVATE ${PROJECT_SOURCE_DIR}/src/physics ${PROJECT_SOURCE_DIR}/tests/support)
target_compile_definitions(HoroPhysicsTests PRIVATE HORO_TEST_PHYSICS_NATIVE=$<BOOL:${HORO_BUILD_PHYSICS_NATIVE}>)
target_link_libraries(HoroPhysicsTests PRIVATE HoroEngine::Physics HoroEngine::PhysicsSceneIntegration HoroEngine::Runtime HoroEngine::Platform)
horo_register_catch_test(HoroPhysicsTests LABELS "unit;physics;headless")

add_executable(HoroPrefabPhysicsGroupTests unit/runtime/physics/PrefabPhysicsGroupTests.cpp)
target_compile_features(HoroPrefabPhysicsGroupTests PRIVATE cxx_std_20)
target_compile_definitions(HoroPrefabPhysicsGroupTests PRIVATE HORO_TEST_PHYSICS_NATIVE=$<BOOL:${HORO_BUILD_PHYSICS_NATIVE}>)
target_link_libraries(HoroPrefabPhysicsGroupTests PRIVATE HoroEngine::Physics)
horo_register_catch_test(HoroPrefabPhysicsGroupTests LABELS "unit;physics;prefab;headless")

add_executable(HoroPrefabPhysicsPublicHeaderConsumer support/PrefabPhysicsPublicHeaderConsumer.cpp)
target_compile_features(HoroPrefabPhysicsPublicHeaderConsumer PRIVATE cxx_std_20)
target_link_libraries(HoroPrefabPhysicsPublicHeaderConsumer PRIVATE HoroEngine::PhysicsSceneIntegration)
add_test(NAME HoroPrefabPhysicsPublicHeaderConsumer COMMAND HoroPrefabPhysicsPublicHeaderConsumer)

add_executable(HoroPrefabAIGroupTests unit/runtime/ai/PrefabAIGroupTests.cpp)
target_compile_features(HoroPrefabAIGroupTests PRIVATE cxx_std_20)
target_link_libraries(HoroPrefabAIGroupTests PRIVATE HoroEngine::AISceneIntegration)
target_include_directories(HoroPrefabAIGroupTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/support)
horo_register_catch_test(HoroPrefabAIGroupTests LABELS "unit;ai;prefab;headless")

add_executable(HoroPrefabAIPublicHeaderConsumer support/PrefabAIPublicHeaderConsumer.cpp)
target_compile_features(HoroPrefabAIPublicHeaderConsumer PRIVATE cxx_std_20)
target_link_libraries(HoroPrefabAIPublicHeaderConsumer PRIVATE HoroEngine::AISceneIntegration)
add_test(NAME HoroPrefabAIPublicHeaderConsumer COMMAND HoroPrefabAIPublicHeaderConsumer)

add_executable(HoroAITests
        support/AllocationProbe.cpp
        unit/runtime/ai/AICanonicalStateTests.cpp
        unit/runtime/ai/AIIdentityTests.cpp
        unit/runtime/ai/AISceneActivationTests.cpp
        unit/runtime/ai/AISceneRestoreTests.cpp
        unit/runtime/ai/AISceneComponentsTests.cpp
        unit/runtime/ai/AITaskLifecycleTests.cpp
        unit/runtime/ai/BehaviorTreeTests.cpp
        unit/runtime/ai/BehaviorTreeRuntimeTests.cpp
        unit/runtime/ai/BehaviorTreeLifecycleTests.cpp
        unit/runtime/ai/BehaviorTreeAsyncTests.cpp
        unit/runtime/ai/BlackboardInstanceTests.cpp
        unit/runtime/ai/BlackboardSchemaTests.cpp
        unit/runtime/ai/DecisionAssetValidationTests.cpp
        unit/runtime/ai/StateMachineTests.cpp
        unit/runtime/ai/EnvironmentQuerySchemaTests.cpp
        unit/runtime/ai/EnvironmentQueryContextTests.cpp
        unit/runtime/ai/NullAIRuntimeTests.cpp
        unit/runtime/ai/PerceptionDescriptorRegistryTests.cpp
        unit/runtime/ai/PerceptionEventRoutingTests.cpp
        unit/runtime/ai/PerceptionMemoryTests.cpp
        unit/runtime/ai/PerceptionSpatialBroadphaseTests.cpp
)
target_compile_features(HoroAITests PRIVATE cxx_std_20)
target_link_libraries(HoroAITests PRIVATE HoroEngine::AI HoroEngine::AISceneIntegration HoroEngine::GameplayPerceptionIntegration)
horo_register_catch_test(HoroAITests LABELS "unit;ai;headless")
target_include_directories(HoroAITests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/support)

add_executable(HoroAISightTests
        support/AllocationProbe.cpp
        unit/runtime/ai/PerceptionSightTests.cpp
        unit/runtime/ai/PerceptionSightBatchTests.cpp
        unit/runtime/ai/PerceptionSightTestSupport.h)
target_compile_features(HoroAISightTests PRIVATE cxx_std_20)
target_link_libraries(HoroAISightTests PRIVATE HoroEngine::AISightIntegration)
target_include_directories(HoroAISightTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/support)
target_compile_definitions(HoroAISightTests PRIVATE HORO_TEST_PHYSICS_NATIVE=$<BOOL:${HORO_BUILD_PHYSICS_NATIVE}>)
horo_register_catch_test(HoroAISightTests LABELS "unit;ai;sight;headless")

add_executable(HoroAnimationApiTests
        unit/runtime/animation/AnimationCompressionTests.cpp
        unit/runtime/animation/AnimationClipTests.cpp
        unit/runtime/animation/AnimationComponentTests.cpp
        unit/runtime/animation/AnimationIdentityTests.cpp
        unit/runtime/animation/PoseStorageTests.cpp
        unit/runtime/animation/SkeletonAssetTests.cpp
        unit/runtime/animation/SkeletalMeshSkinningTests.cpp
)
target_compile_features(HoroAnimationApiTests PRIVATE cxx_std_20)
target_link_libraries(HoroAnimationApiTests PRIVATE HoroEngine::AnimationApi)
horo_register_catch_test(HoroAnimationApiTests LABELS "unit;animation;headless")

add_executable(HoroPCGTests
        unit/runtime/pcg/PCGCookedPlanTests.cpp
        unit/runtime/pcg/PCGGraphAssetTests.cpp
        unit/runtime/pcg/PCGGraphValidationTests.cpp
        unit/runtime/pcg/PCGGenerationPlanTests.cpp
        unit/runtime/pcg/PCGIdentityTests.cpp
        unit/runtime/pcg/PCGPointSchemaTests.cpp
        unit/runtime/pcg/PCGProvenanceTests.cpp
        unit/runtime/pcg/PCGSpatialSnapshotTests.cpp
        unit/runtime/pcg/PCGRegistryTests.cpp
        unit/runtime/pcg/PCGAsyncOperationTests.cpp
)
target_compile_features(HoroPCGTests PRIVATE cxx_std_20)
target_link_libraries(HoroPCGTests PRIVATE HoroEngine::PCG)
horo_register_catch_test(HoroPCGTests LABELS "unit;pcg;headless")

add_executable(HoroPCGTerrainAdapterTests
        unit/runtime/pcg/TerrainInputSnapshotTests.cpp
)
target_compile_features(HoroPCGTerrainAdapterTests PRIVATE cxx_std_20)
target_link_libraries(HoroPCGTerrainAdapterTests PRIVATE HoroEngine::PCGTerrainAdapter)
horo_register_catch_test(HoroPCGTerrainAdapterTests LABELS "unit;pcg;terrain;headless")

add_executable(HoroVfxApiTests
        support/AllocationProbe.cpp
        unit/runtime/vfx/CpuParticleBufferTests.cpp
        unit/runtime/vfx/CpuParticleSimulatorTests.cpp
        unit/runtime/vfx/CpuSimulationQualificationTests.cpp
        unit/runtime/vfx/CpuParticleForceQualificationTests.cpp
        unit/runtime/vfx/CpuParticleOverLifePayloadTests.cpp
        unit/runtime/vfx/CpuParticleSpawnPipelineTests.cpp
        unit/runtime/vfx/EffectInstancePoolTests.cpp
        unit/runtime/vfx/ParticleSystemDescriptorTests.cpp
        unit/runtime/vfx/VfxFoundationQualificationTests.cpp
        unit/runtime/vfx/VfxIdentityTests.cpp
        unit/runtime/vfx/VfxQualityPolicyTests.cpp
)
target_compile_features(HoroVfxApiTests PRIVATE cxx_std_20)
target_include_directories(HoroVfxApiTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(HoroVfxApiTests PRIVATE HORO_VFX_CPU_BASELINE_PATH="${PROJECT_SOURCE_DIR}/tests/fixtures/vfx/cpu_simulation_baseline.json")
target_link_libraries(HoroVfxApiTests PRIVATE HoroEngine::VfxApi nlohmann_json::nlohmann_json)
horo_register_catch_test(HoroVfxApiTests LABELS "unit;vfx;headless")

add_executable(HoroVfxCpuSimulationBenchmark EXCLUDE_FROM_ALL benchmarks/vfx/CpuSimulationBenchmark.cpp)
target_compile_features(HoroVfxCpuSimulationBenchmark PRIVATE cxx_std_20)
target_include_directories(HoroVfxCpuSimulationBenchmark PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(HoroVfxCpuSimulationBenchmark PRIVATE HoroEngine::VfxApi)

add_executable(HoroCpuForceBenchmark EXCLUDE_FROM_ALL benchmarks/vfx/CpuForceBenchmark.cpp)
target_compile_features(HoroCpuForceBenchmark PRIVATE cxx_std_20)
target_link_libraries(HoroCpuForceBenchmark PRIVATE HoroEngine::VfxApi)

add_executable(HoroDestructionApiTests
        support/AllocationProbe.cpp
        unit/runtime/destruction/DestructibleDescriptorTests.cpp
        unit/runtime/destruction/DestructionCommandTests.cpp
        unit/runtime/destruction/DestructionCompositionTests.cpp
        unit/runtime/destruction/DestructionIdentityTests.cpp
        unit/runtime/destruction/DestructionRegistryTests.cpp
        unit/runtime/destruction/DestructionStateMachineTests.cpp
)
target_compile_features(HoroDestructionApiTests PRIVATE cxx_std_20)
target_link_libraries(HoroDestructionApiTests PRIVATE HoroEngine::DestructionApi)
target_include_directories(HoroDestructionApiTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
horo_register_catch_test(HoroDestructionApiTests LABELS "unit;destruction;headless")

add_executable(HoroPreFracturedImportTests
        unit/runtime/destruction/PreFracturedImportTests.cpp
        unit/runtime/destruction/PreFracturedIntersectionTests.cpp
)
target_compile_features(HoroPreFracturedImportTests PRIVATE cxx_std_20)
target_link_libraries(HoroPreFracturedImportTests PRIVATE HoroEngine::DestructionCook)
target_include_directories(HoroPreFracturedImportTests PRIVATE ${PROJECT_SOURCE_DIR}/src/runtime/destruction/cook)
target_compile_definitions(HoroPreFracturedImportTests PRIVATE HORO_UFBX_TEST_DATA_DIR="${ufbx_SOURCE_DIR}/data")
horo_register_catch_test(HoroPreFracturedImportTests LABELS "unit;destruction;headless")
add_executable(HoroDestructionRuntimeTests
        unit/runtime/destruction/DestructionDamageRuntimeTests.cpp
        unit/runtime/destruction/DestructionEventStreamTests.cpp
)
target_compile_features(HoroDestructionRuntimeTests PRIVATE cxx_std_20)
target_link_libraries(HoroDestructionRuntimeTests PRIVATE HoroEngine::DestructionRuntime)
horo_register_catch_test(HoroDestructionRuntimeTests LABELS "unit;destruction;headless")

add_executable(HoroDestructionApplicationTests unit/application/destruction/DestructionEventDispatcherTests.cpp)
target_compile_features(HoroDestructionApplicationTests PRIVATE cxx_std_20)
target_link_libraries(HoroDestructionApplicationTests PRIVATE HoroEngine::DestructionApplication)
horo_register_catch_test(HoroDestructionApplicationTests LABELS "unit;destruction;headless")

add_executable(HoroOfflineVoronoiTests unit/runtime/destruction/OfflineVoronoiTests.cpp)
target_compile_features(HoroOfflineVoronoiTests PRIVATE cxx_std_20)
target_link_libraries(HoroOfflineVoronoiTests PRIVATE HoroEngine::DestructionPhysicsCook)
horo_register_catch_test(HoroOfflineVoronoiTests LABELS "unit;destruction;headless")

add_executable(HoroChunkMeshCookTests unit/runtime/destruction/ChunkMeshCookTests.cpp unit/runtime/destruction/ChunkCollisionCookTests.cpp)
target_compile_features(HoroChunkMeshCookTests PRIVATE cxx_std_20)
target_link_libraries(HoroChunkMeshCookTests PRIVATE HoroEngine::DestructionPhysicsCook)
horo_register_catch_test(HoroChunkMeshCookTests LABELS "unit;destruction;headless")

add_executable(HoroDestructionReplicationTests
        unit/runtime/destruction/DestructionReplicationTests.cpp
)
target_compile_features(HoroDestructionReplicationTests PRIVATE cxx_std_20)
target_link_libraries(HoroDestructionReplicationTests PRIVATE HoroEngine::DestructionReplication)
horo_register_catch_test(HoroDestructionReplicationTests LABELS "unit;destruction;network;headless")

add_executable(HoroCinematicModelTests
        support/AllocationProbe.cpp
        unit/runtime/cinematic/CurveSamplingTests.cpp
        unit/runtime/cinematic/CinematicIdentityTests.cpp
        unit/runtime/cinematic/PropertyTrackCoverageTests.cpp
        unit/runtime/cinematic/PropertyTrackTests.cpp
        unit/runtime/cinematic/SequenceAssetTests.cpp
        unit/runtime/cinematic/TransformTrackTests.cpp
)
target_compile_features(HoroCinematicModelTests PRIVATE cxx_std_20)
target_include_directories(HoroCinematicModelTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(HoroCinematicModelTests PRIVATE HoroEngine::CinematicModel)
horo_register_catch_test(HoroCinematicModelTests LABELS "unit;cinematic;headless")
add_executable(HoroCinematicRuntimeTests
        support/AllocationProbe.cpp
        unit/runtime/cinematic/SequenceEvaluationTests.cpp
        unit/runtime/cinematic/SequencePlayerTests.cpp
        unit/runtime/cinematic/SequencePlaybackRuntimeCoverageTests.cpp
        unit/runtime/cinematic/SequencePlaybackRuntimeTests.cpp
        unit/runtime/cinematic/SequencePlaybackCoordinationTests.cpp
        unit/runtime/cinematic/SequencePlaybackLifecycleTests.cpp
        unit/runtime/cinematic/EventTrackDispatchTests.cpp
        unit/runtime/cinematic/EventTrackNativeIntegrationTests.cpp
)
target_compile_features(HoroCinematicRuntimeTests PRIVATE cxx_std_20)
target_include_directories(HoroCinematicRuntimeTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(HoroCinematicRuntimeTests PRIVATE HoroEngine::CinematicRuntime HoroEngine::CinematicScriptBridge)
horo_register_catch_test(HoroCinematicRuntimeTests LABELS "unit;cinematic;headless")
add_executable(HoroCinematicEventPublicHeaderConsumer support/CinematicEventPublicHeaderConsumer.cpp)
target_compile_features(HoroCinematicEventPublicHeaderConsumer PRIVATE cxx_std_20)
target_link_libraries(HoroCinematicEventPublicHeaderConsumer PRIVATE HoroEngine::CinematicScriptBridge)
add_test(NAME HoroCinematicEventPublicHeaderConsumer COMMAND HoroCinematicEventPublicHeaderConsumer)
add_executable(HoroCinematicPropertyIntegrationTests
        support/AllocationProbe.cpp
        unit/runtime/cinematic/PropertyTrackIntegrationTests.cpp
)
target_compile_features(HoroCinematicPropertyIntegrationTests PRIVATE cxx_std_20)
target_include_directories(HoroCinematicPropertyIntegrationTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(HoroCinematicPropertyIntegrationTests PRIVATE HoroEngine::EditorServices)
horo_register_catch_test(HoroCinematicPropertyIntegrationTests LABELS "unit;cinematic;editor;headless")
add_executable(HoroCameraCutRuntimeTests
        support/AllocationProbe.cpp
        unit/runtime/cinematic/CameraCutRuntimeTests.cpp
)
target_compile_features(HoroCameraCutRuntimeTests PRIVATE cxx_std_20)
target_include_directories(HoroCameraCutRuntimeTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(HoroCameraCutRuntimeTests PRIVATE HoroEngine::CinematicCameraRuntime)
horo_register_catch_test(HoroCameraCutRuntimeTests LABELS "unit;cinematic;camera;headless")
add_executable(HoroCameraCutPublicHeaderConsumer support/CameraCutPublicHeaderConsumer.cpp)
target_compile_features(HoroCameraCutPublicHeaderConsumer PRIVATE cxx_std_20)
target_link_libraries(HoroCameraCutPublicHeaderConsumer PRIVATE HoroEngine::CinematicCameraRuntime)
add_test(NAME HoroCameraCutPublicHeaderConsumer COMMAND HoroCameraCutPublicHeaderConsumer)
set_tests_properties(HoroCameraCutPublicHeaderConsumer PROPERTIES LABELS "cinematic;camera;headless")

add_executable(HoroCinematicCurveSamplingBenchmark EXCLUDE_FROM_ALL
        benchmarks/cinematic/CurveSamplingBenchmark.cpp
)
target_compile_features(HoroCinematicCurveSamplingBenchmark PRIVATE cxx_std_20)
target_link_libraries(HoroCinematicCurveSamplingBenchmark PRIVATE HoroEngine::CinematicModel)

add_executable(HoroNavigationTileDescriptorPublicConsumer support/NavigationTileDescriptorPublicConsumer.cpp)
target_link_libraries(HoroNavigationTileDescriptorPublicConsumer PRIVATE HoroEngine::NavigationApi)
add_test(NAME HoroNavigationTileDescriptorPublicConsumer COMMAND HoroNavigationTileDescriptorPublicConsumer)

add_executable(HoroNavMeshAssetPublicHeaderConsumer support/NavMeshAssetPublicHeaderConsumer.cpp)
target_compile_features(HoroNavMeshAssetPublicHeaderConsumer PRIVATE cxx_std_20)
target_link_libraries(HoroNavMeshAssetPublicHeaderConsumer PRIVATE HoroEngine::NavigationAssetSceneIntegration)
add_test(NAME HoroNavMeshAssetPublicHeaderConsumer COMMAND HoroNavMeshAssetPublicHeaderConsumer)
set_tests_properties(HoroNavMeshAssetPublicHeaderConsumer PROPERTIES LABELS "unit;navigation;headless;public_headers")

add_executable(HoroNavigationContentPublicConsumer support/NavigationContentPublicConsumer.cpp)
target_compile_features(HoroNavigationContentPublicConsumer PRIVATE cxx_std_20)
target_link_libraries(HoroNavigationContentPublicConsumer PRIVATE HoroEngine::NavigationContentIntegration)
add_test(NAME HoroNavigationContentPublicConsumer COMMAND HoroNavigationContentPublicConsumer)
set_tests_properties(HoroNavigationContentPublicConsumer PROPERTIES LABELS "unit;navigation;headless;public_headers")

if(NOT HORO_BUILD_NAVIGATION_RECAST_DETOUR)
    add_executable(HoroNavigationPortableContentTests unit/application/navigation/NavigationPortableContentTests.cpp)
    target_compile_features(HoroNavigationPortableContentTests PRIVATE cxx_std_20)
    target_include_directories(HoroNavigationPortableContentTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/support)
    target_link_libraries(HoroNavigationPortableContentTests PRIVATE HoroEngine::NavigationContentIntegration
            HoroEngine::NavigationRecastDetour HoroEngine::Platform HoroEngine::AudioCook HoroEngine::UpdateDownload miniz)
    horo_register_catch_test(HoroNavigationPortableContentTests LABELS "unit;navigation;headless;content;package;portable")
endif()

add_executable(HoroNavigationReleasePolicyTests unit/application/navigation/NavigationReleasePolicyTests.cpp)
target_compile_features(HoroNavigationReleasePolicyTests PRIVATE cxx_std_20)
target_include_directories(HoroNavigationReleasePolicyTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/support)
target_link_libraries(HoroNavigationReleasePolicyTests PRIVATE HoroEngine::NavigationContentIntegration HoroEngine::Platform HoroEngine::UpdateDownload nlohmann_json::nlohmann_json miniz)
horo_register_catch_test(HoroNavigationReleasePolicyTests LABELS "unit;navigation;headless;content;package")

add_executable(HoroNavigationApiTests
        support/navigation/DeterministicNavigationProvider.cpp
        unit/runtime/navigation/NavMeshDataTests.cpp
        unit/runtime/navigation/NavigationAreaRegistryTests.cpp
        unit/runtime/navigation/NavigationAgentProfileTests.cpp
        unit/runtime/navigation/NavigationBakeInputTests.cpp
        unit/runtime/navigation/NavigationTileDependenciesTests.cpp
        unit/runtime/navigation/NavigationCapabilitiesTests.cpp
        unit/runtime/navigation/NavigationIdentityTests.cpp
        unit/runtime/navigation/NavigationLinkValidationTests.cpp
        unit/runtime/navigation/NavigationLinkValidationGenerationTests.cpp
        unit/runtime/navigation/NavigationOutcomesTests.cpp
        unit/runtime/navigation/NavigationProjectProfileTests.cpp
        unit/runtime/navigation/NavigationProviderTests.cpp
        unit/runtime/navigation/NavigationSourceGeometryTests.cpp
        unit/runtime/navigation/NavigationDataSerializationTests.cpp
        unit/runtime/navigation/NavigationDefinitionTests.cpp
)
target_compile_features(HoroNavigationApiTests PRIVATE cxx_std_20)
target_include_directories(HoroNavigationApiTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/support)
target_link_libraries(HoroNavigationApiTests PRIVATE HoroEngine::NavigationApi HoroEngine::NavigationNull)
horo_register_catch_test(HoroNavigationApiTests LABELS "unit;navigation;headless")

add_executable(HoroNavigationDataQualificationTests
        unit/runtime/navigation/NavigationDataQualificationTests.cpp
        unit/runtime/navigation/NavigationPrefabDataQualificationTests.cpp
        support/AllocationProbe.cpp)
target_compile_features(HoroNavigationDataQualificationTests PRIVATE cxx_std_20)
target_compile_definitions(HoroNavigationDataQualificationTests PRIVATE
        HORO_QUALIFICATION_BUILD_TYPE="$<CONFIG>"
        HORO_QUALIFICATION_PLATFORM="${CMAKE_SYSTEM_NAME}/${CMAKE_SYSTEM_PROCESSOR}")
target_include_directories(HoroNavigationDataQualificationTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/support)
target_link_libraries(HoroNavigationDataQualificationTests PRIVATE HoroEngine::NavigationApi HoroEngine::PrefabSceneExpansion)
horo_register_catch_test(HoroNavigationDataQualificationTests LABELS "unit;navigation;headless;qualification")

add_executable(HoroNavigationRuntimeTests
        support/AllocationProbe.cpp
        unit/runtime/navigation/NavigationBakeJobsTests.cpp
        unit/runtime/navigation/NavigationCrowdSnapshotTests.cpp
        unit/runtime/navigation/NavigationDynamicRegistryTests.cpp
        unit/runtime/navigation/NavigationObstacleOverlayTests.cpp
        unit/runtime/navigation/NavigationAgentRegistryTests.cpp
        unit/runtime/navigation/NavigationPathPolicyTests.cpp
        unit/runtime/navigation/NavigationFoundationQualificationTests.cpp
        unit/runtime/navigation/NavigationRuntimeQueuesTests.cpp
        unit/runtime/navigation/NavigationWorldLifecycleTests.cpp
)
target_compile_features(HoroNavigationRuntimeTests PRIVATE cxx_std_20)
target_include_directories(HoroNavigationRuntimeTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/support)
target_link_libraries(HoroNavigationRuntimeTests PRIVATE HoroEngine::NavigationRuntime)
horo_register_catch_test(HoroNavigationRuntimeTests LABELS "unit;navigation;headless;lifecycle")

add_executable(HoroNavigationBakeDiagnosticsPublicConsumer support/NavigationBakeDiagnosticsPublicConsumer.cpp)
target_link_libraries(HoroNavigationBakeDiagnosticsPublicConsumer PRIVATE HoroEngine::NavigationBakeService)
add_test(NAME HoroNavigationBakeDiagnosticsPublicConsumer COMMAND HoroNavigationBakeDiagnosticsPublicConsumer)

if(HORO_BUILD_NAVIGATION_RECAST_DETOUR)
    add_executable(HoroNavigationBakeServiceTests unit/runtime/navigation/NavigationBakeServiceTests.cpp
            unit/runtime/navigation/NavigationBakeDiagnosticsTests.cpp
            unit/runtime/navigation/NavigationBakeDiagnosticRecoveryTests.cpp
            unit/application/navigation/NavigationBakePublicationTests.cpp)
    target_compile_features(HoroNavigationBakeServiceTests PRIVATE cxx_std_20)
    target_include_directories(HoroNavigationBakeServiceTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/support)
    target_link_libraries(HoroNavigationBakeServiceTests PRIVATE HoroEngine::NavigationBakeService HoroEngine::NavigationRecastDetour)
    horo_register_catch_test(HoroNavigationBakeServiceTests LABELS "unit;navigation;headless;cook")
    add_executable(HoroNavigationRecastDetourTests
            unit/runtime/navigation/RecastDetourProviderTests.cpp
            unit/runtime/navigation/RecastDetourLinkValidationTests.cpp
            unit/runtime/navigation/RecastDetourProviderPathTests.cpp
            unit/runtime/navigation/RecastDetourMeshBuilderTests.cpp
    )
    target_compile_features(HoroNavigationRecastDetourTests PRIVATE cxx_std_20)
    target_include_directories(HoroNavigationRecastDetourTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/support)
    target_link_libraries(HoroNavigationRecastDetourTests PRIVATE HoroEngine::NavigationRecastDetour)
    horo_register_catch_test(HoroNavigationRecastDetourTests LABELS "unit;navigation;headless;provider")
    add_executable(HoroNavigationNativeContentTests unit/application/navigation/NavigationNativeContentTests.cpp)
    target_compile_features(HoroNavigationNativeContentTests PRIVATE cxx_std_20)
    target_include_directories(HoroNavigationNativeContentTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/support)
    target_link_libraries(HoroNavigationNativeContentTests PRIVATE HoroEngine::NavigationContentIntegration HoroEngine::NavigationBakeService
            HoroEngine::NavigationRecastDetour HoroEngine::Platform HoroEngine::AudioCook HoroEngine::UpdateDownload miniz)
    horo_register_catch_test(HoroNavigationNativeContentTests LABELS "unit;navigation;headless;content;package;native")

    add_executable(HoroNavigationAssetLoadingTests unit/runtime/navigation/NavMeshAssetLoadingTests.cpp)
    target_compile_features(HoroNavigationAssetLoadingTests PRIVATE cxx_std_20)
    target_include_directories(HoroNavigationAssetLoadingTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/support)
    target_link_libraries(HoroNavigationAssetLoadingTests PRIVATE HoroEngine::NavigationAssetSceneIntegration HoroEngine::NavigationBakeService HoroEngine::NavigationRecastDetour)
    horo_register_catch_test(HoroNavigationAssetLoadingTests LABELS "unit;navigation;headless;assets;lifecycle;provider")
    add_executable(HoroNavigationAssetDataQualificationTests
            unit/runtime/navigation/NavigationAssetDataQualificationTests.cpp
            support/AllocationProbe.cpp)
    target_compile_features(HoroNavigationAssetDataQualificationTests PRIVATE cxx_std_20)
    target_compile_definitions(HoroNavigationAssetDataQualificationTests PRIVATE
            HORO_QUALIFICATION_BUILD_TYPE="$<CONFIG>"
            HORO_QUALIFICATION_PLATFORM="${CMAKE_SYSTEM_NAME}/${CMAKE_SYSTEM_PROCESSOR}")
    target_include_directories(HoroNavigationAssetDataQualificationTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/support)
    target_link_libraries(HoroNavigationAssetDataQualificationTests PRIVATE HoroEngine::NavigationAssetSceneIntegration HoroEngine::NavigationRecastDetour)
    horo_register_catch_test(HoroNavigationAssetDataQualificationTests LABELS "unit;navigation;headless;assets;qualification;provider")
    add_executable(HoroNavigationCrowdDetourTests unit/runtime/navigation/RecastDetourCrowdProviderTests.cpp)
    target_compile_features(HoroNavigationCrowdDetourTests PRIVATE cxx_std_20)
    target_include_directories(HoroNavigationCrowdDetourTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/support)
    target_link_libraries(HoroNavigationCrowdDetourTests PRIVATE HoroEngine::NavigationCrowdDetour)
    horo_register_catch_test(HoroNavigationCrowdDetourTests LABELS "unit;navigation;headless;provider")
else()
    add_executable(HoroNavigationCrowdOmittedTests unit/runtime/navigation/RecastDetourCrowdOmittedTests.cpp)
    target_compile_features(HoroNavigationCrowdOmittedTests PRIVATE cxx_std_20)
    target_include_directories(HoroNavigationCrowdOmittedTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/support)
    target_link_libraries(HoroNavigationCrowdOmittedTests PRIVATE HoroEngine::NavigationCrowdDetour)
    horo_register_catch_test(HoroNavigationCrowdOmittedTests LABELS "unit;navigation;headless;provider")
endif()

add_executable(HoroXRApiTests
        support/AllocationProbe.cpp
        unit/runtime/xr/XRApiContractTests.cpp
        unit/runtime/xr/XRLoaderPreflightTests.cpp
        unit/runtime/xr/XRSpacePoseTests.cpp
        unit/runtime/xr/XRTrackingSnapshotTests.cpp
        unit/runtime/xr/XRViewRenderPlanTests.cpp
)
target_compile_features(HoroXRApiTests PRIVATE cxx_std_20)
target_include_directories(HoroXRApiTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(HoroXRApiTests PRIVATE HoroEngine::XRApi)
horo_register_catch_test(HoroXRApiTests LABELS "unit;xr;headless")

add_executable(HoroXRRuntimeTests
        support/AllocationProbe.cpp
        unit/runtime/xr/XRFeatureNegotiationTests.cpp
        unit/runtime/xr/XRFrameLifecycleTests.cpp
        unit/runtime/xr/XRSessionLifecycleTests.cpp
)
target_compile_features(HoroXRRuntimeTests PRIVATE cxx_std_20)
target_include_directories(HoroXRRuntimeTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(HoroXRRuntimeTests PRIVATE HoroEngine::XRRuntime)
horo_register_catch_test(HoroXRRuntimeTests LABELS "unit;xr;headless;lifecycle")

if(HORO_BUILD_XR_OPENXR)
    add_executable(HoroXROpenXRTests unit/runtime/xr/OpenXRNativeSessionTests.cpp)
    target_include_directories(HoroXROpenXRTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(HoroXROpenXRTests PRIVATE HoroEngine::XROpenXR HoroXROpenXRHostInterface)
    horo_register_catch_test(HoroXROpenXRTests LABELS "unit;xr;headless;native;lifecycle")

    add_library(HoroXROpenXRHostInterfaceConsumer OBJECT public_headers/OpenXRHostInterfaceConsumer.cpp)
    target_link_libraries(HoroXROpenXRHostInterfaceConsumer PRIVATE HoroXROpenXRHostInterface)
endif()

add_executable(HoroTerrainApiTests
        unit/runtime/terrain/FoliageDefinitionTests.cpp
        unit/runtime/terrain/TerrainCompositionTests.cpp
        unit/runtime/terrain/TerrainDescriptorTests.cpp
        unit/runtime/terrain/TerrainFoliageRegistryTests.cpp
        unit/runtime/terrain/TerrainIdentityTests.cpp
)
target_compile_features(HoroTerrainApiTests PRIVATE cxx_std_20)
target_link_libraries(HoroTerrainApiTests PRIVATE HoroEngine::TerrainApi Threads::Threads)
horo_register_catch_test(HoroTerrainApiTests LABELS "unit;terrain;headless")

add_executable(HoroTerrainMaterialTests
        support/AllocationProbe.cpp
        unit/runtime/terrain/TerrainMaterialTests.cpp
        unit/runtime/terrain/TerrainMaterialBindingTests.cpp
)
target_compile_features(HoroTerrainMaterialTests PRIVATE cxx_std_20)
target_include_directories(HoroTerrainMaterialTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(HoroTerrainMaterialTests PRIVATE HoroEngine::TerrainRender Threads::Threads)
horo_register_catch_test(HoroTerrainMaterialTests LABELS "unit;terrain;renderer;headless;lifecycle")
add_executable(HoroTerrainMaterialPublicHeaderConsumer support/TerrainMaterialPublicHeaderConsumer.cpp)
target_compile_features(HoroTerrainMaterialPublicHeaderConsumer PRIVATE cxx_std_20)
target_link_libraries(HoroTerrainMaterialPublicHeaderConsumer PRIVATE HoroEngine::TerrainRender)
add_test(NAME HoroTerrainMaterialPublicHeaderConsumer COMMAND HoroTerrainMaterialPublicHeaderConsumer)
set_tests_properties(HoroTerrainMaterialPublicHeaderConsumer PROPERTIES LABELS "unit;terrain;renderer;headless")

add_executable(HoroTerrainImportTests
        unit/runtime/terrain/TerrainSourceImportTests.cpp
)
target_compile_features(HoroTerrainImportTests PRIVATE cxx_std_20)
target_link_libraries(HoroTerrainImportTests PRIVATE HoroEngine::TerrainImport)
horo_register_catch_test(HoroTerrainImportTests LABELS "unit;terrain;assets;headless")
add_executable(HoroTerrainTileCookTests
        unit/runtime/terrain/TerrainTileCookTests.cpp
)
target_compile_features(HoroTerrainTileCookTests PRIVATE cxx_std_20)
target_link_libraries(HoroTerrainTileCookTests PRIVATE HoroEngine::TerrainCook)
horo_register_catch_test(HoroTerrainTileCookTests LABELS "unit;terrain;assets;headless")
add_executable(HoroTerrainFoliageClusterTests unit/runtime/terrain/FoliageClusterCookTests.cpp)
target_compile_features(HoroTerrainFoliageClusterTests PRIVATE cxx_std_20)
target_link_libraries(HoroTerrainFoliageClusterTests PRIVATE HoroEngine::TerrainCook)
horo_register_catch_test(HoroTerrainFoliageClusterTests LABELS "unit;terrain;foliage;headless")
add_executable(HoroTerrainFoliageClusterPublicHeaderConsumer support/TerrainFoliageClusterPublicHeaderConsumer.cpp)
target_compile_features(HoroTerrainFoliageClusterPublicHeaderConsumer PRIVATE cxx_std_20)
target_link_libraries(HoroTerrainFoliageClusterPublicHeaderConsumer PRIVATE HoroEngine::TerrainCook)
add_test(NAME HoroTerrainFoliageClusterPublicHeaderConsumer COMMAND HoroTerrainFoliageClusterPublicHeaderConsumer)
set_tests_properties(HoroTerrainFoliageClusterPublicHeaderConsumer PROPERTIES LABELS "unit;terrain;headless")

add_executable(HoroTerrainFoliagePlacementTests
        unit/runtime/terrain/FoliagePlacementCookTests.cpp
)
target_compile_features(HoroTerrainFoliagePlacementTests PRIVATE cxx_std_20)
target_link_libraries(HoroTerrainFoliagePlacementTests PRIVATE HoroEngine::TerrainCook)
horo_register_catch_test(HoroTerrainFoliagePlacementTests LABELS "unit;terrain;foliage;headless")
add_executable(HoroTerrainFoliagePlacementPublicHeaderConsumer support/TerrainFoliagePlacementPublicHeaderConsumer.cpp)
target_compile_features(HoroTerrainFoliagePlacementPublicHeaderConsumer PRIVATE cxx_std_20)
target_link_libraries(HoroTerrainFoliagePlacementPublicHeaderConsumer PRIVATE HoroEngine::TerrainCook)
add_test(NAME HoroTerrainFoliagePlacementPublicHeaderConsumer COMMAND HoroTerrainFoliagePlacementPublicHeaderConsumer)
set_tests_properties(HoroTerrainFoliagePlacementPublicHeaderConsumer PROPERTIES LABELS "unit;terrain;headless")
add_executable(HoroTerrainAsyncJobsTests
        unit/runtime/terrain/TerrainAsyncJobsTests.cpp
)
target_compile_features(HoroTerrainAsyncJobsTests PRIVATE cxx_std_20)
target_link_libraries(HoroTerrainAsyncJobsTests PRIVATE HoroEngine::TerrainRuntime Threads::Threads)
horo_register_catch_test(HoroTerrainAsyncJobsTests LABELS "unit;terrain;headless")

add_executable(HoroWorldStreamingTests
        unit/runtime/world_streaming/CellAttachmentManifestTests.cpp
        unit/runtime/world_streaming/NetworkStreamingAuthorityTests.cpp
        unit/runtime/world_streaming/OriginFrameTests.cpp
        unit/runtime/world_streaming/OriginRebaseTransactionTests.cpp
        unit/runtime/world_streaming/OriginShiftPolicyTests.cpp
        unit/runtime/world_streaming/RuntimeEntityCellExitOperationTests.cpp
        unit/runtime/world_streaming/WorldEntityReferenceFixupTests.cpp
        unit/runtime/world_streaming/CookedWorldIndexManifestTests.cpp
        unit/runtime/world_streaming/FallbackStreamingProviderTests.cpp
        unit/runtime/world_streaming/StreamingBudgetModelTests.cpp
        unit/runtime/world_streaming/StreamingFeatureBudgetReservationsTests.cpp
        unit/runtime/world_streaming/SharedAssetResidencyTests.cpp
        unit/runtime/world_streaming/StreamingCellOperationTests.cpp
        unit/runtime/world_streaming/StreamingCellCandidateTests.cpp
        unit/runtime/world_streaming/StreamingCellArtifactTests.cpp
        unit/runtime/world_streaming/StreamingCellAssetRequestTests.cpp
        unit/runtime/world_streaming/WorldPackageChunkAssignmentTests.cpp
        unit/runtime/world_streaming/StreamingOwnerFrameBudgetTests.cpp
        unit/runtime/world_streaming/StreamingCellActivationTests.cpp
        unit/runtime/world_streaming/StreamingCellDirectionTests.cpp
        unit/runtime/world_streaming/StreamingCellFrameBudgetTests.cpp
        unit/runtime/world_streaming/StreamingCellStateTests.cpp
        unit/runtime/world_streaming/StreamingCellStabilityTests.cpp
        unit/runtime/world_streaming/StreamingFailurePolicyTests.cpp
        unit/runtime/world_streaming/StreamingSchedulerAdmissionTests.cpp
        unit/runtime/world_streaming/WorldDependencyPlanTests.cpp
        unit/runtime/world_streaming/WorldAuthoringContractTests.cpp
        unit/runtime/world_streaming/StreamingDesiredStateTests.cpp
        unit/runtime/world_streaming/StreamingDesiredStateReductionTests.cpp
        unit/runtime/world_streaming/StreamingPriorityPolicyTests.cpp
        unit/runtime/world_streaming/StreamingFairQueueTests.cpp
        unit/runtime/world_streaming/StreamingSourceDescriptorTests.cpp
        unit/runtime/world_streaming/StreamingSourcePrefetchTests.cpp
        unit/runtime/world_streaming/StreamingSourceRangeTests.cpp
        unit/runtime/world_streaming/WorldCellQuantizationTests.cpp
        unit/runtime/world_streaming/WorldPartitionDescriptorTests.cpp
        unit/runtime/world_streaming/WorldLayerFilteringTests.cpp
        unit/runtime/world_streaming/WorldLayerOwnershipModelTests.cpp
        unit/runtime/world_streaming/WorldLayerStateTests.cpp
        unit/runtime/world_streaming/WorldObjectOwnershipTests.cpp
        unit/runtime/world_streaming/WorldPartitionCapabilityProfileTests.cpp
        unit/runtime/world_streaming/WorldPartitionRegistryTests.cpp
        unit/runtime/world_streaming/WorldStreamingFoundationQualificationTests.cpp
        unit/runtime/world_streaming/WorldSpatialAssignmentTests.cpp
        unit/runtime/world_streaming/WorldSpatialObjectDescriptorTests.cpp
        unit/runtime/world_streaming/WorldSpanningObjectPlanTests.cpp
        unit/runtime/world_streaming/WorldStreamingIdentityTests.cpp
        unit/runtime/world_streaming/WorldStreamingRuntimeCompositionTests.cpp
        unit/runtime/world_streaming/WorldStreamingDiagnosticSnapshotTests.cpp
        unit/runtime/world_streaming/WorldStreamingMetricsTests.cpp
        unit/runtime/world_streaming/WorldStreamingTraceTests.cpp
)
target_compile_features(HoroWorldStreamingTests PRIVATE cxx_std_20)
target_link_libraries(HoroWorldStreamingTests PRIVATE HoroEngine::WorldStreaming)
horo_register_catch_test(HoroWorldStreamingTests LABELS "unit;world_streaming;headless")

add_executable(HoroPrefabSceneExpansionContractConsumer support/PrefabSceneExpansionPublicHeaderConsumer.cpp)
target_compile_features(HoroPrefabSceneExpansionContractConsumer PRIVATE cxx_std_20)
target_link_libraries(HoroPrefabSceneExpansionContractConsumer PRIVATE HoroEngine::PrefabSceneExpansion)
add_test(NAME HoroPrefabSceneExpansionContractConsumer COMMAND HoroPrefabSceneExpansionContractConsumer)
set_tests_properties(HoroPrefabSceneExpansionContractConsumer PROPERTIES LABELS "unit;prefab;headless")

add_executable(HoroPrefabSceneExpansionTests unit/runtime/prefab/PrefabSceneExpansionTests.cpp)
target_compile_features(HoroPrefabSceneExpansionTests PRIVATE cxx_std_20)
target_link_libraries(HoroPrefabSceneExpansionTests PRIVATE HoroEngine::PrefabSceneExpansion)
horo_register_catch_test(HoroPrefabSceneExpansionTests LABELS "unit;prefab;headless")

add_executable(HoroSceneRuntimeConversionTests unit/scene/SceneRuntimeConversionTests.cpp unit/scene/CookedSceneDefinitionTests.cpp)
target_compile_features(HoroSceneRuntimeConversionTests PRIVATE cxx_std_20)
target_link_libraries(HoroSceneRuntimeConversionTests PRIVATE HoroEngine::SceneRuntimeConversion HoroEngine::SceneCook)
horo_register_catch_test(HoroSceneRuntimeConversionTests LABELS "unit;scene;prefab;headless")

add_executable(HoroPrefabCookPublicHeaderConsumer support/PrefabCookPublicHeaderConsumer.cpp)
add_executable(HoroPrefabSceneCookHostTests unit/application/PrefabSceneCookHostTests.cpp unit/application/PrefabCookSchemaContextTests.cpp)
target_compile_features(HoroPrefabSceneCookHostTests PRIVATE cxx_std_20)
# Shared AssetCook test fixtures compile their builtin registration helper even
# when the host test selects SettingsCooker. Keep that test-only boundary narrow.
target_include_directories(HoroPrefabSceneCookHostTests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/support
        ${PROJECT_SOURCE_DIR}/src/runtime/assets/cooker/builtin)
target_link_libraries(HoroPrefabSceneCookHostTests PRIVATE HoroEngine::PrefabCookHost HoroEngine::SceneSource nlohmann_json::nlohmann_json miniz)
horo_register_catch_test(HoroPrefabSceneCookHostTests LABELS "unit;prefab;headless;host")
target_compile_features(HoroPrefabCookPublicHeaderConsumer PRIVATE cxx_std_20)
target_link_libraries(HoroPrefabCookPublicHeaderConsumer PRIVATE HoroEngine::SceneRuntimeConversion HoroEngine::SceneCook HoroEngine::Assets HoroEngine::PrefabCookHost)
add_test(NAME HoroPrefabCookPublicHeaderConsumer COMMAND HoroPrefabCookPublicHeaderConsumer)
set_tests_properties(HoroPrefabCookPublicHeaderConsumer PROPERTIES LABELS "unit;prefab;headless;public_headers")

add_executable(HoroCookedPrefabTests unit/runtime/prefab/CookedPrefabTests.cpp)
target_compile_features(HoroCookedPrefabTests PRIVATE cxx_std_20)
target_link_libraries(HoroCookedPrefabTests PRIVATE HoroEngine::Prefab)
horo_register_catch_test(HoroCookedPrefabTests LABELS "unit;prefab;headless")

add_executable(HoroPrefabTemplateProviderTests unit/runtime/prefab/PrefabTemplateProviderTests.cpp)
target_compile_features(HoroPrefabTemplateProviderTests PRIVATE cxx_std_20)
target_link_libraries(HoroPrefabTemplateProviderTests PRIVATE HoroEngine::PrefabRuntime)
horo_register_catch_test(HoroPrefabTemplateProviderTests LABELS "unit;prefab;headless")

add_executable(HoroPrefabTemplateProviderPublicHeaderConsumer support/PrefabTemplateProviderPublicHeaderConsumer.cpp)
target_compile_features(HoroPrefabTemplateProviderPublicHeaderConsumer PRIVATE cxx_std_20)
target_link_libraries(HoroPrefabTemplateProviderPublicHeaderConsumer PRIVATE HoroEngine::PrefabRuntime)
add_test(NAME HoroPrefabTemplateProviderPublicHeaderConsumer COMMAND HoroPrefabTemplateProviderPublicHeaderConsumer)
set_tests_properties(HoroPrefabTemplateProviderPublicHeaderConsumer PROPERTIES LABELS "unit;prefab;headless")

add_executable(HoroPrefabTests
        unit/runtime/prefab/PrefabDependencyGraphTests.cpp
        unit/runtime/prefab/PrefabDiagnosticsTests.cpp
        unit/runtime/prefab/PrefabDocumentTests.cpp
        unit/runtime/prefab/PrefabDocumentSerializationTests.cpp
        unit/runtime/prefab/PrefabIdentityTests.cpp
        unit/runtime/prefab/PrefabLimitsTests.cpp
        unit/runtime/prefab/PrefabSceneIdentityRemapTests.cpp
        unit/runtime/prefab/PrefabSourceResolverTests.cpp
)
target_compile_features(HoroPrefabTests PRIVATE cxx_std_20)
target_link_libraries(HoroPrefabTests PRIVATE HoroEngine::PrefabAuthoring)
horo_register_catch_test(HoroPrefabTests LABELS "unit;prefab;headless")

foreach(horo_physics_probe IN ITEMS iOS Linux Darwin)
    set(horo_physics_expected "Canonical Physics is not qualified for this target")
    if(horo_physics_probe STREQUAL "Linux")
        set(horo_physics_expected "Canonical Physics supports Linux/Windows x86_64 only")
    elseif(horo_physics_probe STREQUAL "Darwin")
        set(horo_physics_expected "Canonical Physics requires macOS 14 or newer")
    endif()
    add_test(NAME HoroPhysicsRejectsUnsupported${horo_physics_probe} COMMAND ${CMAKE_COMMAND}
        -DHORO_SOURCE_DIR=${PROJECT_SOURCE_DIR}
        -DHORO_SYSTEM=${horo_physics_probe} -DHORO_PROCESSOR=arm64 -DHORO_DEPLOYMENT=13
        "-DHORO_EXPECTED_ERROR=${horo_physics_expected}"
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyPhysicsUnsupportedTarget.cmake)
endforeach()
if(HORO_BUILD_PHYSICS_NATIVE)
    add_executable(HoroPhysicsNativeBoundaryTests
        unit/runtime/physics/CanonicalNativeBoundaryTests.cpp
        unit/runtime/physics/CanonicalContainmentTests.cpp
        unit/runtime/physics/CanonicalPhysicsRuntimeTests.cpp
        unit/runtime/physics/CanonicalWorldSettingsTests.cpp)
    target_compile_features(HoroPhysicsNativeBoundaryTests PRIVATE cxx_std_20)
    target_include_directories(HoroPhysicsNativeBoundaryTests PRIVATE ${PROJECT_SOURCE_DIR}/src/physics)
    target_link_libraries(HoroPhysicsNativeBoundaryTests PRIVATE HoroEngine::Physics Jolt)
    horo_register_catch_test(HoroPhysicsNativeBoundaryTests LABELS "unit;physics;headless")
    add_test(NAME HoroPhysicsNotices COMMAND ${CMAKE_COMMAND}
        -DHORO_BINARY_DIR=${CMAKE_BINARY_DIR}
        -DHORO_DATA_DIR=${CMAKE_INSTALL_DATADIR}
        -DHORO_CONFIG=$<CONFIG>
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyPhysicsNotices.cmake)
endif()
add_executable(HoroGameplayRuntimeTests
        unit/gameplay/BehaviorRuntimeTests.cpp
        unit/gameplay/ComponentRegistryTests.cpp
        unit/gameplay/GameAssetTypeRegistryTests.cpp
        unit/gameplay/GameplayRegistrationTests.cpp
        unit/gameplay/ReplicationRegistrationTests.cpp
)
target_compile_features(HoroGameplayRuntimeTests PRIVATE cxx_std_20)
target_include_directories(HoroGameplayRuntimeTests PRIVATE "${PROJECT_SOURCE_DIR}/tests/helpers")
target_link_libraries(HoroGameplayRuntimeTests PRIVATE HoroEngine::GameplayRuntime)
horo_add_gameplay_module(HoroTestGameModule
        MODULE_ID game.tests
        NO_MANIFEST
        SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/gameplay_module/TestGameModule.cpp
)
target_include_directories(HoroTestGameModule PRIVATE "${PROJECT_SOURCE_DIR}/tests/helpers")
target_compile_definitions(HoroTestGameModule PRIVATE HORO_TEST_GAMEPLAY_PHYSICS=1)
target_link_libraries(HoroTestGameModule PRIVATE HoroEngine::GameplayPhysicsIntegration)
get_property(HORO_TEST_GAME_MODULE_REVISION_FILE TARGET HoroTestGameModule PROPERTY HORO_GAMEPLAY_DESCRIPTOR_REVISION_FILE)
target_include_directories(HoroCinematicRuntimeTests PRIVATE "${PROJECT_SOURCE_DIR}/tests/helpers")
target_compile_definitions(HoroCinematicRuntimeTests PRIVATE
        HORO_TEST_GAME_MODULE_PATH="$<TARGET_FILE:HoroTestGameModule>"
        HORO_TEST_GAME_MODULE_REVISION_PATH="${HORO_TEST_GAME_MODULE_REVISION_FILE}"
)
add_dependencies(HoroCinematicRuntimeTests HoroTestGameModule)
add_executable(HoroGameModuleHostTests
        unit/gameplay/GameModuleHostTests.cpp
        unit/gameplay/GameModuleContentPersistenceTests.cpp
)
target_include_directories(HoroGameModuleHostTests PRIVATE "${PROJECT_SOURCE_DIR}/tests/helpers")
target_compile_features(HoroGameModuleHostTests PRIVATE cxx_std_20)
target_compile_definitions(HoroGameModuleHostTests PRIVATE
        HORO_TEST_GAME_MODULE_PATH="$<TARGET_FILE:HoroTestGameModule>"
        HORO_TEST_GAME_MODULE_REVISION_PATH="${HORO_TEST_GAME_MODULE_REVISION_FILE}"
)
target_link_libraries(HoroGameModuleHostTests PRIVATE HoroEngine::GameplayModuleHost)
add_dependencies(HoroGameModuleHostTests HoroTestGameModule)
add_executable(HoroGameplayPhysicsTests unit/gameplay/GameplayPhysicsContextTests.cpp
    integration/gameplay/GameplayPhysicsHostTests.cpp
    integration/gameplay/GameplayCharacterSlopeTests.cpp
    integration/gameplay/LuaPhysicsLifecycleTests.cpp)
target_compile_features(HoroGameplayPhysicsTests PRIVATE cxx_std_20)
target_include_directories(HoroGameplayPhysicsTests PRIVATE "${PROJECT_SOURCE_DIR}/tests/helpers" "${PROJECT_SOURCE_DIR}/tests/unit/runtime/physics")
target_compile_definitions(HoroGameplayPhysicsTests PRIVATE
    HORO_TEST_PHYSICS_NATIVE=$<BOOL:${HORO_BUILD_PHYSICS_NATIVE}>
    HORO_TEST_SOURCE_ROOT="${PROJECT_SOURCE_DIR}"
    HORO_TEST_GAME_MODULE_PATH="$<TARGET_FILE:HoroTestGameModule>"
    HORO_TEST_GAME_MODULE_REVISION_PATH="${HORO_TEST_GAME_MODULE_REVISION_FILE}")
target_link_libraries(HoroGameplayPhysicsTests PRIVATE HoroEngine::GameplayPhysicsIntegration HoroEngine::GameplayModuleHost
    HoroNetworkProductHost)
add_dependencies(HoroGameplayPhysicsTests HoroTestGameModule)
horo_register_catch_test(HoroGameplayPhysicsTests LABELS "unit;gameplay;physics;headless")
add_test(NAME HoroGameplayProductLaunchContract
    COMMAND "${CMAKE_COMMAND}" "-DENGINE=$<TARGET_FILE:horo-engine>" "-DMODULE=$<TARGET_FILE:HoroTestGameModule>"
    "-DREVISION_FILE=${HORO_TEST_GAME_MODULE_REVISION_FILE}" "-DSOURCE_ROOT=${PROJECT_SOURCE_DIR}"
    "-DPHYSICS_NATIVE=$<BOOL:${HORO_BUILD_PHYSICS_NATIVE}>"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/GameplayProductLaunchContract.cmake")
set_tests_properties(HoroGameplayProductLaunchContract PROPERTIES LABELS "integration;gameplay;physics;headless;product" TIMEOUT 60)
add_executable(HoroGameplayPhysicsPublicHeaderConsumer support/GameplayPhysicsPublicHeaderConsumer.cpp)
target_compile_features(HoroGameplayPhysicsPublicHeaderConsumer PRIVATE cxx_std_20)
target_link_libraries(HoroGameplayPhysicsPublicHeaderConsumer PRIVATE HoroEngine::GameplayPhysicsIntegration)
add_test(NAME HoroGameplayPhysicsPublicHeaderConsumer COMMAND HoroGameplayPhysicsPublicHeaderConsumer)
add_executable(HoroGameplayPhysicsSdkHostConsumer support/GameplayPhysicsSdkHostConsumer.cpp)
target_compile_features(HoroGameplayPhysicsSdkHostConsumer PRIVATE cxx_std_20)
target_compile_definitions(HoroGameplayPhysicsSdkHostConsumer PRIVATE HORO_TEST_PHYSICS_NATIVE=$<BOOL:${HORO_BUILD_PHYSICS_NATIVE}>)
target_link_libraries(HoroGameplayPhysicsSdkHostConsumer PRIVATE HoroEngine::GameplayPhysicsIntegration HoroEngine::GameplayModuleHost)
add_test(NAME HoroGameplayPhysicsSdkContract COMMAND ${CMAKE_COMMAND}
    "-DSDK_DIR=${HORO_GAMEPLAY_SDK_PACKAGE_DIR}"
    "-DFIXTURE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/gameplay_physics_sdk"
    "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/physics_sdk_contract"
    "-DHOST_CONSUMER=$<TARGET_FILE:HoroGameplayPhysicsSdkHostConsumer>"
    "-DCXX_COMPILER=${CMAKE_CXX_COMPILER}"
    "-DGENERATOR=${CMAKE_GENERATOR}"
    "-DGENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}"
    "-DGENERATOR_TOOLSET=${CMAKE_GENERATOR_TOOLSET}"
    "-DBUILD_CONFIG=$<CONFIG>"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/GameplayPhysicsSdkContract.cmake")
set_tests_properties(HoroGameplayPhysicsSdkContract PROPERTIES LABELS "gameplay;physics;sdk;headless" RUN_SERIAL TRUE TIMEOUT 120)
add_executable(HoroLuaReplicationPublicHeaderConsumer support/LuaReplicationPublicHeaderConsumer.cpp)
target_compile_features(HoroLuaReplicationPublicHeaderConsumer PRIVATE cxx_std_20)
target_link_libraries(HoroLuaReplicationPublicHeaderConsumer PRIVATE HoroEngine::GameplayLua)
add_test(NAME HoroLuaReplicationPublicHeaderConsumer COMMAND HoroLuaReplicationPublicHeaderConsumer)
set_tests_properties(HoroLuaReplicationPublicHeaderConsumer PROPERTIES LABELS "unit;gameplay;headless")

# Only the declared gameplay dependency closure is visible; no root includes or concrete backend targets.
add_executable(HoroGameplayReplicationPublicHeaderConsumer support/GameplayReplicationPublicHeaderConsumer.cpp)
target_compile_features(HoroGameplayReplicationPublicHeaderConsumer PRIVATE cxx_std_20)
target_link_libraries(HoroGameplayReplicationPublicHeaderConsumer PRIVATE HoroEngine::GameplayLua)
add_test(NAME HoroGameplayReplicationPublicHeaderConsumer COMMAND HoroGameplayReplicationPublicHeaderConsumer)
set_tests_properties(HoroGameplayReplicationPublicHeaderConsumer PROPERTIES LABELS "unit;gameplay;headless;public_headers")

add_executable(HoroLuaBehaviorTests
        unit/gameplay/LuaBehaviorTests.cpp
        unit/gameplay/LuaReplicationTests.cpp
)
target_include_directories(HoroLuaBehaviorTests PRIVATE "${PROJECT_SOURCE_DIR}/tests/helpers")
target_compile_features(HoroLuaBehaviorTests PRIVATE cxx_std_20)
target_link_libraries(HoroLuaBehaviorTests PRIVATE HoroEngine::GameplayLua)
add_executable(HoroEditorPlaySessionControllerTests
        unit/editor/EditorPlaySessionControllerTests.cpp
)
target_compile_features(HoroEditorPlaySessionControllerTests PRIVATE cxx_std_20)
target_link_libraries(HoroEditorPlaySessionControllerTests PRIVATE HoroEngine::EditorServices)
add_executable(HoroUpdateExperienceSessionTests
        unit/editor/UpdateExperienceSessionTests.cpp
)
target_compile_features(HoroUpdateExperienceSessionTests PRIVATE cxx_std_20)
target_link_libraries(HoroUpdateExperienceSessionTests PRIVATE HoroEngine::EditorModel)
add_executable(HoroConfiguredEditorUpdateBackendTests
        unit/editor/ConfiguredEditorUpdateBackendTests.cpp
        ${PROJECT_SOURCE_DIR}/apps/HoroEditor/app/ConfiguredEditorUpdateBackend.cpp
)
target_compile_features(HoroConfiguredEditorUpdateBackendTests PRIVATE cxx_std_20)
target_include_directories(HoroConfiguredEditorUpdateBackendTests PRIVATE ${PROJECT_SOURCE_DIR}/apps ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(HoroConfiguredEditorUpdateBackendTests PRIVATE HoroEngine::EditorModel HoroEngine::UpdateDiscovery
        HoroEngine::UpdateDownload HoroEngine::Platform)
horo_register_catch_test(HoroConfiguredEditorUpdateBackendTests LABELS "editor;update;headless")
add_executable(HoroProjectGameplayRegistryTests
        unit/editor/ProjectGameplayRegistryTests.cpp
)
target_include_directories(HoroProjectGameplayRegistryTests PRIVATE "${PROJECT_SOURCE_DIR}/tests/helpers")
target_compile_features(HoroProjectGameplayRegistryTests PRIVATE cxx_std_20)
target_compile_definitions(HoroProjectGameplayRegistryTests PRIVATE
        HORO_TEST_GAME_MODULE_PATH="$<TARGET_FILE:HoroTestGameModule>"
        HORO_TEST_GAME_MODULE_REVISION_PATH="${HORO_TEST_GAME_MODULE_REVISION_FILE}"
)
target_link_libraries(HoroProjectGameplayRegistryTests PRIVATE HoroEngine::EditorServices)
add_dependencies(HoroProjectGameplayRegistryTests HoroTestGameModule)
add_executable(HoroSceneMathTests
        unit/foundation/SceneMathTests.cpp
)
target_compile_features(HoroSceneMathTests PRIVATE cxx_std_20)
target_link_libraries(HoroSceneMathTests PRIVATE HoroEngine::Foundation)
add_executable(HoroFoundationStringTests
        unit/foundation/StringTests.cpp
)
target_compile_features(HoroFoundationStringTests PRIVATE cxx_std_20)
target_link_libraries(HoroFoundationStringTests PRIVATE HoroEngine::Foundation)
add_executable(HoroFoundationSha256Tests
        unit/foundation/Sha256Tests.cpp
)
target_compile_features(HoroFoundationSha256Tests PRIVATE cxx_std_20)
target_link_libraries(HoroFoundationSha256Tests PRIVATE HoroEngine::Foundation)
add_executable(HoroStructuredLogStoreTests
        unit/foundation/StructuredLogStoreTests.cpp
)
target_compile_features(HoroStructuredLogStoreTests PRIVATE cxx_std_20)
target_link_libraries(HoroStructuredLogStoreTests PRIVATE HoroEngine::Foundation Threads::Threads)
add_executable(HoroObservabilityTests
        unit/foundation/ObservabilityTests.cpp
        unit/foundation/TelemetryContractTests.cpp
)
target_compile_features(HoroObservabilityTests PRIVATE cxx_std_20)
target_link_libraries(HoroObservabilityTests PRIVATE HoroEngine::Foundation Threads::Threads miniz)
add_executable(HoroHostObservabilityTests
        unit/application/HostObservabilityTests.cpp
)
target_compile_features(HoroHostObservabilityTests PRIVATE cxx_std_20)
target_link_libraries(HoroHostObservabilityTests PRIVATE HoroEngine::Application Threads::Threads)
if (HORO_ENABLE_OPENTELEMETRY)
    add_executable(HoroOpenTelemetryTests
            unit/foundation/OpenTelemetrySinkTests.cpp
    )
    target_compile_features(HoroOpenTelemetryTests PRIVATE cxx_std_20)
    target_link_libraries(HoroOpenTelemetryTests
            PRIVATE
            HoroEngine::OpenTelemetry
            Threads::Threads
            nlohmann_json::nlohmann_json
    )
endif ()
add_executable(HoroSaveTelemetryDisabledBenchmark benchmarks/SaveTelemetryDisabledBenchmark.cpp)
target_compile_features(HoroSaveTelemetryDisabledBenchmark PRIVATE cxx_std_20)
target_link_libraries(HoroSaveTelemetryDisabledBenchmark PRIVATE HoroEngine::Runtime)
add_test(NAME HoroSaveTelemetryDisabledBenchmark COMMAND HoroSaveTelemetryDisabledBenchmark)
set_tests_properties(HoroSaveTelemetryDisabledBenchmark PROPERTIES LABELS "unit;save;telemetry;benchmark")
add_executable(HoroTelemetryFastPathBenchmark
        benchmarks/TelemetryFastPathBenchmark.cpp
)
target_compile_features(HoroTelemetryFastPathBenchmark PRIVATE cxx_std_20)
target_link_libraries(HoroTelemetryFastPathBenchmark PRIVATE HoroEngine::Foundation Threads::Threads)
add_test(NAME HoroTelemetryFastPathBenchmark COMMAND HoroTelemetryFastPathBenchmark)
set_tests_properties(HoroTelemetryFastPathBenchmark PROPERTIES LABELS "native;benchmark;observability")
add_executable(HoroActivityStoreTests
        unit/foundation/ActivityStoreTests.cpp
)
target_compile_features(HoroActivityStoreTests PRIVATE cxx_std_20)
target_link_libraries(HoroActivityStoreTests PRIVATE HoroEngine::Foundation Threads::Threads)

add_executable(HoroPrimitiveMeshTests
        unit/runtime/scene/PrimitiveMeshTests.cpp
)
find_package(Threads REQUIRED)
target_compile_features(HoroPrimitiveMeshTests PRIVATE cxx_std_20)
target_link_libraries(HoroPrimitiveMeshTests PRIVATE HoroEngine::SceneModel Threads::Threads)
if (HORO_BUILD_EDITOR_GUI)
    add_executable(HoroEditorWelcomeControllerTests
            unit/editor/WelcomeControllerTests.cpp
            unit/editor/WelcomeProjectListTests.cpp
    )

    target_compile_features(HoroEditorWelcomeControllerTests PRIVATE cxx_std_20)
    target_include_directories(HoroEditorWelcomeControllerTests PRIVATE ${PROJECT_SOURCE_DIR}/tests ${PROJECT_SOURCE_DIR}/src)
    target_link_libraries(HoroEditorWelcomeControllerTests
            PRIVATE
            HoroEngine::EditorServices
            HoroEngine::Gui
            HoroThirdParty::ImGui
    )
endif ()
add_executable(HoroEditorModalHostTests
        unit/editor/EditorModalHostTests.cpp
)

target_compile_features(HoroEditorModalHostTests PRIVATE cxx_std_20)
target_link_libraries(HoroEditorModalHostTests
        PRIVATE
        HoroEngine::EditorServices
)
if (HORO_BUILD_EDITOR_GUI)
    add_executable(HoroGuiScreenHostLifecycleTests
            unit/editor/GuiScreenHostLifecycleTests.cpp
    )
    target_compile_features(HoroGuiScreenHostLifecycleTests PRIVATE cxx_std_20)
    target_link_libraries(HoroGuiScreenHostLifecycleTests PRIVATE HoroEngine::Gui HoroEngine::Extensions)
    target_include_directories(HoroGuiScreenHostLifecycleTests PRIVATE ${PROJECT_SOURCE_DIR}/tests/support)

    add_executable(HoroSettingsModalLifecycleTests
            unit/editor/SettingsModalLifecycleTests.cpp
    )
    target_compile_features(HoroSettingsModalLifecycleTests PRIVATE cxx_std_20)
    target_include_directories(HoroSettingsModalLifecycleTests PRIVATE ${PROJECT_SOURCE_DIR}/tests)
    target_link_libraries(HoroSettingsModalLifecycleTests
            PRIVATE
            HoroEngine::Gui
            HoroEngine::EditorServices
    )
    add_executable(HoroEditorPresentationTests
            unit/editor/SettingsModalPresentationTests.cpp
            unit/editor/ProjectCreationViewPresentationTests.cpp
            unit/editor/AssetImportModalPresentationTests.cpp
            unit/editor/AssetImportSourcePreviewTests.cpp
            unit/editor/ScreenViewPresentationTests.cpp
            unit/editor/EditorThemeBehaviorTests.cpp
    )
    target_compile_features(HoroEditorPresentationTests PRIVATE cxx_std_20)
    target_include_directories(HoroEditorPresentationTests PRIVATE
            ${PROJECT_SOURCE_DIR}/src
            ${PROJECT_SOURCE_DIR}/tests
    )
    target_link_libraries(HoroEditorPresentationTests
            PRIVATE
            HoroEngine::Gui
            HoroEngine::EditorServices
            HoroEngine::Extensions
            HoroThirdParty::ImGui
    )
    add_executable(HoroAssetImportModalLifecycleTests
            unit/editor/AssetImportModalLifecycleTests.cpp
    )
    target_compile_features(HoroAssetImportModalLifecycleTests PRIVATE cxx_std_20)
    target_link_libraries(HoroAssetImportModalLifecycleTests
            PRIVATE
            HoroEngine::Gui
            HoroEngine::EditorServices
            HoroEngine::Assets
            HoroEngine::Foundation
    )
endif ()
add_executable(HoroEditorLocalizationServiceTests
        unit/editor/LocalizationServiceTests.cpp
)

target_compile_features(HoroEditorLocalizationServiceTests PRIVATE cxx_std_20)
target_link_libraries(HoroEditorLocalizationServiceTests
        PRIVATE
        HoroEngine::EditorServices
)
add_executable(HoroProjectCreationControllerTests
        unit/editor/ProjectCreationControllerTests.cpp
)

target_compile_features(HoroProjectCreationControllerTests PRIVATE cxx_std_20)
target_link_libraries(HoroProjectCreationControllerTests
        PRIVATE
        HoroEngine::EditorServices
)
add_executable(HoroRendererAvailabilityTests
        unit/editor/RendererAvailabilityTests.cpp
)
target_compile_features(HoroRendererAvailabilityTests PRIVATE cxx_std_20)
target_link_libraries(HoroRendererAvailabilityTests PRIVATE HoroEngine::EditorServices)
add_executable(HoroProjectMetadataTests
        unit/editor/ProjectMetadataTests.cpp
)
target_compile_features(HoroProjectMetadataTests PRIVATE cxx_std_20)
target_link_libraries(HoroProjectMetadataTests PRIVATE HoroEngine::EditorServices)
add_executable(HoroProjectCompatibilityTests
        unit/application/ProjectCompatibilityTests.cpp
        unit/application/ProjectSourceDocumentTests.cpp
)
target_compile_features(HoroProjectCompatibilityTests PRIVATE cxx_std_20)
target_link_libraries(HoroProjectCompatibilityTests PRIVATE HoroEngine::ProjectSettings nlohmann_json::nlohmann_json)
add_executable(HoroReleaseVersionTests
        unit/application/ReleaseVersionTests.cpp
)
target_compile_features(HoroReleaseVersionTests PRIVATE cxx_std_20)
target_link_libraries(HoroReleaseVersionTests PRIVATE HoroEngine::Application)
add_executable(HoroDistributionModelTests
        unit/application/DistributionModelTests.cpp
)
target_compile_features(HoroDistributionModelTests PRIVATE cxx_std_20)
target_link_libraries(HoroDistributionModelTests PRIVATE HoroEngine::Application)
add_executable(HoroReleaseProfileTests
        unit/application/ReleaseProfileTests.cpp
)
target_compile_features(HoroReleaseProfileTests PRIVATE cxx_std_20)
target_link_libraries(HoroReleaseProfileTests PRIVATE HoroEngine::Application nlohmann_json::nlohmann_json)
add_executable(HoroReleasePreflightTests
        unit/application/ReleasePreflightTests.cpp
)
target_compile_features(HoroReleasePreflightTests PRIVATE cxx_std_20)
target_link_libraries(HoroReleasePreflightTests PRIVATE HoroEngine::Application nlohmann_json::nlohmann_json)
add_executable(HoroReleasePipelineExecutorTests
        unit/application/ReleasePipelineExecutorTests.cpp
)
add_executable(HoroReleaseArtifactManifestTests
        unit/application/ReleaseArtifactManifestTests.cpp
)
target_compile_features(HoroReleaseArtifactManifestTests PRIVATE cxx_std_20)
target_link_libraries(HoroReleaseArtifactManifestTests PRIVATE HoroEngine::Application)
horo_register_catch_test(HoroReleaseArtifactManifestTests LABELS "release;manifest;headless")

add_executable(HoroUpdateManifestTests unit/application/UpdateManifestTests.cpp)
target_compile_features(HoroUpdateManifestTests PRIVATE cxx_std_20)
target_link_libraries(HoroUpdateManifestTests PRIVATE HoroEngine::UpdateManifest nlohmann_json::nlohmann_json)
horo_register_catch_test(HoroUpdateManifestTests LABELS "release;update;headless")

add_executable(HoroUpdateDiscoveryTests unit/application/UpdateDiscoveryTests.cpp)
target_compile_features(HoroUpdateDiscoveryTests PRIVATE cxx_std_20)
target_link_libraries(HoroUpdateDiscoveryTests PRIVATE HoroEngine::UpdateDiscovery)
horo_register_catch_test(HoroUpdateDiscoveryTests LABELS "release;update;headless")

add_executable(HoroUpdateTransferTests unit/application/UpdateTransferTests.cpp unit/application/UpdateFileDeltaTests.cpp)
target_compile_features(HoroUpdateTransferTests PRIVATE cxx_std_20)
target_link_libraries(HoroUpdateTransferTests PRIVATE HoroEngine::UpdateTransfer HoroEngine::Platform)
horo_register_catch_test(HoroUpdateTransferTests LABELS "release;update;headless")
add_executable(HoroUpdateDownloadTests unit/application/UpdateDownloadTests.cpp unit/application/UpdateDeltaStagingTests.cpp)
target_compile_features(HoroUpdateDownloadTests PRIVATE cxx_std_20)
target_link_libraries(HoroUpdateDownloadTests PRIVATE HoroEngine::UpdateDownload miniz)
horo_register_catch_test(HoroUpdateDownloadTests LABELS "release;update;headless")

add_executable(HoroUpdateOfflineSourceTests unit/application/UpdateOfflineSourceTests.cpp)
target_compile_features(HoroUpdateOfflineSourceTests PRIVATE cxx_std_20)
target_link_libraries(HoroUpdateOfflineSourceTests PRIVATE HoroEngine::UpdateOfflineSource HoroEngine::UpdateActivation miniz)
horo_register_catch_test(HoroUpdateOfflineSourceTests LABELS "release;update;headless")

add_executable(HoroUpdateActivationTests unit/application/UpdateActivationTests.cpp unit/application/UpdateRetentionTests.cpp
        unit/application/UpdateProductProcessTests.cpp)
target_compile_features(HoroUpdateActivationTests PRIVATE cxx_std_20)
target_link_libraries(HoroUpdateActivationTests PRIVATE HoroEngine::UpdateActivation miniz)
horo_register_catch_test(HoroUpdateActivationTests LABELS "release;update;headless")

add_executable(HoroUpdateTarGzipIndexTests unit/application/UpdateTarGzipIndexTests.cpp)
target_compile_features(HoroUpdateTarGzipIndexTests PRIVATE cxx_std_20)
target_link_libraries(HoroUpdateTarGzipIndexTests PRIVATE HoroEngine::UpdateDownload miniz)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(HoroUpdateTarGzipIndexTests PRIVATE HoroEngine::UpdateActivation)
endif()
horo_register_catch_test(HoroUpdateTarGzipIndexTests LABELS "release;update;headless")

add_executable(HoroUserStateMigrationTests unit/application/UserStateMigrationTests.cpp)
target_compile_features(HoroUserStateMigrationTests PRIVATE cxx_std_20)
target_link_libraries(HoroUserStateMigrationTests PRIVATE HoroEngine::UserStateMigration)
horo_register_catch_test(HoroUserStateMigrationTests LABELS "release;update;headless")

add_executable(HoroEditorUserStateMigrationTests unit/application/EditorUserStateMigrationTests.cpp
        ${PROJECT_SOURCE_DIR}/apps/HoroEditor/app/EditorUserStateMigration.cpp)
target_compile_features(HoroEditorUserStateMigrationTests PRIVATE cxx_std_20)
target_include_directories(HoroEditorUserStateMigrationTests PRIVATE ${PROJECT_SOURCE_DIR}/apps/HoroEditor/app)
target_link_libraries(HoroEditorUserStateMigrationTests PRIVATE HoroEngine::UserStateMigration nlohmann_json::nlohmann_json)
horo_register_catch_test(HoroEditorUserStateMigrationTests LABELS "release;update;headless")

add_executable(HoroUpdateZipPackageProducerTests unit/application/UpdateZipPackageProducerTests.cpp)
target_compile_features(HoroUpdateZipPackageProducerTests PRIVATE cxx_std_20)
target_link_libraries(HoroUpdateZipPackageProducerTests PRIVATE HoroEngine::UpdateDownload miniz)
if(CMAKE_SYSTEM_NAME STREQUAL "Windows" OR CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    target_link_libraries(HoroUpdateZipPackageProducerTests PRIVATE HoroEngine::UpdateActivation)
endif()
horo_register_catch_test(HoroUpdateZipPackageProducerTests LABELS "release;update;package;headless")
add_executable(HoroUpdateHttpResponseTests unit/application/UpdateHttpResponseTests.cpp
        ${PROJECT_SOURCE_DIR}/src/application/release/UpdateHttpResponse.cpp)
target_compile_features(HoroUpdateHttpResponseTests PRIVATE cxx_std_20)
target_include_directories(HoroUpdateHttpResponseTests PRIVATE ${PROJECT_SOURCE_DIR}/src/application/release)
target_link_libraries(HoroUpdateHttpResponseTests PRIVATE HoroEngine::UpdateTransfer)
horo_register_catch_test(HoroUpdateHttpResponseTests LABELS "release;update;headless")
find_program(HORO_TEST_OPENSSL_EXECUTABLE openssl)
if(Python3_Interpreter_FOUND AND HORO_TEST_OPENSSL_EXECUTABLE)
    add_executable(HoroUpdateHttpsClient integration/UpdateHttpsClient.cpp)
    target_compile_features(HoroUpdateHttpsClient PRIVATE cxx_std_20)
    target_link_libraries(HoroUpdateHttpsClient PRIVATE HoroEngine::UpdateDownload)
    add_test(NAME HoroUpdateHttpsIntegration COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/integration/update_https_server.py" "$<TARGET_FILE:HoroUpdateHttpsClient>")
    set_tests_properties(HoroUpdateHttpsIntegration PROPERTIES LABELS "release;update;integration" TIMEOUT 60)
endif()

add_executable(HoroReleaseChecksumsTests
        unit/application/ReleaseChecksumsTests.cpp)
target_compile_features(HoroReleaseChecksumsTests PRIVATE cxx_std_20)
target_link_libraries(HoroReleaseChecksumsTests PRIVATE HoroEngine::Application)
horo_register_catch_test(HoroReleaseChecksumsTests LABELS "release;checksums;headless")

add_executable(HoroReleaseSigningBoundaryTests
        unit/application/ReleaseSigningBoundaryTests.cpp)
target_compile_features(HoroReleaseSigningBoundaryTests PRIVATE cxx_std_20)
target_link_libraries(HoroReleaseSigningBoundaryTests PRIVATE HoroEngine::Application nlohmann_json::nlohmann_json)
horo_register_catch_test(HoroReleaseSigningBoundaryTests LABELS "release;signing;headless")

add_executable(HoroReleaseCandidateVerificationTests
        unit/application/ReleaseCandidateVerificationTests.cpp)
target_compile_features(HoroReleaseCandidateVerificationTests PRIVATE cxx_std_20)
target_link_libraries(HoroReleaseCandidateVerificationTests PRIVATE HoroEngine::Application)
horo_register_catch_test(HoroReleaseCandidateVerificationTests LABELS "release;verification;headless")

add_executable(HoroReleasePublicationTests
        unit/application/ReleasePublicationTests.cpp)
target_compile_features(HoroReleasePublicationTests PRIVATE cxx_std_20)
target_link_libraries(HoroReleasePublicationTests PRIVATE HoroEngine::Application nlohmann_json::nlohmann_json)
horo_register_catch_test(HoroReleasePublicationTests LABELS "release;publication;headless")

add_executable(HoroGitHubReleasePublicationTests
        unit/application/GitHubReleasePublicationTests.cpp)
target_compile_features(HoroGitHubReleasePublicationTests PRIVATE cxx_std_20)
target_link_libraries(HoroGitHubReleasePublicationTests PRIVATE HoroEngine::ReleaseGitHub nlohmann_json::nlohmann_json)
horo_register_catch_test(HoroGitHubReleasePublicationTests LABELS "release;github;headless")

add_executable(HoroGitHubReleaseCliClientTests unit/application/GitHubReleaseCliClientTests.cpp)
target_compile_features(HoroGitHubReleaseCliClientTests PRIVATE cxx_std_20)
target_link_libraries(HoroGitHubReleaseCliClientTests PRIVATE HoroEngine::ReleaseGitHubCli nlohmann_json::nlohmann_json)
horo_register_catch_test(HoroGitHubReleaseCliClientTests LABELS "release;github;headless")

add_executable(HoroReleaseBuildProvenanceTests
        unit/application/ReleaseBuildProvenanceTests.cpp)
target_compile_features(HoroReleaseBuildProvenanceTests PRIVATE cxx_std_20)
target_link_libraries(HoroReleaseBuildProvenanceTests PRIVATE HoroEngine::Application)
horo_register_catch_test(HoroReleaseBuildProvenanceTests LABELS "release;provenance;headless")

add_executable(HoroReleasePackageProducerTests
        unit/application/ReleasePackageProducerTests.cpp)
target_compile_features(HoroReleasePackageProducerTests PRIVATE cxx_std_20)
target_link_libraries(HoroReleasePackageProducerTests PRIVATE HoroEngine::Application)
horo_register_catch_test(HoroReleasePackageProducerTests LABELS "release;package;headless")

add_executable(HoroReleaseCandidatePublisherTests
        unit/application/ReleaseCandidatePublisherTests.cpp)
target_compile_features(HoroReleaseCandidatePublisherTests PRIVATE cxx_std_20)
target_link_libraries(HoroReleaseCandidatePublisherTests PRIVATE HoroEngine::ReleaseProcess nlohmann_json::nlohmann_json)
horo_register_catch_test(HoroReleaseCandidatePublisherTests LABELS "release;staging;headless")
target_compile_features(HoroReleasePipelineExecutorTests PRIVATE cxx_std_20)
target_link_libraries(HoroReleasePipelineExecutorTests PRIVATE HoroEngine::Application nlohmann_json::nlohmann_json)
add_executable(HoroReleaseServiceTests
        unit/application/ReleaseServiceTests.cpp
        unit/application/ReleaseRunHistoryTests.cpp
)
target_compile_features(HoroReleaseServiceTests PRIVATE cxx_std_20)
target_link_libraries(HoroReleaseServiceTests PRIVATE HoroEngine::ReleaseService HoroEngine::Platform nlohmann_json::nlohmann_json)
add_executable(HoroReleaseTargetMatrixTests
        unit/application/ReleaseTargetMatrixTests.cpp
)
target_compile_features(HoroReleaseTargetMatrixTests PRIVATE cxx_std_20)
target_link_libraries(HoroReleaseTargetMatrixTests PRIVATE HoroEngine::Application nlohmann_json::nlohmann_json)
add_executable(HoroReleaseJobTrackerTests
        unit/application/ReleaseJobTrackerTests.cpp
)
target_compile_features(HoroReleaseJobTrackerTests PRIVATE cxx_std_20)
target_link_libraries(HoroReleaseJobTrackerTests PRIVATE HoroEngine::Application)
add_executable(HoroReleaseProcessTests
        unit/application/ReleaseProcessTests.cpp
)
target_compile_features(HoroReleaseProcessTests PRIVATE cxx_std_20)
target_link_libraries(HoroReleaseProcessTests PRIVATE HoroEngine::ReleaseProcess)
add_executable(HoroProjectMigrationTests
        unit/application/ProjectMigrationTests.cpp
        unit/application/ProjectMigrationCatalogTests.cpp
        unit/application/ProjectAuthoringMigrationTests.cpp
)
target_compile_features(HoroProjectMigrationTests PRIVATE cxx_std_20)
target_link_libraries(HoroProjectMigrationTests PRIVATE HoroEngine::ProjectMigrations
        HoroEngine::NetworkApi HoroEngine::NavigationApi nlohmann_json::nlohmann_json)
add_executable(HoroProjectMigrationTransactionTests
        unit/editor/ProjectMigrationTransactionTests.cpp
)
target_compile_features(HoroProjectMigrationTransactionTests PRIVATE cxx_std_20)
target_link_libraries(HoroProjectMigrationTransactionTests PRIVATE
        HoroEngine::EditorServices HoroEngine::ProjectMigrations)
add_executable(HoroProjectOpenServiceTests
        unit/editor/ProjectOpenServiceTests.cpp
)
target_compile_features(HoroProjectOpenServiceTests PRIVATE cxx_std_20)
target_link_libraries(HoroProjectOpenServiceTests PRIVATE
        HoroEngine::EditorServices HoroEngine::ProjectMigrations nlohmann_json::nlohmann_json)

add_executable(HoroProjectIntegrityValidatorServiceTests
        unit/editor/ProjectIntegrityValidatorServiceTests.cpp
)
target_compile_features(HoroProjectIntegrityValidatorServiceTests PRIVATE cxx_std_20)
target_link_libraries(HoroProjectIntegrityValidatorServiceTests PRIVATE
        HoroEngine::EditorServices)
add_executable(HoroProjectMigrationIntegrationTests
        integration/project/ProjectMigrationIntegrationTests.cpp
        helpers/project_migration/ProjectMigrationTestFixture.cpp
        helpers/project_migration/ProjectMigrationTestFixture.h
)
target_compile_features(HoroProjectMigrationIntegrationTests PRIVATE cxx_std_20)
target_include_directories(HoroProjectMigrationIntegrationTests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/helpers/project_migration)
target_link_libraries(HoroProjectMigrationIntegrationTests PRIVATE
        HoroEngine::EditorServices HoroEngine::ProjectMigrations HoroEngine::NavigationApi nlohmann_json::nlohmann_json)
horo_register_catch_test(HoroProjectMigrationIntegrationTests
        LABELS "migration")
add_executable(HoroRecentProjectInspectionServiceTests
        unit/editor/RecentProjectInspectionServiceTests.cpp
)
target_compile_features(HoroRecentProjectInspectionServiceTests PRIVATE cxx_std_20)
target_link_libraries(HoroRecentProjectInspectionServiceTests PRIVATE
        HoroEngine::EditorServices HoroEngine::ProjectMigrations)
add_executable(HoroProjectCreationServiceTests
        unit/editor/ProjectCreationServiceTests.cpp
)

target_compile_features(HoroProjectCreationServiceTests PRIVATE cxx_std_20)
target_link_libraries(HoroProjectCreationServiceTests
        PRIVATE
        HoroEngine::EditorServices
)
add_executable(HoroEditorSettingsStoreTests
        unit/editor/EditorSettingsStoreTests.cpp
)

target_compile_features(HoroEditorSettingsStoreTests PRIVATE cxx_std_20)
target_link_libraries(HoroEditorSettingsStoreTests
        PRIVATE
        HoroEngine::EditorServices
)
add_executable(HoroWorkspaceLayoutTests
        unit/editor/WorkspaceLayoutTests.cpp
)
target_compile_features(HoroWorkspaceLayoutTests PRIVATE cxx_std_20)
target_link_libraries(HoroWorkspaceLayoutTests PRIVATE HoroEngine::EditorServices)
add_executable(HoroWorkspacePanelHostTests
        unit/editor/WorkspacePanelHostTests.cpp
)
target_link_libraries(HoroWorkspacePanelHostTests PRIVATE HoroEngine::EditorServices)
add_executable(HoroEditorSurfaceIdentityTests
        unit/editor/EditorSurfaceIdentityTests.cpp
)
target_compile_features(HoroEditorSurfaceIdentityTests PRIVATE cxx_std_20)
target_link_libraries(HoroEditorSurfaceIdentityTests PRIVATE HoroEngine::EditorServices)
add_executable(HoroSourceFileOpenServiceTests
        unit/editor/SourceFileOpenServiceTests.cpp
)
target_compile_features(HoroSourceFileOpenServiceTests PRIVATE cxx_std_20)
target_link_libraries(HoroSourceFileOpenServiceTests PRIVATE HoroEngine::EditorServices)
add_executable(HoroUiCanvasDocumentTests
        unit/editor/UiCanvasDocumentTests.cpp
)
target_compile_features(HoroUiCanvasDocumentTests PRIVATE cxx_std_20)
target_link_libraries(HoroUiCanvasDocumentTests PRIVATE HoroEngine::EditorServices nlohmann_json::nlohmann_json)
add_executable(HoroWorkspaceLayoutPersistenceTests
        unit/editor/WorkspaceLayoutPersistenceTests.cpp
        support/AllocationProbe.cpp
)
target_link_libraries(HoroWorkspaceLayoutPersistenceTests PRIVATE HoroEngine::EditorServices)
add_executable(HoroActivityBarLayoutTests
        unit/editor/ActivityBarLayoutTests.cpp
)
target_compile_features(HoroActivityBarLayoutTests PRIVATE cxx_std_20)
target_link_libraries(HoroActivityBarLayoutTests PRIVATE HoroEngine::EditorServices)
add_executable(HoroWorkspaceSplitterInteractionTests
        unit/editor/WorkspaceSplitterInteractionTests.cpp
)
target_compile_features(HoroWorkspaceSplitterInteractionTests PRIVATE cxx_std_20)
target_include_directories(HoroWorkspaceSplitterInteractionTests PRIVATE
        ${PROJECT_SOURCE_DIR}/src/editor/screens/workspace
)
target_link_libraries(HoroWorkspaceSplitterInteractionTests PRIVATE HoroEngine::EditorServices)
if (HORO_BUILD_EDITOR_GUI)
    add_executable(HoroEditorWorkspaceControllerTests
            unit/editor/EditorWorkspaceControllerTests.cpp
            unit/editor/EditorWorkspaceControllerPolicyTests.cpp
            unit/editor/EditorWorkspaceControllerPolicyTestSupport.h
            unit/editor/EditorWorkspaceControllerTransferPolicyTests.cpp
            unit/editor/EditorWorkspaceNativeGameplayReloadTests.cpp
    )

    target_include_directories(HoroEditorWorkspaceControllerTests PRIVATE "${PROJECT_SOURCE_DIR}/tests/helpers")
    target_compile_features(HoroEditorWorkspaceControllerTests PRIVATE cxx_std_20)
    target_compile_definitions(HoroEditorWorkspaceControllerTests PRIVATE
            HORO_TEST_GAME_MODULE_PATH="$<TARGET_FILE:HoroTestGameModule>"
            HORO_TEST_GAME_MODULE_REVISION_PATH="${HORO_TEST_GAME_MODULE_REVISION_FILE}"
    )
    target_link_libraries(HoroEditorWorkspaceControllerTests PRIVATE HoroEngine::Gui nlohmann_json::nlohmann_json)
    add_dependencies(HoroEditorWorkspaceControllerTests HoroTestGameModule)
    add_executable(HoroCameraCutEditorIntegrationTests unit/runtime/cinematic/CameraCutEditorIntegrationTests.cpp)
    target_compile_features(HoroCameraCutEditorIntegrationTests PRIVATE cxx_std_20)
    target_include_directories(HoroCameraCutEditorIntegrationTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(HoroCameraCutEditorIntegrationTests PRIVATE HoroEngine::Gui)
    horo_register_catch_test(HoroCameraCutEditorIntegrationTests LABELS "unit;cinematic;camera;editor;headless")
endif ()

add_executable(HoroGameplayBehaviorFilenameModalStateTests
        unit/editor/GameplayBehaviorFilenameModalStateTests.cpp
)
target_compile_features(HoroGameplayBehaviorFilenameModalStateTests PRIVATE cxx_std_20)
target_include_directories(HoroGameplayBehaviorFilenameModalStateTests PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(HoroGameplayBehaviorFilenameModalStateTests PRIVATE HoroEngine::EditorServices)

add_executable(HoroBuildReleaseModalSessionTests
        unit/editor/BuildReleaseModalSessionTests.cpp
)
target_compile_features(HoroBuildReleaseModalSessionTests PRIVATE cxx_std_20)
target_include_directories(HoroBuildReleaseModalSessionTests PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(HoroBuildReleaseModalSessionTests PRIVATE HoroEngine::EditorServices)

add_executable(HoroNotificationServiceTests
        unit/editor/NotificationServiceTests.cpp
)
target_compile_features(HoroNotificationServiceTests PRIVATE cxx_std_20)
target_link_libraries(HoroNotificationServiceTests PRIVATE HoroEngine::EditorServices)

add_executable(HoroEditorMenuModelTests
        unit/editor/EditorMenuModelTests.cpp
)

target_compile_features(HoroEditorMenuModelTests PRIVATE cxx_std_20)
target_include_directories(HoroEditorMenuModelTests PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(HoroEditorMenuModelTests PRIVATE HoroEngine::EditorServices)
add_executable(HoroEditorStatusBarModelTests
        unit/editor/EditorStatusBarModelTests.cpp
)

target_compile_features(HoroEditorStatusBarModelTests PRIVATE cxx_std_20)
target_link_libraries(HoroEditorStatusBarModelTests PRIVATE HoroEngine::EditorServices)
add_executable(HoroHierarchyModelTests
        unit/editor/HierarchyModelTests.cpp
)

target_compile_features(HoroHierarchyModelTests PRIVATE cxx_std_20)
target_link_libraries(HoroHierarchyModelTests PRIVATE HoroEngine::EditorServices)
add_executable(HoroEditorSelectionModelTests
        unit/editor/EditorSelectionModelTests.cpp
)

target_compile_features(HoroEditorSelectionModelTests PRIVATE cxx_std_20)
target_link_libraries(HoroEditorSelectionModelTests PRIVATE HoroEngine::EditorServices)
add_executable(HoroEditorViewportModelTests
        unit/editor/EditorViewportModelTests.cpp
)

target_compile_features(HoroEditorViewportModelTests PRIVATE cxx_std_20)
target_link_libraries(HoroEditorViewportModelTests PRIVATE HoroEngine::EditorServices)
if (HORO_BUILD_EDITOR_GUI)
    add_executable(HoroHierarchyPanelRenderTests
            unit/editor/HierarchyPanelRenderTests.cpp
    )

    target_compile_features(HoroHierarchyPanelRenderTests PRIVATE cxx_std_20)
    target_include_directories(HoroHierarchyPanelRenderTests PRIVATE
            ${PROJECT_SOURCE_DIR}/src/editor/screens/workspace
    )
    target_link_libraries(HoroHierarchyPanelRenderTests
            PRIVATE
            HoroEngine::Gui
            HoroThirdParty::ImGui
    )
    add_executable(HoroHierarchyEditSessionTests
            unit/editor/HierarchyEditSessionTests.cpp
    )

    target_compile_features(HoroHierarchyEditSessionTests PRIVATE cxx_std_20)
    target_link_libraries(HoroHierarchyEditSessionTests
            PRIVATE
            HoroEngine::Gui
    )
    add_executable(HoroInspectorPanelRenderTests
            unit/editor/InspectorPanelRenderTests.cpp
    )

    target_compile_features(HoroInspectorPanelRenderTests PRIVATE cxx_std_20)
    target_include_directories(HoroInspectorPanelRenderTests PRIVATE
            ${PROJECT_SOURCE_DIR}/src/editor/screens/workspace
    )
    target_link_libraries(HoroInspectorPanelRenderTests
            PRIVATE
            HoroEngine::Gui
            HoroThirdParty::ImGui
    )
    add_executable(HoroInspectorEditSessionTests
            unit/editor/InspectorEditSessionTests.cpp
    )

    target_compile_features(HoroInspectorEditSessionTests PRIVATE cxx_std_20)
    target_include_directories(HoroInspectorEditSessionTests PRIVATE
            ${PROJECT_SOURCE_DIR}/src/editor/screens/workspace
    )
    target_link_libraries(HoroInspectorEditSessionTests
            PRIVATE
            HoroEngine::Gui
    )
    add_executable(HoroGlobalDockPanelRenderTests
            unit/editor/GlobalDockBuildActiveRenderTests.cpp
            unit/editor/GlobalDockBuildOutputPaneTests.cpp
            unit/editor/GlobalDockBuildOutputToolbarRenderTests.cpp
            unit/editor/GlobalDockPanelRenderTests.cpp
    )

    target_compile_features(HoroGlobalDockPanelRenderTests PRIVATE cxx_std_20)
    target_include_directories(HoroGlobalDockPanelRenderTests PRIVATE
            ${PROJECT_SOURCE_DIR}/src/editor/screens/workspace
    )
    target_link_libraries(HoroGlobalDockPanelRenderTests
            PRIVATE
            HoroEngine::Gui
            HoroThirdParty::ImGui
    )
    add_executable(HoroAssetBrowserInteractionSessionTests
            unit/editor/AssetBrowserInteractionSessionTests.cpp
    )

    target_compile_features(
            HoroAssetBrowserInteractionSessionTests PRIVATE cxx_std_20
    )
    target_link_libraries(HoroAssetBrowserInteractionSessionTests
            PRIVATE
            HoroEngine::Gui
    )
    add_executable(HoroExtensionActivityViewTests unit/editor/ExtensionActivityViewTests.cpp support/AllocationProbe.cpp)
    target_sources(HoroGuiPublicHeaderConsumer PRIVATE support/GuiScreenHostPublicContract.cpp)
    target_compile_features(HoroExtensionActivityViewTests PRIVATE cxx_std_20)
    target_include_directories(HoroExtensionActivityViewTests PRIVATE ${PROJECT_SOURCE_DIR}/tests/support)
    target_link_libraries(HoroExtensionActivityViewTests PRIVATE HoroEngine::Gui HoroEngine::Extensions HoroThirdParty::ImGui)
    add_executable(HoroEditorUiComponentsRenderTests
            unit/editor/EditorUiComponentsRenderTests.cpp
            unit/editor/ActivityButtonIdentityTests.cpp
    )

    target_compile_features(HoroEditorUiComponentsRenderTests PRIVATE cxx_std_20)
    target_link_libraries(HoroEditorUiComponentsRenderTests
            PRIVATE
            HoroEngine::Gui
            HoroThirdParty::ImGui
    )
    add_executable(HoroViewportPanelRenderTests
            unit/editor/ViewportPanelRenderTests.cpp
    )

    target_compile_features(HoroViewportPanelRenderTests PRIVATE cxx_std_20)
    target_include_directories(HoroViewportPanelRenderTests PRIVATE
            ${PROJECT_SOURCE_DIR}/src/editor/screens/workspace
    )
    target_link_libraries(HoroViewportPanelRenderTests
            PRIVATE
            HoroEngine::Gui
            HoroThirdParty::ImGui
    )
    add_executable(HoroTransformGizmoMathTests
            unit/editor/TransformGizmoMathTests.cpp
            unit/editor/TransformGizmoGeometryTests.cpp
    )
    target_compile_features(HoroTransformGizmoMathTests PRIVATE cxx_std_20)
    target_link_libraries(HoroTransformGizmoMathTests
            PRIVATE
            HoroEngine::Gui
    )
    horo_register_catch_test(HoroTransformGizmoMathTests
            LABELS "unit;editor;viewport;gizmo"
    )
endif ()

add_executable(HoroDataBusTests
        unit/foundation/DataBusTests.cpp
)

target_compile_features(HoroDataBusTests PRIVATE cxx_std_20)
target_link_libraries(HoroDataBusTests
        PRIVATE
        HoroEngine::EditorServices
)
add_executable(HoroFoundationResultTests
        unit/foundation/ResultTests.cpp
)

target_compile_features(HoroFoundationResultTests PRIVATE cxx_std_20)
target_link_libraries(HoroFoundationResultTests PRIVATE HoroEngine::Foundation)
target_include_directories(HoroFoundationResultTests PRIVATE "${PROJECT_SOURCE_DIR}/src/foundation/assertions")
add_executable(HoroHostErrorTranslationTests unit/hosts/ErrorTranslationTests.cpp)
target_link_libraries(HoroHostErrorTranslationTests PRIVATE HoroEngine::HostErrors nlohmann_json::nlohmann_json)
horo_register_catch_test(HoroHostErrorTranslationTests LABELS "unit;hosts;headless")
add_executable(HoroHostErrorFixture unit/hosts/HostErrorFixture.cpp)
target_link_libraries(HoroHostErrorFixture PRIVATE HoroEngine::HostErrors nlohmann_json::nlohmann_json)
add_test(NAME HoroHostErrorPythonParity
    COMMAND "${CMAKE_COMMAND}"
            "-DHOST_ERROR_FIXTURE=$<TARGET_FILE:HoroHostErrorFixture>"
            "-DHOST_ERROR_PYTHON=${Python3_EXECUTABLE}"
            "-DHOST_ERROR_TEST=${PROJECT_SOURCE_DIR}/tests/python/host_error_parity.py"
            -P "${PROJECT_SOURCE_DIR}/tests/cmake/RunHostErrorPythonParity.cmake")
set_tests_properties(HoroHostErrorPythonParity PROPERTIES LABELS "integration;hosts;python;headless" TIMEOUT 45
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1;PYTHONUTF8=1")

add_executable(HoroCliCommandRegistryTests
        unit/cli/CliCommandRegistryTests.cpp
        unit/cli/CliDispatcherTests.cpp
        unit/cli/CliProgressLifecycleTests.cpp
        unit/cli/CliOptionParserTests.cpp
        unit/cli/CliOutputPresenterTests.cpp
)
target_compile_features(HoroCliCommandRegistryTests PRIVATE cxx_std_20)
target_link_libraries(HoroCliCommandRegistryTests PRIVATE HoroEngine::CliHost HoroEngine::Platform nlohmann_json::nlohmann_json)
horo_register_catch_test(HoroCliCommandRegistryTests LABELS "unit;cli;headless")

add_executable(HoroMcpSessionTests
        unit/mcp/McpSessionTests.cpp
        unit/mcp/McpLocalTransportTests.cpp
        support/AllocationProbe.cpp
)
target_compile_features(HoroMcpSessionTests PRIVATE cxx_std_20)
target_link_libraries(HoroMcpSessionTests PRIVATE HoroEngine::McpSession Threads::Threads)
horo_register_catch_test(HoroMcpSessionTests LABELS "unit;mcp;headless")

add_executable(HoroMcpRegistryTests unit/mcp/McpToolRegistryTests.cpp)
target_compile_features(HoroMcpRegistryTests PRIVATE cxx_std_20)
target_link_libraries(HoroMcpRegistryTests PRIVATE HoroEngine::McpRegistry Threads::Threads)
horo_register_catch_test(HoroMcpRegistryTests LABELS "unit;mcp;headless")

add_executable(HoroMcpControllerTests
    unit/mcp/McpControllerTests.cpp
    unit/mcp/McpAuthorizationTests.cpp
)
target_compile_features(HoroMcpControllerTests PRIVATE cxx_std_20)
target_link_libraries(HoroMcpControllerTests PRIVATE HoroEngine::McpController Threads::Threads)
horo_register_catch_test(HoroMcpControllerTests LABELS "unit;mcp;headless")

add_executable(HoroFoundationErrorCodeRegistryTests
        unit/foundation/ErrorCodeRegistryTests.cpp
)

target_compile_features(HoroFoundationErrorCodeRegistryTests PRIVATE cxx_std_20)
target_link_libraries(HoroFoundationErrorCodeRegistryTests PRIVATE HoroEngine::Foundation)
add_executable(HoroFoundationModuleDescriptorTests
        unit/foundation/ModuleDescriptorTests.cpp
        unit/foundation/ModuleDescriptorGraphTests.cpp
)

target_compile_features(HoroFoundationModuleDescriptorTests PRIVATE cxx_std_20)
target_link_libraries(HoroFoundationModuleDescriptorTests PRIVATE HoroEngine::Foundation)

add_executable(HoroFoundationModuleHostCompositionTests
        unit/foundation/ModuleHostCompositionTests.cpp
        unit/foundation/ModuleHostLifecycleTests.cpp
)

target_compile_features(HoroFoundationModuleHostCompositionTests PRIVATE cxx_std_20)
target_link_libraries(HoroFoundationModuleHostCompositionTests PRIVATE HoroEngine::Foundation)
add_executable(HoroHostModuleCompositionTests
        unit/application/HostModuleCompositionTests.cpp
)
target_compile_features(HoroHostModuleCompositionTests PRIVATE cxx_std_20)
target_link_libraries(HoroHostModuleCompositionTests PRIVATE HoroHostModuleComposition)
add_executable(HoroFoundationPathsTimeHandlesTests
        unit/foundation/PathsTimeHandlesTests.cpp
)

target_compile_features(HoroFoundationPathsTimeHandlesTests PRIVATE cxx_std_20)
target_link_libraries(HoroFoundationPathsTimeHandlesTests PRIVATE HoroEngine::Foundation)
add_executable(HoroFoundationCancellationProgressTests
        unit/foundation/CancellationProgressTests.cpp
)

target_compile_features(HoroFoundationCancellationProgressTests PRIVATE cxx_std_20)
target_link_libraries(HoroFoundationCancellationProgressTests PRIVATE HoroEngine::Foundation)
add_executable(HoroFoundationConfigurationTests
        unit/foundation/ConfigurationTests.cpp
)

target_compile_features(HoroFoundationConfigurationTests PRIVATE cxx_std_20)
target_link_libraries(HoroFoundationConfigurationTests PRIVATE HoroEngine::Foundation)
add_executable(HoroFoundationDiagnosticsTests
        unit/foundation/DiagnosticsTests.cpp
)

target_compile_features(HoroFoundationDiagnosticsTests PRIVATE cxx_std_20)
target_link_libraries(HoroFoundationDiagnosticsTests PRIVATE HoroEngine::Foundation)
add_executable(HoroFoundationValidationResultTests
        unit/foundation/ValidationResultTests.cpp
)

target_compile_features(HoroFoundationValidationResultTests PRIVATE cxx_std_20)
target_link_libraries(HoroFoundationValidationResultTests PRIVATE HoroEngine::Foundation)
add_executable(HoroFoundationJobSystemTests
        unit/foundation/JobSystemTests.cpp
        unit/foundation/JobAdmissionTests.cpp
        unit/foundation/JobAdmissionTelemetryTests.cpp
        support/AllocationProbe.cpp
)

target_compile_features(HoroFoundationJobSystemTests PRIVATE cxx_std_20)
target_link_libraries(HoroFoundationJobSystemTests PRIVATE HoroEngine::Foundation)
# Explicit diagnostic target: run the same admission regressions through the joining fallback on a jthread-capable host.
# Excluded from ALL and CTest discovery so ordinary builds do not silently add another test/build stream.
add_executable(HoroJobAdmissionThreadFallbackTests EXCLUDE_FROM_ALL
        unit/foundation/JobAdmissionTests.cpp
        unit/foundation/JobAdmissionTelemetryTests.cpp
        support/AllocationProbe.cpp)
target_compile_features(HoroJobAdmissionThreadFallbackTests PRIVATE cxx_std_20)
target_compile_definitions(HoroJobAdmissionThreadFallbackTests PRIVATE HORO_JOB_TEST_FORCE_THREAD_FALLBACK=1)
target_link_libraries(HoroJobAdmissionThreadFallbackTests PRIVATE HoroEngine::Foundation Catch2::Catch2WithMain)
add_executable(HoroJobAdmissionPublicHeaderConsumer support/JobAdmissionPublicHeaderConsumer.cpp)
target_compile_features(HoroJobAdmissionPublicHeaderConsumer PRIVATE cxx_std_20)
target_link_libraries(HoroJobAdmissionPublicHeaderConsumer PRIVATE HoroEngine::Foundation)
add_test(NAME HoroJobAdmissionPublicHeaderConsumer COMMAND HoroJobAdmissionPublicHeaderConsumer)
set_tests_properties(HoroJobAdmissionPublicHeaderConsumer PROPERTIES LABELS "unit;foundation;jobs;headless;public_headers")
add_executable(HoroPlatformTests
        unit/foundation/PlatformTests.cpp
        unit/foundation/PlatformPublicationLockTests.cpp
)

add_executable(HoroProcessTestChild helpers/platform/ProcessTreeChild.cpp)
target_compile_features(HoroProcessTestChild PRIVATE cxx_std_20)
target_link_libraries(HoroProcessTestChild PRIVATE HoroEngine::Platform)
add_dependencies(HoroPlatformTests HoroProcessTestChild)
target_compile_definitions(HoroPlatformTests PRIVATE HORO_PROCESS_TEST_CHILD="$<TARGET_FILE:HoroProcessTestChild>")

target_compile_features(HoroPlatformTests PRIVATE cxx_std_20)
target_link_libraries(HoroPlatformTests PRIVATE HoroEngine::Platform)
add_executable(HoroPlatformRequestTests
        support/AllocationProbe.cpp
        unit/platform_services/AchievementDefinitionRegistryTests.cpp
        unit/platform_services/PlatformCloudObjectsTests.cpp
        unit/platform_services/PlatformAchievementCoordinatorTests.cpp
        unit/platform_services/PlatformDefinitionRegistriesTests.cpp
        unit/platform_services/PlatformPresenceCoordinatorTests.cpp
        unit/platform_services/PlatformOfflineQueueStorageTests.cpp
        unit/platform_services/PlatformProgressionIdempotencyTests.cpp
        unit/platform_services/PlatformOfflineQueueTests.cpp
        unit/platform_services/PlatformOfflineQueueLifecycleTests.cpp
        unit/platform_services/PlatformProjectConfigurationTests.cpp
        unit/platform_services/PlatformProviderManifestCookTests.cpp
        unit/platform_services/PlatformRequestTests.cpp
        unit/platform_services/PlatformCompletionQueueTests.cpp
        unit/platform_services/PlatformServiceErrorsTests.cpp
        unit/platform_services/PlatformServicesBackendTests.cpp
        unit/platform_services/PlatformServicesFrontendTests.cpp
        unit/platform_services/PlatformSessionObserverTests.cpp
        unit/platform_services/PlatformServicesMetricsTests.cpp
        unit/platform_services/PlatformStatCacheCoordinatorTests.cpp
        unit/platform_services/PlatformStableIdRegistryTests.cpp
        unit/platform_services/PlatformUserSessionTests.cpp
)
target_compile_features(HoroPlatformRequestTests PRIVATE cxx_std_20)
target_include_directories(HoroPlatformRequestTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/support)
target_link_libraries(HoroPlatformRequestTests PRIVATE HoroEngine::PlatformServices Threads::Threads)
add_executable(HoroPlatformServicesCompositionTests unit/platform_services/PlatformServicesCompositionTests.cpp)
target_compile_features(HoroPlatformServicesCompositionTests PRIVATE cxx_std_20)
target_link_libraries(HoroPlatformServicesCompositionTests PRIVATE HoroEngine::PlatformServices Catch2::Catch2WithMain Threads::Threads)
add_test(NAME HoroPlatformServicesCompositionTests COMMAND HoroPlatformServicesCompositionTests)
set_tests_properties(HoroPlatformServicesCompositionTests PROPERTIES LABELS "unit;platform-services;headless")
add_executable(HoroGameplayBuildServiceTests
        unit/application/CompilerDiagnosticParserTests.cpp
        unit/application/GameplayBuildServiceTests.cpp
)
target_compile_features(HoroGameplayBuildServiceTests PRIVATE cxx_std_20)
target_compile_definitions(HoroGameplayBuildServiceTests PRIVATE
        HORO_GAMEPLAY_SDK_PACKAGE_DIR="${HORO_GAMEPLAY_SDK_PACKAGE_DIR}"
        HORO_GAMEPLAY_CXX_COMPILER="${CMAKE_CXX_COMPILER}"
        HORO_TEST_CMAKE_GENERATOR="${CMAKE_GENERATOR}"
)
target_link_libraries(HoroGameplayBuildServiceTests
        PRIVATE
        HoroEngine::GameplayBuild
        HoroEngine::Platform
)
add_executable(HoroEditorViewportSceneTests
        unit/editor/EditorViewportSceneTests.cpp
)

target_compile_features(HoroEditorViewportSceneTests PRIVATE cxx_std_20)
target_link_libraries(HoroEditorViewportSceneTests PRIVATE HoroEngine::EditorViewportScene)
add_executable(HoroSceneDocumentTests
        unit/editor/SceneDocumentTests.cpp
        unit/editor/SceneDocumentDuplicateIdentityTests.cpp
        unit/editor/SceneDocumentEditingTests.cpp
        unit/editor/SceneDocumentExtractionTests.cpp
        unit/editor/SceneDocumentPrefabTests.cpp
)
target_compile_features(HoroSceneDocumentTests PRIVATE cxx_std_20)
target_link_libraries(HoroSceneDocumentTests PRIVATE HoroEngine::EditorRenderExtraction)
add_executable(HoroSceneDocumentPersistenceTests
        unit/editor/SceneDocumentPersistenceTests.cpp
        unit/editor/SceneDocumentAudioPersistenceTests.cpp
        unit/editor/SceneDocumentTriggerVolumePersistenceTests.cpp
        unit/editor/SceneDocumentGameplayPersistenceTests.cpp
        unit/editor/SceneDocumentComparisonPersistenceTests.cpp
        unit/editor/SceneDocumentFileWatchPersistenceTests.cpp
        unit/editor/SceneDocumentPersistenceTestSupport.cpp
)
target_compile_features(HoroSceneDocumentPersistenceTests PRIVATE cxx_std_20)
target_link_libraries(HoroSceneDocumentPersistenceTests PRIVATE HoroEngine::EditorServices)
add_executable(HoroSceneSourceTests unit/scene/SceneSourceTests.cpp)
target_compile_features(HoroSceneSourceTests PRIVATE cxx_std_20)
target_link_libraries(HoroSceneSourceTests PRIVATE HoroEngine::SceneSource Catch2::Catch2WithMain)
catch_discover_tests(HoroSceneSourceTests TEST_PREFIX "HoroSceneSourceTests::"
        PROPERTIES LABELS "unit;scene;serialization;headless")
add_executable(HoroEditorViewportPickingTests
        unit/editor/EditorViewportPickingTests.cpp
)
target_compile_features(HoroEditorViewportPickingTests PRIVATE cxx_std_20)
target_link_libraries(HoroEditorViewportPickingTests PRIVATE HoroEngine::EditorRenderExtraction)
add_executable(HoroAssetSceneDropTests
        unit/editor/AssetSceneDropTests.cpp
)
target_compile_features(HoroAssetSceneDropTests PRIVATE cxx_std_20)
target_link_libraries(HoroAssetSceneDropTests PRIVATE HoroEngine::EditorRenderExtraction)
add_executable(HoroEditorViewportResourceTests
        unit/editor/EditorViewportResourcesTests.cpp
)
target_link_libraries(HoroEditorViewportResourceTests
        PRIVATE
        HoroEngine::EditorViewportResources
        HoroEngine::RenderNull
)

# White-box editor tests opt into implementation headers explicitly. Keep this
# non-installed test interface separate from every production usage requirement;
# legacy editor/... include spellings require the source root until those tests
# migrate to narrower per-subsystem spellings.
add_library(HoroEditorTestInternals INTERFACE)
target_include_directories(HoroEditorTestInternals INTERFACE ${PROJECT_SOURCE_DIR}/src)
set(HORO_EDITOR_WHITE_BOX_TEST_TARGETS
        HoroCameraCutEditorIntegrationTests
        HoroEditorPlaySessionControllerTests
        HoroUpdateExperienceSessionTests
        HoroProjectGameplayRegistryTests
        HoroGuiScreenHostLifecycleTests
        HoroExtensionActivityViewTests
        HoroProjectCreationControllerTests
        HoroRendererAvailabilityTests
        HoroProjectMetadataTests
        HoroProjectOpenServiceTests
        HoroProjectMigrationIntegrationTests
        HoroEditorWorkspaceControllerTests
        HoroGameplayBehaviorFilenameModalStateTests
        HoroBuildReleaseModalSessionTests
        HoroEditorSelectionModelTests
        HoroEditorViewportModelTests
        HoroHierarchyPanelRenderTests
        HoroHierarchyEditSessionTests
        HoroInspectorPanelRenderTests
        HoroInspectorEditSessionTests
        HoroGlobalDockPanelRenderTests
        HoroAssetBrowserInteractionSessionTests
        HoroViewportPanelRenderTests
        HoroTransformGizmoMathTests
        HoroEditorViewportSceneTests
        HoroSceneDocumentTests
        HoroSceneDocumentPersistenceTests
        HoroEditorViewportPickingTests
        HoroAssetSceneDropTests
        HoroEditorViewportResourceTests
)
foreach (target IN LISTS HORO_EDITOR_WHITE_BOX_TEST_TARGETS)
    if (TARGET ${target})
        target_link_libraries(${target} PRIVATE HoroEditorTestInternals)
    endif ()
endforeach ()

add_executable(HoroRenderBackendRegistryTests
        unit/runtime/renderer/RenderBackendRegistryTests.cpp
        unit/runtime/renderer/RenderCapabilitiesTests.cpp
)

target_compile_features(HoroRenderBackendRegistryTests PRIVATE cxx_std_20)
target_include_directories(HoroRenderBackendRegistryTests PRIVATE ${PROJECT_SOURCE_DIR}/tests/support)
target_link_libraries(HoroRenderBackendRegistryTests
        PRIVATE
        HoroEngine::RenderNull
)
add_executable(HoroRenderGraphTests
        unit/runtime/renderer/RenderGraphExecutionTests.cpp
        unit/runtime/renderer/RenderGraphTests.cpp
        unit/runtime/renderer/RenderGraphValidationTests.cpp
        unit/runtime/renderer/RenderGraphSynchronizationTests.cpp
)
target_compile_features(HoroRenderGraphTests PRIVATE cxx_std_20)
target_link_libraries(HoroRenderGraphTests PRIVATE HoroEngine::RenderApi)
add_executable(HoroRenderGraphLifetimeTests
        unit/runtime/renderer/RenderGraphLifetimeTests.cpp
)
target_compile_features(HoroRenderGraphLifetimeTests PRIVATE cxx_std_20)
target_link_libraries(HoroRenderGraphLifetimeTests PRIVATE HoroEngine::RenderApi)
add_executable(HoroTemporalHistoryTests
        unit/runtime/renderer/TemporalHistoryTests.cpp
)
target_compile_features(HoroTemporalHistoryTests PRIVATE cxx_std_20)
target_link_libraries(HoroTemporalHistoryTests PRIVATE HoroEngine::RenderApi)
add_executable(HoroMotionHistoryTests
        unit/runtime/renderer/MotionHistoryTests.cpp
)
target_compile_features(HoroMotionHistoryTests PRIVATE cxx_std_20)
target_include_directories(HoroMotionHistoryTests PRIVATE ${PROJECT_SOURCE_DIR}/tests)
target_link_libraries(HoroMotionHistoryTests PRIVATE HoroEngine::RenderApi)
add_executable(HoroRenderSubmissionTests
        unit/runtime/renderer/RenderSubmissionTests.cpp
)
target_compile_features(HoroRenderSubmissionTests PRIVATE cxx_std_20)
target_link_libraries(HoroRenderSubmissionTests PRIVATE HoroEngine::RenderApi)
add_executable(HoroRenderAdapterTests
        unit/runtime/renderer/RenderAdapterTests.cpp
)
target_compile_features(HoroRenderAdapterTests PRIVATE cxx_std_20)
target_link_libraries(HoroRenderAdapterTests PRIVATE HoroEngine::RenderApi)
add_executable(HoroRenderDisplayTests
        unit/runtime/renderer/RenderDisplayTests.cpp
)
target_compile_features(HoroRenderDisplayTests PRIVATE cxx_std_20)
target_link_libraries(HoroRenderDisplayTests PRIVATE HoroEngine::RenderApi)
add_executable(HoroPresentModeTests
        unit/runtime/renderer/PresentModeTests.cpp
)
target_compile_features(HoroPresentModeTests PRIVATE cxx_std_20)
target_link_libraries(HoroPresentModeTests PRIVATE HoroEngine::RenderApi)
add_executable(HoroRenderSurfaceLifecycleTests
        unit/runtime/renderer/RenderSurfaceLifecycleTests.cpp
)
target_compile_features(HoroRenderSurfaceLifecycleTests PRIVATE cxx_std_20)
target_link_libraries(HoroRenderSurfaceLifecycleTests PRIVATE HoroEngine::RenderApi)
add_executable(HoroRenderResourceDescriptorTests
        unit/runtime/renderer/RenderResourceDescriptorTests.cpp
)
target_compile_features(HoroRenderResourceDescriptorTests PRIVATE cxx_std_20)
target_link_libraries(HoroRenderResourceDescriptorTests PRIVATE HoroEngine::RenderApi)
add_executable(HoroShaderManifestTests
        unit/runtime/renderer/ShaderManifestTests.cpp
)
target_compile_features(HoroShaderManifestTests PRIVATE cxx_std_20)
target_include_directories(HoroShaderManifestTests PRIVATE ${PROJECT_SOURCE_DIR}/tests)
target_link_libraries(HoroShaderManifestTests PRIVATE HoroEngine::RenderApi)
add_executable(HoroShaderPermutationTests
        unit/runtime/renderer/ShaderPermutationTests.cpp
)
target_compile_features(HoroShaderPermutationTests PRIVATE cxx_std_20)
target_include_directories(HoroShaderPermutationTests PRIVATE ${PROJECT_SOURCE_DIR}/tests)
target_link_libraries(HoroShaderPermutationTests PRIVATE HoroEngine::RenderApi)
add_executable(HoroPipelineCacheTests
        unit/runtime/renderer/PipelineCacheTests.cpp
)
target_compile_features(HoroPipelineCacheTests PRIVATE cxx_std_20)
target_include_directories(HoroPipelineCacheTests PRIVATE ${PROJECT_SOURCE_DIR}/tests)
target_link_libraries(HoroPipelineCacheTests PRIVATE HoroEngine::RenderApi)
add_executable(HoroShaderReflectionTests
        unit/runtime/renderer/ShaderReflectionTests.cpp
)
target_compile_features(HoroShaderReflectionTests PRIVATE cxx_std_20)
target_include_directories(HoroShaderReflectionTests PRIVATE ${PROJECT_SOURCE_DIR}/tests)
target_link_libraries(HoroShaderReflectionTests PRIVATE HoroEngine::RenderApi)
add_executable(HoroStandardPbrMaterialTests
        unit/runtime/renderer/StandardPbrMaterialTests.cpp
)
target_compile_features(HoroStandardPbrMaterialTests PRIVATE cxx_std_20)
target_include_directories(HoroStandardPbrMaterialTests PRIVATE ${PROJECT_SOURCE_DIR}/tests)
target_link_libraries(HoroStandardPbrMaterialTests PRIVATE HoroEngine::RenderApi)
add_executable(HoroStandardPbrPassPlanTests
        unit/runtime/renderer/StandardPbrPassPlanTests.cpp
)
target_compile_features(HoroStandardPbrPassPlanTests PRIVATE cxx_std_20)
target_include_directories(HoroStandardPbrPassPlanTests PRIVATE ${PROJECT_SOURCE_DIR}/tests)
target_link_libraries(HoroStandardPbrPassPlanTests PRIVATE HoroEngine::RenderApi)
add_executable(HoroShaderCompilerPipelineTests
        unit/runtime/renderer/ShaderCompilerPipelineTests.cpp
        unit/runtime/renderer/ShaderCompilerToolchainTests.cpp
)
target_compile_features(HoroShaderCompilerPipelineTests PRIVATE cxx_std_20)
target_include_directories(HoroShaderCompilerPipelineTests PRIVATE ${PROJECT_SOURCE_DIR}/tests)
target_link_libraries(HoroShaderCompilerPipelineTests PRIVATE HoroEngine::ShaderCompilerToolchain HoroEngine::Platform)
add_executable(HoroRenderFrontendTests
        unit/runtime/renderer/RenderFrontendTests.cpp
        unit/runtime/renderer/RenderReadbackTests.cpp
        unit/runtime/renderer/RenderTransferTests.cpp
        unit/runtime/renderer/RenderResourceRegistryTests.cpp
        unit/runtime/renderer/UiRenderCompositionTests.cpp
)

target_compile_features(HoroRenderFrontendTests PRIVATE cxx_std_20)
target_include_directories(HoroRenderFrontendTests PRIVATE
        ${PROJECT_SOURCE_DIR}/src/runtime/renderer/frontend
)
target_link_libraries(HoroRenderFrontendTests
        PRIVATE
        HoroEngine::RenderFrontend
        HoroEngine::RenderNull
)
add_executable(HoroRenderMemoryBudgetTests
        support/AllocationProbe.cpp
        unit/runtime/renderer/RenderMemoryBudgetTests.cpp
)
target_compile_features(HoroRenderMemoryBudgetTests PRIVATE cxx_std_20)
target_include_directories(HoroRenderMemoryBudgetTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(HoroRenderMemoryBudgetTests PRIVATE HoroEngine::RenderFrontend)
add_executable(HoroRuntimeLifecycleTests
        unit/runtime/lifecycle/RuntimeLifecycleTests.cpp
)
target_compile_features(HoroRuntimeLifecycleTests PRIVATE cxx_std_20)
target_link_libraries(HoroRuntimeLifecycleTests
        PRIVATE
        HoroEngine::Runtime
        HoroEngine::Platform
        HoroEngine::RenderFrontend
        HoroEngine::RenderNull
)
add_executable(HoroAndroidLifecycleTests
        unit/platform/AndroidLifecycleTests.cpp
)
target_compile_features(HoroAndroidLifecycleTests PRIVATE cxx_std_20)
target_include_directories(HoroAndroidLifecycleTests PRIVATE
        ${PROJECT_SOURCE_DIR}/src/platform/android
)
target_link_libraries(HoroAndroidLifecycleTests PRIVATE HoroEngine::Platform)
add_executable(HoroRuntimeSaveIdentityTests
        unit/runtime/save/SaveIdentityTests.cpp
)
target_compile_features(HoroRuntimeSaveIdentityTests PRIVATE cxx_std_20)
target_link_libraries(HoroRuntimeSaveIdentityTests PRIVATE HoroEngine::Runtime)
add_executable(HoroRuntimeSaveTelemetryTests unit/runtime/save/SaveTelemetryTests.cpp support/AllocationProbe.cpp)
target_include_directories(HoroRuntimeSaveTelemetryTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(HoroRuntimeSaveTelemetryTests PRIVATE cxx_std_20)
target_link_libraries(HoroRuntimeSaveTelemetryTests PRIVATE HoroEngine::Runtime HoroEngine::Application HoroEngine::PlatformServices Threads::Threads)
add_executable(HoroRuntimeSaveDiagnosticsTests
        unit/runtime/save/SaveDiagnosticsTests.cpp
)
target_compile_features(HoroRuntimeSaveDiagnosticsTests PRIVATE cxx_std_20)
target_link_libraries(HoroRuntimeSaveDiagnosticsTests PRIVATE HoroEngine::Runtime)
add_executable(HoroRuntimeSaveRootResolverTests
        unit/runtime/save/SaveRootResolverTests.cpp
)
target_compile_features(HoroRuntimeSaveRootResolverTests PRIVATE cxx_std_20)
target_link_libraries(HoroRuntimeSaveRootResolverTests PRIVATE HoroEngine::Runtime)

add_executable(HoroRuntimeSaveFilesystemLockTests
        unit/runtime/save/SaveFilesystemLockTests.cpp
)
target_compile_features(HoroRuntimeSaveFilesystemLockTests PRIVATE cxx_std_20)
# Deliberate non-installed test boundary: only the immutable native read mechanism is staged.
add_library(HoroSaveFilesystemReadInternal INTERFACE)
set(save_read_internal_root "${CMAKE_CURRENT_BINARY_DIR}/save-read-internal")
foreach(header SaveFilesystemPinnedRead.h SaveFilesystemStoragePlatform.h SaveFilesystemStorageDetails.h)
    configure_file("${PROJECT_SOURCE_DIR}/src/runtime/save/${header}" "${save_read_internal_root}/${header}" COPYONLY)
endforeach()
target_include_directories(HoroSaveFilesystemReadInternal INTERFACE "${save_read_internal_root}")
target_link_libraries(HoroSaveFilesystemReadInternal INTERFACE HoroEngine::Runtime)
target_link_libraries(HoroRuntimeSaveFilesystemLockTests PRIVATE HoroSaveFilesystemReadInternal)

add_executable(HoroRuntimeSaveNamespaceTests
        unit/runtime/save/SaveNamespaceTests.cpp
)
target_compile_features(HoroRuntimeSaveNamespaceTests PRIVATE cxx_std_20)
target_link_libraries(HoroRuntimeSaveNamespaceTests PRIVATE HoroEngine::Runtime)
add_executable(HoroRuntimeSaveParticipantRegistryTests
        support/AllocationProbe.cpp
        unit/runtime/save/SaveParticipantRegistryTests.cpp
)
target_compile_features(HoroRuntimeSaveParticipantRegistryTests PRIVATE cxx_std_20)
target_include_directories(HoroRuntimeSaveParticipantRegistryTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/support)
target_link_libraries(HoroRuntimeSaveParticipantRegistryTests PRIVATE HoroEngine::Runtime)
add_executable(HoroRuntimeSaveCaptureSnapshotTests
        support/AllocationProbe.cpp
        unit/runtime/save/SaveCaptureSnapshotTests.cpp
        unit/runtime/save/SaveCaptureSnapshotEdgeTests.cpp
        unit/runtime/save/SaveGameplayPersistenceTests.cpp
        unit/runtime/save/SaveGameplayPersistenceLifetimeTests.cpp
)
target_compile_features(HoroRuntimeSaveCaptureSnapshotTests PRIVATE cxx_std_20)
target_include_directories(HoroRuntimeSaveCaptureSnapshotTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/support)
target_link_libraries(HoroRuntimeSaveCaptureSnapshotTests PRIVATE HoroEngine::Runtime HoroEngine::GameplayRuntime)
add_executable(HoroRuntimeSaveRestoreTransactionTests
        support/AllocationProbe.cpp
        unit/runtime/save/SaveRestoreTransactionTests.cpp
)
target_compile_features(HoroRuntimeSaveRestoreTransactionTests PRIVATE cxx_std_20)
target_include_directories(HoroRuntimeSaveRestoreTransactionTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/support)
target_link_libraries(HoroRuntimeSaveRestoreTransactionTests PRIVATE HoroEngine::Runtime)
add_executable(HoroRuntimeUiIdentityTests
        unit/runtime/ui/UiIdentityTests.cpp
)
target_compile_features(HoroRuntimeUiIdentityTests PRIVATE cxx_std_20)
target_link_libraries(HoroRuntimeUiIdentityTests PRIVATE HoroEngine::RuntimeUi)
add_executable(HoroRuntimeUiImageResourceTests
        unit/runtime/ui/UiImageResourceTests.cpp
)
target_compile_features(HoroRuntimeUiImageResourceTests PRIVATE cxx_std_20)
target_link_libraries(HoroRuntimeUiImageResourceTests PRIVATE HoroEngine::RuntimeUi)
add_executable(HoroRuntimeUiBindingTests
        unit/runtime/ui/UiBindingTests.cpp
)
target_compile_features(HoroRuntimeUiBindingTests PRIVATE cxx_std_20)
target_link_libraries(HoroRuntimeUiBindingTests PRIVATE HoroEngine::RuntimeUi)
add_executable(HoroRuntimeUiBindingStoreTests unit/runtime/ui/UiBindingStoreTests.cpp)
add_executable(HoroRuntimeUiBindingWriteTests
        unit/runtime/ui/UiBindingWriteTests.cpp
        unit/runtime/ui/UiBindingWriteConflictTests.cpp
        unit/runtime/ui/UiBindingWriteLifecycleTests.cpp
        unit/runtime/ui/UiBindingWritePublicationTests.cpp
        unit/runtime/ui/UiBindingWriteTestFixture.cpp
        unit/runtime/ui/UiBindingWriteTestAuthority.cpp
        unit/runtime/ui/UiBindingWriteTestAllocations.cpp
)
target_compile_features(HoroRuntimeUiBindingWriteTests PRIVATE cxx_std_20)
target_link_libraries(HoroRuntimeUiBindingWriteTests PRIVATE HoroEngine::RuntimeUi)
add_executable(HoroRuntimeUiBindingWritePublicHeaderConsumer support/RuntimeUiBindingWritePublicHeaderConsumer.cpp)
target_compile_features(HoroRuntimeUiBindingWritePublicHeaderConsumer PRIVATE cxx_std_20)
target_link_libraries(HoroRuntimeUiBindingWritePublicHeaderConsumer PRIVATE HoroEngine::RuntimeUi)
add_test(NAME HoroRuntimeUiBindingWritePublicHeaderConsumer COMMAND HoroRuntimeUiBindingWritePublicHeaderConsumer)
set_tests_properties(HoroRuntimeUiBindingWritePublicHeaderConsumer PROPERTIES LABELS "unit;runtime_ui;headless")
target_compile_features(HoroRuntimeUiBindingStoreTests PRIVATE cxx_std_20)
target_link_libraries(HoroRuntimeUiBindingStoreTests PRIVATE HoroEngine::RuntimeUi)
add_executable(HoroRuntimeUiHotReloadTests
        unit/runtime/ui/UiHotReloadTests.cpp
        unit/runtime/ui/UiHotReloadTestFixture.cpp
        support/AllocationProbe.cpp
)
target_compile_features(HoroRuntimeUiHotReloadTests PRIVATE cxx_std_20)
target_include_directories(HoroRuntimeUiHotReloadTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(HoroRuntimeUiHotReloadTests PRIVATE HoroEngine::RuntimeUi)
add_executable(HoroRuntimeUiHotReloadPublicHeaderConsumer support/RuntimeUiHotReloadPublicHeaderConsumer.cpp)
target_compile_features(HoroRuntimeUiHotReloadPublicHeaderConsumer PRIVATE cxx_std_20)
target_link_libraries(HoroRuntimeUiHotReloadPublicHeaderConsumer PRIVATE HoroEngine::RuntimeUi)
add_test(NAME HoroRuntimeUiHotReloadPublicHeaderConsumer COMMAND HoroRuntimeUiHotReloadPublicHeaderConsumer)
set_tests_properties(HoroRuntimeUiHotReloadPublicHeaderConsumer PROPERTIES LABELS "unit;runtime_ui;headless")
add_executable(HoroRuntimeUiDocumentTests
        unit/runtime/ui/UiDocumentTests.cpp
        unit/runtime/ui/UiAssetLoadingTests.cpp
        unit/runtime/ui/UiDocumentSerializationTests.cpp
)
target_compile_features(HoroRuntimeUiDocumentTests PRIVATE cxx_std_20)
target_link_libraries(HoroRuntimeUiDocumentTests PRIVATE HoroEngine::RuntimeUi nlohmann_json::nlohmann_json)
add_executable(HoroUiTemplateGraphTests unit/ui_templates/UiTemplateDependencyGraphTests.cpp)
target_compile_features(HoroUiTemplateGraphTests PRIVATE cxx_std_20)
target_link_libraries(HoroUiTemplateGraphTests PRIVATE HoroEngine::UiTemplateGraph)
horo_register_catch_test(HoroUiTemplateGraphTests LABELS "unit;runtime_ui;template;headless")
add_executable(HoroRuntimeUiLocalizationTests
        unit/runtime/ui/UiLocalizationTests.cpp
)
target_compile_features(HoroRuntimeUiLocalizationTests PRIVATE cxx_std_20)
target_link_libraries(HoroRuntimeUiLocalizationTests PRIVATE HoroEngine::RuntimeUi)
add_executable(HoroRuntimeUiCanvasSpaceTests
        unit/runtime/ui/UiCanvasSpaceTests.cpp
)
target_compile_features(HoroRuntimeUiCanvasSpaceTests PRIVATE cxx_std_20)
target_link_libraries(HoroRuntimeUiCanvasSpaceTests PRIVATE HoroEngine::RuntimeUi)
add_executable(HoroRuntimeUiDiagnosticsTests
        unit/runtime/ui/UiDiagnosticsTests.cpp
)
target_compile_features(HoroRuntimeUiDiagnosticsTests PRIVATE cxx_std_20)
target_link_libraries(HoroRuntimeUiDiagnosticsTests PRIVATE HoroEngine::RuntimeUi)
add_executable(HoroRuntimeUiAccessibilityTests
        unit/runtime/ui/UiAccessibilityTests.cpp
        unit/runtime/ui/UiAccessibilityChangesTests.cpp
        unit/runtime/ui/UiAccessibilityDeliveryTests.cpp
)
target_compile_features(HoroRuntimeUiAccessibilityTests PRIVATE cxx_std_20)
target_link_libraries(HoroRuntimeUiAccessibilityTests PRIVATE HoroEngine::RuntimeUi)
add_executable(HoroRuntimeUiAccessibilityPublicHeaderConsumer support/RuntimeUiAccessibilityPublicHeaderConsumer.cpp)
target_compile_features(HoroRuntimeUiAccessibilityPublicHeaderConsumer PRIVATE cxx_std_20)
target_link_libraries(HoroRuntimeUiAccessibilityPublicHeaderConsumer PRIVATE HoroEngine::RuntimeUi)
add_test(NAME HoroRuntimeUiAccessibilityPublicHeaderConsumer COMMAND HoroRuntimeUiAccessibilityPublicHeaderConsumer)
set_tests_properties(HoroRuntimeUiAccessibilityPublicHeaderConsumer PROPERTIES LABELS "unit;runtime_ui;headless")
add_executable(HoroRuntimeUiElementTreeTests
        unit/runtime/ui/UiElementTreeTests.cpp
)
target_compile_features(HoroRuntimeUiElementTreeTests PRIVATE cxx_std_20)
target_link_libraries(HoroRuntimeUiElementTreeTests PRIVATE HoroEngine::RuntimeUi)
add_executable(HoroRuntimeUiRenderSnapshotTests
        unit/runtime/ui/UiRenderSnapshotTests.cpp
)
target_compile_features(HoroRuntimeUiRenderSnapshotTests PRIVATE cxx_std_20)
target_link_libraries(HoroRuntimeUiRenderSnapshotTests PRIVATE HoroEngine::RuntimeUi)
add_executable(HoroRuntimeUiRenderGeometryTests
        unit/runtime/ui/UiRenderGeometryTests.cpp
)
target_compile_features(HoroRuntimeUiRenderGeometryTests PRIVATE cxx_std_20)
target_link_libraries(HoroRuntimeUiRenderGeometryTests PRIVATE HoroEngine::RuntimeUi)
add_executable(HoroRuntimeUiLayoutTests
        unit/runtime/ui/UiLayoutTests.cpp
)
target_compile_features(HoroRuntimeUiLayoutTests PRIVATE cxx_std_20)
target_link_libraries(HoroRuntimeUiLayoutTests PRIVATE HoroEngine::RuntimeUi)
add_executable(HoroRuntimeUiLayoutClippingTests
        unit/runtime/ui/UiLayoutClippingTests.cpp
        support/AllocationProbe.cpp
)
target_compile_features(HoroRuntimeUiLayoutClippingTests PRIVATE cxx_std_20)
target_include_directories(HoroRuntimeUiLayoutClippingTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(HoroRuntimeUiLayoutClippingTests PRIVATE HoroEngine::RuntimeUi)
add_executable(HoroRuntimeUiTextLayoutTests
        unit/runtime/ui/UiTextLayoutTests.cpp
        support/AllocationProbe.cpp
)
target_compile_features(HoroRuntimeUiTextLayoutTests PRIVATE cxx_std_20)
target_include_directories(HoroRuntimeUiTextLayoutTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(HoroRuntimeUiTextLayoutTests PRIVATE HoroEngine::RuntimeUi)
add_executable(HoroRuntimeUiGlyphAtlasTests
        unit/runtime/ui/UiGlyphAtlasTests.cpp
)
target_compile_features(HoroRuntimeUiGlyphAtlasTests PRIVATE cxx_std_20)
target_link_libraries(HoroRuntimeUiGlyphAtlasTests PRIVATE HoroEngine::RuntimeUi)
add_executable(HoroRuntimeUiStyleTests
        unit/runtime/ui/UiStyleTests.cpp
)
target_compile_features(HoroRuntimeUiStyleTests PRIVATE cxx_std_20)
target_link_libraries(HoroRuntimeUiStyleTests PRIVATE HoroEngine::RuntimeUi)
add_executable(HoroRuntimeUiHitTestingTests
        unit/runtime/ui/UiHitTestingTests.cpp
        support/AllocationProbe.cpp
)
target_compile_features(HoroRuntimeUiHitTestingTests PRIVATE cxx_std_20)
target_include_directories(HoroRuntimeUiHitTestingTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(HoroRuntimeUiHitTestingTests PRIVATE HoroEngine::RuntimeUi)
add_executable(HoroRuntimeUiEventDispatchTests
        unit/runtime/ui/UiEventDispatchTests.cpp
        support/AllocationProbe.cpp
)
target_compile_features(HoroRuntimeUiEventDispatchTests PRIVATE cxx_std_20)
target_include_directories(HoroRuntimeUiEventDispatchTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(HoroRuntimeUiEventDispatchTests PRIVATE HoroEngine::RuntimeUi)
add_executable(HoroRuntimeUiPointerCaptureTests
        unit/runtime/ui/UiPointerCaptureTests.cpp
        support/AllocationProbe.cpp
)
target_compile_features(HoroRuntimeUiPointerCaptureTests PRIVATE cxx_std_20)
target_include_directories(HoroRuntimeUiPointerCaptureTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(HoroRuntimeUiPointerCaptureTests PRIVATE HoroEngine::RuntimeUi)
add_executable(HoroRuntimeUiActionsTests
        unit/runtime/ui/UiActionsTests.cpp
        unit/runtime/ui/UiAsyncActionsTests.cpp
        support/AllocationProbe.cpp
)
target_compile_features(HoroRuntimeUiActionsTests PRIVATE cxx_std_20)
target_include_directories(HoroRuntimeUiActionsTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(HoroRuntimeUiActionsTests PRIVATE HoroEngine::RuntimeUi)
add_executable(HoroRuntimeUiFeedbackTests
        unit/runtime/ui/UiFeedbackTests.cpp
        support/AllocationProbe.cpp
)
target_compile_features(HoroRuntimeUiFeedbackTests PRIVATE cxx_std_20)
target_include_directories(HoroRuntimeUiFeedbackTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(HoroRuntimeUiFeedbackTests PRIVATE HoroEngine::RuntimeUi)
add_executable(HoroRuntimeUiFeedbackPublicHeaderConsumer support/RuntimeUiFeedbackPublicHeaderConsumer.cpp)
target_compile_features(HoroRuntimeUiFeedbackPublicHeaderConsumer PRIVATE cxx_std_20)
target_link_libraries(HoroRuntimeUiFeedbackPublicHeaderConsumer PRIVATE HoroEngine::RuntimeUi)
add_test(NAME HoroRuntimeUiFeedbackPublicHeaderConsumer COMMAND HoroRuntimeUiFeedbackPublicHeaderConsumer)
set_tests_properties(HoroRuntimeUiFeedbackPublicHeaderConsumer PROPERTIES LABELS "unit;runtime_ui;headless")
add_executable(HoroRuntimeUiScreenStackTests
        unit/runtime/ui/UiScreenStackTests.cpp
        support/AllocationProbe.cpp
)
target_compile_features(HoroRuntimeUiScreenStackTests PRIVATE cxx_std_20)
target_include_directories(HoroRuntimeUiScreenStackTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(HoroRuntimeUiScreenStackTests PRIVATE HoroEngine::RuntimeUi)
add_executable(HoroRuntimeUiFocusGraphTests
        unit/runtime/ui/UiFocusGraphTests.cpp
        unit/runtime/ui/UiFocusSpatialNavigationTests.cpp
        support/AllocationProbe.cpp
)
target_compile_features(HoroRuntimeUiFocusGraphTests PRIVATE cxx_std_20)
target_include_directories(HoroRuntimeUiFocusGraphTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(HoroRuntimeUiFocusGraphTests PRIVATE HoroEngine::RuntimeUi)
add_executable(HoroRuntimeUiControlsTests
        unit/runtime/ui/UiControlsTests.cpp
        support/AllocationProbe.cpp
)
target_compile_features(HoroRuntimeUiControlsTests PRIVATE cxx_std_20)
target_include_directories(HoroRuntimeUiControlsTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(HoroRuntimeUiControlsTests PRIVATE HoroEngine::RuntimeUi)
add_executable(HoroRuntimeUiTextShapingTests
        unit/runtime/ui/UiTextShapingTests.cpp
)
target_compile_features(HoroRuntimeUiTextShapingTests PRIVATE cxx_std_20)
target_link_libraries(HoroRuntimeUiTextShapingTests PRIVATE HoroEngine::RuntimeUi)
add_executable(HoroRuntimeSaveArchiveMetadataTests
        unit/runtime/save/SaveArchiveMetadataTests.cpp
)
target_compile_features(HoroRuntimeSaveArchiveMetadataTests PRIVATE cxx_std_20)
target_link_libraries(HoroRuntimeSaveArchiveMetadataTests PRIVATE HoroEngine::Runtime)
add_executable(HoroRuntimeSaveArchiveFramingTests
        unit/runtime/save/SaveArchiveFramingTests.cpp
)
target_compile_features(HoroRuntimeSaveArchiveFramingTests PRIVATE cxx_std_20)
target_link_libraries(HoroRuntimeSaveArchiveFramingTests PRIVATE HoroEngine::Runtime)
add_executable(HoroRuntimeSaveArchiveFinalizationTests
        unit/runtime/save/SaveArchiveFinalizationTests.cpp
)
target_compile_features(HoroRuntimeSaveArchiveFinalizationTests PRIVATE cxx_std_20)
target_link_libraries(HoroRuntimeSaveArchiveFinalizationTests PRIVATE HoroEngine::Runtime)
add_executable(HoroRuntimeSaveArchiveReaderTests
        unit/runtime/save/SaveArchiveReaderTests.cpp
        unit/runtime/save/SaveArchiveContainerWriterTests.cpp
        unit/runtime/save/SaveArchiveRecordStagingTests.cpp
)
target_compile_features(HoroRuntimeSaveArchiveReaderTests PRIVATE cxx_std_20)
target_link_libraries(HoroRuntimeSaveArchiveReaderTests PRIVATE HoroEngine::Runtime)
add_executable(HoroRuntimeSaveArchiveProtectionTests
        unit/runtime/save/SaveArchiveProtectionTests.cpp
)
target_compile_features(HoroRuntimeSaveArchiveProtectionTests PRIVATE cxx_std_20)
target_link_libraries(HoroRuntimeSaveArchiveProtectionTests PRIVATE HoroEngine::Runtime)
add_executable(HoroRuntimeSaveMigrationTests
        unit/runtime/save/SaveMigrationTests.cpp
        unit/runtime/save/SaveMigrationRecordTests.cpp
)
target_compile_features(HoroRuntimeSaveMigrationTests PRIVATE cxx_std_20)
target_link_libraries(HoroRuntimeSaveMigrationTests PRIVATE HoroEngine::Runtime)
add_executable(HoroRuntimeSaveCanonicalCodecTests
        unit/runtime/save/SaveCanonicalCodecTests.cpp
        unit/runtime/save/SaveSceneCanonicalStateTests.cpp
)
target_compile_features(HoroRuntimeSaveCanonicalCodecTests PRIVATE cxx_std_20)
target_link_libraries(HoroRuntimeSaveCanonicalCodecTests PRIVATE HoroEngine::Runtime)
add_executable(HoroRuntimeSaveReferenceTests
        unit/runtime/save/SaveReferenceTests.cpp
)
target_compile_features(HoroRuntimeSaveReferenceTests PRIVATE cxx_std_20)
target_link_libraries(HoroRuntimeSaveReferenceTests PRIVATE HoroEngine::Runtime)
add_executable(HoroRuntimeSaveSlotMetadataTests
        unit/runtime/save/SaveSlotMetadataTests.cpp
)
target_compile_features(HoroRuntimeSaveSlotMetadataTests PRIVATE cxx_std_20)
target_link_libraries(HoroRuntimeSaveSlotMetadataTests PRIVATE HoroEngine::Runtime)
add_executable(HoroRuntimeSaveSlotIndexTests
        unit/runtime/save/SaveSlotIndexTests.cpp
)
target_compile_features(HoroRuntimeSaveSlotIndexTests PRIVATE cxx_std_20)
target_link_libraries(HoroRuntimeSaveSlotIndexTests PRIVATE HoroEngine::Runtime)
add_executable(HoroRuntimeSaveCloudRevisionMetadataTests
        unit/runtime/save/SaveCloudRevisionMetadataTests.cpp
)
target_compile_features(HoroRuntimeSaveCloudRevisionMetadataTests PRIVATE cxx_std_20)
target_link_libraries(HoroRuntimeSaveCloudRevisionMetadataTests PRIVATE HoroEngine::Runtime)
add_executable(HoroRuntimeSaveManagerProjectionTests
        unit/runtime/save/SaveManagerProjectionTests.cpp
)
target_compile_features(HoroRuntimeSaveManagerProjectionTests PRIVATE cxx_std_20)
target_link_libraries(HoroRuntimeSaveManagerProjectionTests PRIVATE HoroEngine::Runtime)
add_executable(HoroRuntimeSaveOperationTests
        support/AllocationProbe.cpp
        unit/runtime/save/SaveOperationTests.cpp
)
target_compile_features(HoroRuntimeSaveOperationTests PRIVATE cxx_std_20)
target_include_directories(HoroRuntimeSaveOperationTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(HoroRuntimeSaveOperationTests PRIVATE HoroEngine::Runtime)
add_executable(HoroRuntimeSaveParticipationTests
        unit/runtime/save/SaveParticipationTests.cpp
)
target_compile_features(HoroRuntimeSaveParticipationTests PRIVATE cxx_std_20)
target_link_libraries(HoroRuntimeSaveParticipationTests PRIVATE HoroEngine::Runtime)
add_executable(HoroRuntimePersistentEntityIdentityTests
        unit/runtime/scene/PersistentEntityIdentityTests.cpp
)
target_compile_features(HoroRuntimePersistentEntityIdentityTests PRIVATE cxx_std_20)
target_link_libraries(HoroRuntimePersistentEntityIdentityTests PRIVATE HoroEngine::RuntimeScene)
add_executable(HoroRuntimeSaveableComponentStateTests
        unit/runtime/scene/SaveableComponentStateTests.cpp
)
target_compile_features(HoroRuntimeSaveableComponentStateTests PRIVATE cxx_std_20)
target_link_libraries(HoroRuntimeSaveableComponentStateTests PRIVATE HoroEngine::RuntimeScene)
add_executable(HoroRuntimeRestoreReferenceTests
        unit/runtime/scene/SceneRestoreBundleTests.cpp
        unit/runtime/scene/RestoreReferenceGraphTests.cpp
        support/AllocationProbe.cpp
)
target_compile_features(HoroRuntimeRestoreReferenceTests PRIVATE cxx_std_20)
target_include_directories(HoroRuntimeRestoreReferenceTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/support)
target_link_libraries(HoroRuntimeRestoreReferenceTests PRIVATE HoroEngine::RuntimeScene HoroEngine::GameplayRuntime)
add_executable(HoroRestoreReferenceApiPublicHeaderConsumer support/RestoreReferenceApiPublicHeaderConsumer.cpp)
target_compile_features(HoroRestoreReferenceApiPublicHeaderConsumer PRIVATE cxx_std_20)
target_link_libraries(HoroRestoreReferenceApiPublicHeaderConsumer PRIVATE HoroEngine::GameplayApi)
add_test(NAME HoroRestoreReferenceApiPublicHeaderConsumer COMMAND HoroRestoreReferenceApiPublicHeaderConsumer)
set_tests_properties(HoroRestoreReferenceApiPublicHeaderConsumer PROPERTIES LABELS "unit;save;headless")
add_executable(HoroSceneRestoreBundlePublicHeaderConsumer support/SceneRestoreBundlePublicHeaderConsumer.cpp)
target_compile_features(HoroSceneRestoreBundlePublicHeaderConsumer PRIVATE cxx_std_20)
target_link_libraries(HoroSceneRestoreBundlePublicHeaderConsumer PRIVATE HoroEngine::RuntimeScene)
add_test(NAME HoroSceneRestoreBundlePublicHeaderConsumer COMMAND HoroSceneRestoreBundlePublicHeaderConsumer)
set_tests_properties(HoroSceneRestoreBundlePublicHeaderConsumer PROPERTIES LABELS "unit;save;headless")
add_executable(HoroRuntimeSaveStorageAdapterTests
        unit/runtime/save/SaveStorageAdapterTests.cpp
)
target_compile_features(HoroRuntimeSaveStorageAdapterTests PRIVATE cxx_std_20)
target_link_libraries(HoroRuntimeSaveStorageAdapterTests PRIVATE HoroEngine::Runtime)
add_executable(HoroRuntimeSaveStoragePolicyTests
        unit/runtime/save/SaveStoragePolicyTests.cpp
)
target_compile_features(HoroRuntimeSaveStoragePolicyTests PRIVATE cxx_std_20)
target_include_directories(HoroRuntimeSaveStoragePolicyTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(HoroRuntimeSaveStoragePolicyTests PRIVATE HoroEngine::Runtime)
add_executable(HoroRuntimeSaveOperationArbiterTests
        unit/runtime/save/SaveOperationArbiterTests.cpp
)
target_compile_features(HoroRuntimeSaveOperationArbiterTests PRIVATE cxx_std_20)
target_include_directories(HoroRuntimeSaveOperationArbiterTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(HoroRuntimeSaveOperationArbiterTests PRIVATE HoroEngine::Runtime)
add_executable(HoroRuntimeSaveSlotCommitTransactionTests
        unit/runtime/save/SaveSlotCommitTransactionTests.cpp
)
target_compile_features(HoroRuntimeSaveSlotCommitTransactionTests PRIVATE cxx_std_20)
target_link_libraries(HoroRuntimeSaveSlotCommitTransactionTests PRIVATE HoroEngine::Runtime)
add_executable(HoroRuntimeSaveSlotRecoveryTests
        unit/runtime/save/SaveSlotRecoveryTests.cpp
)
target_compile_features(HoroRuntimeSaveSlotRecoveryTests PRIVATE cxx_std_20)
target_link_libraries(HoroRuntimeSaveSlotRecoveryTests PRIVATE HoroEngine::Runtime)
add_executable(HoroRuntimeSaveSafePointCoordinatorTests
        unit/runtime/save/SaveSafePointCoordinatorTests.cpp
        unit/runtime/save/SaveCaptureBarrierTests.cpp
        unit/runtime/save/SaveFoundationQualificationTests.cpp
)
target_compile_features(HoroRuntimeSaveSafePointCoordinatorTests PRIVATE cxx_std_20)
target_include_directories(HoroRuntimeSaveSafePointCoordinatorTests PRIVATE ${PROJECT_SOURCE_DIR}/tests/support)
target_link_libraries(HoroRuntimeSaveSafePointCoordinatorTests PRIVATE HoroEngine::Runtime)

add_executable(HoroSaveCaptureBarrierPublicHeaderConsumer support/SaveCaptureBarrierPublicHeaderConsumer.cpp)
target_compile_features(HoroSaveCaptureBarrierPublicHeaderConsumer PRIVATE cxx_std_20)
target_link_libraries(HoroSaveCaptureBarrierPublicHeaderConsumer PRIVATE HoroEngine::Runtime)
add_test(NAME HoroSaveCaptureBarrierPublicHeaderConsumer COMMAND HoroSaveCaptureBarrierPublicHeaderConsumer)
set_tests_properties(HoroSaveCaptureBarrierPublicHeaderConsumer PROPERTIES LABELS "unit;save;headless")

add_executable(HoroRuntimeSaveAutosaveSchedulerTests
        support/AllocationProbe.cpp
        unit/runtime/save/SaveAutosaveSchedulerTests.cpp
        unit/runtime/save/SaveAutosaveIntegrationTests.cpp
)
target_compile_features(HoroRuntimeSaveAutosaveSchedulerTests PRIVATE cxx_std_20)
target_include_directories(HoroRuntimeSaveAutosaveSchedulerTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/support)
target_link_libraries(HoroRuntimeSaveAutosaveSchedulerTests PRIVATE HoroEngine::Runtime)

add_executable(HoroSaveAutosavePublicHeaderConsumer support/SaveAutosavePublicHeaderConsumer.cpp)
target_compile_features(HoroSaveAutosavePublicHeaderConsumer PRIVATE cxx_std_20)
target_link_libraries(HoroSaveAutosavePublicHeaderConsumer PRIVATE HoroEngine::Runtime)
add_test(NAME HoroSaveAutosavePublicHeaderConsumer COMMAND HoroSaveAutosavePublicHeaderConsumer)
set_tests_properties(HoroSaveAutosavePublicHeaderConsumer PROPERTIES LABELS "unit;save;headless")

add_executable(HoroRuntimeSaveProjectPolicyTests
        unit/runtime/save/SaveProjectPolicyTests.cpp
)
target_compile_features(HoroRuntimeSaveProjectPolicyTests PRIVATE cxx_std_20)
target_link_libraries(HoroRuntimeSaveProjectPolicyTests PRIVATE HoroEngine::Runtime)
add_executable(HoroRuntimeSaveTestCompositionsTests
        unit/runtime/save/SaveTestCompositionsTests.cpp
)
target_compile_features(HoroRuntimeSaveTestCompositionsTests PRIVATE cxx_std_20)
target_link_libraries(HoroRuntimeSaveTestCompositionsTests PRIVATE HoroEngine::Runtime)
add_executable(HoroRuntimeSceneTests
        unit/runtime/scene/AudioSceneExtractionTests.cpp
        unit/runtime/scene/NavigationSceneComponentsTests.cpp
        unit/runtime/scene/NavigationSceneActivationTests.cpp
        unit/runtime/scene/NavigationSceneLinkCaptureTests.cpp
        unit/runtime/scene/PhysicsSceneComponentsTests.cpp
        unit/runtime/scene/RuntimeSceneLifecycleTests.cpp
        unit/runtime/scene/RuntimeSceneTests.cpp
        unit/runtime/scene/RuntimeScenePublicationReceiptTests.cpp
        unit/runtime/scene/RuntimeSceneGroupReferenceTests.cpp
)
target_sources(HoroRuntimeScenePublicHeaderConsumer PRIVATE support/AudioScenePublicContract.cpp)
target_compile_features(HoroRuntimeSceneTests PRIVATE cxx_std_20)
target_include_directories(HoroRuntimeSceneTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/support)
target_link_libraries(HoroRuntimeSceneTests PRIVATE HoroEngine::NavigationSceneIntegration HoroEngine::RuntimeScene)
add_executable(HoroRuntimeSceneCellPayloadTests unit/runtime/scene/RuntimeSceneCellPayloadTests.cpp
    unit/runtime/scene/IncrementalSceneCellCookTests.cpp
    unit/runtime/scene/SceneCellContentHasherTests.cpp)
target_compile_features(HoroRuntimeSceneCellPayloadTests PRIVATE cxx_std_20)
target_include_directories(HoroRuntimeSceneCellPayloadTests PRIVATE ${PROJECT_SOURCE_DIR}/src/runtime/scene)
target_link_libraries(HoroRuntimeSceneCellPayloadTests PRIVATE HoroEngine::SceneCellPayload)
horo_register_catch_test(HoroRuntimeSceneCellPayloadTests LABELS "unit;scene;world_streaming;headless")
target_sources(HoroSceneCellPayloadPublicHeaderConsumer PRIVATE support/SceneCellPayloadPublicHeaderConsumer.cpp)
add_executable(HoroRuntimeSavedSceneBootstrapTests
        unit/runtime/scene/SavedSceneBootstrapTests.cpp
        unit/runtime/scene/SaveContentRequirementsTests.cpp
        unit/runtime/scene/SaveContentWorldTests.cpp
        unit/runtime/scene/SaveContentRetentionTests.cpp
        support/AllocationProbe.cpp
)
target_compile_features(HoroRuntimeSavedSceneBootstrapTests PRIVATE cxx_std_20)
target_include_directories(HoroRuntimeSavedSceneBootstrapTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/support)
target_link_libraries(HoroRuntimeSavedSceneBootstrapTests PRIVATE HoroEngine::RuntimeScene)
add_executable(HoroAssetRegistryTests
        unit/runtime/assets/AssetRegistryTests.cpp
)
target_compile_features(HoroAssetRegistryTests PRIVATE cxx_std_20)
target_link_libraries(HoroAssetRegistryTests PRIVATE HoroEngine::Assets)
add_executable(HoroAssetPayloadReleaseOrderTests unit/runtime/assets/AssetPayloadReleaseOrderTests.cpp)
target_compile_features(HoroAssetPayloadReleaseOrderTests PRIVATE cxx_std_20)
target_link_libraries(HoroAssetPayloadReleaseOrderTests PRIVATE HoroEngine::Assets Threads::Threads)
add_test(NAME HoroAssetPayloadReleaseOrderTests COMMAND HoroAssetPayloadReleaseOrderTests)
set_tests_properties(HoroAssetPayloadReleaseOrderTests PROPERTIES LABELS "unit;assets;headless;allocation;lifecycle" TIMEOUT 30)

add_executable(HoroAssetProviderTests
        unit/runtime/assets/AssetProviderTests.cpp
        unit/runtime/assets/AssetPayloadCacheTests.cpp
)
target_compile_features(HoroAssetProviderTests PRIVATE cxx_std_20)
target_include_directories(HoroAssetProviderTests PRIVATE
        ${PROJECT_SOURCE_DIR}/src/runtime/assets/runtime_provider
)
target_link_libraries(HoroAssetProviderTests PRIVATE HoroEngine::Assets)
add_executable(HoroAssetPreviewServiceTests
        unit/runtime/assets/AssetPreviewServiceTests.cpp
)
target_compile_features(HoroAssetPreviewServiceTests PRIVATE cxx_std_20)
target_link_libraries(HoroAssetPreviewServiceTests PRIVATE HoroEngine::Assets)
horo_register_catch_test(HoroAssetPreviewServiceTests LABELS "assets;preview;headless")
add_executable(HoroAssetCookTests
        unit/runtime/assets/AssetCookContractTests.cpp
)
target_compile_features(HoroAssetCookTests PRIVATE cxx_std_20)
target_link_libraries(HoroAssetCookTests PRIVATE HoroEngine::Assets)
add_executable(HoroAssetChunkPlanTests
        unit/runtime/assets/AssetChunkPlanTests.cpp
        unit/runtime/assets/AssetArchiveTests.cpp)
target_link_libraries(HoroAssetChunkPlanTests PRIVATE HoroEngine::Assets HoroEngine::Platform)
horo_register_catch_test(HoroAssetChunkPlanTests LABELS "assets;release;headless")
add_executable(HoroCookCatalogTests
        unit/runtime/assets/CookCatalogTests.cpp
)
target_compile_features(HoroCookCatalogTests PRIVATE cxx_std_20)
target_link_libraries(HoroCookCatalogTests PRIVATE HoroEngine::Assets)
add_executable(HoroHeadlessNullCookerTests
        unit/runtime/assets/HeadlessNullCookerTests.cpp
)
target_compile_features(HoroHeadlessNullCookerTests PRIVATE cxx_std_20)
target_link_libraries(HoroHeadlessNullCookerTests PRIVATE HoroEngine::Assets)
target_include_directories(HoroHeadlessNullCookerTests PRIVATE
        ${PROJECT_SOURCE_DIR}/src/runtime/assets/cooker/builtin
)
add_executable(HoroAssetCookCacheTests
        unit/runtime/assets/AssetCookCacheTests.cpp
)
target_compile_features(HoroAssetCookCacheTests PRIVATE cxx_std_20)
target_link_libraries(HoroAssetCookCacheTests PRIVATE HoroEngine::Assets)
add_executable(HoroAssetCookOutputTests
        unit/runtime/assets/AssetCookOutputTests.cpp
        unit/runtime/assets/AssetCookPublicationTests.cpp
)
target_compile_features(HoroAssetCookOutputTests PRIVATE cxx_std_20)
target_link_libraries(HoroAssetCookOutputTests PRIVATE HoroEngine::Assets HoroEngine::Platform)
target_include_directories(HoroAssetCookOutputTests PRIVATE ${PROJECT_SOURCE_DIR}/tests/support)
add_executable(HoroAssetCookServiceTests
        unit/runtime/assets/AssetCookServiceTests.cpp
        unit/runtime/assets/AssetCookServicePublicationTests.cpp
        unit/runtime/assets/AssetCookInputSnapshotTests.cpp
)
target_compile_features(HoroAssetCookServiceTests PRIVATE cxx_std_20)
target_link_libraries(HoroAssetCookServiceTests PRIVATE HoroEngine::Assets HoroEngine::Foundation HoroEngine::Platform)
target_include_directories(HoroAssetCookServiceTests PRIVATE
        ${PROJECT_SOURCE_DIR}/src/runtime/assets/cooker/builtin
        ${PROJECT_SOURCE_DIR}/tests/support
)
add_executable(HoroAssetCookIntegrationTests
        integration/assets/AssetCookIntegrationTests.cpp
)
target_compile_features(HoroAssetCookIntegrationTests PRIVATE cxx_std_20)
target_compile_definitions(HoroAssetCookIntegrationTests PRIVATE PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
target_link_libraries(HoroAssetCookIntegrationTests PRIVATE HoroEngine::Assets HoroEngine::Foundation HoroEngine::Platform)
target_include_directories(HoroAssetCookIntegrationTests PRIVATE
        ${PROJECT_SOURCE_DIR}/src/runtime/assets/cooker/builtin
        ${PROJECT_SOURCE_DIR}/tests/support
)
add_executable(HoroAssetImporterCatalogTests
        unit/runtime/assets/AssetImporterCatalogTests.cpp
)
target_compile_features(HoroAssetImporterCatalogTests PRIVATE cxx_std_20)
target_link_libraries(HoroAssetImporterCatalogTests PRIVATE HoroEngine::Assets)
add_executable(HoroAssetImportOperationTests
        unit/runtime/assets/AssetImportOperationTests.cpp
        unit/runtime/assets/AssetReimportTests.cpp
)
target_compile_features(HoroAssetImportOperationTests PRIVATE cxx_std_20)
target_link_libraries(HoroAssetImportOperationTests PRIVATE
        HoroEngine::Assets HoroEngine::Foundation HoroEngine::Platform)
add_executable(HoroObjMeshImporterTests
        unit/runtime/assets/ObjMeshImporterTests.cpp
)
target_compile_features(HoroObjMeshImporterTests PRIVATE cxx_std_20)
target_link_libraries(HoroObjMeshImporterTests PRIVATE HoroEngine::Assets)
target_include_directories(HoroObjMeshImporterTests PRIVATE
        ${PROJECT_SOURCE_DIR}/src/runtime/assets/importer/builtin/obj_mesh
)
add_executable(HoroFbxMeshImporterTests
        unit/runtime/assets/FbxMeshImporterTests.cpp
)
target_compile_features(HoroFbxMeshImporterTests PRIVATE cxx_std_20)
target_link_libraries(HoroFbxMeshImporterTests PRIVATE HoroEngine::Assets)
target_compile_definitions(HoroFbxMeshImporterTests PRIVATE
        HORO_UFBX_TEST_DATA_DIR="${ufbx_SOURCE_DIR}/data"
)
target_include_directories(HoroFbxMeshImporterTests PRIVATE
        ${PROJECT_SOURCE_DIR}/src/runtime/assets/importer/builtin/fbx_mesh
)
add_executable(HoroModularAssetImportIntegrationTests
        integration/assets/ModularAssetImportIntegrationTests.cpp
)
target_compile_features(HoroModularAssetImportIntegrationTests PRIVATE cxx_std_20)
target_compile_definitions(HoroModularAssetImportIntegrationTests PRIVATE PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
target_link_libraries(HoroModularAssetImportIntegrationTests PRIVATE HoroEngine::Assets HoroEngine::Foundation HoroEngine::Platform)
target_include_directories(HoroModularAssetImportIntegrationTests PRIVATE
        ${PROJECT_SOURCE_DIR}/src/runtime/assets/cooker/builtin
        ${PROJECT_SOURCE_DIR}/src/runtime/assets/importer
        ${PROJECT_SOURCE_DIR}/tests/support
)
add_executable(HoroExtensionDiscoveryTests
        unit/extensions/DiscoveryPathsTests.cpp
        unit/extensions/DiscoveryPlanTests.cpp
        unit/extensions/DiscoveryRootPolicyTests.cpp
)
target_compile_features(HoroExtensionDiscoveryTests PRIVATE cxx_std_20)
target_include_directories(HoroExtensionDiscoveryTests PRIVATE ${PROJECT_SOURCE_DIR}/src/extensions/discovery)
target_link_libraries(HoroExtensionDiscoveryTests PRIVATE HoroEngine::Extensions Catch2::Catch2WithMain)
add_test(NAME HoroExtensionDiscoveryTests COMMAND HoroExtensionDiscoveryTests)

add_executable(HoroExtensionAbiCConsumer unit/extensions/ExtensionAbiCConsumer.c)
target_sources(HoroExtensionsPublicHeaderConsumer PRIVATE support/EditorActivityPublicContract.cpp)
target_compile_features(HoroExtensionAbiCConsumer PRIVATE c_std_11)
set_target_properties(HoroExtensionAbiCConsumer PROPERTIES C_EXTENSIONS OFF)
target_link_libraries(HoroExtensionAbiCConsumer PRIVATE HoroEngine::Extensions)
add_test(NAME HoroExtensionAbiCConsumer COMMAND HoroExtensionAbiCConsumer)

# Compile the external plugin source in the root graph against the owning staged
# C ABI headers and generated SDK compatibility contract. The relocated installed
# SDK configure/build/load test above remains the packaging authority.
add_library(HoroExtensionSdkCContract OBJECT cmake/extension_sdk_consumer/ExternalExtension.c)
target_compile_features(HoroExtensionSdkCContract PRIVATE c_std_11)
set_target_properties(HoroExtensionSdkCContract PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_link_libraries(HoroExtensionSdkCContract PRIVATE HoroEngine::Extensions)
target_compile_definitions(HoroExtensionSdkCContract PRIVATE
    "HORO_EXTENSION_SDK_ABI_MAJOR=${HORO_EXTENSION_ABI_MAJOR_VERSION}"
    "HORO_EXTENSION_SDK_ABI_PUBLISHED_MINOR=${HORO_EXTENSION_ABI_PUBLISHED_MINOR}"
    "HORO_EXTENSION_SDK_ABI_MIN_HOST_MINOR=${HORO_EXTENSION_ABI_MIN_HOST_MINOR}"
    "HORO_EXTENSION_SDK_ABI_MAX_HOST_MAJOR_EXCLUSIVE=${HORO_EXTENSION_ABI_MAX_HOST_MAJOR_EXCLUSIVE}")
add_dependencies(HoroExtensionAbiCConsumer HoroExtensionSdkCContract)

add_executable(HoroExtensionRetirementPublicHeaderConsumer support/ExtensionRetirementPublicHeaderConsumer.cpp)
target_compile_features(HoroExtensionRetirementPublicHeaderConsumer PRIVATE cxx_std_20)
target_link_libraries(HoroExtensionRetirementPublicHeaderConsumer PRIVATE HoroEngine::Extensions)
add_test(NAME HoroExtensionRetirementPublicHeaderConsumer COMMAND HoroExtensionRetirementPublicHeaderConsumer)
set_tests_properties(HoroExtensionRetirementPublicHeaderConsumer PROPERTIES LABELS "unit;extensions;headless")

add_executable(HoroPackagedExtensionActivation helpers/extensions/PackagedExtensionActivation.cpp)
target_compile_features(HoroPackagedExtensionActivation PRIVATE cxx_std_20)
target_include_directories(HoroPackagedExtensionActivation PRIVATE ${PROJECT_SOURCE_DIR}/tests/support)
target_link_libraries(HoroPackagedExtensionActivation PRIVATE HoroEngine::Extensions)

add_test(NAME HoroExtensionManifestValidatorValid
    COMMAND "$<TARGET_FILE:horo-extension-validate>" --json --schema-version 1
        "${PROJECT_SOURCE_DIR}/tests/fixtures/extensions/manifest-validator-valid.json")
set_tests_properties(HoroExtensionManifestValidatorValid PROPERTIES
    LABELS "extensions;sdk;cli"
    PASS_REGULAR_EXPRESSION
        "\\\"valid\\\":true,\\\"schemaVersion\\\":1,\\\"id\\\":\\\"com.horo.fixture.validator\\\"")

add_test(NAME HoroExtensionManifestValidatorInvalid
    COMMAND "${CMAKE_COMMAND}"
        "-DVALIDATOR=$<TARGET_FILE:horo-extension-validate>"
        "-DMANIFEST=${PROJECT_SOURCE_DIR}/tests/fixtures/extensions/manifest-validator-invalid.json"
        -P "${PROJECT_SOURCE_DIR}/tests/cmake/VerifyExtensionValidatorFailure.cmake")
set_tests_properties(HoroExtensionManifestValidatorInvalid PROPERTIES
    LABELS "extensions;sdk;cli")

add_test(NAME HoroExtensionManifestValidatorContracts
    COMMAND "${CMAKE_COMMAND}"
        "-DVALIDATOR=$<TARGET_FILE:horo-extension-validate>"
        "-DTEST_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}/extension-validator-contracts"
        "-DESCAPED_MANIFEST=${PROJECT_SOURCE_DIR}/tests/fixtures/extensions/manifest-validator-escaped-path.json"
        -P "${PROJECT_SOURCE_DIR}/tests/cmake/VerifyExtensionValidatorContracts.cmake")
set_tests_properties(HoroExtensionManifestValidatorContracts PROPERTIES
    LABELS "extensions;sdk;cli")

add_test(NAME HoroExtensionManifestValidatorRejectsUnknownSchema
    COMMAND "${CMAKE_COMMAND}"
        "-DVALIDATOR=$<TARGET_FILE:horo-extension-validate>"
        "-DMANIFEST=${PROJECT_SOURCE_DIR}/tests/fixtures/extensions/manifest-validator-valid.json"
        -P "${PROJECT_SOURCE_DIR}/tests/cmake/VerifyExtensionValidatorUsageFailure.cmake")
set_tests_properties(HoroExtensionManifestValidatorRejectsUnknownSchema PROPERTIES
    LABELS "extensions;sdk;cli")

add_test(NAME HoroExtensionManifestValidatorIsPackaged
    COMMAND "${CMAKE_COMMAND}"
        "-DVALIDATOR=${HORO_EXTENSION_SDK_PACKAGE_DIR}/bin/horo-extension-validate${CMAKE_EXECUTABLE_SUFFIX}"
        "-DCONFORMANCE=${HORO_EXTENSION_SDK_PACKAGE_DIR}/bin/horo-extension-conformance${CMAKE_EXECUTABLE_SUFFIX}"
        "-DSCHEMA=${HORO_EXTENSION_SDK_PACKAGE_DIR}/share/horo/extension-sdk/extension-manifest-v1.schema.json"
        -P "${PROJECT_SOURCE_DIR}/tests/cmake/VerifyExtensionValidatorPackage.cmake")
set_tests_properties(HoroExtensionManifestValidatorIsPackaged PROPERTIES
    LABELS "extensions;sdk;cli")

add_test(NAME HoroPackageAuthorCommands
    COMMAND "${Python3_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/tests/python/test_horo_package_commands.py"
        "${HORO_EXTENSION_SDK_PACKAGE_DIR}"
        "${HORO_EXTENSION_SDK_PACKAGE_DIR}/bin/horo-package${CMAKE_EXECUTABLE_SUFFIX}")
set_tests_properties(HoroPackageAuthorCommands PROPERTIES LABELS "packages;sdk;cli")

add_executable(HoroEditorActivityBoundaryTests
        unit/extensions/EditorActivityBoundaryTests.cpp
        support/AllocationProbe.cpp)
target_compile_features(HoroEditorActivityBoundaryTests PRIVATE cxx_std_20)
target_include_directories(HoroEditorActivityBoundaryTests PRIVATE
        ${PROJECT_SOURCE_DIR}/src/extensions/host ${PROJECT_SOURCE_DIR}/tests/support)
target_link_libraries(HoroEditorActivityBoundaryTests PRIVATE HoroEngine::Extensions Catch2::Catch2WithMain)
add_test(NAME HoroEditorActivityBoundaryTests COMMAND HoroEditorActivityBoundaryTests)
set_tests_properties(HoroEditorActivityBoundaryTests PROPERTIES LABELS "unit;extensions;headless;sonar-coverage;ci-windows")

add_executable(HoroExtensionManagerTests
        unit/extensions/ApplicationCapabilityRegistryTests.cpp
        unit/extensions/ExtensionActivationTransactionTests.cpp
        unit/extensions/ExtensionRetirementTests.cpp
        unit/extensions/ExtensionCapabilityAdmissionTests.cpp
        unit/extensions/ExtensionActivationStateTests.cpp
        unit/extensions/EditorSurfaceDescriptorTests.cpp
        unit/extensions/EditorSvgIconTests.cpp
        unit/extensions/EditorActivityHostTests.cpp
        unit/extensions/EditorSurfaceContextTests.cpp
        unit/extensions/EditorSurfaceRegistryTests.cpp
        unit/extensions/EditorActivityRegistryTests.cpp
        unit/extensions/EditorCommandRegistryTests.cpp
        unit/extensions/EditorUiFormTests.cpp
        unit/extensions/ExtensionManagerTests.cpp
        unit/extensions/ExtensionManifestParserTests.cpp
        unit/extensions/ScriptExportDescriptorTests.cpp
        unit/extensions/ExtensionModuleResolutionTests.cpp
        unit/extensions/ExtensionAbiValidationTests.cpp
        unit/extensions/ExtensionAbiConformanceTests.cpp
        unit/extensions/ExtensionAbiIntegrationTests.cpp
        unit/extensions/ExternalImporterRegistrationTests.cpp
        integration/extensions/HeadlessExtensionHostTests.cpp
)
add_library(HoroEditorActivityFixture MODULE fixtures/extensions/EditorActivityModule.c)
target_compile_features(HoroEditorActivityFixture PRIVATE c_std_11)
# MSVC requires explicit opt-in for the C11 atomics used by the deterministic cancellation barrier.
if (MSVC AND CMAKE_C_COMPILER_ID STREQUAL "MSVC")
    target_compile_options(HoroEditorActivityFixture PRIVATE /experimental:c11atomics)
endif ()
set_target_properties(HoroEditorActivityFixture PROPERTIES C_EXTENSIONS OFF)
target_link_libraries(HoroEditorActivityFixture PRIVATE HoroEngine::Extensions)
add_dependencies(HoroExtensionManagerTests HoroEditorActivityFixture)
if (TARGET HoroGuiScreenHostLifecycleTests)
    add_dependencies(HoroGuiScreenHostLifecycleTests HoroEditorActivityFixture)
    target_compile_definitions(HoroGuiScreenHostLifecycleTests PRIVATE HORO_EDITOR_ACTIVITY_FIXTURE="$<TARGET_FILE:HoroEditorActivityFixture>")
endif ()

if (TARGET HoroExtensionActivityViewTests)
    add_dependencies(HoroExtensionActivityViewTests HoroEditorActivityFixture)
    target_compile_definitions(HoroExtensionActivityViewTests PRIVATE HORO_EDITOR_ACTIVITY_FIXTURE="$<TARGET_FILE:HoroEditorActivityFixture>")
endif()

target_compile_definitions(HoroExtensionManagerTests PRIVATE HORO_EDITOR_ACTIVITY_FIXTURE="$<TARGET_FILE:HoroEditorActivityFixture>")
foreach(abi_fixture_mode RANGE 0 11)
    set(abi_fixture_target HoroExtensionAbiFixture${abi_fixture_mode})
    add_library(${abi_fixture_target} MODULE fixtures/extensions/AbiModule.c)
    target_compile_features(${abi_fixture_target} PRIVATE c_std_11)
    set_target_properties(${abi_fixture_target} PROPERTIES C_EXTENSIONS OFF)
    target_link_libraries(${abi_fixture_target} PRIVATE HoroEngine::Extensions)
    target_compile_definitions(${abi_fixture_target} PRIVATE HORO_ABI_FIXTURE_MODE=${abi_fixture_mode})
    add_dependencies(HoroExtensionManagerTests ${abi_fixture_target})
    target_compile_definitions(HoroExtensionManagerTests PRIVATE
            HORO_ABI_FIXTURE_${abi_fixture_mode}="$<TARGET_FILE:${abi_fixture_target}>")
endforeach()
add_library(HoroExtensionAbiMissingLoadFixture MODULE fixtures/extensions/AbiModuleMissingLoad.c)
target_compile_features(HoroExtensionAbiMissingLoadFixture PRIVATE c_std_11)
set_target_properties(HoroExtensionAbiMissingLoadFixture PROPERTIES C_EXTENSIONS OFF)
target_link_libraries(HoroExtensionAbiMissingLoadFixture PRIVATE HoroEngine::Extensions)
add_dependencies(HoroExtensionManagerTests HoroExtensionAbiMissingLoadFixture)
target_compile_definitions(HoroExtensionManagerTests PRIVATE
        HORO_ABI_MISSING_LOAD_FIXTURE="$<TARGET_FILE:HoroExtensionAbiMissingLoadFixture>")
foreach(abi_cleanup_fixture_mode RANGE 0 2)
    set(abi_cleanup_fixture_target HoroExtensionAbiCleanupFixture${abi_cleanup_fixture_mode})
    add_library(${abi_cleanup_fixture_target} MODULE fixtures/extensions/AbiCleanupModule.cpp)
    target_compile_features(${abi_cleanup_fixture_target} PRIVATE cxx_std_20)
    target_link_libraries(${abi_cleanup_fixture_target} PRIVATE HoroEngine::Extensions)
    target_compile_definitions(${abi_cleanup_fixture_target} PRIVATE HORO_ABI_CLEANUP_FIXTURE_MODE=${abi_cleanup_fixture_mode})
    add_dependencies(HoroExtensionManagerTests ${abi_cleanup_fixture_target})
    target_compile_definitions(HoroExtensionManagerTests PRIVATE
            HORO_ABI_CLEANUP_FIXTURE_${abi_cleanup_fixture_mode}="$<TARGET_FILE:${abi_cleanup_fixture_target}>")
endforeach()
add_dependencies(HoroExtensionManagerTests horo-extension-conformance)
foreach(abi_hostile_fixture_mode RANGE 0 7)
    set(abi_hostile_fixture_target HoroExtensionAbiHostileFixture${abi_hostile_fixture_mode})
    add_library(${abi_hostile_fixture_target} MODULE fixtures/extensions/AbiHostileModule.cpp)
    target_compile_features(${abi_hostile_fixture_target} PRIVATE cxx_std_20)
    target_link_libraries(${abi_hostile_fixture_target} PRIVATE HoroEngine::Extensions)
    target_compile_definitions(${abi_hostile_fixture_target} PRIVATE HORO_ABI_HOSTILE_FIXTURE_MODE=${abi_hostile_fixture_mode})
    add_dependencies(HoroExtensionManagerTests ${abi_hostile_fixture_target})
    target_compile_definitions(HoroExtensionManagerTests PRIVATE
            HORO_ABI_HOSTILE_FIXTURE_${abi_hostile_fixture_mode}="$<TARGET_FILE:${abi_hostile_fixture_target}>")
endforeach()
add_test(NAME HoroExtensionAbiConformanceCliSupported
    COMMAND "$<TARGET_FILE:horo-extension-conformance>" --json "$<TARGET_FILE:HoroExtensionAbiFixture1>")
set_tests_properties(HoroExtensionAbiConformanceCliSupported PROPERTIES
    LABELS "extensions;sdk;cli"
    PASS_REGULAR_EXPRESSION
        "\"passed\":true,\"code\":\"extension.abi.conformance.passed\"")
add_test(NAME HoroExtensionAbiConformanceCliIncompatible
    COMMAND "${CMAKE_COMMAND}"
        "-DCONFORMANCE=$<TARGET_FILE:horo-extension-conformance>"
        "-DMODULE=$<TARGET_FILE:HoroExtensionAbiFixture2>"
        -P "${PROJECT_SOURCE_DIR}/tests/cmake/VerifyExtensionAbiConformanceFailure.cmake")
set_tests_properties(HoroExtensionAbiConformanceCliIncompatible PROPERTIES
    LABELS "extensions;sdk;cli")
add_test(NAME HoroExtensionAbiConformanceCliRequiresModule
    COMMAND "$<TARGET_FILE:horo-extension-conformance>")
set_tests_properties(HoroExtensionAbiConformanceCliRequiresModule PROPERTIES
    LABELS "extensions;sdk;cli"
    WILL_FAIL TRUE)
target_include_directories(HoroExtensionManagerTests PRIVATE ${PROJECT_SOURCE_DIR}/src/extensions/host)
target_include_directories(HoroExtensionManagerTests PRIVATE ${PROJECT_SOURCE_DIR}/src/extensions/capabilities/asset_pipeline_points)
target_include_directories(HoroExtensionManagerTests PRIVATE ${PROJECT_SOURCE_DIR}/tests/support)
target_compile_features(HoroExtensionManagerTests PRIVATE cxx_std_20)
target_link_libraries(HoroExtensionManagerTests PRIVATE HoroEngine::Extensions Catch2::Catch2WithMain)
if(TARGET HoroAssetImporterBasicExample)
    add_dependencies(HoroExtensionManagerTests HoroAssetImporterBasicExample)
    target_compile_definitions(HoroExtensionManagerTests PRIVATE
            HORO_BASIC_EXTENSION_DIR="$<TARGET_FILE_DIR:HoroAssetImporterBasicExample>"
    )
endif()
add_test(NAME HoroExtensionManagerTests COMMAND HoroExtensionManagerTests)

add_executable(HoroProcessObserverTests unit/extensions/ProcessObserverRegistryTests.cpp)
target_compile_features(HoroProcessObserverTests PRIVATE cxx_std_20)
target_link_libraries(HoroProcessObserverTests PRIVATE HoroEngine::Extensions Catch2::Catch2WithMain)
add_test(NAME HoroProcessObserverTests COMMAND HoroProcessObserverTests)
set_tests_properties(HoroProcessObserverTests PROPERTIES LABELS "unit;extensions;headless")

add_executable(HoroEditorThemeTokenTests
        unit/extensions/EditorThemeTokenTests.cpp
)
target_compile_features(HoroEditorThemeTokenTests PRIVATE cxx_std_20)
target_link_libraries(HoroEditorThemeTokenTests PRIVATE HoroEngine::Extensions Catch2::Catch2WithMain)
add_test(NAME HoroEditorThemeTokenTests COMMAND HoroEditorThemeTokenTests)

add_executable(HoroProjectValidatorRegistryTests
        unit/extensions/ProjectValidatorRegistryTests.cpp
)
target_compile_features(HoroProjectValidatorRegistryTests PRIVATE cxx_std_20)
target_link_libraries(HoroProjectValidatorRegistryTests PRIVATE HoroEngine::Extensions Catch2::Catch2WithMain)
add_test(NAME HoroProjectValidatorRegistryTests COMMAND HoroProjectValidatorRegistryTests)

add_executable(HoroPipelineStepRegistryTests
        unit/extensions/PipelineStepRegistryTests.cpp
)
target_compile_features(HoroPipelineStepRegistryTests PRIVATE cxx_std_20)
target_link_libraries(HoroPipelineStepRegistryTests PRIVATE HoroEngine::Extensions Catch2::Catch2WithMain)
add_test(NAME HoroPipelineStepRegistryTests COMMAND HoroPipelineStepRegistryTests)

add_executable(HoroAssetCookerRegistryTests
        unit/extensions/AssetCookerRegistryTests.cpp
)
target_compile_features(HoroAssetCookerRegistryTests PRIVATE cxx_std_20)
target_link_libraries(HoroAssetCookerRegistryTests PRIVATE HoroEngine::Extensions Catch2::Catch2WithMain)
add_test(NAME HoroAssetCookerRegistryTests COMMAND HoroAssetCookerRegistryTests)

add_library(HoroBackendServiceReference STATIC
        fixtures/extensions/BackendServiceReference.cpp
)
target_compile_features(HoroBackendServiceReference PUBLIC cxx_std_20)
target_include_directories(HoroBackendServiceReference PUBLIC ${PROJECT_SOURCE_DIR}/tests/fixtures/extensions)
target_link_libraries(HoroBackendServiceReference PUBLIC HoroEngine::Extensions)

add_executable(HoroBackendServiceRegistryTests
        unit/extensions/BackendServiceRegistryTests.cpp
)
target_compile_features(HoroBackendServiceRegistryTests PRIVATE cxx_std_20)
target_link_libraries(HoroBackendServiceRegistryTests PRIVATE HoroBackendServiceReference Catch2::Catch2WithMain)
add_test(NAME HoroBackendServiceRegistryTests COMMAND HoroBackendServiceRegistryTests)

add_executable(HoroPlatformProviderAdmissionTests
        fixtures/extensions/PlatformProviderOperationsFixture.c
        unit/platform_services/PlatformProviderAdmissionTests.cpp
        unit/platform_services/PlatformProviderLifecycleTests.cpp)
target_compile_features(HoroPlatformProviderAdmissionTests PRIVATE cxx_std_20)
target_include_directories(HoroPlatformProviderAdmissionTests PRIVATE ${PROJECT_SOURCE_DIR}/tests/support)
target_link_libraries(HoroPlatformProviderAdmissionTests PRIVATE HoroEngine::PlatformServicesExtension Catch2::Catch2WithMain Threads::Threads)
add_dependencies(HoroPlatformProviderAdmissionTests HoroExtensionAbiFixture10 HoroExtensionAbiFixture11)
target_compile_definitions(HoroPlatformProviderAdmissionTests PRIVATE
        HORO_PLATFORM_PROVIDER_FIXTURE="$<TARGET_FILE:HoroExtensionAbiFixture10>"
        HORO_PLATFORM_PROVIDER_OPERATIONS_FIXTURE="$<TARGET_FILE:HoroExtensionAbiFixture11>")
add_test(NAME HoroPlatformProviderAdmissionTests COMMAND HoroPlatformProviderAdmissionTests)

add_executable(HoroBackendOperationRegistryTests
        unit/extensions/BackendOperationRegistryTests.cpp
)
target_compile_features(HoroBackendOperationRegistryTests PRIVATE cxx_std_20)
target_link_libraries(HoroBackendOperationRegistryTests PRIVATE HoroEngine::Extensions Catch2::Catch2WithMain)
add_test(NAME HoroBackendOperationRegistryTests COMMAND HoroBackendOperationRegistryTests)

add_executable(HoroScriptInvocationTests
        unit/extensions/ScriptValueTests.cpp
        unit/extensions/ScriptInvocationTests.cpp
        unit/extensions/ScriptCapabilityContextTests.cpp
)
target_compile_features(HoroScriptInvocationTests PRIVATE cxx_std_20)
target_link_libraries(HoroScriptInvocationTests PRIVATE HoroEngine::Extensions Catch2::Catch2WithMain)
add_test(NAME HoroScriptInvocationTests COMMAND HoroScriptInvocationTests)

add_test(
        NAME HoroBackendServiceRegistryUnresponsiveRetirementTests
        COMMAND HoroBackendServiceRegistryTests
                "Backend service retains an unresponsive provider and requires restart after its drain deadline")
add_test(
        NAME HoroBackendServiceRegistryConcurrentRetirementTests
        COMMAND HoroBackendServiceRegistryTests
                "Backend service finalization is not complete before Shutdown returns")

add_executable(HoroToolchainProviderRegistryTests
        unit/extensions/ToolchainProviderRegistryTests.cpp
)
target_compile_features(HoroToolchainProviderRegistryTests PRIVATE cxx_std_20)
target_link_libraries(HoroToolchainProviderRegistryTests PRIVATE HoroEngine::Extensions Catch2::Catch2WithMain)
add_test(NAME HoroToolchainProviderRegistryTests COMMAND HoroToolchainProviderRegistryTests)

add_executable(HoroSecurityBaselineTests unit/security/SecurityBaselineTests.cpp)
target_compile_features(HoroSecurityBaselineTests PRIVATE cxx_std_20)
target_link_libraries(HoroSecurityBaselineTests PRIVATE
        HoroEngine::Security HoroEngine::Platform Catch2::Catch2WithMain MbedTLS::mbedcrypto)
add_test(NAME HoroSecurityBaselineTests COMMAND HoroSecurityBaselineTests)

add_executable(HoroAgentPolicyTests unit/security/AgentPolicyTests.cpp)
target_compile_features(HoroAgentPolicyTests PRIVATE cxx_std_20)
target_link_libraries(HoroAgentPolicyTests PRIVATE HoroEngine::Security Catch2::Catch2WithMain)
horo_register_catch_test(HoroAgentPolicyTests LABELS "unit;security;headless")

if (HORO_BUILD_RENDER_OPENGL)
    add_executable(HoroRenderOpenGLTests
            unit/runtime/renderer/OpenGLRenderBackendTests.cpp
            unit/runtime/renderer/OpenGLRenderBackendQualificationTests.cpp
            unit/runtime/renderer/OpenGLRenderResourceTests.cpp
    )

    target_compile_features(HoroRenderOpenGLTests PRIVATE cxx_std_20)
    target_include_directories(HoroRenderOpenGLTests PRIVATE
            ${PROJECT_SOURCE_DIR}/src/runtime/renderer/modules/opengl
            ${PROJECT_SOURCE_DIR}/tests/support
    )
    target_link_libraries(HoroRenderOpenGLTests
            PRIVATE
            HoroEngine::RenderFrontend
            HoroEngine::RenderOpenGL
    )
    if (HORO_BUILD_EDITOR_GUI)
        add_executable(HoroEditorViewportOpenGLSmoke
                integration/renderer/EditorViewportOpenGLSmoke.cpp
        )

        target_compile_features(HoroEditorViewportOpenGLSmoke PRIVATE cxx_std_20)
        target_compile_definitions(HoroEditorViewportOpenGLSmoke PRIVATE GL_SILENCE_DEPRECATION)
        target_include_directories(HoroEditorViewportOpenGLSmoke PRIVATE ${PROJECT_SOURCE_DIR}/src)
        target_link_libraries(HoroEditorViewportOpenGLSmoke
                PRIVATE
                Catch2::Catch2WithMain
                HoroEngine::EditorViewportOpenGL
                HoroEngine::RenderFrontend
                HoroThirdParty::Glad
        )

        if (HORO_ENABLE_GPU_SMOKE_TESTS)
            if (TARGET HoroEditor)
                set(HORO_OPENGL_FIRST_FRAME_HOME "${CMAKE_CURRENT_BINARY_DIR}/first-frame-home/opengl")
                file(MAKE_DIRECTORY "${HORO_OPENGL_FIRST_FRAME_HOME}")
                add_test(NAME HoroEditorOpenGLFirstFrame
                        COMMAND $<TARGET_FILE:HoroEditor> --renderer opengl --exit-after-first-frame)
                set_tests_properties(HoroEditorOpenGLFirstFrame PROPERTIES
                        LABELS "gpu;display;editor"
                        ENVIRONMENT "HOME=${HORO_OPENGL_FIRST_FRAME_HOME};USERPROFILE=${HORO_OPENGL_FIRST_FRAME_HOME}"
                )
            endif ()
        endif ()
    endif ()
endif ()

if (NOT HORO_BUILD_RENDER_METAL)
    add_executable(HoroMetalDeviceCapabilityTests
            unit/runtime/renderer/MetalDeviceCapabilityTests.cpp
            unit/runtime/renderer/MetalRenderBackendTests.cpp
            unit/runtime/renderer/MetalRenderBackendQualificationTests.cpp
            unit/runtime/renderer/MetalResourcePolicyTests.cpp
            ${PROJECT_SOURCE_DIR}/src/runtime/renderer/modules/metal/MetalDeviceCapabilities.cpp
            ${PROJECT_SOURCE_DIR}/src/runtime/renderer/modules/metal/MetalRenderBackend.cpp
            ${PROJECT_SOURCE_DIR}/src/runtime/renderer/modules/metal/MetalRenderBackendErrors.cpp
            ${PROJECT_SOURCE_DIR}/src/runtime/renderer/modules/metal/MetalResourcePolicy.cpp
    )
    target_compile_features(HoroMetalDeviceCapabilityTests PRIVATE cxx_std_20)
    target_include_directories(HoroMetalDeviceCapabilityTests PRIVATE
            ${PROJECT_SOURCE_DIR}/src
            ${PROJECT_SOURCE_DIR}/tests/support
    )
    target_link_libraries(HoroMetalDeviceCapabilityTests PRIVATE
            Catch2::Catch2WithMain
            HoroEngine::RenderFrontend
    )
    horo_register_catch_test(HoroMetalDeviceCapabilityTests LABELS "native;renderer;metal;capabilities")
endif ()

if (HORO_BUILD_RENDER_METAL)
    add_executable(HoroRenderMetalTests
            unit/runtime/renderer/MetalDeviceCapabilityTests.cpp
            unit/runtime/renderer/MetalRenderBackendTests.cpp
            unit/runtime/renderer/MetalRenderBackendQualificationTests.cpp
            unit/runtime/renderer/MetalResourceFormatsTests.mm
            unit/runtime/renderer/MetalResourcePolicyTests.cpp
    )

    target_compile_features(HoroRenderMetalTests PRIVATE cxx_std_20)
    target_include_directories(HoroRenderMetalTests PRIVATE
            ${PROJECT_SOURCE_DIR}/src
            ${PROJECT_SOURCE_DIR}/tests/support
    )
    target_link_libraries(HoroRenderMetalTests
            PRIVATE
            HoroEngine::RenderFrontend
            HoroEngine::RenderMetal
    )
    if (HORO_BUILD_EDITOR_GUI)
        add_executable(HoroEditorViewportMetalSmoke
                integration/renderer/EditorViewportMetalSmoke.mm
        )

        target_compile_features(HoroEditorViewportMetalSmoke PRIVATE cxx_std_20)
        target_include_directories(HoroEditorViewportMetalSmoke PRIVATE ${PROJECT_SOURCE_DIR}/src)
        target_link_libraries(HoroEditorViewportMetalSmoke
                PRIVATE
                Catch2::Catch2WithMain
                HoroEngine::EditorViewportMetal
                HoroEngine::RenderFrontend
                HoroEngine::RenderMetal
                ${HORO_SDL3_TARGET}
                "-framework Metal"
                "-framework QuartzCore"
        )

        add_executable(HoroMetalUploadShutdownSmoke
                integration/renderer/MetalUploadShutdownSmoke.mm
        )
        target_compile_features(HoroMetalUploadShutdownSmoke PRIVATE cxx_std_20)
        target_include_directories(HoroMetalUploadShutdownSmoke PRIVATE ${PROJECT_SOURCE_DIR}/src)
        target_link_libraries(HoroMetalUploadShutdownSmoke
                PRIVATE
                Catch2::Catch2WithMain
                HoroEngine::EditorViewportMetal
                HoroEngine::RenderFrontend
                HoroEngine::RenderMetal
                ${HORO_SDL3_TARGET}
                "-framework Metal"
                "-framework QuartzCore"
        )

        if (HORO_ENABLE_GPU_SMOKE_TESTS)
            if (TARGET HoroEditor)
                set(HORO_METAL_FIRST_FRAME_HOME "${CMAKE_CURRENT_BINARY_DIR}/first-frame-home/metal")
                file(MAKE_DIRECTORY "${HORO_METAL_FIRST_FRAME_HOME}")
                add_test(NAME HoroEditorMetalFirstFrame
                        COMMAND $<TARGET_FILE:HoroEditor> --renderer metal --exit-after-first-frame)
                set_tests_properties(HoroEditorMetalFirstFrame PROPERTIES
                        LABELS "gpu;display;metal;editor"
                        ENVIRONMENT "HOME=${HORO_METAL_FIRST_FRAME_HOME};USERPROFILE=${HORO_METAL_FIRST_FRAME_HOME}"
                        FAIL_REGULAR_EXPRESSION "Runtime frame failed"
                )
            endif ()
        endif ()
    endif ()
endif ()

if (HORO_BUILD_RENDER_VULKAN)
    add_executable(HoroRenderVulkanTests
            unit/runtime/renderer/VulkanRenderBackendTests.cpp
            unit/runtime/renderer/VulkanResourceRuntimeTests.cpp
    )
    target_compile_features(HoroRenderVulkanTests PRIVATE cxx_std_20)
    target_include_directories(HoroRenderVulkanTests PRIVATE
            ${PROJECT_SOURCE_DIR}/src
            ${PROJECT_SOURCE_DIR}/tests/support
    )
    target_link_libraries(HoroRenderVulkanTests PRIVATE
            Catch2::Catch2WithMain
            HoroEngine::RenderFrontend
            HoroEngine::RenderVulkan
            Vulkan::Headers
    )
    horo_register_catch_test(HoroRenderVulkanTests LABELS "native;renderer;vulkan;qualification")
endif ()

set(HORO_CATCH_TEST_TARGETS
        HoroInputTests
        HoroGameplayRuntimeTests
        HoroGameModuleHostTests
        HoroLuaBehaviorTests
        HoroEditorPlaySessionControllerTests
        HoroUpdateExperienceSessionTests
        HoroProjectGameplayRegistryTests
        HoroProjectIntegrityValidatorServiceTests
        HoroSceneMathTests
        HoroFoundationStringTests
        HoroFoundationSha256Tests
        HoroStructuredLogStoreTests
        HoroObservabilityTests
        HoroHostObservabilityTests
        HoroOpenTelemetryTests
        HoroActivityStoreTests
        HoroPrimitiveMeshTests
        HoroEditorWelcomeControllerTests
        HoroEditorModalHostTests
        HoroGuiScreenHostLifecycleTests
        HoroSettingsModalLifecycleTests
        HoroEditorPresentationTests
        HoroAssetImportModalLifecycleTests
        HoroEditorLocalizationServiceTests
        HoroProjectCreationControllerTests
        HoroRendererAvailabilityTests
        HoroProjectMetadataTests
        HoroProjectCompatibilityTests
        HoroReleaseVersionTests
        HoroDistributionModelTests
        HoroReleaseProfileTests
        HoroReleasePreflightTests
        HoroReleasePipelineExecutorTests
        HoroReleaseServiceTests
        HoroReleaseTargetMatrixTests
        HoroReleaseJobTrackerTests
        HoroReleaseProcessTests
        HoroProjectMigrationTests
        HoroProjectMigrationTransactionTests
        HoroProjectOpenServiceTests
        HoroRecentProjectInspectionServiceTests
        HoroProjectCreationServiceTests
        HoroEditorSettingsStoreTests
        HoroWorkspaceLayoutTests
        HoroWorkspacePanelHostTests
        HoroEditorSurfaceIdentityTests
        HoroSourceFileOpenServiceTests
        HoroUiCanvasDocumentTests
        HoroWorkspaceLayoutPersistenceTests
        HoroActivityBarLayoutTests
        HoroWorkspaceSplitterInteractionTests
        HoroEditorWorkspaceControllerTests
        HoroGameplayBehaviorFilenameModalStateTests
        HoroBuildReleaseModalSessionTests
        HoroNotificationServiceTests
        HoroEditorMenuModelTests
        HoroEditorStatusBarModelTests
        HoroHierarchyModelTests
        HoroEditorSelectionModelTests
        HoroEditorViewportModelTests
        HoroHierarchyPanelRenderTests
        HoroHierarchyEditSessionTests
        HoroInspectorEditSessionTests
        HoroInspectorPanelRenderTests
        HoroGlobalDockPanelRenderTests
        HoroAssetBrowserInteractionSessionTests
        HoroEditorUiComponentsRenderTests
        HoroExtensionActivityViewTests
        HoroViewportPanelRenderTests
        HoroDataBusTests
        HoroFoundationResultTests
        HoroFoundationErrorCodeRegistryTests
        HoroFoundationModuleDescriptorTests
        HoroFoundationModuleHostCompositionTests
        HoroHostModuleCompositionTests
        HoroFoundationPathsTimeHandlesTests
        HoroFoundationCancellationProgressTests
        HoroFoundationConfigurationTests
        HoroFoundationDiagnosticsTests
        HoroFoundationValidationResultTests
        HoroFoundationJobSystemTests
        HoroPlatformTests
        HoroAndroidLifecycleTests
        HoroPlatformRequestTests
        HoroGameplayBuildServiceTests
        HoroEditorViewportSceneTests
        HoroSceneDocumentTests
        HoroSceneDocumentPersistenceTests
        HoroEditorViewportPickingTests
        HoroAssetSceneDropTests
        HoroRenderBackendRegistryTests
        HoroRenderAdapterTests
        HoroRenderDisplayTests
        HoroPresentModeTests
        HoroRenderSurfaceLifecycleTests
        HoroRenderGraphTests
        HoroRenderGraphLifetimeTests
        HoroTemporalHistoryTests
        HoroMotionHistoryTests
        HoroRenderResourceDescriptorTests
        HoroPipelineCacheTests
        HoroShaderManifestTests
        HoroShaderPermutationTests
        HoroShaderReflectionTests
        HoroStandardPbrMaterialTests
        HoroStandardPbrPassPlanTests
        HoroShaderCompilerPipelineTests
        HoroRenderSubmissionTests
        HoroRenderFrontendTests
        HoroRenderMemoryBudgetTests
        HoroEditorViewportResourceTests
        HoroRuntimeLifecycleTests
        HoroRuntimeSaveIdentityTests
        HoroRuntimeSaveDiagnosticsTests
        HoroRuntimeSaveTelemetryTests
        HoroRuntimeSaveRootResolverTests
        HoroRuntimeSaveFilesystemLockTests
        HoroRuntimeSaveNamespaceTests
        HoroRuntimeSaveParticipantRegistryTests
        HoroRuntimeSaveCaptureSnapshotTests
        HoroRuntimeSaveRestoreTransactionTests
        HoroRuntimeUiIdentityTests
        HoroRuntimeUiImageResourceTests
        HoroRuntimeUiBindingTests
        HoroRuntimeUiBindingStoreTests
        HoroRuntimeUiBindingWriteTests
        HoroRuntimeUiDocumentTests
        HoroRuntimeUiHotReloadTests
        HoroRuntimeUiLocalizationTests
        HoroRuntimeUiCanvasSpaceTests
        HoroRuntimeUiDiagnosticsTests
        HoroRuntimeUiAccessibilityTests
        HoroRuntimeUiElementTreeTests
        HoroRuntimeUiRenderSnapshotTests
        HoroRuntimeUiRenderGeometryTests
        HoroRuntimeUiLayoutTests
        HoroRuntimeUiLayoutClippingTests
        HoroRuntimeUiTextLayoutTests
        HoroRuntimeUiGlyphAtlasTests
        HoroRuntimeUiStyleTests
        HoroRuntimeUiHitTestingTests
        HoroRuntimeUiEventDispatchTests
        HoroRuntimeUiPointerCaptureTests
        HoroRuntimeUiActionsTests
        HoroRuntimeUiFeedbackTests
        HoroRuntimeUiScreenStackTests
        HoroRuntimeUiFocusGraphTests
        HoroRuntimeUiControlsTests
        HoroRuntimeUiTextShapingTests
        HoroRuntimeSaveArchiveMetadataTests
        HoroRuntimeSaveArchiveFramingTests
        HoroRuntimeSaveArchiveFinalizationTests
        HoroRuntimeSaveArchiveReaderTests
        HoroRuntimeSaveArchiveProtectionTests
        HoroRuntimeSaveMigrationTests
        HoroRuntimeSaveCanonicalCodecTests
        HoroRuntimeSaveReferenceTests
        HoroRuntimeSaveSlotMetadataTests
        HoroRuntimeSaveSlotIndexTests
        HoroRuntimeSaveCloudRevisionMetadataTests
        HoroRuntimeSaveManagerProjectionTests
        HoroRuntimeSaveOperationTests
        HoroRuntimeSaveParticipationTests
        HoroRuntimePersistentEntityIdentityTests
        HoroRuntimeSaveableComponentStateTests
        HoroRuntimeRestoreReferenceTests
        HoroRuntimeSaveStorageAdapterTests
        HoroRuntimeSaveStoragePolicyTests
        HoroRuntimeSaveOperationArbiterTests
        HoroRuntimeSaveAutosaveSchedulerTests
        HoroRuntimeSaveSlotCommitTransactionTests
        HoroRuntimeSaveSlotRecoveryTests
        HoroRuntimeSaveSafePointCoordinatorTests
        HoroRuntimeSaveProjectPolicyTests
        HoroRuntimeSaveTestCompositionsTests
        HoroRuntimeSceneTests
        HoroRuntimeSavedSceneBootstrapTests
        HoroAssetRegistryTests
        HoroAssetProviderTests
        HoroAssetCookTests
        HoroCookCatalogTests
        HoroHeadlessNullCookerTests
        HoroAssetCookCacheTests
        HoroAssetCookOutputTests
        HoroAssetCookServiceTests
        HoroAssetCookIntegrationTests
        HoroAssetImporterCatalogTests
        HoroAssetImportOperationTests
        HoroModularAssetImportIntegrationTests
        HoroObjMeshImporterTests
        HoroFbxMeshImporterTests
        HoroRenderOpenGLTests
        HoroRenderMetalTests
)

foreach (target IN LISTS HORO_CATCH_TEST_TARGETS)
    if (TARGET ${target})
        horo_register_catch_test(${target} LABELS "native")
    endif ()
endforeach ()

if (HORO_ENABLE_GPU_SMOKE_TESTS)
    if (TARGET HoroEditorViewportOpenGLSmoke)
        horo_register_catch_test(HoroEditorViewportOpenGLSmoke LABELS "gpu;display;opengl")
    endif ()
    if (TARGET HoroEditorViewportMetalSmoke)
        horo_register_catch_test(HoroEditorViewportMetalSmoke
                LABELS "gpu;display;metal"
                PROPERTIES ENVIRONMENT "MTL_DEBUG_LAYER=1"
        )
    endif ()
    if (TARGET HoroMetalUploadShutdownSmoke)
        horo_register_catch_test(HoroMetalUploadShutdownSmoke
                LABELS "gpu;display;metal"
                PROPERTIES ENVIRONMENT "MTL_DEBUG_LAYER=1"
        )
    endif ()
endif ()

add_executable(HoroFeatureBudgetReservationsPublicConsumer support/FeatureBudgetReservationsPublicConsumer.cpp)
target_compile_features(HoroFeatureBudgetReservationsPublicConsumer PRIVATE cxx_std_20)
target_link_libraries(HoroFeatureBudgetReservationsPublicConsumer PRIVATE HoroEngine::WorldStreaming)
add_test(NAME HoroFeatureBudgetReservationsPublicConsumer COMMAND HoroFeatureBudgetReservationsPublicConsumer)
set_tests_properties(HoroFeatureBudgetReservationsPublicConsumer PROPERTIES LABELS "unit;world_streaming;headless")

add_executable(HoroScriptCapabilityPublicHeaderConsumer support/ScriptCapabilityPublicHeaderConsumer.cpp)
target_compile_features(HoroScriptCapabilityPublicHeaderConsumer PRIVATE cxx_std_20)
target_link_libraries(HoroScriptCapabilityPublicHeaderConsumer PRIVATE HoroEngine::Extensions)
add_test(NAME HoroScriptCapabilityPublicHeaderConsumer COMMAND HoroScriptCapabilityPublicHeaderConsumer)
set_tests_properties(HoroScriptCapabilityPublicHeaderConsumer PROPERTIES LABELS "unit;extensions;headless")



add_executable(HoroWorldPackageChunkPublicConsumer support/WorldPackageChunkPublicConsumer.cpp)
target_compile_features(HoroWorldPackageChunkPublicConsumer PRIVATE cxx_std_20)
target_link_libraries(HoroWorldPackageChunkPublicConsumer PRIVATE HoroEngine::WorldStreaming)
add_test(NAME HoroWorldPackageChunkPublicConsumer COMMAND HoroWorldPackageChunkPublicConsumer)
set_tests_properties(HoroWorldPackageChunkPublicConsumer PROPERTIES LABELS "unit;world_streaming;headless")

add_executable(HoroCliOutputPublicHeaderConsumer support/CliOutputPublicHeaderConsumer.cpp)
target_compile_features(HoroCliOutputPublicHeaderConsumer PRIVATE cxx_std_20)
target_link_libraries(HoroCliOutputPublicHeaderConsumer PRIVATE HoroEngine::CliHost)
add_test(NAME HoroCliOutputPublicHeaderConsumer COMMAND HoroCliOutputPublicHeaderConsumer)
set_tests_properties(HoroCliOutputPublicHeaderConsumer PROPERTIES LABELS "cli;headless;sonar-coverage;ci-windows")
add_executable(HoroCliProductionOutputContract integration/CliProductionOutputContract.cpp)
target_compile_features(HoroCliProductionOutputContract PRIVATE cxx_std_20)
target_link_libraries(HoroCliProductionOutputContract PRIVATE HoroEngine::Platform nlohmann_json::nlohmann_json)
add_dependencies(HoroCliProductionOutputContract horo-engine)
add_test(NAME HoroCliProductionOutputContract COMMAND HoroCliProductionOutputContract "$<TARGET_FILE:horo-engine>")
set_tests_properties(HoroCliProductionOutputContract PROPERTIES LABELS "cli;integration;headless;sonar-coverage;ci-windows" TIMEOUT 120)

add_executable(HoroMixerDocumentTests unit/editor/MixerAssetDocumentTests.cpp)
target_link_libraries(HoroMixerDocumentTests PRIVATE HoroEngine::EditorServices)
horo_register_catch_test(HoroMixerDocumentTests LABELS "unit;editor;audio;headless")

horo_finalize_ci_suites()
