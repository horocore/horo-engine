# Desired Project Trees

## Purpose

This document captures the desired high-level tree structure for Horo Engine,
Horo game projects, official Horo packages, package archives, package caches,
and release outputs.

This is an architectural target, not a command to physically refactor the repo in
one step. New systems should move toward this structure through focused CMake
targets and isolated implementation directories.

During the transition, the previous source organization is moved under
`deprecated/` as a temporary migration reference. New work should target the
desired structure below; code may be moved out of `deprecated/` incrementally when
the owning module, CMake target, tests, and documentation are ready.

Mermaid is intentionally not used here. ASCII trees are easier to read in the
terminal and avoid renderer-specific centering/layout behavior.

## Horo Engine Repository

```text
horo-engine/
├── pyproject.toml
├── CMakeLists.txt
├── CMakePresets.json
├── README.md
├── AGENTS.md
├── cmake/
│   ├── HoroCompilerOptions.cmake
│   ├── HoroTargets.cmake
│   ├── HoroDependencies.cmake
│   ├── HoroPackaging.cmake
│   ├── HoroSDK.cmake
│   ├── GenerateProjectCompatibility.cmake
│   ├── GenerateProjectMigrations.cmake
│   └── modules/
├── releases/
│   ├── 0.0.1/
│   ├── 0.1.0/
│   └── <horo-semver>/
│       ├── release.json
│       ├── project-contract.json
│       └── migration-recovery-contract.json
├── mock-studio/                  # Standalone React design workspace
│   ├── src/                      # Shared shell, components, and screen models
│   ├── public/assets/            # Fonts and static design assets
│   ├── package.json
│   ├── designs.md
│   └── README.md
├── docs/
│   ├── architecture/
│   │   ├── README.md
│   │   ├── desired-project-tree.md
│   │   ├── foundation/
│   │   │   ├── system-design.md
│   │   │   ├── glossary.md
│   │   │   ├── error-and-diagnostics.md
│   │   │   ├── scene-math.md
│   │   │   ├── project-versioning-and-migration.md
│   │   │   ├── configuration-system.md
│   │   │   ├── concurrency-and-jobs.md
│   │   │   ├── engine-data-bus.md
│   │   │   ├── ownership-and-resource-lifetime.md
│   │   │   └── platform-abstraction.md
│   │   ├── runtime/
│   │   │   ├── runtime-lifecycle.md
│   │   │   ├── scene-runtime.md
│   │   │   ├── rendering-architecture.md
│   │   │   ├── render-backend-parity-contract.md
│   │   │   ├── renderer-distribution-and-availability.md
│   │   │   ├── renderer-module-package-manifest.md
│   │   │   ├── material-and-shader-model.md
│   │   │   ├── advanced-rendering-architecture.md
│   │   │   ├── animation-architecture.md
│   │   │   ├── vfx-and-particles-architecture.md
│   │   │   ├── character-controller-architecture.md
│   │   │   ├── physics-architecture.md
│   │   │   ├── audio-architecture.md
│   │   │   ├── input-architecture.md
│   │   │   ├── game-ui-and-hud.md
│   │   │   ├── networking-architecture.md
│   │   │   ├── asset-pipeline.md
│   │   │   ├── prefab-architecture.md
│   │   │   ├── built-in-scene-primitives.md
│   │   │   ├── debug-console-and-overlays.md
│   │   │   ├── platform-services-architecture.md
│   │   │   ├── terrain-and-foliage-architecture.md
│   │   │   ├── world-streaming-architecture.md
│   │   │   ├── save-game-and-persistence.md
│   │   │   ├── navigation-and-ai-architecture.md
│   │   │   ├── cinematic-sequencer-architecture.md
│   │   │   ├── post-processing-and-effects-architecture.md
│   │   │   ├── lod-and-culling-architecture.md
│   │   │   ├── accessibility-architecture.md
│   │   │   ├── decal-system-architecture.md
│   │   │   ├── virtual-texturing-architecture.md
│   │   │   ├── destruction-and-fracture-architecture.md
│   │   │   ├── procedural-generation-architecture.md
│   │   │   ├── multiplayer-replication-architecture.md
│   │   │   ├── vr-ar-architecture.md
│   │   ├── editor/
│   │   │   ├── ProjectMigrationTransactionTests.cpp
│   │   │   ├── ProjectOpenServiceTests.cpp
│   │   │   └── RecentProjectInspectionServiceTests.cpp
│   │   │   ├── gui-screen-host.md
│   │   │   ├── ui-design-system.md
│   │   │   ├── localization.md
│   │   │   ├── editor-document-model.md
│   │   │   ├── editor-data-bus.md
│   │   │   ├── editor-panel-host.md
│   │   │   ├── editor-modal-host.md
│   │   │   ├── editor-ai-agent-architecture.md
│   │   │   ├── project-model.md
│   │   ├── extensions/
│   │   │   ├── gameplay-module.md
│   │   │   ├── gameplay-module-boundary.md
│   │   │   ├── gameplay-behavior-authoring.md
│   │   │   ├── gameplay-runtime-integration.md
│   │   │   ├── gameplay-module-verification.md
│   │   │   └── plugin-system.md
│   │   ├── packages/
│   │   │   ├── README.md
│   │   │   ├── package-system.md
│   │   │   ├── package-restore.md
│   │   │   ├── package-lifecycle.md
│   │   │   └── package-release-integration.md
│   │   ├── interfaces/
│   │   │   ├── cli-architecture.md
│   │   │   └── mcp-architecture.md
│   │   ├── security/
│   │   │   └── application-security.md
│   │   ├── observability/
│   │   │   ├── observability.md
│   │   │   ├── observability-logging.md
│   │   │   └── observability-performance.md
│   │   ├── delivery/
│   │   │   ├── developer-environment.md
│   │   │   ├── build-system.md
│   │   │   ├── build-cache.md
│   │   │   ├── testing-architecture.md
│   │   │   ├── game-project-testing.md
│   │   │   └── quality-and-ci.md
│   │   └── release/
│   │       ├── release.md
│   │       ├── release-security.md
│   │       └── distribution-and-update.md
│   ├── guides/
│   ├── adr/
│   ├── merged/
│   └── screenshots/
├── include/
│   └── Horo/
│       ├── Foundation/
│       │   ├── Result.h
│       │   ├── ErrorCode.h
│       │   ├── Diagnostics.h
│       │   ├── Configuration.h
│       │   ├── JobSystem.h
│       │   ├── CancellationToken.h
│       │   ├── Progress.h
│       │   ├── DataBus.h
│       │   ├── Platform.h
│       │   ├── Paths.h
│       │   ├── Time.h
│       │   └── Handles.h
│       ├── Math/
│       │   └── SceneMath.h
│       ├── Application/
│       │   ├── ProjectVersion.h
│       │   ├── ProjectCompatibility.h
│       │   ├── ProjectMigration.h
│       │   └── ProjectMigrationCatalog.h
│       ├── Editor/
│       │   ├── ProjectMutation.h
│       │   ├── ProjectMigrationTransaction.h
│       │   ├── ProjectOpenService.h
│       │   ├── ProjectSession.h
│       │   ├── RecentProject.h
│       │   └── RecentProjectInspectionService.h
│       ├── Network/
│       │   ├── NetworkApi.h
│       │   ├── NetworkHandles.h
│       │   ├── NetworkTypes.h
│       │   ├── NetworkAddress.h
│       │   ├── NetworkMessage.h
│       │   ├── NetworkErrors.h
│       │   └── INetworkTransport.h
│       ├── Runtime/
│       │   ├── Runtime.h
│       │   ├── RuntimeLifecycle.h
│       │   ├── FrameScheduler.h
│       │   ├── TimerQueue.h
│       │   ├── Scene.h
│       │   ├── Entity.h
│       │   ├── Component.h
│       │   ├── System.h
│       │   ├── Scene/
│       │   │   ├── RuntimeSceneDefinition.h
│       │   │   ├── RuntimeScene.h
│       │   │   ├── PrimitiveCatalog.h
│       │   │   ├── PrimitiveMesh.h
│       │   │   └── SceneComponents.h
│       │   ├── AssetHandle.h
│       │   ├── Input.h
│       │   ├── Physics.h
│       │   ├── Renderer.h
│       │   ├── Render/
│       │   │   ├── Mesh.h
│       │   │   ├── RenderScene.h
│       │   │   ├── RenderBackend.h
│       │   │   ├── RenderBackendRegistry.h
│       │   │   ├── RenderFrontend.h
│       │   ├── Network/
│       │   │   ├── NetworkRuntime.h
│       │   │   ├── NetworkSession.h
│       │   │   ├── ReplicationManager.h
│       │   │   └── ReplicationTraits.h
│       │   ├── Terrain/
│       │   │   ├── Api/
│       │   │   │   ├── TerrainTypes.h
│       │   │   │   ├── TerrainDescriptors.h
│       │   │   │   └── TerrainSnapshots.h
│       │   │   └── Runtime/
│       │   │       └── TerrainRuntime.h
│       │   ├── GameUI.h
│       │   ├── DebugConsole.h
│       │   ├── PlatformServices.h
│       │   └── Audio/
│       │       ├── Audio.h
│       │       ├── AudioMixer.h
│       │       ├── AudioVoice.h
│       │       ├── AudioCommandQueue.h
│       │       ├── AudioDSPNode.h
│       │       ├── AudioSpatializer.h
│       │       ├── AudioOcclusionProvider.h
│       │       ├── AudioMiddlewareBridge.h
│       │       └── AudioDecoder.h
│       ├── Navigation/
│       │   ├── NavMeshTypes.h
│       │   ├── NavMeshBuildSettings.h
│       │   ├── NavMeshData.h
│       │   ├── NavMeshPath.h
│       │   ├── NavMeshQuery.h
│       │   ├── NavAgentProperties.h
│       │   ├── DynamicObstacle.h
│       │   ├── NavigationBackend.h
│       │   ├── NavigationCoordinator.h  # NavigationRuntime-owned
│       │   ├── NavigationErrors.h
│       │   └── Backends/
│       │       ├── RecastDetourProvider.h  # provider-owned Horo-only factory
│       │       └── NullProvider.h         # provider-owned Horo-only factory
│       ├── Gameplay/
│       │   ├── GameModule.h
│       │   ├── GameRegistrationContext.h
│       │   ├── GameRuntimeContext.h
│       │   ├── BehaviorDescriptor.h
│       │   ├── ServiceDescriptor.h
│       │   ├── ScriptModuleDescriptor.h
│       │   └── ImportedLibraryModule.h
│       ├── Assets/
│       │   ├── AssetId.h
│       │   ├── AssetRegistry.h
│       │   ├── AssetProvider.h
│       │   ├── AssetImporter.h
│       │   ├── AssetCooker.h
│       │   ├── PackageAssetReference.h
│       │   └── RuntimeArchive.h
│       ├── Packages/
│       │   ├── PackageId.h
│       │   ├── PackageManifest.h
│       │   ├── PackageContribution.h
│       │   ├── PackageSource.h
│       │   ├── PackageResolver.h
│       │   ├── PackageLockfile.h
│       │   ├── PackageCache.h
│       │   ├── PackageRestore.h
│       │   ├── PackageLifecycle.h
│       │   ├── PackageTrust.h
│       │   └── PackageValidation.h
│       ├── Extensions/
│       │   ├── ExtensionManifest.h
│       │   ├── ExtensionPoint.h
│       │   ├── ExtensionHost.h
│       │   ├── ExtensionRegistry.h
│       │   └── ExtensionCapability.h
│       ├── Observability/
│       │   ├── Logger.h
│       │   ├── LogCategory.h
│       │   ├── Metrics.h
│       │   ├── Profiler.h
│       │   ├── DiagnosticBundle.h
│       │   └── ObservabilityContribution.h
│       └── Release/
│           ├── ReleaseProfile.h
│           ├── ReleasePipeline.h
│           ├── ReleaseManifest.h
│           ├── ReleaseCandidate.h
│           ├── Signing.h
│           └── DistributionManifest.h
├── src/
│   ├── application/
│   │   ├── project/
│   │   │   ├── ProjectVersion.cpp
│   │   │   ├── ProjectCompatibility.cpp
│   │   │   ├── ProjectErrors.h
│   │   │   └── ProjectErrors.cpp
│   │   ├── project_migration/
│   │   │   ├── ProjectMigrationRegistry.cpp
│   │   │   └── ProjectMigrationExecution.cpp
│   │   └── project_migrations/
│   │       ├── definitions/
│   │       │   ├── 0.1.0/
│   │       │   └── <target-semver>/
│   │       └── checkpoints/
│   │           └── <target-semver>/<source-semver>/
│   ├── foundation/
│   │   ├── error/
│   │   ├── diagnostics/
│   │   ├── config/
│   │   ├── jobs/
│   │   ├── data_bus/
│   │   ├── math/
│   │   ├── memory/
│   │   ├── platform/
│   │   │   ├── common/
│   │   │   ├── macos/
│   │   │   ├── linux/
│   │   │   └── windows/
│   │   └── time/
│   ├── runtime/
│   │   ├── lifecycle/
│   │   ├── frame/
│   │   │   ├── frame_scheduler/
│   │   │   └── timer_queue/
│   │   ├── scene/
│   │   │   ├── RuntimeSceneDefinition.cpp
│   │   │   ├── RuntimeScene.cpp
│   │   │   ├── RuntimeSceneErrors.h
│   │   │   ├── RuntimeSceneErrors.cpp
│   │   │   ├── entity/
│   │   │   ├── component/
│   │   │   ├── primitive/
│   │   │   │   ├── PrimitiveCatalog.cpp
│   │   │   │   └── PrimitiveMesh.cpp
│   │   │   ├── system/
│   │   │   ├── serialization/
│   │   │   └── transitions/
│   │   ├── assets/
│   │   │   ├── AssetErrors.h
│   │   │   ├── AssetErrors.cpp
│   │   │   ├── importer/
│   │   │   ├── cooker/
│   │   │   ├── registry/
│   │   │   │   ├── AssetId.cpp
│   │   │   │   └── AssetRegistry.cpp
│   │   │   ├── cache/
│   │   │   ├── runtime_provider/
│   │   │   │   ├── AssetProviderRead.h
│   │   │   │   └── AssetProvider.cpp
│   │   │   ├── hot_reload/
│   │   │   └── archive/
│   │   ├── renderer/
│   │   │   ├── frontend/
│   │   │   ├── render_graph/
│   │   │   ├── rhi/
│   │   │   ├── backend_registry/
│   │   │   ├── module_abi/
│   │   │   ├── module_host/
│   │   │   ├── modules/
│   │   │   │   ├── null/
│   │   │   │   ├── opengl/
│   │   │   │   ├── vulkan/
│   │   │   │   ├── metal/
│   │   │   │   └── d3d12/
│   │   │   └── shaders/
│   │   ├── physics/
│   │   │   ├── broadphase/
│   │   │   ├── narrowphase/
│   │   │   ├── constraints/
│   │   │   ├── integration/
│   │   │   └── queries/
│   │   ├── terrain/
│   │   │   ├── api/
│   │   │   ├── runtime/
│   │   │   ├── streaming/
│   │   │   ├── render_bridge/
│   │   │   ├── physics_bridge/
│   │   │   └── navigation_bridge/
│   │   ├── audio/
│   │   │   ├── frontend/
│   │   │   ├── mixer/
│   │   │   ├── voice_registry/
│   │   │   ├── command_queue/
│   │   │   ├── decoders/
│   │   │   ├── dsp_nodes/
│   │   │   ├── spatializers/
│   │   │   ├── middleware_bridge/
│   │   │   ├── streaming/
│   │   │   └── null_backend/
│   │   ├── game_ui/
│   │   ├── input/
│   │   │   ├── Input.cpp
│   │   │   └── sdl/
│   │   │       ├── SdlInputBackend.h
│   │   │       └── SdlInputBackend.cpp
│   │   ├── networking/
│   │   │   ├── api/
│   │   │   │   ├── NetworkAddress.cpp
│   │   │   │   ├── NetworkErrors.cpp
│   │   │   │   └── NetworkMessage.cpp
│   │   │   ├── runtime/
│   │   │   │   ├── NetworkRuntime.cpp
│   │   │   │   ├── NetworkSession.cpp
│   │   │   │   ├── SessionStateMachine.cpp
│   │   │   │   └── ReplicationManager.cpp
│   │   │   └── backends/
│   │   │       ├── null/
│   │   │       │   ├── NullTransportBackend.h
│   │   │       │   └── NullTransportBackend.cpp
│   │   │       └── enet/
│   │   │           ├── ENetTransportBackend.h
│   │   │           └── ENetTransportBackend.cpp
│   │   ├── navigation/
│   │   │   ├── api/
│   │   │   ├── runtime/
│   │   │   │   ├── coordinator/
│   │   │   │   ├── cache/
│   │   │   │   ├── crowd/
│   │   │   │   ├── hierarchical/
│   │   │   │   └── obstacles/
│   │   │   └── backends/
│   │   │       ├── recast_detour/
│   │   │       │   ├── runtime/
│   │   │       │   └── build/  # omitted from runtime-only provider composition
│   │   │       └── null/
│   │   ├── debug/
│   │   └── platform_services/
│   │       ├── frontend/
│   │       ├── backend/
│   │       ├── null_backend/
│   │       ├── request_queue/
│   │       ├── offline_queue/
│   │       ├── achievement_service/
│   │       ├── leaderboard_service/
│   │       ├── cloud_save_service/
│   │       ├── presence_service/
│   │       └── friends_service/
│   ├── gameplay/
│   │   ├── module_boundary/
│   │   ├── behavior/
│   │   ├── service_registry/
│   │   ├── script_runtime/
│   │   ├── imported_library_module/
│   │   └── verification/
│   ├── packages/
│   │   ├── manifest/
│   │   ├── source/
│   │   │   ├── file_source/
│   │   │   ├── url_source/
│   │   │   ├── static_index_source/
│   │   │   ├── registry_source/
│   │   │   └── git_source_future/
│   │   ├── resolver/
│   │   ├── lockfile/
│   │   ├── archive/
│   │   │   ├── reader/
│   │   │   ├── writer/
│   │   │   ├── files_manifest/
│   │   │   └── validator/
│   │   ├── cache/
│   │   │   ├── content_addressed_store/
│   │   │   ├── leases/
│   │   │   ├── gc/
│   │   │   └── quarantine/
│   │   ├── trust/
│   │   │   ├── publisher_identity/
│   │   │   ├── signing_roots/
│   │   │   ├── trust_policy/
│   │   │   └── audit/
│   │   ├── restore/
│   │   ├── lifecycle/
│   │   │   ├── install/
│   │   │   ├── enable/
│   │   │   ├── activate/
│   │   │   ├── update/
│   │   │   ├── uninstall/
│   │   │   └── transaction_journal/
│   │   ├── contribution/
│   │   │   ├── assets/
│   │   │   ├── scripts/
│   │   │   ├── behaviors/
│   │   │   ├── runtime_services/
│   │   │   ├── runtime_plugins/
│   │   │   │   ├── audio_decoders/
│   │   │   │   ├── audio_dsp_nodes/
│   │   │   │   ├── audio_spatializers/
│   │   │   │   └── audio_middleware_backends/
│   │   │   ├── platform_services_backends/
│   │   │   │   ├── steam/
│   │   │   │   ├── psn/
│   │   │   │   ├── xbox_live/
│   │   │   │   └── nintendo_online/
│   │   │   ├── editor_extensions/
│   │   │   ├── samples/
│   │   │   ├── templates/
│   │   │   └── documentation/
│   │   ├── ui_plan/
│   │   └── diagnostics/
│   ├── extensions/
│   │   ├── abi/
│   │   ├── manifest/
│   │   ├── host/
│   │   ├── registry/
│   │   └── capabilities/
│   │       ├── audio/
│   │       │   ├── decoder_capability.md
│   │       │   ├── dsp_node_capability.md
│   │       │   ├── spatializer_capability.md
│   │       │   └── middleware_backend_capability.md
│   │       ├── editor_points/
│   │       ├── asset_pipeline_points/
│   │       ├── cli_points/
│   │       └── mcp_points/
│   ├── editor/
│   │   ├── app/
│   │   ├── screens/
│   │   ├── design_system/
│   │   ├── localization/
│   │   ├── document/
│   │   ├── data_bus/
│   │   ├── panels/
│   │   ├── modals/
│   │   ├── source_editor/
│   │   ├── graph_editor/
│   │   ├── project_model/
│   │   │   ├── ProjectMutation.cpp
│   │   │   ├── ProjectMigrationTransaction.cpp
│   │   │   ├── ProjectOpenService.cpp
│   │   │   └── RecentProjectInspectionService.cpp
│   │   └── mcp_bridge/
│   ├── interfaces/
│   │   ├── cli/
│   │   └── mcp/
│   ├── observability/
│   │   ├── runtime/
│   │   ├── logging/
│   │   ├── metrics/
│   │   ├── profiler/
│   │   ├── diagnostic_bundle/
│   │   └── schemas/
│   ├── security/
│   │   ├── trust/
│   │   ├── credentials/
│   │   ├── path_policy/
│   │   ├── process_policy/
│   │   ├── parser_limits/
│   │   ├── network_policy/
│   │   └── audit/
│   └── release/
│       ├── service/
│       ├── pipeline/
│       ├── profiles/
│       ├── manifest/
│       ├── candidate/
│       ├── promotion/
│       ├── signing/
│       ├── notarization/
│       ├── sbom/
│       ├── provenance/
│       ├── distribution/
│       ├── update/
│       └── dlc/
├── apps/
│   ├── HoroEditor/
│   │   ├── main.cpp
│   │   ├── app/
│   │   └── resources/
│   ├── horo-engine/
│   │   ├── main.cpp
│   │   └── cli_host/
│   └── horopak/
│       ├── main.cpp
│       └── package_tool/
├── sdk/
│   ├── cmake/
│   ├── include/
│   ├── templates/
│   │   ├── game-project/
│   │   ├── game-library-source/
│   │   ├── game-library-prebuilt/
│   │   ├── asset-package/
│   │   ├── hybrid-package/
│   │   └── editor-extension/
│   └── schemas/
│       ├── project.schema.json
│       ├── package-manifest.schema.json
│       ├── package-lock.schema.json
│       ├── release-manifest.schema.json
│       ├── observability-log.schema.json
│       └── mcp-tools.schema.json
├── examples/
│   ├── projects/
│   │   ├── minimal-2d/
│   │   ├── minimal-3d/
│   │   ├── package-restore-demo/
│   │   └── release-profile-demo/
│   ├── packages/
│   │   ├── asset-pack-basic/
│   │   ├── game-library-scheduler-minimal/
│   │   ├── hybrid-gun-pack-minimal/
│   │   └── template-package-basic/
│   └── extensions/
│       ├── editor-panel-basic/
│       ├── asset-importer-basic/
│       └── mcp-tool-basic/
├── tests/
│   ├── CMakeLists.txt
│   ├── unit/
│   │   ├── foundation/
│   │   ├── application/
│   │   │   ├── ProjectCompatibilityTests.cpp
│   │   │   └── ProjectMigrationTests.cpp
│   │   ├── runtime/
│   │   │   ├── frame/
│   │   │   ├── scene/
│   │   │   ├── assets/
│   │   │   ├── renderer/
│   │   │   ├── physics/
│   │   │   ├── audio/
│   │   │   │   ├── command_queue/
│   │   │   │   ├── mixer/
│   │   │   │   ├── voice_registry/
│   │   │   │   ├── decoders/
│   │   │   │   ├── dsp_nodes/
│   │   │   │   ├── spatializers/
│   │   │   │   └── middleware_bridge/
│   │   │   ├── input/
│   │   │   ├── networking/
│   │   │   │   ├── NetworkAddressTests.cpp
│   │   │   │   ├── NetworkApiTests.cpp
│   │   │   │   ├── TransportConnectionStateTests.cpp
│   │   │   │   ├── SessionStateMachineTests.cpp
│   │   │   │   ├── NetworkHandleTests.cpp
│   │   │   │   ├── SendPayloadLifetimeTests.cpp
│   │   │   │   ├── ReplicationManagerTests.cpp
│   │   │   │   ├── NetworkTransportNullTests.cpp
│   │   │   │   ├── NetworkTransportGNSTests.cpp
│   │   │   │   └── NetworkLifecycleAndShutdownTests.cpp
│   │   │   ├── game_ui/
│   │   │   ├── debug/
│   │   │   └── platform_services/
│   │   │       ├── frontend/
│   │   │       ├── request_queue/
│   │   │       ├── offline_queue/
│   │   │       ├── achievement_service/
│   │   │       ├── leaderboard_service/
│   │   │       ├── cloud_save_service/
│   │   │       ├── presence_service/
│   │   │       └── friends_service/
│   │   ├── editor/
│   │   ├── packages/
│   │   ├── extensions/
│   │   ├── observability/
│   │   ├── release/
│   │   └── security/
│   ├── integration/
│   │   ├── project_open/
│   │   ├── asset_pipeline/
│   │   ├── package_restore/
│   │   ├── package_lifecycle/
│   │   ├── release_pipeline/
│   │   ├── cli/
│   │   └── mcp/
│   ├── fixtures/
│   │   ├── projects/
│   │   │   └── horo_0_0_1_compression/
│   │   ├── packages/
│   │   ├── release/
│   │   └── observability/
│   ├── gui/
│   │   ├── EditorUiAutomationHarnessTests.cpp
│   │   └── FullEditorUiAutomationScenarios.cpp
│   ├── helpers/
│   │   └── editor_ui/
│   │       ├── EditorUiTestHarness.h
│   │       ├── EditorUiTestHarness.cpp
│   │       ├── EditorUiTestSurface.h
│   │       ├── EditorUiTestSurface.cpp
│   │       ├── FullEditorUiTestActions.h
│   │       ├── FullEditorUiTestActions.cpp
│   │       ├── FullEditorUiTestHost.h
│   │       ├── FullEditorUiTestHost.cpp
│   │       ├── FullEditorUiTestSetups.h
│   │       ├── FullEditorUiTestSetups.cpp
│   │       └── InteractiveOpenGlEditorUiTestSurface.cpp
│   ├── mocks/
│   └── python/
│       ├── conftest.py
│       ├── test_project_compatibility_generator.py
│       └── test_project_migration_generator.py
├── scripts/
│   ├── requirements.txt
│   ├── dev.py
│   ├── ci.py
│   ├── package.py
│   ├── release.py
│   ├── dependency-state.py
│   ├── validate-docs.py
│   ├── generate-schemas.py
│   ├── generate_project_compatibility.py
│   └── generate_project_migration_catalog.py
├── tools/
│   ├── package-index-server/
│   ├── schema-validator/
│   ├── asset-cooker/
│   ├── release-verifier/
│   ├── observability-viewer/
│   ├── project-migration-catalog/
│   └── mcp-devtools/
├── vendor/
└── deprecated/
```

## Structural Notes

### Packages vs Extensions

- **Packages** are distribution units: versioned archives, assets, scripts,
  templates, and configuration. They are installed, enabled, and activated by the
  package lifecycle.
- **Extensions** are ABI-level plugin points registered with the engine host.
  They extend the editor, asset pipeline, CLI, MCP, or runtime behavior through
  stable interfaces.

A real-world integration usually uses both. For example, a Wwise middleware
integration ships as a **package** containing bank assets, project settings, and
cook rules, plus an **extension** providing `IAudioMiddlewareBackend` and
`IAudioMiddlewareEventBridge` implementations. The package contributes the
extension through `packages/contribution/runtime_plugins/audio_middleware_backends/`.

An editor panel package contributes through a similar split: the **package**
carries the panel UI definition, icons, and layout templates, while the
**extension** implements `IEditorPanel` and registers menu entry points through
`packages/contribution/editor_extensions/`.

### `vendor/` Naming

- `horo-engine/vendor/` holds third-party C/C++ dependencies built with the
  engine (source or prebuilt).
- Game projects do not use a top-level `vendor/` directory for Horo packages.
  Offline or vendored packages live under `packages/vendored/` inside the game
  project tree. `packages/source/git_source_future/` is a future package source
  type for remote Horo packages, not a replacement for `vendor/` or
  `packages/vendored/`.

### Audio Plugin Categories Across Layers

Audio plugin categories (`audio_decoders`, `audio_dsp_nodes`,
`audio_spatializers`, `audio_middleware_backends`) appear in three places:

- `src/packages/contribution/runtime_plugins/`: package-authored plugin
  binaries/assets.
- `src/extensions/capabilities/audio/`: extension capability contracts and
  registration interfaces.
- `tests/unit/runtime/audio/`: per-category unit tests.

This repetition is intentional: each place owns a different view (distribution,
ABI contract, tests). The capability contract in
`src/extensions/capabilities/audio/*.md` is the authoritative interface; the
other two mirror it for packaging and verification.

### Runtime Components As Scene Primitives

Runtime subsystems such as Audio, Game UI, and Input expose components that are
scene primitives owned by Runtime and Editor. `AudioSourceComponent`,
`AudioListenerComponent`, canvas components, and input action maps are
serialized with the scene and documented in their respective runtime subsystem
architecture documents, not duplicated inside `built-in-scene-primitives.md`.

### SDK Schemas

JSON schemas under `sdk/schemas/` are generated from the C++ data model and
canonical type definitions, not the other way around. `scripts/generate-schemas.py`
reads the engine/package/release types and emits the schema files; the schema
validator and external tooling consume them. If the model changes, regenerate
schemas rather than hand-editing the JSON files.

### Deprecated Legacy Source During Restructure

`deprecated/` is a temporary migration reference for the pre-restructure source
layout. The legacy source tree is moved there so the repository root can expose
the new desired structure without duplicate active source trees. Do not add new
systems there, do not make it part of CMake target discovery, and do not include
it in generated release artifacts. New implementation work belongs under
`include/Horo/`, `src/`, `apps/`, `sdk/`, `tools/`, or the matching
documentation area above.

When old code is migrated, move the owning module deliberately: public headers,
private implementation, CMake target wiring, tests, docs, and asset references
should move as one reviewed unit. After a module has been migrated and validated,
its backup copy may be removed in a later cleanup.

### Repository Release Policy

The desired tree assumes maintainer-driven releases. The repository should not
contain generated release-PR configuration or automation that decides when a
release exists. Version changes, changelog content, tags, and GitHub Releases are
manual maintainer actions. CI may validate and upload artifacts for an explicit
published tag, but it must not create the release decision itself.

### Package Overrides vs Local Dev Overrides

`assets/package_overrides/` contains committed, asset-level override files that
are layered over mounted package assets (see [Package Lifecycle](./packages/package-lifecycle.md)).
`.horo/local/packages.local.json` contains user-local source overrides that
replace a package source with a local path during development (see
[Package Restore](./packages/package-restore.md)). The two mechanisms operate at
different layers: project overrides patch assets, local overrides patch the
restore graph.

## Horo Game Project Repository

```text
MyGame/
├── README.md
├── CMakeLists.txt
├── .gitignore
├── .horo/
│   ├── project.json
│   ├── packages.json
│   ├── packages.lock
│   ├── release-profiles.json
│   ├── editor_workspace.json
│   ├── asset_index.json
│   ├── scene_index.json
│   ├── generated_bindings.json
│   ├── local/
│   │   ├── packages.local.json
│   │   ├── trust.local.json
│   │   ├── credentials.local.json
│   │   ├── toolchains.local.json
│   │   └── editor_state.local.json
│   └── recovery/
│       ├── autosave/
│       └── transaction_journal/
├── assets/
│   ├── scenes/
│   │   ├── main.horo
│   │   ├── boot.horo
│   │   └── test/
│   ├── prefabs/
│   │   ├── player.prefab
│   │   ├── enemy.prefab
│   │   └── weapons/
│   ├── models/
│   │   ├── characters/
│   │   ├── props/
│   │   └── weapons/
│   ├── animations/
│   │   ├── humanoid/
│   │   ├── weapons/
│   │   └── cinematics/
│   ├── textures/
│   │   ├── characters/
│   │   ├── environment/
│   │   └── ui/
│   ├── materials/
│   │   ├── characters/
│   │   ├── environment/
│   │   └── weapons/
│   ├── audio/
│   │   ├── music/
│   │   ├── sfx/
│   │   ├── ambience/
│   │   └── voice/
│   ├── shaders/
│   ├── fonts/
│   ├── localization/
│   │   ├── en-US.json
│   │   └── tr-TR.json
│   └── package_overrides/
│       ├── com.vendor.weapon-pack/
│       └── com.vendor.humanoid-animation-pack/
├── packages/
│   ├── README.md
│   ├── vendored/
│   │   ├── index.json
│   │   ├── com.vendor.weapon-pack-2.0.0.horopkg
│   │   └── com.team.internal-ui-1.4.2.horopkg
│   └── imported_samples/
│       ├── com.horo.fps-starter/
│       └── com.horo.dialogue-sample/
├── src/
│   ├── GameModule.cpp
│   ├── GameModule.h
│   ├── components/
│   │   ├── PlayerComponent.h
│   │   ├── WeaponComponent.h
│   │   └── HealthComponent.h
│   ├── systems/
│   │   ├── PlayerMovementSystem.cpp
│   │   ├── WeaponSystem.cpp
│   │   └── AISystem.cpp
│   ├── services/
│   │   ├── SaveGameService.cpp
│   │   ├── DialogueService.cpp
│   │   └── GameStateService.cpp
│   ├── behaviors/
│   │   ├── PlayerController.cpp
│   │   ├── DoorBehavior.cpp
│   │   └── WeaponPickupBehavior.cpp
│   └── generated/
│       ├── behavior_descriptors.generated.cpp
│       ├── component_descriptors.generated.cpp
│       └── script_bindings.generated.cpp
├── scripts/
│   ├── game/
│   │   ├── boot.lua
│   │   ├── player.lua
│   │   ├── weapons.lua
│   │   └── ui.lua
│   ├── behaviors/
│   │   ├── rotating_platform.lua
│   │   ├── trigger_zone.lua
│   │   └── scripted_door.lua
│   └── tests/
│       ├── player_test.lua
│       └── weapon_test.lua
├── config/
│   ├── game.json
│   ├── input-actions.json
│   ├── render-profile.json
│   ├── physics-profile.json
│   ├── audio-profile.json
│   ├── audio-focus-policy.json
│   ├── package-policy.json
│   ├── trust-policy.json
│   └── release/
│       ├── windows-release.json
│       ├── macos-release.json
│       ├── linux-release.json
│       ├── steam-release.json
│       ├── itch-release.json
│       └── dedicated-server.json
├── content/
│   ├── chunks/
│   │   ├── core.chunk.json
│   │   ├── level_01.chunk.json
│   │   ├── level_02.chunk.json
│   │   └── cinematics.chunk.json
│   ├── dlc/
│   │   ├── dlc_weapons/
│   │   │   ├── dlc.json
│   │   │   └── chunk.json
│   │   └── dlc_cosmetics/
│   │       ├── dlc.json
│   │       └── chunk.json
│   └── packaging/
│       ├── asset-rules.json
│       ├── compression-rules.json
│       ├── encryption-rules.json
│       └── platform-overrides.json
├── tests/
│   ├── unit/
│   ├── integration/
│   ├── fixtures/
│   └── smoke/
├── tools/
├── build/
├── logs/
└── dist/
```

Committed project state:

```text
.horo/project.json
.horo/packages.json
.horo/packages.lock
source assets
package provenance metadata
package overrides if project-owned
vendored .horopkg files only when intentionally offline/vendor mode
```

User-local / generated state:

```text
.horo/local/
user trust decisions
credentials
global package cache
build/
cooked/
release intermediates
```

## Official Horo Packages Repository

```text
horo-packages/
├── README.md
├── package-index.json
├── publisher.json
├── signing/
│   ├── public-publisher-keys/
│   ├── key-rotation-policy.md
│   └── revocations.json
├── packages/
│   ├── com.horo.scheduler/
│   │   ├── horo-package.toml
│   │   ├── files.manifest.json
│   │   ├── src/
│   │   ├── scripts/
│   │   ├── behaviors/
│   │   ├── services/
│   │   ├── samples/
│   │   ├── docs/
│   │   ├── tests/
│   │   ├── NOTICE.md
│   │   └── CHANGELOG.md
│   ├── com.horo.dialogue/
│   ├── com.horo.inventory/
│   ├── com.horo.quest/
│   ├── com.horo.ai.behavior-tree/
│   ├── com.horo.localization-runtime/
│   ├── com.horo.save-system/
│   ├── com.horo.tween/
│   ├── com.horo.fps-controller/
│   └── com.horo.third-person-controller/
├── asset-packs/
│   ├── com.horo.sample.gun-pack/
│   │   ├── horo-package.toml
│   │   ├── files.manifest.json
│   │   ├── assets/
│   │   │   ├── models/
│   │   │   ├── animations/
│   │   │   ├── sounds/
│   │   │   ├── materials/
│   │   │   └── textures/
│   │   ├── samples/
│   │   ├── docs/
│   │   └── NOTICE.md
│   ├── com.horo.sample.humanoid-animation-pack/
│   └── com.horo.sample.ui-sounds/
├── templates/
│   ├── com.horo.template.empty-3d/
│   ├── com.horo.template.fps-starter/
│   ├── com.horo.template.third-person/
│   └── com.horo.template.dedicated-server/
├── hybrid/
│   ├── com.horo.fps-starter-kit/
│   └── com.horo.ai-debug-kit/
├── dist/
│   ├── index/
│   │   ├── static-index.json
│   │   ├── static-index.sig
│   │   └── publisher-metadata.json
│   ├── packages/
│   │   ├── com.horo.scheduler-0.1.0.horopkg
│   │   └── com.horo.scheduler-0.1.0.horopkg.sig
│   ├── sbom/
│   ├── provenance/
│   └── notices/
├── tests/
└── scripts/
```

## `.horopkg` Archive Layout

```text
com.vendor.weapon-pack-2.0.0.horopkg
├── horo-package.toml
├── files.manifest.json
├── package-lock.json
├── publisher.cert.json
├── signature.sig
├── assets/
│   ├── models/
│   │   └── rifle.fbx
│   ├── animations/
│   │   ├── rifle_reload.anim
│   │   └── rifle_fire.anim
│   ├── audio/
│   │   ├── rifle_fire.wav
│   │   └── rifle_reload.wav
│   ├── textures/
│   └── materials/
├── scripts/
│   └── weapon_recoil.lua
├── behaviors/
│   ├── weapon_behavior.horo-behavior.json
│   └── recoil_behavior.horo-behavior.json
├── services/
│   └── recoil_service.horo-service.json
├── extensions/
│   └── com.vendor.weapon-tools.editor/
│       ├── extension.json
│       ├── bin/
│       │   ├── linux-x64/libweapon_tools.so
│       │   ├── macos-arm64/libweapon_tools.dylib
│       │   └── windows-x64/weapon_tools.dll
│       └── resources/
│           └── icons/weapon-stats.svg
├── samples/
│   ├── scenes/
│   │   └── weapon_demo.horo
│   └── prefabs/
│       └── rifle_demo.prefab
├── templates/
├── docs/
│   └── README.md
└── NOTICE.md
```

`horo-package.toml` and `files.manifest.json` remain authoritative for the whole
archive. Each `extensions/<module-id>/extension.json` describes only that module,
its entry variants and contributions as defined by
[ADR-054](../adr/054-extension-and-package-authority-boundary.md).

## Global Package Cache

```text
~/.cache/horo/
├── packages/
│   ├── by-hash/
│   │   └── sha256/
│   │       └── a1b2c3.../
│   │           ├── archive.horopkg
│   │           ├── extracted/
│   │           ├── files.manifest.json
│   │           ├── verification.json
│   │           └── leases.json
│   ├── by-id/
│   │   └── com.vendor.weapon-pack/
│   │       └── 2.0.0 -> ../../by-hash/sha256/a1b2c3...
│   ├── quarantine/
│   │   ├── hash-mismatch/
│   │   ├── invalid-signature/
│   │   ├── path-traversal/
│   │   └── archive-bomb/
│   ├── downloads/
│   │   └── partial/
│   ├── staging/
│   ├── pins.json
│   ├── gc-state.json
│   └── cache-index.json
├── trust/
│   ├── publisher-trust.json
│   ├── package-trust.local.json
│   ├── organization-policy.json
│   └── revocations.json
└── logs/
```

## Build And Release Output

```text
build/
├── debug/
├── tests/
├── generated/
├── asset-cache/
├── package-restore/
│   ├── restore-report.json
│   ├── package-graph.json
│   └── verification/
├── cooked/
│   ├── windows-x64/
│   ├── linux-x64/
│   └── macos-arm64/
└── release/
    ├── candidates/
    │   └── MyGame-1.0.0-rc.1/
    │       ├── release-manifest.json
    │       ├── packages-used.json
    │       ├── compatibility-report.json
    │       ├── license-notices/
    │       ├── sbom/
    │       ├── provenance/
    │       ├── symbols/
    │       ├── artifacts/
    │       │   ├── windows-x64/
    │       │   ├── linux-x64/
    │       │   └── macos-arm64/
    │       └── verification/
    │           ├── package-restore.json
    │           ├── hash-verification.json
    │           ├── signature-verification.json
    │           ├── smoke-tests.json
    │           └── notarization.json
    ├── chunks/
    │   ├── core.assets.horo
    │   ├── level_01.assets.horo
    │   └── dlc_weapons.assets.horo
    └── published/
```

## Dependency Direction

```text
foundation
├── runtime
│   └── gameplay
│       └── packages
│           ├── editor adapters
│           ├── cli adapters
│           ├── mcp adapters
│           └── release integration
└── observability
    ├── logs
    ├── metrics
    ├── profiler
    └── diagnostic bundles
```

Rules:

- Foundation owns cross-cutting system primitives (jobs, locks, data-bus
  message types, cancellation tokens, result types). Scene primitives
  (meshes, colliders, lights, cameras) are owned by Runtime and Editor as
  documented in [Built-In Scene Primitives](./runtime/built-in-scene-primitives.md).
- Runtime owns simulation, assets, rendering, audio, input, physics, and scene execution.
- Gameplay owns project/game code boundaries.
- Packages compose package primitives into restore/lifecycle/release use cases.
- Editor, CLI, and MCP are adapters over shared services.
- Data buses publish committed-state notifications only; they do not carry command requests.

## Package Primitive Composition

```text
Package primitives
├── PackageId
├── PackageSource
├── PackageManifest
├── PackageContributionDescriptor
├── PackageLockEntry
├── PackageGraph
├── PackageCacheEntry
├── PackageTrustRequirement
├── PackageRestorePlan
├── PackageLifecycleTransaction
└── PackageReleaseClosure

PackageRestoreService
├── PackageResolver
├── PackageCache
├── PackageVerifier
├── TrustCheck
└── MountPlan

PackageLifecycleService
├── RestorePlan
├── TransactionJournal
├── EnableState
└── ActivationRegistry

PackageReleaseIntegration
├── FrozenLockfile
├── RestoreVerifier
├── AssetCook
├── RuntimeClosure
└── LicenseNotices
```
