add_library(HoroFoundation STATIC
        foundation/assertions/Assertions.cpp
        foundation/AssetCookTargetId.cpp
        foundation/FoundationErrors.cpp
        foundation/modules/ModuleActivationContext.cpp
        foundation/modules/ModuleDescriptor.cpp
        foundation/modules/ModuleHost.cpp
        foundation/modules/ModuleHostLifecycle.cpp
        foundation/modules/ModuleHostRegistration.cpp
        foundation/error/ErrorCode.cpp
        foundation/error/ErrorCodeRegistry.cpp
        foundation/error/ValidationResult.cpp
        foundation/diagnostics/DiagnosticBundle.cpp
        foundation/hashing/Sha256.cpp
        foundation/math/SceneMath.cpp
        foundation/math/WorldCoordinate64.cpp
        foundation/text/String.cpp
        foundation/text/Utf8.cpp
        foundation/logging/Logger.cpp
        foundation/logging/LogContext.cpp
        foundation/logging/StructuredLogStore.cpp
        foundation/telemetry/Telemetry.cpp
        foundation/telemetry/Operation.cpp
        foundation/diagnostics/BuildOutputStore.cpp
        foundation/diagnostics/OperationStore.cpp
        foundation/diagnostics/OperationHistory.cpp
        foundation/data_bus/DataBus.cpp
        foundation/jobs/JobSystem.cpp
        foundation/config/Configuration.cpp
        foundation/diagnostics/DiagnosticsEngine.cpp
        foundation/paths/PathUtils.cpp
)

add_library(HoroEngine::Foundation ALIAS HoroFoundation)

target_compile_features(HoroFoundation PUBLIC cxx_std_20)
target_compile_definitions(HoroFoundation PUBLIC HORO_ENABLE_TELEMETRY=$<BOOL:${HORO_ENABLE_TELEMETRY}>)
target_include_directories(HoroFoundation
        PUBLIC
        ${PROJECT_SOURCE_DIR}/include
        PRIVATE
        $<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>
)
add_library(HoroSecurity STATIC
        security/AgentPolicy.cpp
        security/SecurityErrors.cpp
        security/SecureMemory.cpp
        security/CredentialStore.cpp
        security/ArtifactSignature.cpp
        security/MbedTlsSignatureProvider.cpp
)
add_library(HoroEngine::Security ALIAS HoroSecurity)
target_compile_features(HoroSecurity PUBLIC cxx_std_20)
target_link_libraries(HoroSecurity PUBLIC HoroEngine::Foundation PRIVATE MbedTLS::mbedcrypto)

add_library(HoroCliHost STATIC
        cli/CliCommandRegistry.cpp
        cli/CliCommonOptions.cpp
        cli/CliDispatcher.cpp
        cli/CliExecutionLifecycle.cpp
        cli/CliErrors.cpp
        cli/CliOptionParser.cpp
)
add_library(HoroEngine::CliHost ALIAS HoroCliHost)
target_compile_features(HoroCliHost PUBLIC cxx_std_20)
target_link_libraries(HoroCliHost
        PUBLIC HoroEngine::Foundation
        PRIVATE nlohmann_json::nlohmann_json
)

add_library(HoroMcpSession STATIC
        mcp/McpErrors.cpp
        mcp/McpJsonBounds.cpp
        mcp/McpSession.cpp
        mcp/McpInProcessAdapter.cpp
        mcp/McpLocalTransport.cpp
)
add_library(HoroEngine::McpSession ALIAS HoroMcpSession)
target_compile_features(HoroMcpSession PUBLIC cxx_std_20)
target_link_libraries(HoroMcpSession PUBLIC HoroEngine::Foundation nlohmann_json::nlohmann_json)

add_library(HoroMcpRegistry STATIC mcp/McpToolRegistry.cpp)
add_library(HoroEngine::McpRegistry ALIAS HoroMcpRegistry)
target_compile_features(HoroMcpRegistry PUBLIC cxx_std_20)
target_link_libraries(HoroMcpRegistry PUBLIC HoroEngine::McpSession)

add_library(HoroMcpController STATIC mcp/McpController.cpp)
add_library(HoroEngine::McpController ALIAS HoroMcpController)
target_compile_features(HoroMcpController PUBLIC cxx_std_20)
target_link_libraries(HoroMcpController PUBLIC HoroEngine::McpRegistry)

add_library(HoroModelProvider STATIC editor/agent/ModelProvider.cpp)
add_library(HoroEngine::ModelProvider ALIAS HoroModelProvider)
target_compile_features(HoroModelProvider PUBLIC cxx_std_20)

add_library(HoroModelProviderAdapters STATIC editor/agent/ReferenceModelProviders.cpp editor/agent/ReferenceModelStream.cpp)
add_library(HoroEngine::ModelProviderAdapters ALIAS HoroModelProviderAdapters)
target_compile_features(HoroModelProviderAdapters PUBLIC cxx_std_20)
target_link_libraries(HoroModelProviderAdapters
        PUBLIC HoroEngine::ModelProvider
        PRIVATE nlohmann_json::nlohmann_json CURL::libcurl)

if (HORO_ENABLE_OPENTELEMETRY)
    add_library(HoroOpenTelemetry STATIC
            foundation/telemetry/OpenTelemetrySink.cpp
    )
    add_library(HoroEngine::OpenTelemetry ALIAS HoroOpenTelemetry)
    target_compile_features(HoroOpenTelemetry PUBLIC cxx_std_20)
    target_include_directories(HoroOpenTelemetry
            PUBLIC
            ${PROJECT_SOURCE_DIR}/include
    )
    target_link_libraries(HoroOpenTelemetry
            PUBLIC
            HoroEngine::Foundation
            PRIVATE
            nlohmann_json::nlohmann_json
            CURL::libcurl
    )
endif ()

add_library(HoroPlatform STATIC
        platform/Platform.cpp
        platform/ProductProbeLease.cpp
        platform/PlatformErrors.cpp
        platform/ConfigurationFileStore.cpp
        platform/android/AndroidLifecycle.cpp
        platform/android/AndroidLifecycleAdapter.cpp
)

if (WIN32)
    target_sources(HoroPlatform PRIVATE
            platform/windows/DynamicLibrary_Windows.cpp
            platform/windows/ExternalProcess_Windows.cpp
            platform/windows/SecureRandom_Windows.cpp)
    target_link_libraries(HoroPlatform PRIVATE bcrypt)
else ()
    target_sources(HoroPlatform PRIVATE
            platform/posix/DynamicLibrary_Posix.cpp
            platform/posix/ExternalProcess_Posix.cpp
            platform/posix/SecureRandom_Posix.cpp)
    target_link_libraries(HoroPlatform PRIVATE dl)
    if (APPLE)
        target_link_libraries(HoroPlatform PRIVATE "-framework Security")
    endif ()
endif ()

add_library(HoroEngine::Platform ALIAS HoroPlatform)

target_compile_features(HoroPlatform PUBLIC cxx_std_20)
target_include_directories(HoroPlatform
        PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)
target_link_libraries(HoroPlatform PUBLIC HoroEngine::Foundation HoroEngine::Security)

add_library(HoroPlatformServices STATIC
        platform_services/AchievementDefinitionRegistry.cpp
        platform_services/PlatformCloudObjects.cpp
        platform_services/PlatformAchievementCoordinator.cpp
        platform_services/PlatformOfflineQueue.cpp
        platform_services/PlatformOfflineQueueAdmission.cpp
        platform_services/PlatformOfflineQueueErrors.cpp
        platform_services/PlatformOfflineQueueLifecycle.cpp
        platform_services/PlatformDefinitionErrors.cpp
        platform_services/PlatformDefinitionRegistries.cpp
        platform_services/PlatformPresenceCoordinator.cpp
        platform_services/PlatformOfflineQueueStorageErrors.cpp
        platform_services/PlatformOfflineQueueStorage.cpp
        platform_services/PlatformProgressionErrors.cpp
        platform_services/PlatformProgressionIdempotency.cpp
        platform_services/PlatformProjectConfiguration.cpp
        platform_services/PlatformProviderManifestCook.cpp
        platform_services/PlatformRequest.cpp
        platform_services/PlatformRequestCompletion.cpp
        platform_services/PlatformRequestErrors.cpp
        platform_services/PlatformServiceErrors.cpp
        platform_services/PlatformServicesBackend.cpp
        platform_services/PlatformServicesFrontend.cpp
        platform_services/PlatformSessionObserver.cpp
        platform_services/PlatformServicesMetrics.cpp
        platform_services/PlatformStatCacheErrors.cpp
        platform_services/PlatformStatCacheCoordinator.cpp
        platform_services/PlatformStableIdRegistry.cpp
        platform_services/PlatformUserSession.cpp
)
add_library(HoroEngine::PlatformServices ALIAS HoroPlatformServices)
target_compile_features(HoroPlatformServices PUBLIC cxx_std_20)
target_link_libraries(HoroPlatformServices PUBLIC HoroEngine::Foundation HoroEngine::Platform)

add_library(HoroPackages STATIC
        packages/cache/PackageCache.cpp
        packages/resolver/PackageDependencyResolver.cpp
        packages/resolver/PackageLockfile.cpp
        packages/validation/PackagePath.cpp
        packages/validation/PackageArchive.cpp
        packages/validation/PackageFileManifest.cpp
        packages/validation/PackageValidationDetail.cpp
)
add_library(HoroEngine::Packages ALIAS HoroPackages)
target_compile_features(HoroPackages PUBLIC cxx_std_20)
target_link_libraries(HoroPackages PUBLIC HoroEngine::Foundation PRIVATE utf8proc nlohmann_json::nlohmann_json miniz)

add_library(HoroPackageSecurity STATIC
        packages/restore/PackageInstall.cpp
        packages/restore/PackageInstallErrors.cpp
        packages/restore/PackageRestore.cpp
        packages/restore/PackageRestoreService.cpp
        packages/restore/PackageRestoreErrors.cpp
        packages/verification/PackagePublisherVerification.cpp
        packages/verification/PackagePublisherVerificationErrors.cpp
)
add_library(HoroEngine::PackageSecurity ALIAS HoroPackageSecurity)
target_compile_features(HoroPackageSecurity PUBLIC cxx_std_20)
target_link_libraries(HoroPackageSecurity PUBLIC HoroEngine::Foundation HoroPackages HoroEngine::Security PRIVATE nlohmann_json::nlohmann_json)

add_library(HoroApplication STATIC
        application/observability/HostObservability.cpp
        application/project/ProjectErrors.cpp
        application/project/ProjectVersion.cpp
        application/project/ProjectCompatibility.cpp
        application/project_migration/ProjectMigrationRegistry.cpp
        application/project_migration/ProjectMigrationExecution.cpp
        application/release/DistributionModel.cpp
        application/release/ReleaseArtifactManifest.cpp
        application/release/ReleaseCandidateVerification.cpp
        application/release/ReleaseArtifactVerification.cpp
        application/release/ReleaseChecksums.cpp
        application/release/ReleaseBuildProvenance.cpp
        application/release/ReleaseBuildProvenanceCapture.cpp
        application/release/ReleaseErrors.cpp
        application/release/ReleaseJobTracker.cpp
        application/release/ReleasePipelineExecutor.cpp
        application/release/ReleasePackageProducer.cpp
        application/release/ReleasePublication.cpp
        application/release/ReleaseSigningBoundary.cpp
        application/release/ReleaseProfile.cpp
        application/release/ReleasePreflight.cpp
        application/release/ReleaseTargetMatrix.cpp
        application/release/ReleaseVersion.cpp
)
add_library(HoroEngine::Application ALIAS HoroApplication)
target_compile_features(HoroApplication PUBLIC cxx_std_20)
target_include_directories(HoroApplication
        PUBLIC ${PROJECT_SOURCE_DIR}/include
        PRIVATE ${HORO_GENERATED_DIR}
)
target_link_libraries(HoroApplication
        PUBLIC HoroEngine::Foundation
        PRIVATE nlohmann_json::nlohmann_json
)
add_dependencies(HoroApplication HoroProjectCompatibilityCatalog)

add_library(HoroReleaseProcess STATIC
        application/release/ReleaseProcess.cpp
        application/release/ReleaseCandidatePublisher.cpp)
add_library(HoroEngine::ReleaseProcess ALIAS HoroReleaseProcess)
target_compile_features(HoroReleaseProcess PUBLIC cxx_std_20)
target_link_libraries(HoroReleaseProcess PUBLIC HoroEngine::Application HoroEngine::Platform)

add_library(HoroReleaseService STATIC application/release/ReleaseService.cpp application/release/ReleaseRunHistory.cpp)
add_library(HoroEngine::ReleaseService ALIAS HoroReleaseService)
target_compile_features(HoroReleaseService PUBLIC cxx_std_20)
target_link_libraries(HoroReleaseService PUBLIC HoroEngine::Application HoroEngine::Foundation PRIVATE nlohmann_json::nlohmann_json)

add_library(HoroReleaseGitHub STATIC application/release/GitHubReleasePublication.cpp)
add_library(HoroEngine::ReleaseGitHub ALIAS HoroReleaseGitHub)
target_compile_features(HoroReleaseGitHub PUBLIC cxx_std_20)
target_link_libraries(HoroReleaseGitHub PUBLIC HoroEngine::Application PRIVATE nlohmann_json::nlohmann_json)

add_library(HoroReleaseGitHubCli STATIC application/release/GitHubReleaseCliClient.cpp)
add_library(HoroEngine::ReleaseGitHubCli ALIAS HoroReleaseGitHubCli)
target_compile_features(HoroReleaseGitHubCli PUBLIC cxx_std_20)
target_link_libraries(HoroReleaseGitHubCli PUBLIC HoroEngine::ReleaseGitHub HoroEngine::Platform
        PRIVATE nlohmann_json::nlohmann_json)
if(WIN32)
    target_link_libraries(HoroReleaseGitHubCli PRIVATE advapi32)
endif()

add_library(HoroUpdateManifest STATIC application/release/UpdateManifest.cpp application/release/UpdateManifestErrors.cpp
        application/release/UpdateSignatureCodec.cpp application/release/UpdateTrustRoot.cpp)
add_library(HoroEngine::UpdateManifest ALIAS HoroUpdateManifest)
target_compile_features(HoroUpdateManifest PUBLIC cxx_std_20)
target_link_libraries(HoroUpdateManifest PUBLIC HoroEngine::Application HoroEngine::Security PRIVATE nlohmann_json::nlohmann_json)

add_library(HoroUpdateDiscovery STATIC application/release/UpdateDiscovery.cpp application/release/UpdateDiscoveryErrors.cpp)
add_library(HoroEngine::UpdateDiscovery ALIAS HoroUpdateDiscovery)
target_compile_features(HoroUpdateDiscovery PUBLIC cxx_std_20)
target_link_libraries(HoroUpdateDiscovery PUBLIC HoroEngine::UpdateManifest)

add_library(HoroUpdateTransfer STATIC application/release/UpdateTransfer.cpp application/release/UpdateTransferErrors.cpp
        application/release/UpdateArchiveIndex.cpp application/release/UpdateStagedTree.cpp application/release/UpdateFileDelta.cpp
        application/release/UpdateTransferCheckpointStore.cpp)
add_library(HoroEngine::UpdateTransfer ALIAS HoroUpdateTransfer)
target_compile_features(HoroUpdateTransfer PUBLIC cxx_std_20)
target_link_libraries(HoroUpdateTransfer PUBLIC HoroEngine::UpdateManifest)

add_library(HoroUpdateDownload STATIC application/release/UpdateDownloadSession.cpp
        application/release/UpdateHttpResponse.cpp application/release/UpdateHttpDownload.cpp
        application/release/UpdateStageReady.cpp application/release/UpdateStageFileOperations.cpp
        application/release/UpdateZipStaging.cpp
        application/release/UpdateTarGzipIndex.cpp
        application/release/UpdateTarGzipStaging.cpp
        application/release/UpdateDeltaStaging.cpp
        application/release/UpdateDeltaZipRepack.cpp
        application/release/UpdateSelectedZipStaging.cpp
        application/release/UpdateZipPackageProducer.cpp application/release/UpdateZipStagingJob.cpp
        application/release/UpdateTarGzipStagingJob.cpp)
add_library(HoroEngine::UpdateDownload ALIAS HoroUpdateDownload)
target_compile_features(HoroUpdateDownload PUBLIC cxx_std_20)
target_compile_definitions(HoroUpdateDownload PRIVATE $<$<PLATFORM_ID:Windows>:NOMINMAX>)
target_link_libraries(HoroUpdateDownload PUBLIC HoroEngine::UpdateTransfer HoroEngine::Platform PRIVATE CURL::libcurl miniz)

add_library(HoroUpdateOfflineSource STATIC application/release/UpdateOfflineSource.cpp application/release/UpdateOfflineSourceErrors.cpp)
add_library(HoroEngine::UpdateOfflineSource ALIAS HoroUpdateOfflineSource)
target_compile_features(HoroUpdateOfflineSource PUBLIC cxx_std_20)
target_link_libraries(HoroUpdateOfflineSource PUBLIC HoroEngine::UpdateDiscovery HoroEngine::UpdateDownload HoroEngine::Platform)

add_library(HoroUpdateActivation STATIC application/release/UpdateActivation.cpp application/release/UpdateActivationErrors.cpp
        application/release/UpdateProductProcess.cpp
        application/release/BootstrapInstallation.cpp application/release/BootstrapInstallationErrors.cpp
        application/release/PortableBootstrapOwnedFiles.cpp
        application/release/UpdateRollback.cpp application/release/UpdateRollbackErrors.cpp
        application/release/UpdateRetention.cpp application/release/UpdateRetentionErrors.cpp)
add_library(HoroEngine::UpdateActivation ALIAS HoroUpdateActivation)
target_compile_features(HoroUpdateActivation PUBLIC cxx_std_20)
target_link_libraries(HoroUpdateActivation PUBLIC HoroEngine::UpdateDownload HoroEngine::Platform)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(HoroUpdateActivation PRIVATE application/release/LinuxPortableBootstrapHost.cpp)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows" OR CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    target_sources(HoroUpdateActivation PRIVATE application/release/ZipPortableBootstrapHost.cpp)
endif()

add_library(HoroUserStateMigration STATIC application/release/UserStateMigration.cpp
        application/release/UserStateMigrationErrors.cpp)
add_library(HoroEngine::UserStateMigration ALIAS HoroUserStateMigration)
target_compile_features(HoroUserStateMigration PUBLIC cxx_std_20)
target_link_libraries(HoroUserStateMigration PUBLIC HoroEngine::Platform)

add_library(HoroProjectMigrations STATIC
        ${HORO_PROJECT_MIGRATION_CATALOG_CPP}
        ${HORO_PROJECT_MIGRATION_SOURCES}
)
add_library(HoroEngine::ProjectMigrations ALIAS HoroProjectMigrations)
target_compile_features(HoroProjectMigrations PUBLIC cxx_std_20)
target_include_directories(HoroProjectMigrations
        PUBLIC ${PROJECT_SOURCE_DIR}/include
        PRIVATE ${PROJECT_SOURCE_DIR} ${HORO_GENERATED_DIR}
)
target_link_libraries(HoroProjectMigrations PUBLIC HoroEngine::Application PRIVATE nlohmann_json::nlohmann_json)
add_dependencies(HoroProjectMigrations HoroProjectMigrationCatalog)

add_library(HoroRuntime STATIC
        runtime/frame/FrameScheduler.cpp
        runtime/lifecycle/RuntimeLifecycle.cpp
        runtime/lifecycle/RuntimeHost.cpp
        runtime/lifecycle/RuntimeErrors.cpp
        runtime/save/SaveErrors.cpp
        runtime/save/SaveProtectionErrors.cpp
        runtime/save/SaveDiagnostics.cpp
        runtime/save/SaveIdentity.cpp
        runtime/save/SaveRootResolver.cpp
        runtime/save/SaveFilesystemStorage.cpp
        runtime/save/SaveNamespace.cpp
        runtime/save/SaveParticipantRegistry.cpp
        runtime/save/SaveParticipation.cpp
        runtime/save/SaveCaptureSnapshot.cpp
        runtime/save/SaveRestoreTransaction.cpp
        runtime/save/SaveArchiveMetadata.cpp
        runtime/save/SaveCompatibility.cpp
        runtime/save/SaveMigration.cpp
        runtime/save/SaveMigrationExecutor.cpp
        runtime/save/SaveMigrationPlanner.cpp
        runtime/save/SaveMigrationRegistry.cpp
        runtime/save/SaveMigrationValidation.cpp
        runtime/save/SaveArchiveFraming.cpp
        runtime/save/SaveChunkCompression.cpp
        runtime/save/SaveArchiveFinalization.cpp
        runtime/save/SaveArchiveFraming.cpp
        runtime/save/SaveArchiveReader.cpp
        runtime/save/SaveArchiveReaderEnvelope.cpp
        runtime/save/SaveArchiveReaderValidation.cpp
        runtime/save/SaveArchiveProtection.cpp
        runtime/save/SaveArchiveAuthenticity.cpp
        runtime/save/SaveCanonicalCodec.cpp
        runtime/save/SaveCanonicalScalarCodec.cpp
        runtime/save/SaveCanonicalCompositeWriter.cpp
        runtime/save/SaveCanonicalCompositeReader.cpp
        runtime/save/SaveReference.cpp
        runtime/save/SaveSlotMetadata.cpp
        runtime/save/SaveSlotIndex.cpp
        runtime/save/SaveManagerProjection.cpp
        runtime/save/SaveManagerProjectionErrors.cpp
        runtime/save/SaveStorageAdapter.cpp
        runtime/save/SaveSlotCommitTransaction.cpp
        runtime/save/SaveSlotRecovery.cpp
        runtime/save/SaveStoragePolicy.cpp
        runtime/save/SaveOperation.cpp
        runtime/save/SaveOperationArbiter.cpp
        runtime/save/SaveProjectPolicy.cpp
        runtime/save/SaveSafePointCoordinator.cpp
        runtime/save/SaveCaptureBarrier.cpp
        runtime/save/SaveTestCompositions.cpp
)
add_library(HoroEngine::Runtime ALIAS HoroRuntime)
target_compile_features(HoroRuntime PUBLIC cxx_std_20)
target_include_directories(HoroRuntime PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(HoroRuntime PUBLIC HoroEngine::Foundation PRIVATE nlohmann_json::nlohmann_json utf8proc miniz)

add_library(HoroNetworkApi STATIC
        runtime/networking/api/MessageCodecRegistry.cpp
        runtime/networking/api/MessageDeliveryGate.cpp
        runtime/networking/api/NetworkAddress.cpp
        runtime/networking/api/NetworkErrors.cpp
        runtime/networking/api/NetworkTransportErrors.cpp
        runtime/networking/api/NetworkFailure.cpp
        runtime/networking/api/NetworkIoService.cpp
        runtime/networking/api/NetworkLifecycle.cpp
        runtime/networking/api/NetworkMetrics.cpp
        runtime/networking/api/NetworkMetricTelemetry.cpp
        runtime/networking/api/NetworkMetricTransport.cpp
        runtime/networking/api/NetworkObjectIdentity.cpp
        runtime/networking/api/NetworkProjectSettings.cpp
        runtime/networking/api/NetworkProjectSettingsSchema.cpp
        runtime/networking/api/NetworkProjectSettingsErrors.cpp
        runtime/networking/api/NetworkTargetCapabilities.cpp
        runtime/networking/api/NetworkTargetManifest.cpp
        runtime/networking/api/NetworkTargetManifestErrors.cpp
        runtime/networking/api/PacketBuffer.cpp
        runtime/networking/api/PacketQueue.cpp
        runtime/networking/api/ProtocolIdentityRegistry.cpp
        runtime/networking/api/ReplicationDescriptor.cpp
        runtime/networking/api/ReplicationDescriptorRegistry.cpp
        runtime/networking/api/ReplicationSerializer.cpp
        runtime/networking/api/ReplicationSerializerErrors.cpp
        runtime/networking/api/ReplicationRoles.cpp
        runtime/networking/api/RpcDescriptor.cpp
        runtime/networking/api/RpcDescriptorErrors.cpp
        runtime/networking/api/RpcDescriptorRegistry.cpp
        runtime/networking/api/TransportCapabilities.cpp
        runtime/networking/api/TransportBackendErrors.cpp
        runtime/networking/api/TransportBudget.cpp
)
add_library(HoroEngine::NetworkApi ALIAS HoroNetworkApi)
target_compile_features(HoroNetworkApi PUBLIC cxx_std_20)
target_link_libraries(HoroNetworkApi PUBLIC HoroEngine::Foundation)
target_include_directories(HoroNetworkApi PRIVATE $<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>)

add_library(HoroNetworkTransportNull STATIC
        runtime/networking/null/DeterministicTransport.cpp
        runtime/networking/null/NullTransportBackend.cpp
)
add_library(HoroEngine::NetworkTransportNull ALIAS HoroNetworkTransportNull)
target_compile_features(HoroNetworkTransportNull PUBLIC cxx_std_20)
target_link_libraries(HoroNetworkTransportNull PUBLIC HoroEngine::NetworkApi)

if(HORO_BUILD_NETWORK_GNS)
    add_library(HoroNetworkTransportGNSFactoryInternal INTERFACE)
    target_include_directories(HoroNetworkTransportGNSFactoryInternal INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/runtime/networking/gns/factory>)
    target_link_libraries(HoroNetworkTransportGNSFactoryInternal INTERFACE HoroEngine::NetworkApi)
    add_library(HoroNetworkTransportGNS STATIC
        runtime/networking/gns/GnsAddressResolution.h
        runtime/networking/gns/GnsAddressResolution.cpp
        runtime/networking/gns/factory/GnsTransportFactory.h
        runtime/networking/gns/GnsTransportInternal.h
        runtime/networking/gns/GnsTransport.cpp
        runtime/networking/gns/GnsTransportPackets.cpp
        runtime/networking/gns/GnsTransportDiagnostics.cpp
    )
    add_library(HoroEngine::NetworkTransportGNS ALIAS HoroNetworkTransportGNS)
    target_compile_features(HoroNetworkTransportGNS PUBLIC cxx_std_20)
    target_compile_definitions(HoroNetworkTransportGNS PRIVATE $<$<PLATFORM_ID:Windows>:NOMINMAX>)
    target_link_libraries(HoroNetworkTransportGNS PUBLIC HoroNetworkTransportGNSFactoryInternal
        PRIVATE GameNetworkingSockets::static c-ares::cares)
endif()

add_library(HoroRuntimeUi STATIC
        runtime/ui/UiAssetDependencyInternal.h
        runtime/ui/UiErrors.cpp
        runtime/ui/UiAssetErrors.cpp
        runtime/ui/UiImageResourceErrors.cpp
        runtime/ui/UiImageResource.cpp
        runtime/ui/UiFocusErrors.cpp
        runtime/ui/UiAccessibilityErrors.cpp
        runtime/ui/UiControlErrors.cpp
        runtime/ui/UiIdentity.cpp
        runtime/ui/UiBinding.cpp
        runtime/ui/UiBindingFingerprint.cpp
        runtime/ui/UiBindingValidation.cpp
        runtime/ui/UiBindingStore.cpp
        runtime/ui/UiBindingWrites.cpp
        runtime/ui/UiBindingWriteAdmission.cpp
        runtime/ui/UiBindingStorePreparation.cpp
        runtime/ui/UiCanvasSpace.cpp
        runtime/ui/UiLocalization.cpp
        runtime/ui/UiDocument.cpp
        runtime/ui/UiRuntimeInstance.cpp
        runtime/ui/UiDocumentCook.cpp
        runtime/ui/UiAssetLoading.cpp
        runtime/ui/UiAssetLoadingService.cpp
        runtime/ui/UiDocumentSerialization.cpp
        runtime/ui/UiDocumentLocalizationSerialization.cpp
        runtime/ui/UiDocumentSerializationOperations.cpp
        runtime/ui/UiDocumentSerializationErrors.cpp
        runtime/ui/UiDiagnostics.cpp
        runtime/ui/UiDiagnosticDescriptors.cpp
        runtime/ui/UiAccessibility.cpp
        runtime/ui/UiAccessibilityReading.cpp
        runtime/ui/UiAccessibilityInteraction.cpp
        runtime/ui/UiAccessibilityChanges.cpp
        runtime/ui/UiAccessibilityAnnouncementQueue.cpp
        runtime/ui/UiAccessibilityPolicy.cpp
        runtime/ui/UiAccessibilityStorage.cpp
        runtime/ui/UiElementTree.cpp
        runtime/ui/UiEventDispatch.cpp
        runtime/ui/UiPointerCapture.cpp
        runtime/ui/UiPointerCaptureErrors.cpp
        runtime/ui/UiActions.cpp
        runtime/ui/UiActionRouter.cpp
        runtime/ui/UiAsyncActions.cpp
        runtime/ui/UiAsyncActionErrors.cpp
        runtime/ui/UiFeedback.cpp
        runtime/ui/UiFeedbackErrors.cpp
        runtime/ui/UiScreenStack.cpp
        runtime/ui/UiRouteOperations.cpp
        runtime/ui/UiFocusGraph.cpp
        runtime/ui/UiFocusGraphOperations.cpp
        runtime/ui/UiFocusGraphParticipation.cpp
        runtime/ui/UiFocusSpatialNavigation.cpp
        runtime/ui/UiControls.cpp
        runtime/ui/UiControlStateMachine.cpp
        runtime/ui/UiControlStateMachineInput.cpp
        runtime/ui/UiLayout.cpp
        runtime/ui/UiLayoutClipping.cpp
        runtime/ui/UiLayoutClippingEngine.cpp
        runtime/ui/UiLayoutSemantics.cpp
        runtime/ui/UiLayoutArrangement.cpp
        runtime/ui/UiLayoutArrangementInternal.h
        runtime/ui/UiLayoutArrangementSupport.cpp
        runtime/ui/UiLayoutFlowArrangement.cpp
        runtime/ui/UiLayoutGridArrangement.cpp
        runtime/ui/UiLayoutTypes.cpp
        runtime/ui/UiTextLayoutErrors.cpp
        runtime/ui/UiTextLayout.cpp
        runtime/ui/UiTextLayoutOutput.cpp
        runtime/ui/UiTextLayoutPlanning.cpp
        runtime/ui/UiTextLayoutPublication.cpp
        runtime/ui/UiTextLayoutTypes.cpp
        runtime/ui/UiGlyphAtlas.cpp
        runtime/ui/UiGlyphAtlasStorage.cpp
        runtime/ui/UiGlyphAtlasValidation.cpp
        runtime/ui/UiStyle.cpp
        runtime/ui/UiStyleHash.cpp
        runtime/ui/UiStyleRegistry.cpp
        runtime/ui/UiStyleResolution.cpp
        runtime/ui/UiStyleResolver.cpp
        runtime/ui/UiStyleSnapshot.cpp
        runtime/ui/UiStyleValidation.cpp
        runtime/ui/UiHitTesting.cpp
        runtime/ui/UiRenderSnapshot.cpp
        runtime/ui/UiRenderGeometry.cpp
        runtime/ui/UiRenderGeometryInternal.cpp
        runtime/ui/UiPresentationReceipt.cpp
        runtime/ui/UiTextShaping.cpp
        runtime/ui/UiTextErrors.cpp
        runtime/ui/UiTextShape.cpp
        runtime/ui/UiTextShaper.cpp
)
add_library(HoroEngine::RuntimeUi ALIAS HoroRuntimeUi)
target_compile_features(HoroRuntimeUi PUBLIC cxx_std_20)
target_include_directories(HoroRuntimeUi PRIVATE ${PROJECT_SOURCE_DIR}/src/foundation/json)
target_link_libraries(HoroRuntimeUi PUBLIC HoroEngine::Foundation HoroEngine::Assets
        PRIVATE nlohmann_json::nlohmann_json utf8proc harfbuzz)

add_library(HoroUiTemplateGraph STATIC
        ui_templates/UiTemplateDependencyGraph.cpp
        ui_templates/UiTemplateErrors.cpp
)
add_library(HoroEngine::UiTemplateGraph ALIAS HoroUiTemplateGraph)
target_compile_features(HoroUiTemplateGraph PUBLIC cxx_std_20)
target_link_libraries(HoroUiTemplateGraph PUBLIC HoroEngine::Foundation HoroEngine::Assets HoroEngine::Packages)

add_library(HoroGameplayApi STATIC
        gameplay/api/BehaviorTypes.cpp
        gameplay/api/GameAsset.cpp
        gameplay/api/GameAssetTypeRegistry.cpp
        gameplay/api/GameAssetValidation.cpp
        gameplay/api/GameAssetValidation.h
        gameplay/api/Component.cpp
        gameplay/api/ComponentRegistry.cpp
        gameplay/api/ComponentValidation.h
        gameplay/api/ComponentValidation.cpp
        gameplay/api/GameplayIdentityValidation.h
        gameplay/api/GameplayRegistration.cpp
        gameplay/api/ReplicationRegistration.cpp
        gameplay/api/GameServiceRegistryDetail.h
        gameplay/api/GameServiceValidation.cpp
        gameplay/api/GameServiceGraph.cpp
        gameplay/api/GameServiceSchedule.cpp
        gameplay/api/GameServiceRegistry.cpp
        gameplay/api/SystemRegistryDetail.h
        gameplay/api/SystemValidation.cpp
        gameplay/api/SystemGraph.cpp
        gameplay/api/SystemAccessValidation.cpp
        gameplay/api/SystemDescriptorValidation.cpp
        gameplay/api/SystemReachability.cpp
        gameplay/api/SystemSchedule.cpp
        gameplay/api/SystemRegistry.cpp
        gameplay/api/GameModule.cpp
        gameplay/api/GameplayComponentErrors.cpp
        gameplay/api/GameplayErrors.cpp
)
add_library(HoroEngine::GameplayApi ALIAS HoroGameplayApi)
target_compile_features(HoroGameplayApi PUBLIC cxx_std_20)
target_include_directories(HoroGameplayApi PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(HoroGameplayApi PUBLIC HoroEngine::Foundation HoroEngine::NetworkApi)
target_compile_definitions(HoroGameplayApi PRIVATE
        HORO_GAMEPLAY_SDK_FINGERPRINT="${HORO_GAMEPLAY_SDK_FINGERPRINT}"
)

add_library(HoroRuntimeScene STATIC
        runtime/scene/NavigationSceneComponents.cpp
        runtime/scene/PersistentEntityIdentity.cpp
        runtime/scene/SaveableComponentState.cpp
        runtime/scene/RuntimeSceneDefinition.cpp
        runtime/scene/SavedSceneBootstrap.cpp
        runtime/scene/RuntimeScene.cpp
        runtime/scene/RuntimeSceneService.cpp
        runtime/scene/RuntimeSceneErrors.cpp
)
add_library(HoroEngine::RuntimeScene ALIAS HoroRuntimeScene)
target_compile_features(HoroRuntimeScene PUBLIC cxx_std_20)
target_include_directories(HoroRuntimeScene PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(HoroRuntimeScene PUBLIC HoroEngine::Foundation HoroEngine::Runtime HoroEngine::GameplayApi HoroEngine::RuntimeUi)

add_library(HoroNetworkRuntime STATIC
        runtime/networking/AdmissionProtection.cpp
        runtime/networking/InboundMessageDispatcher.cpp
        runtime/networking/NetworkModeComposition.cpp
        runtime/networking/TransportBackendComposition.cpp
        runtime/networking/TransportTargetComposition.cpp
        runtime/networking/NetworkDiagnostics.cpp
        runtime/networking/AuthenticationSessionAdapter.cpp
        runtime/networking/HandshakeNegotiation.cpp
        runtime/networking/NetworkObjectMapping.cpp
        runtime/networking/NetworkTickAlignment.cpp
        runtime/networking/PeerSessionLifecycle.cpp
        runtime/networking/ReplicationWorldLifecycle.cpp
)
add_library(HoroEngine::NetworkRuntime ALIAS HoroNetworkRuntime)
target_compile_features(HoroNetworkRuntime PUBLIC cxx_std_20)
target_link_libraries(HoroNetworkRuntime PUBLIC HoroEngine::NetworkApi HoroEngine::RuntimeScene HoroEngine::Runtime)

add_library(HoroGameplayRuntime STATIC
        gameplay/runtime/BehaviorRegistry.cpp
        gameplay/runtime/BehaviorRuntime.cpp
        gameplay/runtime/GameplayRegistrationRuntimeDetail.h
        gameplay/runtime/GameplayRuntimeInvocation.h
        gameplay/runtime/GameplayRuntimeInvocation.cpp
        gameplay/runtime/GameplayRuntimeValidation.h
        gameplay/runtime/GameplayRuntimeValidation.cpp
        gameplay/runtime/GameplayServiceRuntime.cpp
        gameplay/runtime/GameplaySystemRuntime.cpp
        gameplay/runtime/GameplaySystemDispatch.cpp
)
add_library(HoroEngine::GameplayRuntime ALIAS HoroGameplayRuntime)
target_compile_features(HoroGameplayRuntime PUBLIC cxx_std_20)
target_include_directories(HoroGameplayRuntime PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(HoroGameplayRuntime PUBLIC HoroEngine::GameplayApi HoroEngine::RuntimeScene)

add_library(HoroGameplayModuleHost STATIC
        gameplay/module/GameModuleHostDetail.h
        gameplay/module/GameModuleHostAccess.cpp
        gameplay/module/GameModuleHostLifecycle.cpp
        gameplay/module/GameModuleHost.cpp
        gameplay/module/GameModuleHostShadowCopy.cpp
)
add_library(HoroEngine::GameplayModuleHost ALIAS HoroGameplayModuleHost)
target_compile_features(HoroGameplayModuleHost PUBLIC cxx_std_20)
target_include_directories(HoroGameplayModuleHost PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(HoroGameplayModuleHost PUBLIC HoroEngine::GameplayRuntime PRIVATE HoroEngine::Platform)

add_library(HoroGameplayLua STATIC
        gameplay/lua/LuaBehavior.cpp
)

add_library(HoroGameplayBuild STATIC
        application/gameplay/CompilerDiagnosticParser.cpp
        application/gameplay/GameplayBuildInputs.cpp
        application/gameplay/GameplayBuildInvocation.cpp
        application/gameplay/GameplayBuildService.cpp
)
add_library(HoroEngine::GameplayBuild ALIAS HoroGameplayBuild)
target_compile_features(HoroGameplayBuild PUBLIC cxx_std_20)
target_include_directories(HoroGameplayBuild PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(HoroGameplayBuild
        PUBLIC HoroEngine::Foundation
        PRIVATE HoroEngine::Platform HoroEngine::GameplayModuleHost nlohmann_json::nlohmann_json)
add_library(HoroEngine::GameplayLua ALIAS HoroGameplayLua)
target_compile_features(HoroGameplayLua PUBLIC cxx_std_20)
target_include_directories(HoroGameplayLua PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(HoroGameplayLua
        PUBLIC HoroEngine::GameplayRuntime
        PRIVATE HoroThirdParty::Lua nlohmann_json::nlohmann_json
)

add_library(HoroAssets STATIC
        runtime/assets/AssetErrors.cpp
        runtime/assets/package/AssetArchive.cpp
        runtime/assets/package/AssetChunkPlan.cpp
        runtime/assets/registry/AssetId.cpp
        runtime/assets/registry/AssetRegistry.cpp
        runtime/assets/runtime_provider/AssetProviderRead.h
        runtime/assets/runtime_provider/AssetProvider.cpp
        runtime/assets/preview/AssetPreviewService.cpp
        runtime/assets/cooker/AssetCookTypes.cpp
        runtime/assets/cooker/CookCatalog.cpp
        runtime/assets/cooker/builtin/HeadlessMeshCooker.cpp
        runtime/assets/cache/AssetCookCache.cpp
        runtime/assets/cache/AssetPayloadCache.cpp
        runtime/assets/cooker/AssetCookOutput.cpp
        runtime/assets/cooker/AssetCookService.cpp
        runtime/assets/cooker/AssetCookTransaction.cpp
        runtime/assets/importer/AssetImporterCatalog.cpp
        runtime/assets/importer/AssetImportMetadata.cpp
        runtime/assets/importer/AssetImportOperation.cpp
        runtime/assets/importer/AssetImportFileFacts.cpp
        runtime/assets/importer/AssetReimport.cpp
        runtime/assets/importer/ProjectAssetImportCommitter.cpp
        runtime/assets/importer/builtin/obj_mesh/ObjMeshImporter.cpp
        runtime/assets/importer/builtin/obj_mesh/ObjMeshPreviewProvider.cpp
        runtime/assets/importer/builtin/fbx_mesh/FbxMeshImporter.cpp
        runtime/assets/importer/builtin/fbx_mesh/FbxMeshParser.cpp
        runtime/assets/importer/builtin/fbx_mesh/PreFracturedFbxParser.cpp
)
add_library(HoroEngine::Assets ALIAS HoroAssets)
target_compile_features(HoroAssets PUBLIC cxx_std_20)
target_include_directories(HoroAssets PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(HoroAssets
        PUBLIC HoroEngine::Foundation
        PRIVATE nlohmann_json::nlohmann_json HoroThirdParty::Ufbx
)
target_link_libraries(HoroRuntimeScene PUBLIC HoroEngine::Assets)

add_library(HoroPhysicsModel STATIC
        physics/PhysicsErrors.cpp
        physics/PhysicsFilterIdentity.cpp
        runtime/scene/PhysicsSceneComponents.cpp
)
add_library(HoroEngine::PhysicsModel ALIAS HoroPhysicsModel)
target_compile_features(HoroPhysicsModel PUBLIC cxx_std_20)
target_link_libraries(HoroPhysicsModel PUBLIC HoroEngine::Foundation HoroEngine::Assets HoroEngine::Runtime)
target_link_libraries(HoroRuntimeScene PUBLIC HoroEngine::PhysicsModel)

add_library(HoroAudioApi STATIC
        audio/api/AudioAcousticQuery.cpp
        audio/api/AudioAssetSchema.cpp
        audio/api/AudioBackendCapabilities.cpp
        audio/api/AudioCallbackEvents.cpp
        audio/api/AudioDeviceDiscovery.cpp
        audio/api/AudioDeviceNegotiation.cpp
        audio/api/AudioDeviceTiming.cpp
        audio/api/AudioFormat.cpp
        audio/api/AudioMediaFormatRegistry.cpp
        audio/api/MixerAssetSchema.cpp
        audio/api/AudioPlanarBlock.cpp
        audio/api/AudioErrors.cpp
        audio/api/AudioFailureErrors.cpp
        audio/api/AudioFailureRecovery.cpp
        audio/api/AudioResamplerPlan.cpp
        audio/api/AudioPlaybackRequest.cpp
        audio/api/AudioSoundReference.cpp
        audio/api/AudioStreamDecoder.cpp
        audio/api/AudioStreamDecoderErrors.cpp
        audio/api/AudioConcurrencyGroup.cpp
        audio/api/AudioVoiceStateMachine.cpp
        audio/api/AudioVoiceControls.cpp
        audio/api/AudioHandleRegistry.h
)
add_library(HoroEngine::AudioApi ALIAS HoroAudioApi)
target_compile_features(HoroAudioApi PUBLIC cxx_std_20)
target_link_libraries(HoroAudioApi PUBLIC HoroEngine::Foundation HoroEngine::Assets)

add_library(HoroAudioImport STATIC
        audio/import/AudioSourceImporter.cpp
        audio/import/AudioSourceAnalysis.cpp
        audio/import/AudioSourceAnalysis.h
        audio/import/AudioSourceProbeDetail.inl
)
add_library(HoroEngine::AudioImport ALIAS HoroAudioImport)
target_compile_features(HoroAudioImport PUBLIC cxx_std_20)
target_link_libraries(HoroAudioImport
        PUBLIC HoroEngine::AudioApi
        PRIVATE HoroThirdParty::Miniaudio HoroThirdParty::Ebur128
)

add_library(HoroAudioCook STATIC
        audio/cook/AudioCookProfile.cpp
        audio/cook/AudioCooker.cpp
        audio/cook/AudioCookInspector.cpp
)
add_library(HoroEngine::AudioCook ALIAS HoroAudioCook)
target_compile_features(HoroAudioCook PUBLIC cxx_std_20)
target_link_libraries(HoroAudioCook PUBLIC HoroEngine::AudioImport HoroEngine::Assets)

# Build-tree-only contract shared privately by Audio control and concrete backend adapters.
add_library(HoroAudioBackendContract INTERFACE)
target_compile_features(HoroAudioBackendContract INTERFACE cxx_std_20)
target_include_directories(HoroAudioBackendContract INTERFACE
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/audio/backend/include>")
target_link_libraries(HoroAudioBackendContract INTERFACE HoroEngine::AudioApi)

# Build-tree-only hook boundary shared by backend callback scopes and Horo-owned audio operations.
add_library(HoroAudioSafetyHooks STATIC
        audio/safety/AudioCallbackSafetyHooks.cpp
)
target_compile_features(HoroAudioSafetyHooks PUBLIC cxx_std_20)
target_include_directories(HoroAudioSafetyHooks PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/audio/safety/include>")

add_library(HoroAudioWatchdog STATIC
        audio/backend/AudioCallbackWatchdog.cpp
)
target_compile_features(HoroAudioWatchdog PUBLIC cxx_std_20)
target_link_libraries(HoroAudioWatchdog PUBLIC HoroAudioBackendContract HoroAudioSafetyHooks)

add_library(HoroAudioNull STATIC
        audio/backend/null/NullAudioBackend.cpp
)
add_library(HoroEngine::AudioNull ALIAS HoroAudioNull)
target_compile_features(HoroAudioNull PUBLIC cxx_std_20)
target_include_directories(HoroAudioNull PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/audio/backend/null/include>")
target_link_libraries(HoroAudioNull PUBLIC HoroAudioBackendContract HoroEngine::AudioCommands PRIVATE HoroAudioWatchdog)

if(HORO_BUILD_AUDIO_SDL3)
    add_library(HoroAudioSdl3 STATIC
            audio/backend/sdl3/Sdl3AudioBackend.cpp
    )
    add_library(HoroEngine::AudioSdl3 ALIAS HoroAudioSdl3)
    target_compile_features(HoroAudioSdl3 PUBLIC cxx_std_20)
    target_include_directories(HoroAudioSdl3 PUBLIC
            "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/audio/backend/sdl3/include>")
    target_link_libraries(HoroAudioSdl3
            PUBLIC HoroAudioBackendContract HoroEngine::AudioCommands
            PRIVATE HoroAudioWatchdog ${HORO_SDL3_TARGET})
endif()

add_library(HoroAudioDsp STATIC
        audio/api/AudioDSPNode.cpp
        audio/dsp/CoreAudioDSPNode.cpp
        audio/resampling/AudioResampler.cpp
        audio/resampling/ResamplerSimd.cpp
        audio/resampling/ResamplerSimd.h
        audio/resampling/ResamplerKernel.cpp
        audio/resampling/ResamplerKernel.h
)
add_library(HoroEngine::AudioDsp ALIAS HoroAudioDsp)
target_compile_features(HoroAudioDsp PUBLIC cxx_std_20)
target_link_libraries(HoroAudioDsp PUBLIC HoroEngine::AudioApi)

add_library(HoroAudioPlayback STATIC audio/playback/AudioVoicePlayback.cpp)
add_library(HoroEngine::AudioPlayback ALIAS HoroAudioPlayback)
target_compile_features(HoroAudioPlayback PUBLIC cxx_std_20)
target_link_libraries(HoroAudioPlayback PUBLIC HoroEngine::AudioDsp HoroEngine::AudioApi)

add_library(HoroAudioMemory STATIC
        audio/memory/AudioMemoryPool.cpp
        audio/memory/AudioScratchArena.cpp
        audio/memory/AlignedAudioStorage.h
)
add_library(HoroEngine::AudioMemory ALIAS HoroAudioMemory)
target_compile_features(HoroAudioMemory PUBLIC cxx_std_20)
target_link_libraries(HoroAudioMemory PUBLIC HoroEngine::AudioApi PRIVATE HoroAudioSafetyHooks)

add_library(HoroAudioCommands STATIC
        audio/commands/AudioClock.cpp
        audio/commands/AudioParameterAutomation.cpp
        audio/commands/AudioCommands.cpp
        audio/commands/AudioCommandBuffer.cpp
        audio/commands/AudioCommandStaging.cpp
        audio/commands/AudioEventQueue.cpp
        audio/commands/AudioFocusPolicy.cpp
        audio/commands/AudioLifecycleReconciler.cpp
        audio/commands/ScheduledAudioCommandBatch.cpp
)
add_library(HoroEngine::AudioCommands ALIAS HoroAudioCommands)
target_compile_features(HoroAudioCommands PUBLIC cxx_std_20)
target_link_libraries(HoroAudioCommands PUBLIC HoroEngine::AudioMemory PRIVATE HoroAudioSafetyHooks)

add_library(HoroAudioMetrics STATIC
        audio/metrics/AudioMetricExtraction.cpp
        audio/metrics/AudioMetrics.cpp
        audio/metrics/AudioMetricTelemetry.cpp
)
add_library(HoroEngine::AudioMetrics ALIAS HoroAudioMetrics)
target_compile_features(HoroAudioMetrics PUBLIC cxx_std_20)
target_link_libraries(HoroAudioMetrics PUBLIC HoroEngine::AudioCommands HoroEngine::Foundation)

add_library(HoroPhysics STATIC
        physics/CharacterControllerContracts.cpp
        physics/CharacterDiagnostics.cpp
        physics/CharacterErrors.cpp
        physics/CharacterMetrics.cpp
        physics/CharacterControllerRegistry.h
        physics/CharacterFastPathStorage.h
        physics/CharacterWorld.cpp
        physics/CharacterWorldCommandPipeline.cpp
        physics/CharacterWorldCommandInternal.h
        physics/CharacterWorldMovementInternal.h
        physics/CharacterWorldInternal.h
        physics/CharacterWorldPlacementInternal.h
        physics/CharacterWorldShapeInternal.h
        physics/CharacterWorldSettings.cpp
        physics/GenerationalSlotStorage.h
        physics/PhysicsBodyDescriptor.cpp
        physics/PhysicsBodyDynamics.cpp
        physics/PhysicsCapabilities.cpp
        physics/PhysicsCollisionSchema.cpp
        physics/PhysicsConstraintDescriptor.cpp
        physics/PhysicsCompoundArtifact.cpp
        physics/PhysicsCompoundCook.cpp
        physics/PhysicsConvexHullCook.cpp
        physics/PhysicsConvexHullArtifact.cpp
        physics/PhysicsTriangleMeshCook.cpp
        physics/PhysicsTriangleMeshArtifact.cpp
        physics/PhysicsHeightFieldCook.cpp
        physics/PhysicsHeightFieldArtifact.cpp
        physics/PhysicsCookedShapeCache.cpp
        physics/PhysicsCookedShapeDescriptor.cpp
        physics/PhysicsDiagnostics.cpp
        physics/PhysicsDebugSnapshot.cpp
        physics/PhysicsDeterminismPolicy.cpp
        physics/PhysicsEventProjection.cpp
        physics/PhysicsEventProjection.h
        physics/PhysicsHandleRegistry.h
        physics/PhysicsIdentity.cpp
        physics/PhysicsMetrics.cpp
        physics/PhysicsMaterialAsset.cpp
        physics/PhysicsMassProperties.cpp
        physics/PhysicsPose.cpp
        physics/PhysicsQuery.cpp
        physics/PhysicsSaturatingAdd.h
        physics/PhysicsShapeDescriptor.cpp
        physics/PhysicsStepPolicy.cpp
        physics/PhysicsTransformAuthority.cpp
        physics/PhysicsTransformContracts.cpp
        physics/PhysicsWorld.cpp
        physics/PhysicsQueryEventCapability.cpp
        physics/PhysicsWorldInternal.h
        physics/PhysicsWorldScene.cpp
        physics/PhysicsWorldTickHelpers.h
        physics/PhysicsWorldBudgets.cpp
        physics/PhysicsWorldDescriptor.cpp
        physics/PhysicsWorldSettings.cpp
)
add_library(HoroEngine::Physics ALIAS HoroPhysics)
target_compile_features(HoroPhysics PUBLIC cxx_std_20)
target_link_libraries(HoroPhysics PUBLIC HoroEngine::Foundation HoroEngine::Assets HoroEngine::PhysicsModel)
if(HORO_BUILD_PHYSICS_NATIVE)
    target_sources(HoroPhysics PRIVATE
        physics/CanonicalPhysicsRuntime.cpp
        physics/CanonicalPhysicsRuntimeBodyCreation.cpp
        physics/CanonicalPhysicsRuntimeDebug.cpp
        physics/CanonicalPhysicsRuntimeContainment.cpp
        physics/CanonicalPhysicsRuntimeDiagnostics.cpp
        physics/CanonicalPhysicsRuntimeEvents.cpp
        physics/CanonicalPhysicsRuntimeQuery.cpp
        physics/CanonicalPhysicsRuntimeQueryBridge.cpp
        physics/CanonicalPhysicsRuntimeReadback.cpp
        physics/CanonicalPhysicsRuntimeScene.cpp
        physics/CanonicalSolver.cpp
        physics/CanonicalWorldSettings.cpp)
    target_link_libraries(HoroPhysics PRIVATE Jolt)
    set_source_files_properties(
        physics/CanonicalPhysicsRuntime.cpp
        physics/CanonicalPhysicsRuntimeBodyCreation.cpp
        physics/CanonicalPhysicsRuntimeDebug.cpp
        physics/CanonicalPhysicsRuntimeContainment.cpp
        physics/CanonicalPhysicsRuntimeDiagnostics.cpp
        physics/CanonicalPhysicsRuntimeEvents.cpp
        physics/CanonicalPhysicsRuntimeQuery.cpp
        physics/CanonicalPhysicsRuntimeQueryBridge.cpp
        physics/CanonicalPhysicsRuntimeReadback.cpp
        physics/CanonicalPhysicsRuntimeScene.cpp
        physics/CanonicalSolver.cpp
        physics/CanonicalWorldSettings.cpp
        PROPERTIES
        COMPILE_OPTIONS "$<$<CXX_COMPILER_ID:MSVC>:/GR-;/EHs-c->;$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-fno-rtti;-fno-exceptions>")
else()
    target_sources(HoroPhysics PRIVATE physics/CanonicalPhysicsRuntimeOmitted.cpp physics/CanonicalSolverOmitted.cpp)
endif()

add_library(HoroPhysicsSceneIntegration STATIC
        physics/PhysicsSceneActivation.cpp
        physics/PhysicsSceneActivationPlan.cpp
        physics/PhysicsSceneActivationCapabilities.cpp
        physics/PhysicsSceneActivationLifecycle.cpp)
add_library(HoroEngine::PhysicsSceneIntegration ALIAS HoroPhysicsSceneIntegration)
target_compile_features(HoroPhysicsSceneIntegration PUBLIC cxx_std_20)
target_link_libraries(HoroPhysicsSceneIntegration PUBLIC HoroEngine::Physics HoroEngine::RuntimeScene)

add_library(HoroAI STATIC
        runtime/ai/api/AICanonicalErrors.cpp
        runtime/ai/api/AICanonicalState.cpp
        runtime/ai/api/BlackboardCanonicalState.cpp
        runtime/ai/api/AIErrors.cpp
        runtime/ai/api/AISceneComponents.cpp
        runtime/ai/api/AISceneErrors.cpp
        runtime/ai/api/AIIdentity.cpp
        runtime/ai/api/AITaskLifecycle.cpp
        runtime/ai/api/AITaskContinuation.cpp
        runtime/ai/api/AITaskJobService.cpp
        runtime/ai/api/BehaviorTree.cpp
        runtime/ai/api/BehaviorTreeExecutionPlan.cpp
        runtime/ai/api/BehaviorTreeInstance.cpp
        runtime/ai/api/BehaviorTreeTaskExecution.cpp
        runtime/ai/api/BlackboardInstance.cpp
        runtime/ai/api/BlackboardSchema.cpp
        runtime/ai/api/DecisionAssetPlan.cpp
        runtime/ai/api/DecisionAssetValidation.cpp
        runtime/ai/api/StateMachinePlan.cpp
        runtime/ai/api/StateMachineInstance.cpp
        runtime/ai/api/EnvironmentQueryAsset.cpp
        runtime/ai/api/EnvironmentQueryErrors.cpp
        runtime/ai/api/EnvironmentQueryRegistry.cpp
        runtime/ai/api/NullAIRuntime.cpp
        runtime/ai/api/PerceptionDescriptorRegistry.cpp
        runtime/ai/api/PerceptionEventRouting.cpp
        runtime/ai/api/PerceptionErrors.cpp
        runtime/ai/api/PerceptionMemory.cpp
)
add_library(HoroEngine::AI ALIAS HoroAI)
target_compile_features(HoroAI PUBLIC cxx_std_20)
target_link_libraries(HoroAI PUBLIC HoroEngine::Foundation)

target_link_libraries(HoroRuntimeScene PUBLIC HoroEngine::AI)

add_library(HoroAISceneIntegration STATIC
        runtime/ai/integration/AISceneActivation.cpp
        runtime/ai/integration/AISceneRestore.cpp
        runtime/ai/integration/AIScenePerceptionSource.cpp
        runtime/ai/integration/EnvironmentQueryContexts.cpp
        runtime/ai/integration/PerceptionSpatialBroadphase.cpp)
add_library(HoroEngine::AISceneIntegration ALIAS HoroAISceneIntegration)
target_compile_features(HoroAISceneIntegration PUBLIC cxx_std_20)
target_link_libraries(HoroAISceneIntegration PUBLIC HoroEngine::AI HoroEngine::RuntimeScene)

add_library(HoroGameplayPerceptionIntegration STATIC gameplay/runtime/PerceptionEventSource.cpp)
add_library(HoroEngine::GameplayPerceptionIntegration ALIAS HoroGameplayPerceptionIntegration)
target_compile_features(HoroGameplayPerceptionIntegration PUBLIC cxx_std_20)
target_link_libraries(HoroGameplayPerceptionIntegration PUBLIC HoroEngine::AISceneIntegration HoroEngine::NetworkRuntime)

add_library(HoroAnimationApi STATIC
        runtime/animation/api/AnimationCompression.cpp
        runtime/animation/api/AnimationClip.cpp
        runtime/animation/api/AnimationComponents.cpp
        runtime/animation/api/AnimationErrors.cpp
        runtime/animation/api/AnimationIdentity.cpp
        runtime/animation/api/PoseStorage.cpp
        runtime/animation/api/SkeletonAsset.cpp
        runtime/animation/api/SkeletalMeshSkinning.cpp
)
add_library(HoroEngine::AnimationApi ALIAS HoroAnimationApi)
target_compile_features(HoroAnimationApi PUBLIC cxx_std_20)
target_link_libraries(HoroAnimationApi PUBLIC HoroEngine::Foundation HoroEngine::Assets)

add_library(HoroPCG STATIC
        runtime/pcg/api/PCGCookedPlan.cpp
        runtime/pcg/api/PCGErrors.cpp
        runtime/pcg/api/PCGGenerationPlan.cpp
        runtime/pcg/api/PCGGraphAsset.cpp
        runtime/pcg/api/PCGGraphAssetSerialization.cpp
        runtime/pcg/api/PCGGraphAssetValidation.cpp
        runtime/pcg/api/PCGGraphValidation.cpp
        runtime/pcg/api/PCGIdentity.cpp
        runtime/pcg/api/PCGPointSchema.cpp
        runtime/pcg/api/PCGPointCloudWorkspace.cpp
        runtime/pcg/api/PCGProvenance.cpp
        runtime/pcg/api/PCGSpatialSnapshot.cpp
        runtime/pcg/api/PCGRegistry.cpp
        runtime/pcg/api/PCGAsyncOperation.cpp
)
add_library(HoroEngine::PCG ALIAS HoroPCG)
target_compile_features(HoroPCG PUBLIC cxx_std_20)
target_link_libraries(HoroPCG PUBLIC HoroEngine::Foundation)

add_library(HoroPCGTerrainAdapter STATIC
        runtime/pcg/terrain/TerrainInputSnapshot.cpp
)
add_library(HoroEngine::PCGTerrainAdapter ALIAS HoroPCGTerrainAdapter)
target_compile_features(HoroPCGTerrainAdapter PUBLIC cxx_std_20)
target_link_libraries(HoroPCGTerrainAdapter PUBLIC HoroEngine::PCG HoroEngine::TerrainApi)

add_library(HoroVfxApi STATIC
        runtime/vfx/api/CpuParticleBuffer.cpp
        runtime/vfx/api/CpuParticleSimulator.cpp
        runtime/vfx/api/CpuParticleSimulatorPreparation.cpp
        runtime/vfx/api/CpuParticleSimulatorStages.cpp
        runtime/vfx/api/CpuParticleSpawnPipeline.cpp
        runtime/vfx/api/EffectInstancePool.cpp
        runtime/vfx/api/ParticleSystemDescriptor.cpp
        runtime/vfx/api/VfxErrors.cpp
        runtime/vfx/api/VfxIdentity.cpp
        runtime/vfx/api/VfxQualityPolicy.cpp
)
add_library(HoroEngine::VfxApi ALIAS HoroVfxApi)
target_compile_features(HoroVfxApi PUBLIC cxx_std_20)
target_link_libraries(HoroVfxApi
        PUBLIC HoroEngine::Foundation HoroEngine::Assets
        PRIVATE nlohmann_json::nlohmann_json
)

add_library(HoroDestructionApi STATIC
        runtime/destruction/api/DestructionCommand.cpp
        runtime/destruction/api/DestructionComposition.cpp
        runtime/destruction/api/DestructionErrors.cpp
        runtime/destruction/api/DestructionIdentity.cpp
        runtime/destruction/api/DestructionRegistry.cpp
        runtime/destruction/api/DestructionStateMachine.cpp
        runtime/destruction/api/DestructibleDescriptor.cpp
)
add_library(HoroEngine::DestructionApi ALIAS HoroDestructionApi)
target_compile_features(HoroDestructionApi PUBLIC cxx_std_20)
target_link_libraries(HoroDestructionApi PUBLIC HoroEngine::Foundation HoroEngine::Assets)

add_library(HoroDestructionCook STATIC
        runtime/destruction/cook/StructuralGraphCook.cpp
        runtime/destruction/cook/ChunkMeshCook.cpp
        runtime/destruction/cook/ChunkMeshVoronoi.cpp
        runtime/destruction/cook/ChunkMeshImport.cpp
        runtime/destruction/cook/OfflineVoronoi.cpp
        runtime/destruction/cook/OfflineVoronoiChunks.cpp
        runtime/destruction/cook/OfflineVoronoiClipping.cpp
        runtime/destruction/cook/OfflineVoronoiProvenance.cpp
        runtime/destruction/cook/OfflineVoronoiValidation.cpp
        runtime/destruction/cook/PreFracturedIntersection.cpp
        runtime/destruction/cook/PreFracturedImport.cpp
)
add_library(HoroEngine::DestructionCook ALIAS HoroDestructionCook)
target_compile_features(HoroDestructionCook PUBLIC cxx_std_20)
target_link_libraries(HoroDestructionCook PUBLIC HoroEngine::DestructionApi HoroEngine::Assets)
# Packaged artifact codec contains no source geometry or offline cook implementation.
add_library(HoroDestructionCollisionArtifacts STATIC runtime/destruction/cook/ChunkCollisionArtifact.cpp)
add_library(HoroEngine::DestructionCollisionArtifacts ALIAS HoroDestructionCollisionArtifacts)
target_compile_features(HoroDestructionCollisionArtifacts PUBLIC cxx_std_20)
target_link_libraries(HoroDestructionCollisionArtifacts PUBLIC HoroEngine::DestructionApi HoroEngine::Physics)

# Offline adapter; runtime/API targets retain no Physics cooker dependency.
add_library(HoroDestructionPhysicsCook STATIC runtime/destruction/cook/ChunkCollisionCook.cpp)
add_library(HoroEngine::DestructionPhysicsCook ALIAS HoroDestructionPhysicsCook)
target_compile_features(HoroDestructionPhysicsCook PUBLIC cxx_std_20)
target_link_libraries(HoroDestructionPhysicsCook PUBLIC HoroEngine::DestructionCook HoroEngine::DestructionCollisionArtifacts)

add_library(HoroDestructionRuntime STATIC
        runtime/destruction/runtime/DestructionDamageRuntime.cpp
        runtime/destruction/runtime/DestructionEventStream.cpp
)
add_library(HoroEngine::DestructionRuntime ALIAS HoroDestructionRuntime)
target_compile_features(HoroDestructionRuntime PUBLIC cxx_std_20)
target_link_libraries(HoroDestructionRuntime PUBLIC HoroEngine::DestructionApi)

add_library(HoroDestructionApplication STATIC application/destruction/DestructionEventDispatcher.cpp)
add_library(HoroEngine::DestructionApplication ALIAS HoroDestructionApplication)
target_compile_features(HoroDestructionApplication PUBLIC cxx_std_20)
target_link_libraries(HoroDestructionApplication PUBLIC HoroEngine::DestructionRuntime)

add_library(HoroDestructionReplication STATIC
        runtime/destruction/network/DestructionReplicationDescriptor.cpp
        runtime/destruction/network/DestructionReplicationEncoding.cpp
)
add_library(HoroEngine::DestructionReplication ALIAS HoroDestructionReplication)
target_compile_features(HoroDestructionReplication PUBLIC cxx_std_20)
target_link_libraries(HoroDestructionReplication PUBLIC HoroEngine::DestructionApi HoroEngine::NetworkApi)

add_library(HoroCinematicModel STATIC
        runtime/cinematic/model/CurveSampling.cpp
        runtime/cinematic/model/CinematicErrors.cpp
        runtime/cinematic/model/CinematicIdentity.cpp
        runtime/cinematic/model/PropertyTrack.cpp
        runtime/cinematic/model/SequenceAsset.cpp
        runtime/cinematic/model/SequenceAssetCook.cpp
        runtime/cinematic/model/TransformTrack.cpp
)
add_library(HoroEngine::CinematicModel ALIAS HoroCinematicModel)
target_compile_features(HoroCinematicModel PUBLIC cxx_std_20)
target_include_directories(HoroCinematicModel PRIVATE ${PROJECT_SOURCE_DIR}/src/foundation/json)
target_link_libraries(HoroCinematicModel
        PUBLIC HoroEngine::Foundation HoroEngine::Runtime HoroEngine::Assets
        PRIVATE nlohmann_json::nlohmann_json
)

add_library(HoroCinematicRuntime STATIC
        runtime/cinematic/player/PropertyTrackRuntime.cpp
        runtime/cinematic/player/SequenceEvaluation.cpp
        runtime/cinematic/player/SequenceFrameBlend.cpp
        runtime/cinematic/player/SequenceEvaluationErrors.cpp
        runtime/cinematic/player/SequencePlayer.cpp
        runtime/cinematic/player/SequencePlayerErrors.cpp
        runtime/cinematic/player/SequencePlaybackRuntime.cpp
        runtime/cinematic/player/SequencePlaybackRuntimeErrors.cpp
        runtime/cinematic/player/SequencePlaybackRuntimeService.cpp
)
add_library(HoroEngine::CinematicRuntime ALIAS HoroCinematicRuntime)
target_compile_features(HoroCinematicRuntime PUBLIC cxx_std_20)
target_link_libraries(HoroCinematicRuntime PUBLIC HoroEngine::CinematicModel)

add_library(HoroNavigationApi STATIC
        runtime/navigation/api/NavMeshData.cpp
        runtime/navigation/api/mesh_codec/NavMeshCodec.cpp
        runtime/navigation/api/NavigationAreas.cpp
        runtime/navigation/api/NavigationAgentProfiles.cpp
        runtime/navigation/api/NavigationBakeFingerprint.cpp
        runtime/navigation/api/NavigationBakeInput.cpp
        runtime/navigation/api/NavigationBakeInputSnapshot.cpp
        runtime/navigation/api/NavigationCapabilities.cpp
        runtime/navigation/api/NavigationDataSerialization.cpp
        runtime/navigation/api/NavigationDataSerializationDecode.cpp
        runtime/navigation/api/NavigationDataSerializationMigration.cpp
        runtime/navigation/api/NavigationDynamicErrors.cpp
        runtime/navigation/api/NavigationErrors.cpp
        runtime/navigation/api/NavigationErrorsScene.cpp
        runtime/navigation/api/NavigationIdentity.cpp
        runtime/navigation/api/NavigationLinkValidation.cpp
        runtime/navigation/api/NavigationLinkValidationSnapshot.cpp
        runtime/navigation/api/NavigationLinkBoundaryAnchors.cpp
        runtime/navigation/api/NavigationOutcomes.cpp
        runtime/navigation/api/NavigationProjectProfiles.cpp
        runtime/navigation/api/NavigationSourceGeometry.cpp
        runtime/navigation/api/NavigationTileDependencies.cpp
        runtime/navigation/api/NavigationTileArtifact.cpp
        runtime/navigation/api/NavigationTileSetCodec.cpp
)
add_library(HoroEngine::NavigationApi ALIAS HoroNavigationApi)
target_compile_features(HoroNavigationApi PUBLIC cxx_std_20)
target_link_libraries(HoroNavigationApi PUBLIC HoroEngine::Foundation)
target_link_libraries(HoroRuntimeScene PUBLIC HoroEngine::NavigationApi)

add_library(HoroNavigationRuntime STATIC
        runtime/navigation/runtime/NavigationBakeJobs.cpp
        runtime/navigation/runtime/NavigationCrowdSnapshot.cpp
        runtime/navigation/runtime/NavigationCrowdSnapshotGather.cpp
        runtime/navigation/runtime/NavigationCrowdSnapshotPartition.cpp
        runtime/navigation/runtime/NavigationDynamicRegistry.cpp
        runtime/navigation/runtime/NavigationDynamicRegistryCommit.cpp
        runtime/navigation/runtime/NavigationDynamicRegistrySnapshot.cpp
        runtime/navigation/runtime/NavigationObstacleOverlay.cpp
        runtime/navigation/runtime/NavigationAgentRegistry.cpp
        runtime/navigation/runtime/NavigationPathPolicy.cpp
        runtime/navigation/runtime/NavigationRuntimeQueues.cpp
        runtime/navigation/runtime/NavigationWorldLifecycle.cpp
)
add_library(HoroEngine::NavigationRuntime ALIAS HoroNavigationRuntime)
target_compile_features(HoroNavigationRuntime PUBLIC cxx_std_20)
target_link_libraries(HoroNavigationRuntime PUBLIC HoroEngine::NavigationApi)

add_library(HoroNavigationSceneIntegration STATIC
        runtime/navigation/integration/NavigationSceneActivation.cpp
        runtime/navigation/integration/NavigationSceneLinkCapture.cpp
        runtime/navigation/integration/NavMeshAssetLoading.cpp
        runtime/navigation/integration/NavigationAssetSceneActivation.cpp
)
add_library(HoroEngine::NavigationSceneIntegration ALIAS HoroNavigationSceneIntegration)
target_compile_features(HoroNavigationSceneIntegration PUBLIC cxx_std_20)
target_link_libraries(HoroNavigationSceneIntegration PUBLIC HoroEngine::NavigationRuntime HoroEngine::RuntimeScene HoroEngine::Assets)

add_library(HoroNavigationBakeService STATIC
        application/navigation/NavigationBakeService.cpp
        application/navigation/NavigationBakeCook.cpp
        application/navigation/NavigationBakeDiagnostics.cpp
        application/navigation/NavigationBakeDiagnosticCodec.cpp
        application/navigation/NavigationBakeDiagnosticProjection.cpp
)
add_library(HoroEngine::NavigationBakeService ALIAS HoroNavigationBakeService)
target_compile_features(HoroNavigationBakeService PUBLIC cxx_std_20)
target_link_libraries(HoroNavigationBakeService PUBLIC HoroEngine::NavigationRuntime HoroEngine::Assets HoroEngine::Platform HoroEngine::Runtime
        PRIVATE nlohmann_json::nlohmann_json)

add_library(HoroXRApi STATIC
        runtime/xr/api/XRCapabilities.cpp
        runtime/xr/api/XRContract.cpp
        runtime/xr/api/XRErrors.cpp
        runtime/xr/api/XRIdentity.cpp
        runtime/xr/api/XRLoaderPreflight.cpp
        runtime/xr/api/XRSpacePose.cpp
        runtime/xr/api/XRTrackingSnapshot.cpp
        runtime/xr/api/XRViewRenderPlan.cpp
)
add_library(HoroEngine::XRApi ALIAS HoroXRApi)
target_compile_features(HoroXRApi PUBLIC cxx_std_20)
target_link_libraries(HoroXRApi PUBLIC HoroEngine::Foundation HoroEngine::RenderApi)

add_library(HoroXRRuntime STATIC
        runtime/xr/runtime/XRFeatureNegotiation.cpp
        runtime/xr/runtime/XRFrameLifecycle.cpp
        runtime/xr/runtime/XRSessionErrors.cpp
        runtime/xr/runtime/XRSessionLifecycle.cpp
)
add_library(HoroEngine::XRRuntime ALIAS HoroXRRuntime)
target_compile_features(HoroXRRuntime PUBLIC cxx_std_20)
target_link_libraries(HoroXRRuntime PUBLIC HoroEngine::XRApi)

add_library(HoroTerrainApi STATIC
        runtime/terrain/api/FoliageDefinition.cpp
        runtime/terrain/api/TerrainComposition.cpp
        runtime/terrain/api/TerrainDescriptor.cpp
        runtime/terrain/api/TerrainErrors.cpp
        runtime/terrain/api/TerrainFoliageRegistry.cpp
        runtime/terrain/api/TerrainIdentity.cpp
)
add_library(HoroEngine::TerrainApi ALIAS HoroTerrainApi)
target_compile_features(HoroTerrainApi PUBLIC cxx_std_20)
target_link_libraries(HoroTerrainApi PUBLIC HoroEngine::Foundation)

add_library(HoroTerrainImport STATIC
        runtime/terrain/import/TerrainPngDecoder.cpp
        runtime/terrain/import/TerrainSourceErrors.cpp
        runtime/terrain/import/TerrainSourceImport.cpp
)
add_library(HoroEngine::TerrainImport ALIAS HoroTerrainImport)
target_compile_features(HoroTerrainImport PUBLIC cxx_std_20)
target_include_directories(HoroTerrainImport PRIVATE ${PROJECT_SOURCE_DIR}/vendor/stb)
target_link_libraries(HoroTerrainImport PUBLIC HoroEngine::TerrainApi HoroEngine::Assets)
add_library(HoroTerrainCook STATIC
        runtime/terrain/cook/FoliageClusterCook.cpp
        runtime/terrain/cook/FoliageClusterCookOwner.cpp
        runtime/terrain/cook/FoliagePlacementCook.cpp
        runtime/terrain/cook/TerrainTileCook.cpp
        runtime/terrain/cook/TerrainTileCookCodec.cpp
        runtime/terrain/cook/TerrainTileCookErrors.cpp
)
add_library(HoroEngine::TerrainCook ALIAS HoroTerrainCook)
target_compile_features(HoroTerrainCook PUBLIC cxx_std_20)
target_link_libraries(HoroTerrainCook PUBLIC HoroEngine::TerrainImport)
add_library(HoroTerrainRuntime STATIC
        runtime/terrain/runtime/TerrainAsyncJobs.cpp
)
add_library(HoroEngine::TerrainRuntime ALIAS HoroTerrainRuntime)
target_compile_features(HoroTerrainRuntime PUBLIC cxx_std_20)
target_link_libraries(HoroTerrainRuntime PUBLIC HoroEngine::TerrainApi HoroEngine::Foundation)

add_library(HoroNavigationNull STATIC
        runtime/navigation/backends/null/NullProvider.cpp
)
add_library(HoroEngine::NavigationNull ALIAS HoroNavigationNull)
target_compile_features(HoroNavigationNull PUBLIC cxx_std_20)
target_link_libraries(HoroNavigationNull PUBLIC HoroEngine::NavigationApi)

add_library(HoroNavigationRecastDetour STATIC)
add_library(HoroEngine::NavigationRecastDetour ALIAS HoroNavigationRecastDetour)
target_compile_features(HoroNavigationRecastDetour PUBLIC cxx_std_20)
target_link_libraries(HoroNavigationRecastDetour PUBLIC HoroEngine::NavigationApi)
if(HORO_BUILD_NAVIGATION_RECAST_DETOUR)
    target_sources(HoroNavigationRecastDetour PRIVATE
            runtime/navigation/backends/recast_detour/RecastDetourProvider.cpp
            runtime/navigation/backends/recast_detour/RecastDetourProviderEndpoint.cpp
            runtime/navigation/backends/recast_detour/RecastDetourProviderInternal.h
            runtime/navigation/backends/recast_detour/RecastDetourProviderQueries.cpp
            runtime/navigation/backends/recast_detour/RecastDetourProviderQueriesInternal.h
            runtime/navigation/backends/recast_detour/RecastDetourProviderPath.cpp
            runtime/navigation/backends/recast_detour/RecastDetourProviderSpatialQueries.cpp
            runtime/navigation/backends/recast_detour/RecastDetourProviderSpatialQueryResults.cpp
            runtime/navigation/backends/recast_detour/RecastDetourMeshBuilder.cpp
            runtime/navigation/backends/recast_detour/RecastDetourMeshBuilderInternal.h
            runtime/navigation/backends/recast_detour/RecastDetourMeshBuilderPreparation.cpp
            runtime/navigation/backends/recast_detour/RecastDetourMeshBuilderPipeline.cpp
            runtime/navigation/backends/recast_detour/RecastDetourMeshBuilderTranslation.cpp
            runtime/navigation/backends/recast_detour/RecastDetourProviderValidation.h
    )
    target_compile_definitions(HoroNavigationRecastDetour PRIVATE DT_POLYREF64 RC_DISABLE_ASSERTS)
    target_include_directories(HoroNavigationRecastDetour PRIVATE
            ${HORO_RECAST_NAVIGATION_SOURCE_DIR}/Detour/Include
            ${HORO_RECAST_NAVIGATION_SOURCE_DIR}/Recast/Include
    )
    target_link_libraries(HoroNavigationRecastDetour PRIVATE HoroThirdPartyDetour HoroThirdPartyRecast)
else()
    target_sources(HoroNavigationRecastDetour PRIVATE
            runtime/navigation/backends/recast_detour/RecastDetourProviderOmitted.cpp
    )
endif()

add_library(HoroNavigationCrowdDetour STATIC)
add_library(HoroEngine::NavigationCrowdDetour ALIAS HoroNavigationCrowdDetour)
target_compile_features(HoroNavigationCrowdDetour PUBLIC cxx_std_20)
target_link_libraries(HoroNavigationCrowdDetour PUBLIC HoroEngine::NavigationRuntime)
if(HORO_BUILD_NAVIGATION_RECAST_DETOUR)
    target_sources(HoroNavigationCrowdDetour PRIVATE
            runtime/navigation/backends/recast_detour/RecastDetourCrowdGeometry.cpp
            runtime/navigation/backends/recast_detour/RecastDetourCrowdGeometry.h
            runtime/navigation/backends/recast_detour/RecastDetourCrowdProvider.cpp
    )
    target_compile_definitions(HoroNavigationCrowdDetour PRIVATE DT_POLYREF64 RC_DISABLE_ASSERTS)
    target_include_directories(HoroNavigationCrowdDetour PRIVATE
            ${HORO_RECAST_NAVIGATION_SOURCE_DIR}/DetourCrowd/Include
            ${HORO_RECAST_NAVIGATION_SOURCE_DIR}/Detour/Include
    )
    target_link_libraries(HoroNavigationCrowdDetour PRIVATE HoroThirdPartyDetourCrowd)
else()
    target_sources(HoroNavigationCrowdDetour PRIVATE
            runtime/navigation/backends/recast_detour/RecastDetourCrowdOmitted.cpp
    )
endif()

add_library(HoroWorldStreaming STATIC
        runtime/world_streaming/NetworkStreamingAuthority.cpp
        runtime/world_streaming/OriginFrame.cpp
        runtime/world_streaming/OriginRebaseTransaction.cpp
        runtime/world_streaming/OriginRebaseTransactionErrors.cpp
        runtime/world_streaming/OriginShiftPolicy.cpp
        runtime/world_streaming/OriginShiftPolicyErrors.cpp
        runtime/world_streaming/RuntimeEntityCellExitOperation.cpp
        runtime/world_streaming/WorldEntityReferenceFixup.cpp
        runtime/world_streaming/CookedWorldIndexManifest.cpp
        runtime/world_streaming/FallbackStreamingProvider.cpp
        runtime/world_streaming/StreamingBudgetModel.cpp
        runtime/world_streaming/StreamingFeatureBudgetPolicy.cpp
        runtime/world_streaming/StreamingFeatureBudgetReservations.cpp
        runtime/world_streaming/StreamingFeatureBudgetErrors.cpp
        runtime/world_streaming/SharedAssetResidency.cpp
        runtime/world_streaming/SharedAssetResidencyErrors.cpp
        runtime/world_streaming/StreamingCellOperation.cpp
        runtime/world_streaming/StreamingCellCandidate.cpp
        runtime/world_streaming/StreamingCellAssetRequest.cpp
        runtime/world_streaming/StreamingCellActivation.cpp
        runtime/world_streaming/StreamingCellDirection.cpp
        runtime/world_streaming/StreamingCellStability.cpp
        runtime/world_streaming/StreamingCellState.cpp
        runtime/world_streaming/StreamingSchedulerAdmission.cpp
        runtime/world_streaming/WorldDependencyPlan.cpp
        runtime/world_streaming/WorldAuthoringContract.cpp
        runtime/world_streaming/StreamingDesiredState.cpp
        runtime/world_streaming/StreamingDesiredStateReduction.cpp
        runtime/world_streaming/StreamingPriorityPolicy.cpp
        runtime/world_streaming/StreamingFairQueue.cpp
        runtime/world_streaming/StreamingFairQueueErrors.cpp
        runtime/world_streaming/StreamingSourceDescriptor.cpp
        runtime/world_streaming/StreamingSourcePrefetch.cpp
        runtime/world_streaming/StreamingSourceRange.cpp
        runtime/world_streaming/WorldCellQuantization.cpp
        runtime/world_streaming/WorldPartitionDescriptor.cpp
        runtime/world_streaming/WorldLayerFiltering.cpp
        runtime/world_streaming/WorldLayerOwnershipModel.cpp
        runtime/world_streaming/WorldLayerState.cpp
        runtime/world_streaming/WorldObjectOwnership.cpp
        runtime/world_streaming/WorldPartitionCapabilityProfile.cpp
        runtime/world_streaming/WorldPartitionRegistry.cpp
        runtime/world_streaming/WorldSpatialAssignment.cpp
        runtime/world_streaming/WorldSpatialObjectDescriptor.cpp
        runtime/world_streaming/WorldSpanningObjectPlan.cpp
        runtime/world_streaming/WorldStreamingRuntimeComposition.cpp
        runtime/world_streaming/WorldStreamingDiagnosticSnapshot.cpp
        runtime/world_streaming/WorldStreamingMetricErrors.cpp
        runtime/world_streaming/WorldStreamingMetrics.cpp
        runtime/world_streaming/WorldStreamingTraceErrors.cpp
        runtime/world_streaming/WorldStreamingTrace.cpp
        runtime/world_streaming/WorldStreamingErrors.cpp
        runtime/world_streaming/WorldStreamingLifecycleErrors.cpp
        runtime/world_streaming/WorldStreamingIdentity.cpp
)
add_library(HoroEngine::WorldStreaming ALIAS HoroWorldStreaming)
target_compile_features(HoroWorldStreaming PUBLIC cxx_std_20)
target_link_libraries(HoroWorldStreaming PUBLIC HoroEngine::Foundation HoroEngine::Assets)

add_library(HoroPrefab STATIC
        runtime/prefab/CookedPrefab.cpp
        runtime/prefab/CookedPrefabCodec.cpp
        runtime/prefab/PrefabErrors.cpp
        runtime/prefab/PrefabIdentity.cpp
        runtime/prefab/PrefabLimits.cpp
)
add_library(HoroEngine::Prefab ALIAS HoroPrefab)
target_compile_features(HoroPrefab PUBLIC cxx_std_20)
target_link_libraries(HoroPrefab PUBLIC HoroEngine::Foundation HoroEngine::Assets HoroEngine::GameplayApi)

add_library(HoroPrefabAuthoring STATIC
        runtime/prefab/PrefabAssetDependencyClosure.cpp
        runtime/prefab/PrefabDependencyGraph.cpp
        runtime/prefab/PrefabDiagnostics.cpp
        runtime/prefab/PrefabDocumentBehaviorComposition.cpp
        runtime/prefab/PrefabDocumentCanonical.cpp
        runtime/prefab/PrefabDocument.cpp
        runtime/prefab/PrefabDocumentSerialization.cpp
        runtime/prefab/PrefabSceneIdentityRemap.cpp
        runtime/prefab/PrefabSourceResolver.cpp
)
add_library(HoroEngine::PrefabAuthoring ALIAS HoroPrefabAuthoring)
target_compile_features(HoroPrefabAuthoring PUBLIC cxx_std_20)
target_include_directories(HoroPrefabAuthoring PRIVATE ${PROJECT_SOURCE_DIR}/src/foundation/json)
target_link_libraries(HoroPrefabAuthoring PUBLIC HoroEngine::Prefab HoroEngine::Application
        PRIVATE nlohmann_json::nlohmann_json)

# Load/cook bridge. RuntimeScene never depends on prefab source authoring or Editor.
add_library(HoroPrefabSceneExpansion STATIC runtime/prefab/PrefabSceneExpansion.cpp)
add_library(HoroEngine::PrefabSceneExpansion ALIAS HoroPrefabSceneExpansion)
target_compile_features(HoroPrefabSceneExpansion PUBLIC cxx_std_20)
target_link_libraries(HoroPrefabSceneExpansion PUBLIC HoroEngine::PrefabAuthoring HoroEngine::RuntimeScene)

add_library(HoroInput STATIC
        runtime/input/Input.cpp
        runtime/input/InputPresentation.cpp
        runtime/input/InputErrors.cpp
)
add_library(HoroEngine::Input ALIAS HoroInput)
target_compile_features(HoroInput PUBLIC cxx_std_20)
target_include_directories(HoroInput PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(HoroInput PUBLIC HoroEngine::Foundation PRIVATE nlohmann_json::nlohmann_json)

# Host-facing bridge: Input remains independent of Runtime UI, and Runtime UI
# retains its Foundation/Assets-only contract.
add_library(HoroRuntimeUiInput STATIC
        runtime/ui/input/UiTextInputAdapter.cpp
        runtime/ui/input/UiNavigationInput.cpp
)
add_library(HoroEngine::RuntimeUiInput ALIAS HoroRuntimeUiInput)
target_compile_features(HoroRuntimeUiInput PUBLIC cxx_std_20)
target_link_libraries(HoroRuntimeUiInput PUBLIC HoroEngine::Input HoroEngine::RuntimeUi)

add_library(HoroRenderApi STATIC
        runtime/renderer/api/MotionHistory.cpp
        runtime/renderer/api/MotionHistoryErrors.cpp
        runtime/renderer/api/PipelineCache.cpp
        runtime/renderer/api/PipelineCacheErrors.cpp
        runtime/renderer/api/RenderAdapter.cpp
        runtime/renderer/api/RenderAdapterErrors.cpp
        runtime/renderer/api/RenderDisplay.cpp
        runtime/renderer/api/RenderDisplayErrors.cpp
        runtime/renderer/api/PresentMode.cpp
        runtime/renderer/api/PresentModeErrors.cpp
        runtime/renderer/api/RenderSurfaceLifecycle.cpp
        runtime/renderer/api/RenderSurfaceLifecycleErrors.cpp
        runtime/renderer/api/RenderGraph.cpp
        runtime/renderer/api/RenderGraphExecution.cpp
        runtime/renderer/api/RenderGraphExecutionErrors.cpp
        runtime/renderer/api/RenderGraphErrors.cpp
        runtime/renderer/api/RenderGraphSynchronization.cpp
        runtime/renderer/api/RenderGraphSynchronizationErrors.cpp
        runtime/renderer/api/RenderGraphLifetime.cpp
        runtime/renderer/api/RenderGraphLifetimeErrors.cpp
        runtime/renderer/api/RenderGraphValidation.cpp
        runtime/renderer/api/RenderResourceDescriptorErrors.cpp
        runtime/renderer/api/RenderResourceAdmission.cpp
        runtime/renderer/api/RenderResourceDescriptors.cpp
        runtime/renderer/api/ShaderCompilerPipeline.cpp
        runtime/renderer/api/ShaderCompilerPipelineErrors.cpp
        runtime/renderer/api/ShaderManifest.cpp
        runtime/renderer/api/ShaderManifestErrors.cpp
        runtime/renderer/api/ShaderPermutation.cpp
        runtime/renderer/api/ShaderPermutationErrors.cpp
        runtime/renderer/api/ShaderReflection.cpp
        runtime/renderer/api/ShaderReflectionErrors.cpp
        runtime/renderer/api/StandardPbrMaterial.cpp
        runtime/renderer/api/StandardPbrMaterialErrors.cpp
        runtime/renderer/api/StandardPbrPassPlan.cpp
        runtime/renderer/api/StandardPbrPassPlanErrors.cpp
        runtime/renderer/api/TemporalHistory.cpp
        runtime/renderer/api/TemporalHistoryErrors.cpp
)
add_library(HoroEngine::RenderApi ALIAS HoroRenderApi)

target_compile_features(HoroRenderApi PUBLIC cxx_std_20)
target_include_directories(HoroRenderApi
        PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)
target_link_libraries(HoroRenderApi PUBLIC HoroEngine::Foundation)

add_library(HoroShaderCompilerToolchain STATIC
        runtime/renderer/toolchain/ShaderCompilerToolchainCatalog.cpp
        runtime/renderer/toolchain/ShaderCompilerToolchain.cpp
        runtime/renderer/toolchain/ShaderCompilerToolchainSupport.cpp
)
add_library(HoroEngine::ShaderCompilerToolchain ALIAS HoroShaderCompilerToolchain)
target_compile_features(HoroShaderCompilerToolchain PUBLIC cxx_std_20)
target_include_directories(HoroShaderCompilerToolchain
        PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/runtime/renderer/toolchain>
)
target_link_libraries(HoroShaderCompilerToolchain
        PUBLIC HoroEngine::RenderApi
        PRIVATE HoroEngine::Platform
)

add_library(HoroRenderBackendRegistry STATIC
        runtime/renderer/backend_registry/RenderBackendRegistry.cpp
        runtime/renderer/backend_registry/RenderBackendRegistryErrors.cpp
)
add_library(HoroEngine::RenderBackendRegistry ALIAS HoroRenderBackendRegistry)

target_compile_features(HoroRenderBackendRegistry PUBLIC cxx_std_20)
target_include_directories(HoroRenderBackendRegistry
        PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)
target_link_libraries(HoroRenderBackendRegistry PUBLIC HoroEngine::RenderApi)

add_library(HoroRenderFrontend STATIC
        runtime/renderer/frontend/BoundedRenderQueueCore.h
        runtime/renderer/frontend/RenderTransferErrorDescriptor.h
        runtime/renderer/frontend/RenderFrontend.cpp
        runtime/renderer/frontend/RenderFrontendResourceAccess.cpp
        runtime/renderer/frontend/RenderFrontendResourceValidation.cpp
        runtime/renderer/frontend/RenderFrontendResources.cpp
        runtime/renderer/frontend/RenderFrontendErrors.cpp
        runtime/renderer/frontend/RenderMemoryBudget.cpp
        runtime/renderer/frontend/RenderMemoryBudgetErrors.cpp
        runtime/renderer/frontend/RenderReadback.cpp
        runtime/renderer/frontend/RenderReadbackErrors.cpp
        runtime/renderer/frontend/RenderUpload.cpp
        runtime/renderer/frontend/RenderUploadErrors.cpp
        runtime/renderer/frontend/RenderQuery.cpp
        runtime/renderer/frontend/RenderQueryErrors.cpp
        runtime/renderer/frontend/RenderResourceOperations.cpp
        runtime/renderer/frontend/RenderResourceRegistry.cpp
        runtime/renderer/frontend/UiRenderComposition.cpp
)
add_library(HoroEngine::RenderFrontend ALIAS HoroRenderFrontend)

target_compile_features(HoroRenderFrontend PUBLIC cxx_std_20)
target_include_directories(HoroRenderFrontend
        PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)
target_link_libraries(HoroRenderFrontend
        PUBLIC
        HoroEngine::RenderApi
        HoroEngine::RenderBackendRegistry
        HoroEngine::RuntimeUi
)

add_library(HoroSceneModel STATIC
        runtime/scene/PropertyBindingErrors.cpp
        runtime/scene/PropertyBindingRegistry.cpp
        runtime/scene/primitive/PrimitiveCatalog.cpp
        runtime/scene/primitive/PrimitiveMesh.cpp
        runtime/scene/primitive/PrimitiveMeshErrors.cpp
)
add_library(HoroEngine::SceneModel ALIAS HoroSceneModel)

target_compile_features(HoroSceneModel PUBLIC cxx_std_20)
target_include_directories(HoroSceneModel
        PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)
target_link_libraries(HoroSceneModel
        PUBLIC HoroEngine::Foundation HoroEngine::GameplayApi HoroEngine::RenderApi HoroEngine::RuntimeUi HoroEngine::AudioApi
)

# CinematicModel is declared earlier for the runtime target graph, while its
# SceneModel dependency is defined in this block. Keep the public edge explicit
# after both targets exist.
target_link_libraries(HoroCinematicModel PUBLIC HoroEngine::SceneModel)

# RuntimeSceneDefinition exposes PrimitiveMeshDescriptor in its public scene
# contract, so the owning SceneModel dependency is part of the public edge.
target_link_libraries(HoroRuntimeScene PUBLIC HoroEngine::SceneModel)

add_library(HoroRenderNull STATIC
        runtime/renderer/modules/null/NullRenderBackend.cpp
        runtime/renderer/modules/null/NullRenderBackendErrors.cpp
)
add_library(HoroEngine::RenderNull ALIAS HoroRenderNull)

target_compile_features(HoroRenderNull PUBLIC cxx_std_20)
target_include_directories(HoroRenderNull
        PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)
target_link_libraries(HoroRenderNull
        PUBLIC
        HoroEngine::RenderApi
        HoroEngine::RenderBackendRegistry
)

if (HORO_BUILD_RENDER_OPENGL)
    add_library(HoroRenderOpenGL STATIC
            runtime/renderer/modules/opengl/OpenGLBackendModule.cpp
            runtime/renderer/modules/opengl/OpenGLCommandFunctions.cpp
            runtime/renderer/modules/opengl/OpenGLRenderBackend.cpp
            runtime/renderer/modules/opengl/OpenGLRenderBackendErrors.cpp
            runtime/renderer/modules/opengl/OpenGLRenderBackendResources.inl
    )
    add_library(HoroEngine::RenderOpenGL ALIAS HoroRenderOpenGL)

    target_compile_features(HoroRenderOpenGL PUBLIC cxx_std_20)
    target_include_directories(HoroRenderOpenGL
            PUBLIC
            ${PROJECT_SOURCE_DIR}/include
    )
    target_compile_definitions(HoroRenderOpenGL PRIVATE GL_SILENCE_DEPRECATION)
    target_link_libraries(HoroRenderOpenGL
            PUBLIC
            HoroEngine::RenderBackendRegistry
            PRIVATE
            HoroThirdParty::Glad
            OpenGL::GL
    )
endif ()

if (HORO_BUILD_RENDER_METAL)
    add_library(HoroRenderMetal STATIC
            runtime/renderer/modules/metal/MetalDeviceCapabilities.cpp
            runtime/renderer/modules/metal/MetalRenderBackend.cpp
            runtime/renderer/modules/metal/MetalRenderBackendErrors.cpp
            runtime/renderer/modules/metal/MetalResourceFormats.mm
            runtime/renderer/modules/metal/MetalResourcePolicy.cpp
            runtime/renderer/modules/metal/MetalResourceRuntime.mm
            runtime/renderer/modules/metal/MetalRuntime.mm
    )
    add_library(HoroEngine::RenderMetal ALIAS HoroRenderMetal)

    target_compile_features(HoroRenderMetal PUBLIC cxx_std_20)
    target_compile_options(HoroRenderMetal PRIVATE -fobjc-arc)
    target_include_directories(HoroRenderMetal
            PUBLIC
            ${PROJECT_SOURCE_DIR}/include
    )
    target_link_libraries(HoroRenderMetal
            PUBLIC
            HoroEngine::RenderBackendRegistry
            PRIVATE
            "-framework Foundation"
            "-framework Metal"
            "-framework QuartzCore"
    )
endif ()

if (HORO_BUILD_RENDER_VULKAN)
    add_library(HoroRenderVulkan STATIC
            runtime/renderer/modules/vulkan/VulkanAdapterDiscovery.cpp
            runtime/renderer/modules/vulkan/VulkanBackendModule.cpp
            runtime/renderer/modules/vulkan/VulkanNativeIdentity.cpp
            runtime/renderer/modules/vulkan/VulkanNativeRuntime.cpp
            runtime/renderer/modules/vulkan/VulkanResourceConversions.cpp
            runtime/renderer/modules/vulkan/VulkanResourceRuntime.cpp
            runtime/renderer/modules/vulkan/VulkanRenderBackend.cpp
            runtime/renderer/modules/vulkan/VulkanRenderBackendErrors.cpp
    )
    add_library(HoroEngine::RenderVulkan ALIAS HoroRenderVulkan)

    target_compile_features(HoroRenderVulkan PUBLIC cxx_std_20)
    target_link_libraries(HoroRenderVulkan
            PUBLIC HoroEngine::RenderBackendRegistry
            PRIVATE Vulkan::Headers
    )
endif ()

add_library(HoroEditorModel STATIC
        editor/data_bus/EditorDataBus.cpp
        editor/update/UpdateExperienceSession.cpp
        editor/document/NavigationAgentJson.cpp
        editor/document/SceneDocument.cpp
        editor/document/SceneDocumentCommands.cpp
        editor/document/SceneDocumentObjectCommands.cpp
        editor/document/RuntimeSceneConversion.cpp
        editor/project_model/EditorModelErrors.cpp
        editor/project_model/EditorSelectionModel.cpp
        editor/project_model/EditorViewportCamera.cpp
        editor/project_model/EditorViewportModel.cpp
)
add_library(HoroEngine::EditorModel ALIAS HoroEditorModel)

target_compile_features(HoroEditorModel PUBLIC cxx_std_20)
target_include_directories(HoroEditorModel
        PUBLIC
        ${PROJECT_SOURCE_DIR}/include
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
)
target_link_libraries(HoroEditorModel
        PUBLIC
        HoroEngine::Foundation
        HoroEngine::AI
        HoroEngine::Prefab
        HoroEngine::PrefabAuthoring
        HoroEngine::PrefabSceneExpansion
        HoroEngine::SceneModel
        HoroEngine::RuntimeScene
        PRIVATE nlohmann_json::nlohmann_json
)

add_library(HoroEditorViewportScene STATIC
        editor/renderer/EditorViewportScene.cpp
        editor/renderer/grid/EditorViewportGridGeometry.cpp
        editor/screens/workspace/panels/viewport/visualizers/light/LightVisualizerGeometry.cpp
)
add_library(HoroEngine::EditorViewportScene ALIAS HoroEditorViewportScene)

target_compile_features(HoroEditorViewportScene PUBLIC cxx_std_20)
target_include_directories(HoroEditorViewportScene
        PUBLIC
        ${PROJECT_SOURCE_DIR}/include
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
)
target_link_libraries(HoroEditorViewportScene PUBLIC HoroEngine::EditorModel)

add_library(HoroEditorViewportResources STATIC
        editor/renderer/EditorViewportResources.cpp
)
add_library(HoroEngine::EditorViewportResources ALIAS HoroEditorViewportResources)

target_compile_features(HoroEditorViewportResources PUBLIC cxx_std_20)
target_include_directories(HoroEditorViewportResources
        PRIVATE
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
)
target_link_libraries(HoroEditorViewportResources
        PUBLIC
        HoroEngine::EditorViewportScene
        HoroEngine::RenderFrontend
)

add_library(HoroEditorRenderExtraction STATIC
        editor/document/EditorAssetMeshCache.cpp
        editor/document/EditorRenderExtractionErrors.cpp
        editor/document/EditorViewportSceneExtractor.cpp
        editor/document/EditorViewportPicking.cpp
        editor/screens/workspace/AssetSceneDrop.cpp
)
add_library(HoroEngine::EditorRenderExtraction ALIAS HoroEditorRenderExtraction)

target_compile_features(HoroEditorRenderExtraction PUBLIC cxx_std_20)
target_include_directories(HoroEditorRenderExtraction
        PUBLIC
        ${PROJECT_SOURCE_DIR}/include
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
)
target_link_libraries(HoroEditorRenderExtraction
        PUBLIC
        HoroEngine::EditorModel
        HoroEngine::EditorViewportScene
)

if (HORO_BUILD_EDITOR_GUI AND HORO_BUILD_RENDER_METAL)
    add_library(HoroEditorViewportMetal STATIC
            editor/renderer/metal/EditorGuiRendererMetal.mm
            editor/renderer/metal/EditorViewportRendererMetal.mm
            editor/renderer/metal/MetalViewportResourceBridge.mm
            editor/renderer/metal/MetalViewportShaders.cpp
            editor/renderer/metal/SdlMetalPresentationPort.mm
    )
    add_library(HoroEngine::EditorViewportMetal ALIAS HoroEditorViewportMetal)

    target_compile_features(HoroEditorViewportMetal PUBLIC cxx_std_20)
    target_compile_options(HoroEditorViewportMetal PRIVATE -fobjc-arc)
    target_include_directories(HoroEditorViewportMetal
            PUBLIC
            ${PROJECT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}
    )
    target_link_libraries(HoroEditorViewportMetal
            PUBLIC
            HoroEngine::EditorViewportScene
            HoroEngine::EditorViewportResources
            HoroEngine::RenderMetal
            HoroEngine::RenderFrontend
            ${HORO_SDL3_TARGET}
            PRIVATE
            HoroThirdParty::ImGuiMetal
            "-framework Metal"
            "-framework QuartzCore"
    )
endif ()

add_library(HoroEditorServices STATIC
        editor/CinematicPropertyBindings.cpp
        editor/EditorServiceErrors.cpp
        editor/data_bus/EditorEngineEventBridge.cpp
        editor/data_bus/EditorSurfaceEventContext.cpp
        editor/EditorSurfaceIdentity.cpp
        editor/SourceFileOpenService.cpp
        editor/UiCanvasDocument.cpp
        editor/UiCanvasDocumentSerialization.cpp
        editor/document/SceneDocumentComparison.cpp
        editor/document/SceneDocumentPersistence.cpp
        editor/document/SceneDocumentPersistenceCommon.cpp
        editor/document/SceneDocumentPersistenceComponents.cpp
        editor/document/SceneDocumentPersistencePhysics.cpp
        editor/document/SceneDocumentPersistenceComponentParsing.cpp
        editor/document/SceneDocumentPersistenceNavigationParsing.cpp
        editor/document/SceneDocumentPersistencePhysicsParsing.cpp
        editor/document/SceneDocumentPersistenceScene.cpp
        editor/document/SceneFileWatchService.cpp
        editor/screens/welcome/WelcomeController.cpp
        editor/screens/project_creation/ProjectCreationController.cpp
        editor/gameplay/ProjectGameplayRegistry.cpp
        editor/gameplay/EditorPlaySessionController.cpp
        editor/screens/workspace/ContentBrowserModel.cpp
        editor/screens/workspace/GameplayBehaviorRequestValidation.cpp
        editor/modals/gameplay_behavior/GameplayBehaviorFilenameModalState.cpp
        editor/modals/build_release/BuildReleaseModalSession.cpp
        editor/project_model/ProjectCreationService.cpp
        editor/project_model/ProjectMutation.cpp
        editor/project_model/ProjectMigrationTransaction.cpp
        editor/project_model/ProjectOpenService.cpp
        editor/project_model/ProjectIntegrityValidatorService.cpp
        editor/project_model/RecentProjectInspectionService.cpp
        editor/project_model/ProjectMetadata.cpp
        editor/project_model/RendererAvailability.cpp
        editor/settings/EditorConfiguration.cpp
        editor/settings/EditorSettingsService.cpp
        editor/settings/EditorSettingsStore.cpp
        editor/settings/EditorSettingsStoreSerialization.cpp
        editor/localization/LocalizationService.cpp
        editor/menu/EditorMenuModel.cpp
        editor/status_bar/EditorStatusBarModel.cpp
        editor/hierarchy/HierarchyModel.cpp
        editor/NotificationService.cpp
        editor/input/EditorInputActions.cpp
        editor/input/EditorScrollSmoother.cpp
        editor/modals/EditorModalHost.cpp
        editor/screens/workspace/WorkspaceLayout.cpp
        editor/screens/workspace/WorkspacePanelHost.cpp
        editor/screens/workspace/WorkspaceLayoutPersistence.cpp
        editor/screens/workspace/ActivityBarLayout.cpp
        editor/screens/workspace/WorkspaceSplitterInteraction.cpp
)

add_library(HoroEngine::EditorServices ALIAS HoroEditorServices)

target_compile_features(HoroEditorServices PUBLIC cxx_std_20)
target_include_directories(HoroEditorServices
        PUBLIC
        ${PROJECT_SOURCE_DIR}/include
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
        PRIVATE
        ${HORO_GENERATED_DIR}
)

target_link_libraries(HoroEditorServices
        PUBLIC
        HoroEngine::Foundation
        HoroEngine::CinematicRuntime
        HoroEngine::NetworkApi
        HoroEngine::Application
        HoroEngine::Platform
        HoroEngine::RuntimeUi
        HoroEngine::EditorModel
        HoroEngine::GameplayLua
        HoroEngine::GameplayModuleHost
        HoroEngine::GameplayBuild
        HoroEngine::Input
        HoroEngine::ProjectMigrations
        PRIVATE
        HoroEngine::Assets
        nlohmann_json::nlohmann_json
)

if (TARGET HoroBuildInfo)
    add_dependencies(HoroEditorServices HoroBuildInfo)
endif ()

if (HORO_BUILD_EDITOR_GUI AND HORO_BUILD_RENDER_OPENGL)
    add_library(HoroEditorViewportOpenGL STATIC
            editor/renderer/opengl/EditorGuiRendererOpenGL.cpp
            editor/renderer/opengl/OpenGLViewportResourceBridge.cpp
            editor/renderer/opengl/OpenGLStateSnapshot.cpp
            editor/renderer/opengl/EditorViewportRendererOpenGL.cpp
            editor/renderer/opengl/SdlOpenGLPresentationPort.cpp
    )
    add_library(HoroEngine::EditorViewportOpenGL ALIAS HoroEditorViewportOpenGL)

    target_compile_features(HoroEditorViewportOpenGL PUBLIC cxx_std_20)
    target_include_directories(HoroEditorViewportOpenGL
            PUBLIC
            ${PROJECT_SOURCE_DIR}/include
            PRIVATE
            ${PROJECT_SOURCE_DIR}/src
    )
    target_compile_definitions(HoroEditorViewportOpenGL PRIVATE GL_SILENCE_DEPRECATION)
    target_link_libraries(HoroEditorViewportOpenGL
            PUBLIC
            HoroEngine::EditorViewportScene
            HoroEngine::EditorViewportResources
            HoroEngine::RenderOpenGL
            HoroEngine::RenderFrontend
            ${HORO_SDL3_TARGET}
            PRIVATE
            HoroThirdParty::Glad
            HoroThirdParty::ImGuiOpenGL
    )

endif ()

if (HORO_BUILD_INPUT_SDL3)
    add_library(HoroInputSdl STATIC
            runtime/input/sdl/SdlInputBackend.cpp
    )
    add_library(HoroEngine::InputSdl ALIAS HoroInputSdl)
    target_compile_features(HoroInputSdl PUBLIC cxx_std_20)
    target_include_directories(HoroInputSdl PUBLIC ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/src)
    target_link_libraries(HoroInputSdl PUBLIC HoroEngine::Input PRIVATE ${HORO_SDL3_TARGET})
endif ()

if (HORO_BUILD_EDITOR_GUI)
    add_library(HoroGui STATIC
            editor/design_system/components/EditorUiComponents.cpp
            editor/design_system/icons/EditorIcons.cpp
            editor/design_system/components/EditorSnackbarHost.cpp
            editor/app/EditorTheme.cpp
            editor/menu/EditorMenuPlatform.cpp
            editor/status_bar/EditorStatusBar.cpp
            editor/modals/settings/SettingsModal.cpp
            editor/modals/settings/SettingsModalExtensions.cpp
            editor/modals/settings/SettingsModalFooter.cpp
            editor/modals/settings/SettingsModalLifecycle.cpp
            editor/modals/settings/SettingsModalNavigation.cpp
            editor/modals/settings/SettingsModalUpdates.cpp
            editor/modals/settings/SettingsModalPluginDetails.cpp
            editor/modals/settings/SettingsModalPluginList.cpp
            editor/modals/settings/SettingsModalPluginShared.cpp
            editor/modals/settings/SettingsModalState.cpp
            editor/modals/asset_import/AssetImportModal.cpp
            editor/modals/asset_import/AssetImportModalLifecycle.cpp
            editor/modals/asset_import/AssetImportModalActions.cpp
            editor/modals/asset_import/AssetImportSourcePreview.cpp
            editor/modals/asset_import/AssetImportModalPresentation.cpp
            editor/modals/asset_import/AssetImportModalPresentationDetails.cpp
            editor/modals/scene_compare/SceneConflictCompareModal.cpp
            editor/modals/gameplay_behavior/GameplayBehaviorFilenameModal.cpp
            editor/screens/welcome/WelcomeView.cpp
            editor/screens/welcome/WelcomeScreen.cpp
            editor/screens/project_creation/ProjectCreationView.cpp
            editor/screens/project_creation/ProjectCreationScreen.cpp
            editor/screens/project_loading/ProjectLoadingView.cpp
            editor/screens/project_loading/ProjectLoadingScreen.cpp
            editor/screens/workspace/panels/hierarchy/HierarchyEditSession.cpp
            editor/screens/workspace/panels/hierarchy/HierarchyPanel.cpp
            editor/screens/workspace/panels/inspector/InspectorEditSession.cpp
            editor/screens/workspace/panels/inspector/InspectorPanel.cpp
            editor/screens/workspace/panels/inspector/InspectorPanelComponentWidgets.cpp
            editor/screens/workspace/panels/input_mapping/InputMappingPanel.cpp
            editor/screens/workspace/panels/game/GamePanel.cpp
            editor/screens/workspace/panels/viewport/ViewportPanel.cpp
            editor/screens/workspace/panels/viewport/ViewportOverlay.cpp
            editor/screens/workspace/panels/viewport/visualizers/light/LightMarkerLayer.cpp
        editor/screens/workspace/panels/viewport/gizmo/TransformGizmoController.cpp
            editor/screens/workspace/panels/viewport/gizmo/TransformGizmoGeometry.cpp
            editor/screens/workspace/panels/viewport/gizmo/TransformGizmoMath.cpp
            editor/screens/workspace/panels/viewport/interaction/ViewportInteractionCapture.cpp
            editor/screens/workspace/panels/viewport/interaction/ViewportInteractionController.cpp
            editor/screens/workspace/panels/viewport/navigation/ViewportNavigationController.cpp
            editor/screens/workspace/WorkspacePanelRegistry.cpp
            editor/screens/workspace/DefaultWorkspacePanels.cpp
            editor/screens/workspace/panels/global_dock/GlobalDockPanel.cpp
            editor/screens/workspace/panels/global_dock/GlobalDockPaneChrome.cpp
            editor/screens/workspace/panels/global_dock/panes/asset_browser/AssetBrowserGrid.cpp
            editor/screens/workspace/panels/global_dock/panes/asset_browser/AssetBrowserActions.cpp
            editor/screens/workspace/panels/global_dock/panes/asset_browser/AssetBrowserCards.cpp
            editor/screens/workspace/panels/global_dock/panes/asset_browser/AssetBrowserDialogs.cpp
            editor/screens/workspace/panels/global_dock/panes/asset_browser/AssetBrowserInteractionSession.cpp
            editor/screens/workspace/panels/global_dock/panes/asset_browser/AssetBrowserPane.cpp
            editor/screens/workspace/panels/global_dock/panes/asset_browser/AssetBrowserToolbar.cpp
            editor/screens/workspace/panels/global_dock/panes/audio/GlobalDockAudioPane.cpp
            editor/screens/workspace/panels/global_dock/panes/build_output/GlobalDockBuildOutputPane.cpp
            editor/screens/workspace/panels/global_dock/panes/build_output/GlobalDockBuildOutputPaneToolbar.cpp
            editor/screens/workspace/panels/global_dock/panes/console/GlobalDockConsolePane.cpp
            editor/screens/workspace/panels/global_dock/panes/mcp/GlobalDockMcpPane.cpp
            editor/screens/workspace/panels/global_dock/panes/network/GlobalDockNetworkPane.cpp
            editor/screens/workspace/panels/global_dock/panes/operations/GlobalDockOperationsPane.cpp
            editor/screens/workspace/panels/global_dock/panes/performance/GlobalDockPerformancePane.cpp
            editor/screens/workspace/panels/global_dock/panes/physics/GlobalDockPhysicsPane.cpp
            editor/screens/workspace/EditorWorkspaceController.cpp
            editor/screens/workspace/EditorWorkspaceControllerScenePersistence.cpp
            editor/screens/workspace/EditorWorkspaceControllerViewportEditing.cpp
            editor/screens/workspace/EditorWorkspaceControllerAssetInstantiation.cpp
            editor/screens/workspace/EditorWorkspaceControllerContentBrowserCommands.cpp
            editor/screens/workspace/EditorWorkspaceControllerDockCommands.cpp
            editor/screens/workspace/EditorWorkspaceControllerSceneAssets.cpp
            editor/screens/workspace/EditorWorkspaceControllerSceneRecovery.cpp
            editor/screens/workspace/EditorWorkspaceControllerSceneProjection.cpp
            editor/screens/workspace/EditorWorkspaceControllerPlay.cpp
            editor/screens/workspace/EditorWorkspaceControllerCommandRouting.cpp
            editor/screens/workspace/EditorWorkspaceControllerCommands.cpp
            editor/screens/workspace/EditorWorkspaceControllerComponentCommands.cpp
            editor/screens/workspace/EditorWorkspaceControllerViewportCommands.cpp
            editor/screens/workspace/EditorWorkspaceControllerContentBrowser.cpp
            editor/screens/workspace/EditorWorkspaceControllerContentBrowserGameplay.cpp
            editor/screens/workspace/EditorWorkspaceControllerContentBrowserAssetOperations.cpp
            editor/screens/workspace/EditorWorkspaceControllerContentBrowserSourceOperations.cpp
            editor/screens/workspace/EditorWorkspaceView.cpp
            editor/screens/workspace/EditorWorkspaceScreen.cpp
            editor/screens/NavigationErrors.cpp
            editor/screens/GuiScreenHost.cpp
    )

    add_library(HoroEngine::Gui ALIAS HoroGui)

    target_compile_features(HoroGui PUBLIC cxx_std_20)
    target_include_directories(HoroGui
            PUBLIC
            ${PROJECT_SOURCE_DIR}/include
            $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
            PRIVATE
            ${PROJECT_SOURCE_DIR}/vendor/stb
            ${PROJECT_SOURCE_DIR}/vendor/pfd
            ${HORO_GENERATED_DIR}
    )
    target_compile_definitions(HoroGui
            PRIVATE
            GL_SILENCE_DEPRECATION
            HORO_ENGINE_VERSION_STRING="${PROJECT_VERSION}"
    )
    target_link_libraries(HoroGui
            PUBLIC
            HoroEngine::EditorServices
            HoroEngine::Foundation
            HoroThirdParty::ImGui
            PRIVATE
            HoroEngine::EditorRenderExtraction
            nlohmann_json::nlohmann_json
    )

    if (APPLE)
        set(_HORO_EDITOR_MENU_MAC_SOURCE editor/menu/macos/EditorMenuPlatformMac.mm)
        set_source_files_properties(${_HORO_EDITOR_MENU_MAC_SOURCE} PROPERTIES
                LANGUAGE CXX
                COMPILE_OPTIONS "-x;objective-c++"
        )
        target_sources(HoroGui PRIVATE ${_HORO_EDITOR_MENU_MAC_SOURCE})
        target_link_libraries(HoroGui PRIVATE "-framework AppKit")
    endif ()
endif ()

add_library(HoroExtensions STATIC
        extensions/discovery/DiscoveryPaths.cpp
        extensions/discovery/DiscoveryPlan.cpp
        extensions/discovery/DiscoveryRootPolicy.cpp
        extensions/host/ExtensionManager.cpp
        extensions/host/ExtensionCapabilityAdmission.cpp
        extensions/host/EditorSurfaceContext.cpp
        extensions/host/EditorSurfaceRegistry.cpp
        extensions/host/EditorSurfaceRegistryOperations.cpp
        extensions/host/EditorSurfaceRegistryPersistence.cpp
        extensions/host/EditorSurfaceRegistryInternal.h
        extensions/host/EditorCommandRegistry.cpp
        extensions/host/EditorThemeTokensInternal.h
        extensions/host/EditorThemeTokensValidation.cpp
        extensions/host/EditorThemeTokensColor.cpp
        extensions/host/EditorThemeTokensLayout.cpp
        extensions/host/EditorThemeTokensBehavior.cpp
        extensions/host/ExtensionActivationTransaction.cpp
        extensions/host/ExtensionModuleResolution.cpp
        extensions/host/HeadlessExtensionHost.cpp
        extensions/host/ExtensionAbiValidation.cpp
        extensions/host/ExtensionAbiConformance.cpp
        extensions/registry/ApplicationCapabilityRegistry.cpp
        extensions/registry/ProcessObserverRegistry.cpp
        extensions/registry/BackendServiceRegistry.cpp
        extensions/registry/BackendOperationRegistry.cpp
        extensions/registry/BackendOperationRegistryInternal.h
        extensions/registry/BackendOperationValidation.h
        extensions/manifest/ExtensionManifestParser.cpp
        extensions/manifest/ExtensionManifestJson.cpp
        extensions/manifest/EditorSurfaceDescriptor.cpp
    extensions/manifest/EditorUiForm.cpp
    extensions/manifest/EditorUiFormBuilder.cpp
    extensions/manifest/EditorUiFormRender.cpp
        extensions/manifest/ScriptExportDescriptor.cpp
        extensions/manifest/ScriptExportDescriptorInternal.h
        extensions/manifest/ScriptExportDescriptorValidationSupport.cpp
        extensions/manifest/ScriptExportDescriptorTypeValidation.cpp
        extensions/manifest/ScriptExportDescriptorFunctionValidation.cpp
        extensions/manifest/ScriptExportDescriptorCanonical.cpp
        extensions/manifest/ScriptExportDescriptorCompatibility.cpp
        extensions/script/ScriptValue.cpp
        extensions/script/ScriptValueValidation.cpp
        extensions/script/ScriptValueCodec.cpp
        extensions/script/ScriptValueCodecInternal.h
        extensions/script/ScriptValueWire.cpp
        extensions/script/ScriptInvocationInternal.cpp
        extensions/script/ScriptInvocationInternal.h
        extensions/script/ScriptInvocationRegistration.cpp
        extensions/script/ScriptInvocation.cpp
        extensions/script/ScriptCapabilityContext.cpp
        extensions/script/ScriptCapabilityContextInternal.h
        extensions/registry/ExtensionActivationState.cpp
        extensions/registry/EditorSurfaceContextErrors.cpp
        extensions/registry/EditorCommandErrors.cpp
        extensions/registry/ExtensionErrors.cpp
        extensions/registry/ScriptExportDescriptorErrors.cpp
        extensions/registry/ScriptInvocationErrors.cpp
        extensions/registry/ExtensionInventory.cpp
        extensions/marketplace/ExtensionMarketplace.cpp
        extensions/capabilities/asset_pipeline_points/ExternalAssetImporter.cpp
        extensions/capabilities/asset_pipeline_points/ExternalAssetImporterConversion.cpp
        extensions/capabilities/asset_cooker/AssetCookerErrors.cpp
        extensions/capabilities/asset_cooker/AssetCookerRegistry.cpp
        extensions/capabilities/project_validator/ProjectValidatorRegistry.cpp
        extensions/capabilities/pipeline_step/PipelineStepRegistry.cpp
        extensions/capabilities/toolchain_provider/ToolchainProviderRegistry.cpp
)

add_library(HoroEngine::Extensions ALIAS HoroExtensions)

target_compile_features(HoroExtensions PUBLIC cxx_std_20)
target_include_directories(HoroExtensions
        PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)
target_link_libraries(HoroExtensions
        PUBLIC HoroEngine::Foundation HoroEngine::Platform HoroEngine::Assets HoroEngine::Security
        PRIVATE nlohmann_json::nlohmann_json CURL::libcurl miniz
)

# Host composition bridge: neither Platform Services nor Extensions depends on the other.
add_library(HoroPlatformServicesExtension STATIC platform_services/PlatformProviderAdmission.cpp)
add_library(HoroEngine::PlatformServicesExtension ALIAS HoroPlatformServicesExtension)
target_compile_features(HoroPlatformServicesExtension PUBLIC cxx_std_20)
target_link_libraries(HoroPlatformServicesExtension PUBLIC HoroEngine::PlatformServices HoroEngine::Extensions)

if (HORO_BUILD_EDITOR_GUI)
    target_link_libraries(HoroGui PRIVATE HoroEngine::Extensions)
endif ()

# Apply the build-tree public/private include contract only after every optional
# production target has been declared.
include(HoroPublicHeaderOwnership)
