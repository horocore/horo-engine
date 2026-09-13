include(Catch)

add_test(NAME HoroDependencyDirectionNegativeConfigureTest
    COMMAND "${CMAKE_COMMAND}"
        "-DHORO_ENGINE_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
        "-DHORO_TEST_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/dependency-direction-negative"
        -P "${PROJECT_SOURCE_DIR}/tests/cmake/RunDependencyDirectionNegativeTest.cmake")
set_tests_properties(HoroDependencyDirectionNegativeConfigureTest PROPERTIES
    LABELS "architecture;cmake")

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

# One translation unit per registered public header verifies that consumers can
# compile the contract using only its owning target's usage requirements.
horo_add_public_header_consumer_targets()

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
    cmake_parse_arguments(ARG "" "" "LABELS;EXTRA_ARGS;PROPERTIES" ${ARGN})
    target_compile_features(${target} PRIVATE cxx_std_20)
    target_compile_definitions(${target} PRIVATE HORO_PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    target_link_libraries(${target} PRIVATE Catch2::Catch2WithMain)
    catch_discover_tests(${target}
            TEST_PREFIX "${target}::"
            EXTRA_ARGS ${ARG_EXTRA_ARGS}
            PROPERTIES LABELS "${ARG_LABELS}" ${ARG_PROPERTIES}
    )
endfunction()

add_executable(HoroPackageValidationTests
    unit/packages/PackageDependencyResolverTests.cpp
    unit/packages/PackageLockfileTests.cpp
    unit/packages/PackagePathTests.cpp
    unit/packages/PackageArchiveTests.cpp
    unit/packages/PackageCacheTests.cpp
    unit/packages/PackageFileManifestTests.cpp)
target_link_libraries(HoroPackageValidationTests PRIVATE HoroEngine::Packages HoroEngine::Platform nlohmann_json::nlohmann_json miniz)
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
        unit/runtime/input/InputTests.cpp
)
target_compile_features(HoroInputTests PRIVATE cxx_std_20)
target_link_libraries(HoroInputTests PRIVATE HoroEngine::Input)
add_executable(HoroNetworkApiTests
        unit/runtime/networking/MessageEnvelopeTests.cpp
        unit/runtime/networking/NetworkAddressTests.cpp
        unit/runtime/networking/NetworkHandleTests.cpp
        unit/runtime/networking/NetworkFailureTests.cpp
        unit/runtime/networking/NetworkIoServiceTests.cpp
        unit/runtime/networking/NetworkLifecycleTests.cpp
        unit/runtime/networking/PacketQueueTests.cpp
        unit/runtime/networking/ProtocolIdentityTests.cpp
        unit/runtime/networking/ReplicationDescriptorTests.cpp
        unit/runtime/networking/ReplicationDescriptorRegistryTests.cpp
        unit/runtime/networking/ReplicationRolesTests.cpp
        unit/runtime/networking/TransportCapabilitiesTests.cpp
        unit/runtime/networking/TransportBudgetTests.cpp
)
target_compile_features(HoroNetworkApiTests PRIVATE cxx_std_20)
target_link_libraries(HoroNetworkApiTests PRIVATE HoroEngine::NetworkApi)
horo_register_catch_test(HoroNetworkApiTests LABELS "unit;network;headless")
add_executable(HoroNetworkTransportNullTests
        unit/runtime/networking/DeterministicTransportTests.cpp
)
target_compile_features(HoroNetworkTransportNullTests PRIVATE cxx_std_20)
target_link_libraries(HoroNetworkTransportNullTests PRIVATE HoroEngine::NetworkTransportNull)
horo_register_catch_test(HoroNetworkTransportNullTests LABELS "unit;network;headless")
add_executable(HoroNetworkRuntimeTests
        unit/runtime/networking/AuthenticationSessionAdapterTests.cpp
        unit/runtime/networking/HandshakeNegotiationTests.cpp
        unit/runtime/networking/NetworkObjectMappingTests.cpp
        unit/runtime/networking/PeerSessionLifecycleTests.cpp
)
target_compile_features(HoroNetworkRuntimeTests PRIVATE cxx_std_20)
target_link_libraries(HoroNetworkRuntimeTests PRIVATE HoroEngine::NetworkRuntime)
horo_register_catch_test(HoroNetworkRuntimeTests LABELS "unit;network;headless")
add_executable(HoroAudioApiTests
        unit/runtime/audio/AudioAssetSchemaTests.cpp
        unit/runtime/audio/AudioBackendCapabilitiesTests.cpp
        unit/runtime/audio/AudioCallbackEventsTests.cpp
        unit/runtime/audio/AudioDeviceDiscoveryTests.cpp
        unit/runtime/audio/AudioDeviceNegotiationTests.cpp
        unit/runtime/audio/AudioDeviceTimingTests.cpp
        unit/runtime/audio/AudioFormatTests.cpp
        unit/runtime/audio/AudioMediaFormatRegistryTests.cpp
        unit/runtime/audio/AudioIdentityTests.cpp
        unit/runtime/audio/AudioResamplerPlanTests.cpp
        unit/runtime/audio/AudioPlanarBlockTests.cpp
)
target_include_directories(HoroAudioApiTests PRIVATE ${PROJECT_SOURCE_DIR}/src/audio/api)
target_compile_features(HoroAudioApiTests PRIVATE cxx_std_20)
target_link_libraries(HoroAudioApiTests PRIVATE HoroEngine::AudioApi)
horo_register_catch_test(HoroAudioApiTests LABELS "unit;audio;headless")
add_executable(HoroAudioBackendContractTests
        unit/runtime/audio/AudioBackendContractTests.cpp
)
target_compile_features(HoroAudioBackendContractTests PRIVATE cxx_std_20)
target_link_libraries(HoroAudioBackendContractTests PRIVATE HoroAudioBackendContract)
horo_register_catch_test(HoroAudioBackendContractTests LABELS "unit;audio;headless")
add_executable(HoroAudioNullTests
        unit/runtime/audio/NullAudioBackendTests.cpp
)
target_compile_features(HoroAudioNullTests PRIVATE cxx_std_20)
target_link_libraries(HoroAudioNullTests PRIVATE HoroEngine::AudioNull)
horo_register_catch_test(HoroAudioNullTests LABELS "unit;audio;headless")
add_executable(HoroAudioDspTests
    unit/runtime/audio/ResamplerKernelTests.cpp
    unit/runtime/audio/AudioResamplerTests.cpp
)
target_include_directories(HoroAudioDspTests PRIVATE ${PROJECT_SOURCE_DIR}/src/audio/resampling)
target_compile_features(HoroAudioDspTests PRIVATE cxx_std_20)
target_link_libraries(HoroAudioDspTests PRIVATE HoroEngine::AudioDsp)
horo_register_catch_test(HoroAudioDspTests LABELS "unit;audio;headless")
add_executable(HoroAudioResamplerBenchmark EXCLUDE_FROM_ALL benchmarks/audio/ResamplerBenchmark.cpp)
target_compile_features(HoroAudioResamplerBenchmark PRIVATE cxx_std_20)
target_link_libraries(HoroAudioResamplerBenchmark PRIVATE HoroEngine::AudioDsp)

add_executable(HoroAudioMemoryTests unit/runtime/audio/AudioMemoryTests.cpp)
target_compile_features(HoroAudioMemoryTests PRIVATE cxx_std_20)
target_link_libraries(HoroAudioMemoryTests PRIVATE HoroEngine::AudioMemory)
horo_register_catch_test(HoroAudioMemoryTests LABELS "unit;audio;headless")
add_executable(HoroAudioCommandTests
    unit/runtime/audio/AudioClockTests.cpp
    unit/runtime/audio/AudioCommandTests.cpp
    unit/runtime/audio/AudioCommandBufferTests.cpp
    unit/runtime/audio/AudioCommandStagingTests.cpp
    unit/runtime/audio/AudioEventQueueTests.cpp
    unit/runtime/audio/AudioLifecycleReconcilerTests.cpp
    unit/runtime/audio/ScheduledAudioCommandBatchTests.cpp
)
target_compile_features(HoroAudioCommandTests PRIVATE cxx_std_20)
target_link_libraries(HoroAudioCommandTests PRIVATE HoroEngine::AudioCommands)
horo_register_catch_test(HoroAudioCommandTests LABELS "unit;audio;headless")

add_executable(HoroPhysicsTests
        unit/runtime/physics/CharacterControllerContractsTests.cpp
        unit/runtime/physics/CharacterWorldSettingsTests.cpp
        unit/runtime/physics/CharacterWorldTests.cpp
        unit/runtime/physics/CanonicalSolverTests.cpp
        unit/runtime/physics/PhysicsBodyDescriptorTests.cpp
        unit/runtime/physics/PhysicsBodyDynamicsTests.cpp
        unit/runtime/physics/PhysicsCollisionSchemaTests.cpp
        unit/runtime/physics/PhysicsConstraintDescriptorTests.cpp
        unit/runtime/physics/PhysicsCookedShapeDescriptorTests.cpp
        unit/runtime/physics/PhysicsDeterminismPolicyTests.cpp
        unit/runtime/physics/PhysicsDiagnosticsTests.cpp
        unit/runtime/physics/PhysicsFoundationQualificationTests.cpp
        unit/runtime/physics/PhysicsHandleRegistryTests.cpp
        unit/runtime/physics/PhysicsIdentityTests.cpp
        unit/runtime/physics/PhysicsMetricsTests.cpp
        unit/runtime/physics/PhysicsPoseTests.cpp
        unit/runtime/physics/PhysicsQueryTests.cpp
        unit/runtime/physics/PhysicsSceneActivationTests.cpp
        unit/runtime/physics/PhysicsShapeDescriptorTests.cpp
        unit/runtime/physics/PhysicsStepPolicyTests.cpp
        unit/runtime/physics/PhysicsWorldBudgetsTests.cpp
        unit/runtime/physics/PhysicsWorldDescriptorTests.cpp
        unit/runtime/physics/PhysicsWorldSettingsTests.cpp
        unit/runtime/physics/PhysicsWorldTests.cpp
)
target_compile_features(HoroPhysicsTests PRIVATE cxx_std_20)
target_include_directories(HoroPhysicsTests PRIVATE ${PROJECT_SOURCE_DIR}/src/physics)
target_compile_definitions(HoroPhysicsTests PRIVATE HORO_TEST_PHYSICS_NATIVE=$<BOOL:${HORO_BUILD_PHYSICS_NATIVE}>)
target_link_libraries(HoroPhysicsTests PRIVATE HoroEngine::Physics HoroEngine::PhysicsSceneIntegration HoroEngine::Runtime HoroEngine::Platform)
horo_register_catch_test(HoroPhysicsTests LABELS "unit;physics;headless")

add_executable(HoroAITests
        unit/runtime/ai/AIIdentityTests.cpp
        unit/runtime/ai/AITaskLifecycleTests.cpp
        unit/runtime/ai/BlackboardInstanceTests.cpp
        unit/runtime/ai/BlackboardSchemaTests.cpp
        unit/runtime/ai/NullAIRuntimeTests.cpp
        unit/runtime/ai/PerceptionDescriptorRegistryTests.cpp
)
target_compile_features(HoroAITests PRIVATE cxx_std_20)
target_link_libraries(HoroAITests PRIVATE HoroEngine::AI)
horo_register_catch_test(HoroAITests LABELS "unit;ai;headless")

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
        unit/runtime/pcg/PCGGraphAssetTests.cpp
        unit/runtime/pcg/PCGGraphValidationTests.cpp
        unit/runtime/pcg/PCGGenerationPlanTests.cpp
        unit/runtime/pcg/PCGIdentityTests.cpp
        unit/runtime/pcg/PCGPointSchemaTests.cpp
        unit/runtime/pcg/PCGSpatialSnapshotTests.cpp
        unit/runtime/pcg/PCGRegistryTests.cpp
)
target_compile_features(HoroPCGTests PRIVATE cxx_std_20)
target_link_libraries(HoroPCGTests PRIVATE HoroEngine::PCG)
horo_register_catch_test(HoroPCGTests LABELS "unit;pcg;headless")

add_executable(HoroVfxApiTests
        support/AllocationProbe.cpp
        unit/runtime/vfx/CpuParticleBufferTests.cpp
        unit/runtime/vfx/CpuParticleSpawnPipelineTests.cpp
        unit/runtime/vfx/ParticleSystemDescriptorTests.cpp
        unit/runtime/vfx/VfxIdentityTests.cpp
        unit/runtime/vfx/VfxQualityPolicyTests.cpp
)
target_compile_features(HoroVfxApiTests PRIVATE cxx_std_20)
target_include_directories(HoroVfxApiTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(HoroVfxApiTests PRIVATE HoroEngine::VfxApi)
horo_register_catch_test(HoroVfxApiTests LABELS "unit;vfx;headless")

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

add_executable(HoroCinematicModelTests
        support/AllocationProbe.cpp
        unit/runtime/cinematic/CurveSamplingTests.cpp
        unit/runtime/cinematic/CinematicIdentityTests.cpp
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
)
target_compile_features(HoroCinematicRuntimeTests PRIVATE cxx_std_20)
target_include_directories(HoroCinematicRuntimeTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(HoroCinematicRuntimeTests PRIVATE HoroEngine::CinematicRuntime)
horo_register_catch_test(HoroCinematicRuntimeTests LABELS "unit;cinematic;headless")
add_executable(HoroCinematicCurveSamplingBenchmark EXCLUDE_FROM_ALL
        benchmarks/cinematic/CurveSamplingBenchmark.cpp
)
target_compile_features(HoroCinematicCurveSamplingBenchmark PRIVATE cxx_std_20)
target_link_libraries(HoroCinematicCurveSamplingBenchmark PRIVATE HoroEngine::CinematicModel)

add_executable(HoroNavigationApiTests
        support/navigation/DeterministicNavigationProvider.cpp
        unit/runtime/navigation/NavMeshDataTests.cpp
        unit/runtime/navigation/NavigationAreaRegistryTests.cpp
        unit/runtime/navigation/NavigationAgentProfileTests.cpp
        unit/runtime/navigation/NavigationBakeInputTests.cpp
        unit/runtime/navigation/NavigationCapabilitiesTests.cpp
        unit/runtime/navigation/NavigationIdentityTests.cpp
        unit/runtime/navigation/NavigationOutcomesTests.cpp
        unit/runtime/navigation/NavigationProjectProfileTests.cpp
        unit/runtime/navigation/NavigationProviderTests.cpp
        unit/runtime/navigation/NavigationSourceGeometryTests.cpp
)
target_compile_features(HoroNavigationApiTests PRIVATE cxx_std_20)
target_include_directories(HoroNavigationApiTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/support)
target_link_libraries(HoroNavigationApiTests PRIVATE HoroEngine::NavigationApi HoroEngine::NavigationNull)
horo_register_catch_test(HoroNavigationApiTests LABELS "unit;navigation;headless")

add_executable(HoroNavigationRuntimeTests
        support/AllocationProbe.cpp
        unit/runtime/navigation/NavigationFoundationQualificationTests.cpp
        unit/runtime/navigation/NavigationRuntimeQueuesTests.cpp
        unit/runtime/navigation/NavigationWorldLifecycleTests.cpp
)
target_compile_features(HoroNavigationRuntimeTests PRIVATE cxx_std_20)
target_include_directories(HoroNavigationRuntimeTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/support)
target_link_libraries(HoroNavigationRuntimeTests PRIVATE HoroEngine::NavigationRuntime)
horo_register_catch_test(HoroNavigationRuntimeTests LABELS "unit;navigation;headless;lifecycle")

if(HORO_BUILD_NAVIGATION_RECAST_DETOUR)
    add_executable(HoroNavigationRecastDetourTests
            unit/runtime/navigation/RecastDetourProviderTests.cpp
    )
    target_compile_features(HoroNavigationRecastDetourTests PRIVATE cxx_std_20)
    target_include_directories(HoroNavigationRecastDetourTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/support)
    target_link_libraries(HoroNavigationRecastDetourTests PRIVATE HoroEngine::NavigationRecastDetour)
    horo_register_catch_test(HoroNavigationRecastDetourTests LABELS "unit;navigation;headless;provider")
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

add_executable(HoroTerrainApiTests
        unit/runtime/terrain/FoliageDefinitionTests.cpp
        unit/runtime/terrain/TerrainDescriptorTests.cpp
        unit/runtime/terrain/TerrainIdentityTests.cpp
)
target_compile_features(HoroTerrainApiTests PRIVATE cxx_std_20)
target_link_libraries(HoroTerrainApiTests PRIVATE HoroEngine::TerrainApi)
horo_register_catch_test(HoroTerrainApiTests LABELS "unit;terrain;headless")

add_executable(HoroWorldStreamingTests
        unit/runtime/world_streaming/NetworkStreamingAuthorityTests.cpp
        unit/runtime/world_streaming/OriginFrameTests.cpp
        unit/runtime/world_streaming/RuntimeEntityCellExitOperationTests.cpp
        unit/runtime/world_streaming/CookedWorldIndexManifestTests.cpp
        unit/runtime/world_streaming/FallbackStreamingProviderTests.cpp
        unit/runtime/world_streaming/StreamingBudgetModelTests.cpp
        unit/runtime/world_streaming/StreamingCellOperationTests.cpp
        unit/runtime/world_streaming/StreamingCellCandidateTests.cpp
        unit/runtime/world_streaming/StreamingCellAssetRequestTests.cpp
        unit/runtime/world_streaming/StreamingCellActivationTests.cpp
        unit/runtime/world_streaming/StreamingCellStateTests.cpp
        unit/runtime/world_streaming/StreamingCellStabilityTests.cpp
        unit/runtime/world_streaming/StreamingSchedulerAdmissionTests.cpp
        unit/runtime/world_streaming/WorldDependencyPlanTests.cpp
        unit/runtime/world_streaming/WorldAuthoringContractTests.cpp
        unit/runtime/world_streaming/StreamingDesiredStateTests.cpp
        unit/runtime/world_streaming/StreamingDesiredStateReductionTests.cpp
        unit/runtime/world_streaming/StreamingPriorityPolicyTests.cpp
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
        unit/runtime/world_streaming/WorldSpatialAssignmentTests.cpp
        unit/runtime/world_streaming/WorldSpatialObjectDescriptorTests.cpp
        unit/runtime/world_streaming/WorldSpanningObjectPlanTests.cpp
        unit/runtime/world_streaming/WorldStreamingIdentityTests.cpp
        unit/runtime/world_streaming/WorldStreamingRuntimeCompositionTests.cpp
        unit/runtime/world_streaming/WorldStreamingDiagnosticSnapshotTests.cpp
)
target_compile_features(HoroWorldStreamingTests PRIVATE cxx_std_20)
target_link_libraries(HoroWorldStreamingTests PRIVATE HoroEngine::WorldStreaming)
horo_register_catch_test(HoroWorldStreamingTests LABELS "unit;world_streaming;headless")

add_executable(HoroPrefabTests
        unit/runtime/prefab/PrefabDependencyGraphTests.cpp
        unit/runtime/prefab/PrefabDocumentTests.cpp
        unit/runtime/prefab/PrefabIdentityTests.cpp
        unit/runtime/prefab/PrefabLimitsTests.cpp
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
get_property(HORO_TEST_GAME_MODULE_REVISION_FILE TARGET HoroTestGameModule PROPERTY HORO_GAMEPLAY_DESCRIPTOR_REVISION_FILE)
add_executable(HoroGameModuleHostTests
        unit/gameplay/GameModuleHostTests.cpp
)
target_include_directories(HoroGameModuleHostTests PRIVATE "${PROJECT_SOURCE_DIR}/tests/helpers")
target_compile_features(HoroGameModuleHostTests PRIVATE cxx_std_20)
target_compile_definitions(HoroGameModuleHostTests PRIVATE
        HORO_TEST_GAME_MODULE_PATH="$<TARGET_FILE:HoroTestGameModule>"
        HORO_TEST_GAME_MODULE_REVISION_PATH="${HORO_TEST_GAME_MODULE_REVISION_FILE}"
)
target_link_libraries(HoroGameModuleHostTests PRIVATE HoroEngine::GameplayModuleHost)
add_dependencies(HoroGameModuleHostTests HoroTestGameModule)
add_executable(HoroLuaBehaviorTests
        unit/gameplay/LuaBehaviorTests.cpp
)
target_include_directories(HoroLuaBehaviorTests PRIVATE "${PROJECT_SOURCE_DIR}/tests/helpers")
target_compile_features(HoroLuaBehaviorTests PRIVATE cxx_std_20)
target_link_libraries(HoroLuaBehaviorTests PRIVATE HoroEngine::GameplayLua)
add_executable(HoroEditorPlaySessionControllerTests
        unit/editor/EditorPlaySessionControllerTests.cpp
)
target_compile_features(HoroEditorPlaySessionControllerTests PRIVATE cxx_std_20)
target_link_libraries(HoroEditorPlaySessionControllerTests PRIVATE HoroEngine::EditorServices)
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
add_executable(HoroEditorWelcomeControllerTests
        unit/editor/WelcomeControllerTests.cpp
)

target_compile_features(HoroEditorWelcomeControllerTests PRIVATE cxx_std_20)
target_link_libraries(HoroEditorWelcomeControllerTests
        PRIVATE
        HoroEngine::EditorServices
)
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
    target_link_libraries(HoroGuiScreenHostLifecycleTests PRIVATE HoroEngine::Gui)

    add_executable(HoroSettingsModalLifecycleTests
            unit/editor/SettingsModalLifecycleTests.cpp
    )
    target_compile_features(HoroSettingsModalLifecycleTests PRIVATE cxx_std_20)
    target_link_libraries(HoroSettingsModalLifecycleTests
            PRIVATE
            HoroEngine::Gui
            HoroEngine::EditorServices
    )
    add_executable(HoroEditorPresentationTests
            unit/editor/SettingsModalPresentationTests.cpp
            unit/editor/ProjectCreationViewPresentationTests.cpp
            unit/editor/AssetImportModalPresentationTests.cpp
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
)
target_compile_features(HoroProjectCompatibilityTests PRIVATE cxx_std_20)
target_link_libraries(HoroProjectCompatibilityTests PRIVATE HoroEngine::Application)
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
add_executable(HoroProjectMigrationTests
        unit/application/ProjectMigrationTests.cpp
)
target_compile_features(HoroProjectMigrationTests PRIVATE cxx_std_20)
target_link_libraries(HoroProjectMigrationTests PRIVATE HoroEngine::ProjectMigrations)
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
        HoroEngine::EditorServices HoroEngine::ProjectMigrations nlohmann_json::nlohmann_json)
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
add_executable(HoroWorkspaceLayoutPersistenceTests
        unit/editor/WorkspaceLayoutPersistenceTests.cpp
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
endif ()

add_executable(HoroGameplayBehaviorFilenameModalStateTests
        unit/editor/GameplayBehaviorFilenameModalStateTests.cpp
)
target_compile_features(HoroGameplayBehaviorFilenameModalStateTests PRIVATE cxx_std_20)
target_include_directories(HoroGameplayBehaviorFilenameModalStateTests PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(HoroGameplayBehaviorFilenameModalStateTests PRIVATE HoroEngine::EditorServices)

add_executable(HoroNotificationServiceTests
        unit/editor/NotificationServiceTests.cpp
)
target_compile_features(HoroNotificationServiceTests PRIVATE cxx_std_20)
target_link_libraries(HoroNotificationServiceTests PRIVATE HoroEngine::EditorServices)

add_executable(HoroEditorMenuModelTests
        unit/editor/EditorMenuModelTests.cpp
)

target_compile_features(HoroEditorMenuModelTests PRIVATE cxx_std_20)
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
    add_executable(HoroEditorUiComponentsRenderTests
            unit/editor/EditorUiComponentsRenderTests.cpp
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
add_executable(HoroCliCommandRegistryTests
        unit/cli/CliCommandRegistryTests.cpp
        unit/cli/CliDispatcherTests.cpp
        unit/cli/CliOptionParserTests.cpp
)
target_compile_features(HoroCliCommandRegistryTests PRIVATE cxx_std_20)
target_link_libraries(HoroCliCommandRegistryTests PRIVATE HoroEngine::CliHost)
horo_register_catch_test(HoroCliCommandRegistryTests LABELS "unit;cli;headless")

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
)

target_compile_features(HoroFoundationJobSystemTests PRIVATE cxx_std_20)
target_link_libraries(HoroFoundationJobSystemTests PRIVATE HoroEngine::Foundation)
add_executable(HoroPlatformTests
        unit/foundation/PlatformTests.cpp
)

target_compile_features(HoroPlatformTests PRIVATE cxx_std_20)
target_link_libraries(HoroPlatformTests PRIVATE HoroEngine::Platform)
add_executable(HoroPlatformRequestTests
        unit/platform_services/AchievementDefinitionRegistryTests.cpp
        unit/platform_services/PlatformDefinitionRegistriesTests.cpp
        unit/platform_services/PlatformProjectConfigurationTests.cpp
        unit/platform_services/PlatformRequestTests.cpp
        unit/platform_services/PlatformServicesBackendTests.cpp
        unit/platform_services/PlatformServicesFrontendTests.cpp
        unit/platform_services/PlatformStableIdRegistryTests.cpp
        unit/platform_services/PlatformUserSessionTests.cpp
)
target_compile_features(HoroPlatformRequestTests PRIVATE cxx_std_20)
target_link_libraries(HoroPlatformRequestTests PRIVATE HoroEngine::PlatformServices Threads::Threads)
add_executable(HoroGameplayBuildServiceTests
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
)
target_compile_features(HoroSceneDocumentTests PRIVATE cxx_std_20)
target_link_libraries(HoroSceneDocumentTests PRIVATE HoroEngine::EditorRenderExtraction)
add_executable(HoroSceneDocumentPersistenceTests
        unit/editor/SceneDocumentPersistenceTests.cpp
)
target_compile_features(HoroSceneDocumentPersistenceTests PRIVATE cxx_std_20)
target_link_libraries(HoroSceneDocumentPersistenceTests PRIVATE HoroEngine::EditorServices)
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
        HoroEditorPlaySessionControllerTests
        HoroProjectGameplayRegistryTests
        HoroGuiScreenHostLifecycleTests
        HoroProjectCreationControllerTests
        HoroRendererAvailabilityTests
        HoroProjectMetadataTests
        HoroProjectOpenServiceTests
        HoroProjectMigrationIntegrationTests
        HoroEditorWorkspaceControllerTests
        HoroGameplayBehaviorFilenameModalStateTests
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
add_executable(HoroRenderResourceDescriptorTests
        unit/runtime/renderer/RenderResourceDescriptorTests.cpp
)
target_compile_features(HoroRenderResourceDescriptorTests PRIVATE cxx_std_20)
target_link_libraries(HoroRenderResourceDescriptorTests PRIVATE HoroEngine::RenderApi)
add_executable(HoroShaderManifestTests
        unit/runtime/renderer/ShaderManifestTests.cpp
)
target_compile_features(HoroShaderManifestTests PRIVATE cxx_std_20)
target_link_libraries(HoroShaderManifestTests PRIVATE HoroEngine::RenderApi)
add_executable(HoroRenderFrontendTests
        unit/runtime/renderer/RenderFrontendTests.cpp
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
        unit/runtime/save/SaveCaptureSnapshotTests.cpp
        unit/runtime/save/SaveCaptureSnapshotEdgeTests.cpp
)
target_compile_features(HoroRuntimeSaveCaptureSnapshotTests PRIVATE cxx_std_20)
target_link_libraries(HoroRuntimeSaveCaptureSnapshotTests PRIVATE HoroEngine::Runtime)
add_executable(HoroRuntimeUiIdentityTests
        unit/runtime/ui/UiIdentityTests.cpp
)
target_compile_features(HoroRuntimeUiIdentityTests PRIVATE cxx_std_20)
target_link_libraries(HoroRuntimeUiIdentityTests PRIVATE HoroEngine::RuntimeUi)
add_executable(HoroRuntimeUiDocumentTests
        unit/runtime/ui/UiDocumentTests.cpp
)
target_compile_features(HoroRuntimeUiDocumentTests PRIVATE cxx_std_20)
target_link_libraries(HoroRuntimeUiDocumentTests PRIVATE HoroEngine::RuntimeUi)
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
add_executable(HoroRuntimeUiLayoutTests
        unit/runtime/ui/UiLayoutTests.cpp
)
target_compile_features(HoroRuntimeUiLayoutTests PRIVATE cxx_std_20)
target_link_libraries(HoroRuntimeUiLayoutTests PRIVATE HoroEngine::RuntimeUi)
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
add_executable(HoroRuntimeSaveCanonicalCodecTests
        unit/runtime/save/SaveCanonicalCodecTests.cpp
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
add_executable(HoroRuntimeSaveOperationTests
        support/AllocationProbe.cpp
        unit/runtime/save/SaveOperationTests.cpp
)
target_compile_features(HoroRuntimeSaveOperationTests PRIVATE cxx_std_20)
target_include_directories(HoroRuntimeSaveOperationTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(HoroRuntimeSaveOperationTests PRIVATE HoroEngine::Runtime)
add_executable(HoroRuntimeSaveTestCompositionsTests
        unit/runtime/save/SaveTestCompositionsTests.cpp
)
target_compile_features(HoroRuntimeSaveTestCompositionsTests PRIVATE cxx_std_20)
target_link_libraries(HoroRuntimeSaveTestCompositionsTests PRIVATE HoroEngine::Runtime)
add_executable(HoroRuntimeSceneTests
        unit/runtime/scene/NavigationSceneComponentsTests.cpp
        unit/runtime/scene/RuntimeSceneTests.cpp
)
target_compile_features(HoroRuntimeSceneTests PRIVATE cxx_std_20)
target_link_libraries(HoroRuntimeSceneTests PRIVATE HoroEngine::RuntimeScene)
add_executable(HoroRuntimeSavedSceneBootstrapTests
        unit/runtime/scene/SavedSceneBootstrapTests.cpp
)
target_compile_features(HoroRuntimeSavedSceneBootstrapTests PRIVATE cxx_std_20)
target_link_libraries(HoroRuntimeSavedSceneBootstrapTests PRIVATE HoroEngine::RuntimeScene)
add_executable(HoroAssetRegistryTests
        unit/runtime/assets/AssetRegistryTests.cpp
)
target_compile_features(HoroAssetRegistryTests PRIVATE cxx_std_20)
target_link_libraries(HoroAssetRegistryTests PRIVATE HoroEngine::Assets)
add_executable(HoroAssetProviderTests
        unit/runtime/assets/AssetProviderTests.cpp
)
target_compile_features(HoroAssetProviderTests PRIVATE cxx_std_20)
target_include_directories(HoroAssetProviderTests PRIVATE
        ${PROJECT_SOURCE_DIR}/src/runtime/assets/runtime_provider
)
target_link_libraries(HoroAssetProviderTests PRIVATE HoroEngine::Assets)
add_executable(HoroAssetCookTests
        unit/runtime/assets/AssetCookContractTests.cpp
)
target_compile_features(HoroAssetCookTests PRIVATE cxx_std_20)
target_link_libraries(HoroAssetCookTests PRIVATE HoroEngine::Assets)
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
)
target_compile_features(HoroAssetCookOutputTests PRIVATE cxx_std_20)
target_link_libraries(HoroAssetCookOutputTests PRIVATE HoroEngine::Assets)
add_executable(HoroAssetCookServiceTests
        unit/runtime/assets/AssetCookServiceTests.cpp
)
target_compile_features(HoroAssetCookServiceTests PRIVATE cxx_std_20)
target_link_libraries(HoroAssetCookServiceTests PRIVATE HoroEngine::Assets HoroEngine::Foundation)
target_include_directories(HoroAssetCookServiceTests PRIVATE
        ${PROJECT_SOURCE_DIR}/src/runtime/assets/cooker/builtin
)
add_executable(HoroAssetCookIntegrationTests
        integration/assets/AssetCookIntegrationTests.cpp
)
target_compile_features(HoroAssetCookIntegrationTests PRIVATE cxx_std_20)
target_compile_definitions(HoroAssetCookIntegrationTests PRIVATE PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
target_link_libraries(HoroAssetCookIntegrationTests PRIVATE HoroEngine::Assets HoroEngine::Foundation)
target_include_directories(HoroAssetCookIntegrationTests PRIVATE
        ${PROJECT_SOURCE_DIR}/src/runtime/assets/cooker/builtin
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
target_link_libraries(HoroModularAssetImportIntegrationTests PRIVATE HoroEngine::Assets HoroEngine::Foundation)
target_include_directories(HoroModularAssetImportIntegrationTests PRIVATE
        ${PROJECT_SOURCE_DIR}/src/runtime/assets/cooker/builtin
        ${PROJECT_SOURCE_DIR}/src/runtime/assets/importer
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
target_compile_features(HoroExtensionAbiCConsumer PRIVATE c_std_11)
set_target_properties(HoroExtensionAbiCConsumer PROPERTIES C_EXTENSIONS OFF)
target_link_libraries(HoroExtensionAbiCConsumer PRIVATE HoroEngine::Extensions)
add_test(NAME HoroExtensionAbiCConsumer COMMAND HoroExtensionAbiCConsumer)

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
        "-DSCHEMA=${HORO_EXTENSION_SDK_PACKAGE_DIR}/share/horo/extension-sdk/extension-manifest-v1.schema.json"
        -P "${PROJECT_SOURCE_DIR}/tests/cmake/VerifyExtensionValidatorPackage.cmake")
set_tests_properties(HoroExtensionManifestValidatorIsPackaged PROPERTIES
    LABELS "extensions;sdk;cli")

add_executable(HoroExtensionManagerTests
        unit/extensions/ApplicationCapabilityRegistryTests.cpp
        unit/extensions/ExtensionActivationTransactionTests.cpp
        unit/extensions/ExtensionCapabilityAdmissionTests.cpp
        unit/extensions/ExtensionActivationStateTests.cpp
        unit/extensions/ExtensionManagerTests.cpp
        unit/extensions/ExtensionManifestParserTests.cpp
        unit/extensions/ExtensionModuleResolutionTests.cpp
        unit/extensions/ExtensionAbiValidationTests.cpp
        unit/extensions/ExtensionAbiIntegrationTests.cpp
        unit/extensions/ExternalImporterRegistrationTests.cpp
)
foreach(abi_fixture_mode RANGE 0 3)
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

if (HORO_BUILD_RENDER_OPENGL)
    add_executable(HoroRenderOpenGLTests
            unit/runtime/renderer/OpenGLRenderBackendTests.cpp
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

if (HORO_BUILD_RENDER_METAL)
    add_executable(HoroRenderMetalTests
            unit/runtime/renderer/MetalRenderBackendTests.cpp
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

set(HORO_CATCH_TEST_TARGETS
        HoroInputTests
        HoroGameplayRuntimeTests
        HoroGameModuleHostTests
        HoroLuaBehaviorTests
        HoroEditorPlaySessionControllerTests
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
        HoroProjectMigrationTests
        HoroProjectMigrationTransactionTests
        HoroProjectOpenServiceTests
        HoroRecentProjectInspectionServiceTests
        HoroProjectCreationServiceTests
        HoroEditorSettingsStoreTests
        HoroWorkspaceLayoutTests
        HoroWorkspacePanelHostTests
        HoroWorkspaceLayoutPersistenceTests
        HoroActivityBarLayoutTests
        HoroWorkspaceSplitterInteractionTests
        HoroEditorWorkspaceControllerTests
        HoroGameplayBehaviorFilenameModalStateTests
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
        HoroRenderGraphTests
        HoroRenderGraphLifetimeTests
        HoroTemporalHistoryTests
        HoroRenderResourceDescriptorTests
        HoroShaderManifestTests
        HoroRenderSubmissionTests
        HoroRenderFrontendTests
        HoroEditorViewportResourceTests
        HoroRuntimeLifecycleTests
        HoroRuntimeSaveIdentityTests
        HoroRuntimeSaveDiagnosticsTests
        HoroRuntimeSaveRootResolverTests
        HoroRuntimeSaveNamespaceTests
        HoroRuntimeSaveParticipantRegistryTests
        HoroRuntimeSaveCaptureSnapshotTests
        HoroRuntimeUiIdentityTests
        HoroRuntimeUiDocumentTests
        HoroRuntimeUiCanvasSpaceTests
        HoroRuntimeUiDiagnosticsTests
        HoroRuntimeUiElementTreeTests
        HoroRuntimeUiRenderSnapshotTests
        HoroRuntimeUiLayoutTests
        HoroRuntimeUiHitTestingTests
        HoroRuntimeUiEventDispatchTests
        HoroRuntimeSaveArchiveMetadataTests
        HoroRuntimeSaveArchiveFramingTests
        HoroRuntimeSaveCanonicalCodecTests
        HoroRuntimeSaveReferenceTests
        HoroRuntimeSaveSlotMetadataTests
        HoroRuntimeSaveOperationTests
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
endif ()
