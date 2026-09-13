include_guard(GLOBAL)

# Public header ownership is intentionally explicit. Adding a header under
# include/Horo requires assigning it to exactly one production target here.
message(STATUS "Configuring target-specific public header boundaries")

horo_configure_target_header_boundary(HoroFoundation PUBLIC_HEADERS
    Horo/Foundation/AssetCookTargetId.h
    Horo/Foundation/BuildOutputStore.h
    Horo/Foundation/CancellationToken.h
    Horo/Foundation/Configuration.h
    Horo/Foundation/DataBus.h
    Horo/Foundation/Diagnostics.h
    Horo/Foundation/Diagnostics/DiagnosticBundle.h
    Horo/Foundation/Diagnostics/OperationHistory.h
    Horo/Foundation/DiagnosticsEngine.h
    Horo/Foundation/ErrorCode.h
    Horo/Foundation/ErrorCodeRegistry.h
    Horo/Foundation/ValidationResult.h
    Horo/Foundation/Handles.h
    Horo/Foundation/JobSystem.h
    Horo/Foundation/Logging/LogContext.h
    Horo/Foundation/Logging/LogLevel.h
    Horo/Foundation/Logging/Logger.h
    Horo/Foundation/Logging/StructuredLogStore.h
    Horo/Foundation/ModuleDescriptor.h
    Horo/Foundation/ModuleHost.h
    Horo/Foundation/OperationStore.h
    Horo/Foundation/PathUtils.h
    Horo/Foundation/Paths.h
    Horo/Foundation/Platform.h
    Horo/Foundation/Progress.h
    Horo/Foundation/Result.h
    Horo/Foundation/Sha256.h
    Horo/Foundation/StrongId.h
    Horo/Foundation/String.h
    Horo/Foundation/Telemetry/Operation.h
    Horo/Foundation/Telemetry/Telemetry.h
    Horo/Foundation/Time.h
    Horo/Foundation/TransparentString.h
    Horo/Foundation/Utf8.h
    Horo/Math/SceneMath.h
    Horo/Math/WorldCoordinate64.h
)

horo_configure_target_header_boundary(HoroSecurity PUBLIC_HEADERS
    Horo/Security/ArtifactSignature.h
    Horo/Security/CredentialStore.h
    Horo/Security/SecureMemory.h
    Horo/Security/SecurityErrors.h
)

horo_configure_target_header_boundary(HoroCliHost PUBLIC_HEADERS
    Horo/Cli/CliCommandDescriptor.h
    Horo/Cli/CliCommandRegistry.h
    Horo/Cli/CliDispatcher.h
    Horo/Cli/CliErrors.h
    Horo/Cli/CliOptionParser.h
)

horo_configure_target_header_boundary(HoroOpenTelemetry PUBLIC_HEADERS
    Horo/Foundation/Telemetry/OpenTelemetrySink.h)

horo_configure_target_header_boundary(HoroPlatform PUBLIC_HEADERS
    Horo/Platform/AndroidLifecycle.h
    Horo/Platform/ConfigurationFileStore.h
    Horo/Platform/DynamicLibrary.h
    Horo/Platform/ExternalProcess.h
    Horo/Platform/PlatformErrors.h
    Horo/Platform/SecureRandom.h
)

horo_configure_target_header_boundary(HoroPlatformServices PUBLIC_HEADERS
    Horo/PlatformServices/AchievementDefinitionRegistry.h
    Horo/PlatformServices/PlatformDefinitionRegistries.h
    Horo/PlatformServices/PlatformProjectConfiguration.h
    Horo/PlatformServices/PlatformRequest.h
    Horo/PlatformServices/PlatformRequestErrors.h
    Horo/PlatformServices/PlatformServiceInterfaces.h
    Horo/PlatformServices/PlatformServicesBackend.h
    Horo/PlatformServices/PlatformServicesFrontend.h
    Horo/PlatformServices/PlatformStableIdRegistry.h
    Horo/PlatformServices/PlatformUserSession.h
)

horo_configure_target_header_boundary(HoroApplication PUBLIC_HEADERS
    Horo/Application/HostObservability.h
    Horo/Application/ProjectCompatibility.h
    Horo/Application/ProjectMigration.h
    Horo/Application/ProjectMigrationCatalog.h
    Horo/Application/ProjectVersion.h
    Horo/Release/DistributionModel.h
    Horo/Release/ReleaseErrors.h
    Horo/Release/ReleaseProfile.h
    Horo/Release/ReleaseVersion.h
)
horo_configure_target_header_boundary(HoroPackages PUBLIC_HEADERS
    Horo/Packages/PackageDependencyResolver.h
    Horo/Packages/PackageLockfile.h
    Horo/Packages/PackagePath.h
    Horo/Packages/PackageArchive.h
    Horo/Packages/PackageCache.h
    Horo/Packages/PackageFileManifest.h
)
horo_configure_target_header_boundary(HoroProjectMigrations)
horo_configure_target_header_boundary(HoroRuntime PUBLIC_HEADERS
    Horo/Runtime/FrameScheduler.h
    Horo/Runtime/RuntimeHost.h
    Horo/Runtime/RuntimeLifecycle.h
    Horo/Runtime/Save/SaveErrors.h
    Horo/Runtime/Save/SaveDiagnostics.h
    Horo/Runtime/Save/SaveIdentity.h
    Horo/Runtime/Save/SaveRootResolver.h
    Horo/Runtime/Save/SaveNamespace.h
    Horo/Runtime/Save/SaveParticipantRegistry.h
    Horo/Runtime/Save/SaveCaptureSnapshot.h
    Horo/Runtime/Save/SaveArchiveMetadata.h
    Horo/Runtime/Save/SaveArchiveFraming.h
    Horo/Runtime/Save/SaveCanonicalCodec.h
    Horo/Runtime/Save/SaveReference.h
    Horo/Runtime/Save/SaveSlotMetadata.h
    Horo/Runtime/Save/SaveOperation.h
    Horo/Runtime/Save/SaveTestCompositions.h
)
horo_configure_target_header_boundary(HoroRuntimeUi PUBLIC_HEADERS
    Horo/Runtime/Ui/UiErrors.h
    Horo/Runtime/Ui/UiIdentity.h
    Horo/Runtime/Ui/UiCanvasSpace.h
    Horo/Runtime/Ui/UiDocument.h
    Horo/Runtime/Ui/UiDiagnostics.h
    Horo/Runtime/Ui/UiElementTree.h
    Horo/Runtime/Ui/UiEventDispatch.h
    Horo/Runtime/Ui/UiLayout.h
    Horo/Runtime/Ui/UiHitTesting.h
    Horo/Runtime/Ui/UiRenderSnapshot.h
    Horo/Runtime/Ui/UiPresentationReceipt.h
)
horo_configure_target_header_boundary(HoroNetworkApi PUBLIC_HEADERS
    Horo/Network/MessageCodecRegistry.h
    Horo/Network/MessageEnvelope.h
    Horo/Network/NetworkAddress.h
    Horo/Network/NetworkErrors.h
    Horo/Network/NetworkFailure.h
    Horo/Network/NetworkIoService.h
    Horo/Network/NetworkLifecycle.h
    Horo/Network/NetworkHandles.h
    Horo/Network/NetworkObjectIdentity.h
    Horo/Network/PacketBuffer.h
    Horo/Network/PacketQueue.h
    Horo/Network/ProtocolIdentity.h
    Horo/Network/ProtocolIdentityRegistry.h
    Horo/Network/ReplicationDescriptor.h
    Horo/Network/ReplicationDescriptorRegistry.h
    Horo/Network/ReplicationIdentity.h
    Horo/Network/ReplicationRoles.h
    Horo/Network/TransportCapabilities.h
    Horo/Network/TransportBudget.h
)
horo_configure_target_header_boundary(HoroNetworkRuntime PUBLIC_HEADERS
    Horo/Network/AuthenticationSessionAdapter.h
    Horo/Network/HandshakeNegotiation.h
    Horo/Network/NetworkObjectMapping.h
    Horo/Network/PeerSessionLifecycle.h
)
horo_configure_target_header_boundary(HoroNetworkTransportNull PUBLIC_HEADERS
    Horo/Network/DeterministicTransport.h
)

horo_configure_target_header_boundary(HoroGameplayApi PUBLIC_HEADERS
    Horo/Gameplay/Behavior.h
    Horo/Gameplay/BehaviorTypes.h
    Horo/Gameplay/Component.h
    Horo/Gameplay/ComponentRegistry.h
    Horo/Gameplay/GameAsset.h
    Horo/Gameplay/GameAssetTypeRegistry.h
    Horo/Gameplay/GameServiceRegistry.h
    Horo/Gameplay/GameModule.h
    Horo/Gameplay/GameplayErrors.h
    Horo/Gameplay/GameplayRegistration.h
    Horo/Gameplay/NativeBehavior.h
    Horo/Gameplay/SystemRegistry.h
)
horo_configure_target_header_boundary(HoroRuntimeScene PUBLIC_HEADERS
    Horo/Runtime/Scene/NavigationSceneComponents.h
    Horo/Runtime/Scene/RuntimeScene.h
    Horo/Runtime/Scene/RuntimeSceneDefinition.h
    Horo/Runtime/Scene/SavedSceneBootstrap.h
)
horo_configure_target_header_boundary(HoroGameplayRuntime PUBLIC_HEADERS
    Horo/Gameplay/BehaviorRegistry.h
    Horo/Gameplay/BehaviorRuntime.h
    Horo/Gameplay/GameplayRegistrationRuntime.h
)
horo_configure_target_header_boundary(HoroGameplayModuleHost PUBLIC_HEADERS
    Horo/Gameplay/GameModuleHost.h
)
horo_configure_target_header_boundary(HoroGameplayBuild PUBLIC_HEADERS
    Horo/Application/CompilerDiagnosticParser.h
    Horo/Application/GameplayBuildService.h
)
horo_configure_target_header_boundary(HoroGameplayLua PUBLIC_HEADERS
    Horo/Gameplay/LuaBehavior.h
)

horo_configure_target_header_boundary(HoroAssets PUBLIC_HEADERS
    Horo/Assets/AssetCook.h
    Horo/Assets/AssetCookCache.h
    Horo/Assets/AssetCookOutput.h
    Horo/Assets/AssetCookService.h
    Horo/Assets/AssetId.h
    Horo/Assets/AssetImportMetadata.h
    Horo/Assets/AssetImportOperation.h
    Horo/Assets/AssetImporter.h
    Horo/Assets/AssetPreview.h
    Horo/Assets/AssetProvider.h
    Horo/Assets/AssetRegistry.h
    Horo/Assets/AssetReimport.h
    Horo/Assets/CookCatalog.h
    Horo/Assets/MeshEditorPayload.h
)
horo_configure_target_header_boundary(HoroAudioApi PUBLIC_HEADERS
    Horo/Audio/AudioAssetSchema.h
    Horo/Audio/AudioBackendCapabilities.h
    Horo/Audio/AudioCallbackEvents.h
    Horo/Audio/AudioDeviceDiscovery.h
    Horo/Audio/AudioDeviceNegotiation.h
    Horo/Audio/AudioDeviceTiming.h
    Horo/Audio/AudioFormat.h
    Horo/Audio/AudioMediaFormatRegistry.h
    Horo/Audio/AudioPlanarBlock.h
    Horo/Audio/AudioErrors.h
    Horo/Audio/AudioIdentity.h
    Horo/Audio/AudioResamplerPlan.h
)
horo_configure_target_header_boundary(HoroAudioDsp PUBLIC_HEADERS
    Horo/Audio/AudioResampler.h
)
horo_configure_target_header_boundary(HoroAudioMemory PUBLIC_HEADERS
    Horo/Audio/AudioMemory.h
)
horo_configure_target_header_boundary(HoroAudioCommands PUBLIC_HEADERS
    Horo/Audio/AudioClock.h
    Horo/Audio/AudioCommands.h
    Horo/Audio/AudioCommandBuffer.h
    Horo/Audio/AudioCommandStaging.h
    Horo/Audio/AudioEventQueue.h
    Horo/Audio/AudioLifecycleReconciler.h
    Horo/Audio/ScheduledAudioCommandBatch.h
)
horo_configure_target_header_boundary(HoroInput PUBLIC_HEADERS
    Horo/Runtime/Input.h
)
horo_configure_target_header_boundary(HoroPhysics PUBLIC_HEADERS
    Horo/Physics/CharacterControllerContracts.h
    Horo/Physics/CharacterErrors.h
    Horo/Physics/CharacterWorld.h
    Horo/Physics/CharacterWorldSettings.h
    Horo/Physics/PhysicsBodyDescriptor.h
    Horo/Physics/PhysicsBodyDynamics.h
    Horo/Physics/PhysicsCapabilities.h
    Horo/Physics/PhysicsCollisionSchema.h
    Horo/Physics/PhysicsConstraintDescriptor.h
    Horo/Physics/PhysicsCookedShapeDescriptor.h
    Horo/Physics/PhysicsDiagnostics.h
    Horo/Physics/PhysicsDeterminismPolicy.h
    Horo/Physics/PhysicsErrors.h
    Horo/Physics/PhysicsFilterIdentity.h
    Horo/Physics/PhysicsIdentity.h
    Horo/Physics/PhysicsMetrics.h
    Horo/Physics/PhysicsPose.h
    Horo/Physics/PhysicsQuery.h
    Horo/Physics/PhysicsShapeDescriptor.h
    Horo/Physics/PhysicsStepPolicy.h
    Horo/Physics/PhysicsTickPipeline.h
    Horo/Physics/PhysicsWorld.h
    Horo/Physics/PhysicsWorldBudgets.h
    Horo/Physics/PhysicsWorldDescriptor.h
    Horo/Physics/PhysicsWorldSettings.h
)

horo_configure_target_header_boundary(HoroPhysicsSceneIntegration PUBLIC_HEADERS
    Horo/Physics/PhysicsSceneActivation.h
)
horo_configure_target_header_boundary(HoroAI PUBLIC_HEADERS
    Horo/AI/AIErrors.h
    Horo/AI/AIIdentity.h
    Horo/AI/AITaskLifecycle.h
    Horo/AI/BlackboardInstance.h
    Horo/AI/BlackboardSchema.h
    Horo/AI/NullAIRuntime.h
    Horo/AI/PerceptionDescriptorRegistry.h
)
horo_configure_target_header_boundary(HoroAnimationApi PUBLIC_HEADERS
    Horo/Animation/AnimationCompression.h
    Horo/Animation/AnimationClip.h
    Horo/Animation/AnimationComponents.h
    Horo/Animation/AnimationErrors.h
    Horo/Animation/AnimationIdentity.h
    Horo/Animation/PoseStorage.h
    Horo/Animation/SkeletonAsset.h
    Horo/Animation/SkeletalMeshSkinning.h
)
horo_configure_target_header_boundary(HoroPCG PUBLIC_HEADERS
    Horo/PCG/PCGErrors.h
    Horo/PCG/PCGGenerationPlan.h
    Horo/PCG/PCGGraphAsset.h
    Horo/PCG/PCGGraphValidation.h
    Horo/PCG/PCGIdentity.h
    Horo/PCG/PCGPointSchema.h
    Horo/PCG/PCGSpatialSnapshot.h
    Horo/PCG/PCGRegistry.h
)
horo_configure_target_header_boundary(HoroVfxApi PUBLIC_HEADERS
    Horo/Vfx/CpuParticleBuffer.h
    Horo/Vfx/CpuParticleSpawnPipeline.h
    Horo/Vfx/ParticleSystemDescriptor.h
    Horo/Vfx/VfxErrors.h
    Horo/Vfx/VfxIdentity.h
    Horo/Vfx/VfxQualityPolicy.h
)
horo_configure_target_header_boundary(HoroDestructionApi PUBLIC_HEADERS
    Horo/Destruction/DestructibleDescriptor.h
    Horo/Destruction/DestructionCommand.h
    Horo/Destruction/DestructionComposition.h
    Horo/Destruction/DestructionErrors.h
    Horo/Destruction/DestructionIdentity.h
    Horo/Destruction/DestructionRegistry.h
    Horo/Destruction/DestructionStateMachine.h
)
horo_configure_target_header_boundary(HoroCinematicModel PUBLIC_HEADERS
    Horo/Cinematic/CurveSampling.h
    Horo/Cinematic/CinematicErrors.h
    Horo/Cinematic/CinematicIdentity.h
    Horo/Cinematic/SequenceAsset.h
    Horo/Cinematic/TransformTrack.h
)
horo_configure_target_header_boundary(HoroCinematicRuntime PUBLIC_HEADERS
    Horo/Cinematic/SequenceEvaluation.h
    Horo/Cinematic/SequenceEvaluationErrors.h
    Horo/Cinematic/SequencePlayer.h
    Horo/Cinematic/SequencePlayerErrors.h
)
horo_configure_target_header_boundary(HoroNavigationApi PUBLIC_HEADERS
    Horo/Navigation/NavMeshData.h
    Horo/Navigation/NavigationBakeInput.h
    Horo/Navigation/NavigationAreas.h
    Horo/Navigation/NavigationAgentProfiles.h
    Horo/Navigation/NavigationBackend.h
    Horo/Navigation/NavigationCapabilities.h
    Horo/Navigation/NavigationErrors.h
    Horo/Navigation/NavigationIdentity.h
    Horo/Navigation/NavigationOutcomes.h
    Horo/Navigation/NavigationProjectProfiles.h
    Horo/Navigation/NavigationSourceGeometry.h
)
horo_configure_target_header_boundary(HoroNavigationRuntime PUBLIC_HEADERS
    Horo/Navigation/NavigationRuntimeQueues.h
    Horo/Navigation/NavigationWorldLifecycle.h
)
horo_configure_target_header_boundary(HoroXRApi PUBLIC_HEADERS
    Horo/XR/XRCapabilities.h
    Horo/XR/XRContract.h
    Horo/XR/XRErrors.h
    Horo/XR/XRIdentity.h
    Horo/XR/XRLoaderPreflight.h
    Horo/XR/XRSpacePose.h
    Horo/XR/XRTrackingSnapshot.h
    Horo/XR/XRViewRenderPlan.h
)
horo_configure_target_header_boundary(HoroTerrainApi PUBLIC_HEADERS
    Horo/Terrain/FoliageDefinition.h
    Horo/Terrain/TerrainDescriptor.h
    Horo/Terrain/TerrainErrors.h
    Horo/Terrain/TerrainIdentity.h
)
horo_configure_target_header_boundary(HoroNavigationNull PUBLIC_HEADERS
    Horo/Navigation/Backends/NullProvider.h
)
horo_configure_target_header_boundary(HoroNavigationRecastDetour PUBLIC_HEADERS
    Horo/Navigation/Backends/RecastDetourProvider.h
)
horo_configure_target_header_boundary(HoroWorldStreaming PUBLIC_HEADERS
    Horo/WorldStreaming/NetworkStreamingAuthority.h
    Horo/WorldStreaming/OriginFrame.h
    Horo/WorldStreaming/RuntimeEntityCellExitOperation.h
    Horo/WorldStreaming/CookedWorldIndexManifest.h
    Horo/WorldStreaming/FallbackStreamingProvider.h
    Horo/WorldStreaming/StreamingBudgetModel.h
    Horo/WorldStreaming/StreamingCellOperation.h
    Horo/WorldStreaming/StreamingCellCandidate.h
    Horo/WorldStreaming/StreamingCellAssetRequest.h
    Horo/WorldStreaming/StreamingCellActivation.h
    Horo/WorldStreaming/StreamingCellState.h
    Horo/WorldStreaming/StreamingCellStability.h
    Horo/WorldStreaming/StreamingSchedulerAdmission.h
    Horo/WorldStreaming/WorldDependencyPlan.h
    Horo/WorldStreaming/WorldAuthoringContract.h
    Horo/WorldStreaming/StreamingDesiredState.h
    Horo/WorldStreaming/StreamingDesiredStateReduction.h
    Horo/WorldStreaming/StreamingPriorityPolicy.h
    Horo/WorldStreaming/StreamingSourceDescriptor.h
    Horo/WorldStreaming/StreamingSourcePrefetch.h
    Horo/WorldStreaming/StreamingSourceRange.h
    Horo/WorldStreaming/WorldCellQuantization.h
    Horo/WorldStreaming/WorldPartitionDescriptor.h
    Horo/WorldStreaming/WorldLayerFiltering.h
    Horo/WorldStreaming/WorldLayerOwnershipModel.h
    Horo/WorldStreaming/WorldLayerState.h
    Horo/WorldStreaming/WorldObjectOwnership.h
    Horo/WorldStreaming/WorldPartitionCapabilityProfile.h
    Horo/WorldStreaming/WorldPartitionRegistry.h
    Horo/WorldStreaming/WorldSpatialAssignment.h
    Horo/WorldStreaming/WorldSpatialObjectDescriptor.h
    Horo/WorldStreaming/WorldSpanningObjectPlan.h
    Horo/WorldStreaming/WorldStreamingRuntimeComposition.h
    Horo/WorldStreaming/WorldStreamingDiagnosticSnapshot.h
    Horo/WorldStreaming/WorldStreamingErrors.h
    Horo/WorldStreaming/WorldStreamingIdentity.h
)

horo_configure_target_header_boundary(HoroPrefab PUBLIC_HEADERS
    Horo/Prefab/PrefabErrors.h
    Horo/Prefab/PrefabIdentity.h
    Horo/Prefab/PrefabLimits.h
)

horo_configure_target_header_boundary(HoroPrefabAuthoring PUBLIC_HEADERS
    Horo/Prefab/PrefabDocument.h
)

horo_configure_target_header_boundary(HoroRenderApi PUBLIC_HEADERS
    Horo/Runtime/Render/Mesh.h
    Horo/Runtime/Render/RenderAdapter.h
    Horo/Runtime/Render/RenderAdapterErrors.h
    Horo/Runtime/Render/RenderBackend.h
    Horo/Runtime/Render/RenderDisplay.h
    Horo/Runtime/Render/RenderDisplayErrors.h
    Horo/Runtime/Render/PresentMode.h
    Horo/Runtime/Render/PresentModeErrors.h
    Horo/Runtime/Render/RenderGraph.h
    Horo/Runtime/Render/RenderGraphExecution.h
    Horo/Runtime/Render/RenderGraphExecutionErrors.h
    Horo/Runtime/Render/RenderGraphErrors.h
    Horo/Runtime/Render/RenderGraphSynchronization.h
    Horo/Runtime/Render/RenderGraphSynchronizationErrors.h
    Horo/Runtime/Render/RenderGraphLifetime.h
    Horo/Runtime/Render/RenderGraphLifetimeErrors.h
    Horo/Runtime/Render/RenderResourceDescriptorErrors.h
    Horo/Runtime/Render/RenderResourceDescriptors.h
    Horo/Runtime/Render/RenderSubmission.h
    Horo/Runtime/Render/ShaderCompilerPipeline.h
    Horo/Runtime/Render/ShaderCompilerPipelineErrors.h
    Horo/Runtime/Render/ShaderManifest.h
    Horo/Runtime/Render/ShaderManifestErrors.h
    Horo/Runtime/Render/TemporalHistory.h
    Horo/Runtime/Render/TemporalHistoryErrors.h
    Horo/Runtime/Render/Texture.h
    Horo/Runtime/Render/RenderResource.h
    Horo/Runtime/Render/RenderScene.h
)
horo_configure_target_header_boundary(HoroRenderBackendRegistry PUBLIC_HEADERS
    Horo/Runtime/Render/RenderBackendRegistry.h
)
horo_configure_target_header_boundary(HoroRenderFrontend PUBLIC_HEADERS
    Horo/Runtime/Render/RenderFrontend.h
    Horo/Runtime/Render/UiRenderComposition.h
)
horo_configure_target_header_boundary(HoroSceneModel PUBLIC_HEADERS
    Horo/Runtime/Scene/PrimitiveCatalog.h
    Horo/Runtime/Scene/PrimitiveMesh.h
    Horo/Runtime/Scene/PrimitiveMeshDescriptor.h
    Horo/Runtime/Scene/SceneComponents.h
)
horo_configure_target_header_boundary(HoroRenderNull PUBLIC_HEADERS
    Horo/Runtime/Render/NullBackendModule.h
)
horo_configure_target_header_boundary(HoroRenderOpenGL)
horo_configure_target_header_boundary(HoroRenderMetal)

horo_configure_target_header_boundary(HoroEditorModel PUBLIC_HEADERS
    Horo/Editor/EditorDataBus.h
)
horo_configure_target_header_boundary(HoroEditorViewportScene)
horo_configure_target_header_boundary(HoroEditorRenderExtraction)

horo_configure_target_header_boundary(HoroEditorServices PUBLIC_HEADERS
    Horo/Editor/ActivityBarLayout.h
    Horo/Editor/EditorConfiguration.h
    Horo/Editor/EditorMenuModel.h
    Horo/Editor/EditorModalHost.h
    Horo/Editor/EditorServiceRegistry.h
    Horo/Editor/EditorSettingsEvents.h
    Horo/Editor/EditorSettingsService.h
    Horo/Editor/EditorSettingsStore.h
    Horo/Editor/EditorStatusBarModel.h
    Horo/Editor/EditorWorkspaceEvents.h
    Horo/Editor/GuiRoute.h
    Horo/Editor/HierarchyModel.h
    Horo/Editor/Localization/ILocalizationService.h
    Horo/Editor/Localization/LocalizationService.h
    Horo/Editor/Localization/LocalizationTypes.h
    Horo/Editor/NotificationService.h
    Horo/Editor/ProjectCreationController.h
    Horo/Editor/ProjectCreationService.h
    Horo/Editor/ProjectIntegrityValidatorService.h
    Horo/Editor/ProjectMigrationTransaction.h
    Horo/Editor/ProjectMutation.h
    Horo/Editor/ProjectOpenService.h
    Horo/Editor/ProjectSession.h
    Horo/Editor/RecentProject.h
    Horo/Editor/RecentProjectInspectionService.h
    Horo/Editor/WelcomeController.h
    Horo/Editor/WorkspaceDockArea.h
    Horo/Editor/WorkspaceLayout.h
    Horo/Editor/WorkspaceLayoutPersistence.h
    Horo/Editor/WorkspacePanelHost.h
)

horo_configure_target_header_boundary(HoroEditorViewportOpenGL)
horo_configure_target_header_boundary(HoroEditorViewportMetal)
horo_configure_target_header_boundary(HoroInputSdl)
horo_configure_target_header_boundary(HoroGui PUBLIC_HEADERS
    Horo/Editor/AssetImportModal.h
    Horo/Editor/DefaultScreenFactories.h
    Horo/Editor/DefaultWorkspacePanels.h
    Horo/Editor/DesignSystem/DesignTokens.h
    Horo/Editor/EditorGuiContext.h
    Horo/Editor/EditorIcons.h
    Horo/Editor/EditorSnackbarHost.h
    Horo/Editor/EditorTheme.h
    Horo/Editor/EditorUiComponents.h
    Horo/Editor/GuiScreen.h
    Horo/Editor/GuiScreenHost.h
    Horo/Editor/IWorkspacePanel.h
    Horo/Editor/IGlobalDockPane.h
    Horo/Editor/ScreenRegistry.h
    Horo/Editor/SettingsModal.h
    Horo/Editor/SettingsModalDraft.h
    Horo/Editor/WorkspacePanelRegistry.h
)

horo_configure_target_header_boundary(HoroExtensions PUBLIC_HEADERS
    Horo/Extensions/ApplicationCapabilityRegistry.h
    Horo/Extensions/BackendServiceRegistry.h
    Horo/Extensions/ExtensionAbi.h
    Horo/Extensions/ExtensionCapabilityAdmission.h
    Horo/Extensions/ExtensionActivationState.h
    Horo/Extensions/ExtensionDiscovery.h
    Horo/Extensions/ExtensionErrors.h
    Horo/Extensions/ExtensionInventory.h
    Horo/Extensions/ExtensionManager.h
    Horo/Extensions/ExtensionManifest.h
    Horo/Extensions/ExtensionModuleResolution.h
    Horo/Extensions/ExtensionMarketplace.h
    Horo/Extensions/PipelineStepRegistry.h
    Horo/Extensions/ProjectValidatorRegistry.h
    Horo/Extensions/ToolchainProviderRegistry.h
)

horo_verify_public_header_inventory()
message(STATUS "Target-specific public header inventory is complete")
