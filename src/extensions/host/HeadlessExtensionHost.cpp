#include "Horo/Extensions/HeadlessExtensionHost.h"

#include "Horo/Extensions/ExtensionErrors.h"

#include <atomic>
#include <mutex>
#include <optional>
#include <utility>

namespace Horo::Extensions {
    struct HeadlessExtensionHost::Impl final {
        HeadlessExtensionHostConfiguration configuration;
        mutable std::mutex lifecycleMutex;
        mutable std::mutex diagnosticMutex;
        std::atomic<HeadlessExtensionHostState> state{HeadlessExtensionHostState::Configuring};
        std::vector<std::string> discoveredPackages;
        std::vector<Discovery::RootDiagnostic> rootDiagnostics;
        mutable std::vector<HeadlessExtensionHostDiagnostic> diagnostics;

        std::unique_ptr<Assets::AssetImporterCatalog> importers;
        std::unique_ptr<ExtensionManager> manager;
        std::shared_ptr<const Assets::AssetImporterCatalogSnapshot> importerSnapshot;
        ApplicationCapabilityRegistry capabilities;
        AssetCookerRegistry cookers;
        ProjectValidatorRegistry validators;
        PipelineStepRegistry pipeline;
        ToolchainProviderRegistry toolchains;

        std::vector<ApplicationCapabilityProviderRegistration> capabilityRegistrations;
        std::vector<AssetCookerRegistration> cookerRegistrations;
        std::vector<ProjectValidatorRegistration> validatorRegistrations;
        std::vector<PipelineStepRegistration> pipelineRegistrations;
        std::vector<ToolchainProviderRegistration> toolchainRegistrations;

        Impl(HeadlessExtensionHostConfiguration hostConfiguration, ProjectValidatorRegistry validatorRegistry,
             const IToolchainInvocationPolicy &toolchainPolicy, IExternalProcessRunner &processes)
            : configuration(std::move(hostConfiguration)), importers(std::make_unique<Assets::AssetImporterCatalog>()),
              manager(std::make_unique<ExtensionManager>(importers.get(), ExtensionHostProfile::Headless, configuration.capabilities,
                                                         configuration.artifactGate, configuration.libraryLoader)),
              validators(std::move(validatorRegistry)), toolchains(toolchainPolicy, processes) {}

        void Record(const HeadlessExtensionHostStage stage, std::string subject, const Error &error) const {
            std::scoped_lock lock{diagnosticMutex};
            if (diagnostics.size() >= configuration.maximumDiagnostics)
                diagnostics.erase(diagnostics.begin());
            diagnostics.push_back({stage, std::move(subject), error});
        }

        [[nodiscard]] Result<void> RequireConfiguring() const {
            if (state.load(std::memory_order_acquire) == HeadlessExtensionHostState::Configuring)
                return Result<void>::Success();
            return Result<void>::Failure(
                MakeError(ExtensionErrors::HeadlessHostStateInvalid, "Provider registration is closed after host startup begins."));
        }

        [[nodiscard]] std::optional<Error> ReadyFailure() const {
            if (state.load(std::memory_order_acquire) == HeadlessExtensionHostState::Ready)
                return std::nullopt;
            return MakeError(ExtensionErrors::HeadlessHostStateInvalid, "Headless extension work requires a ready host.");
        }

        template <typename Registration>
        [[nodiscard]] Result<void> Retain(Result<Registration> registration, std::vector<Registration> &registrations) {
            if (registration.HasError())
                return Result<void>::Failure(registration.ErrorValue());
            registrations.push_back(std::move(registration).Value());
            return Result<void>::Success();
        }

        template <typename Operation> [[nodiscard]] auto Configure(std::string subject, Operation &&operation) -> decltype(operation()) {
            using OperationResult = decltype(operation());
            std::scoped_lock lock{lifecycleMutex};
            if (Result<void> configuring = RequireConfiguring(); configuring.HasError()) {
                Record(HeadlessExtensionHostStage::Registration, std::move(subject), configuring.ErrorValue());
                return OperationResult::Failure(configuring.ErrorValue());
            }
            OperationResult result = operation();
            if (result.HasError())
                Record(HeadlessExtensionHostStage::Registration, std::move(subject), result.ErrorValue());
            return result;
        }

        template <typename T>
        [[nodiscard]] Result<T> RejectWork(const HeadlessExtensionHostStage stage, const std::string_view subject,
                                           const Error &error) const {
            Record(stage, std::string{subject}, error);
            return Result<T>::Failure(error);
        }

        [[nodiscard]] Result<void> FailStartup(const HeadlessExtensionHostStage stage, std::string subject, Error error) {
            Record(stage, std::move(subject), error);
            manager->UnloadAll();
            importers->Reset();
            state.store(HeadlessExtensionHostState::Failed, std::memory_order_release);
            return Result<void>::Failure(std::move(error));
        }
    };

    namespace {
        constexpr std::size_t MaximumRetainedHeadlessDiagnostics = 4096U;
    }

    /** @copydoc HeadlessExtensionHost::Create */
    Result<std::unique_ptr<HeadlessExtensionHost>> HeadlessExtensionHost::Create(HeadlessExtensionHostConfiguration configuration,
                                                                                 const IToolchainInvocationPolicy &toolchainPolicy,
                                                                                 IExternalProcessRunner &processes) {
        if (configuration.maximumDiagnostics == 0U || configuration.maximumDiagnostics > MaximumRetainedHeadlessDiagnostics) {
            return Result<std::unique_ptr<HeadlessExtensionHost>>::Failure(
                MakeError(ExtensionErrors::HeadlessHostConfigurationInvalid, "Headless diagnostic retention bound is invalid."));
        }
        auto validatorRegistry = ProjectValidatorRegistry::Create(configuration.validationErrors, configuration.validationLimits);
        if (validatorRegistry.HasError()) {
            return Result<std::unique_ptr<HeadlessExtensionHost>>::Failure(WrapError(ExtensionErrors::HeadlessHostConfigurationInvalid,
                                                                                     validatorRegistry.ErrorValue(),
                                                                                     "Headless validator result policy is invalid."));
        }
        auto impl = std::make_unique<Impl>(std::move(configuration), std::move(validatorRegistry).Value(), toolchainPolicy, processes);
        return Result<std::unique_ptr<HeadlessExtensionHost>>::Success(
            std::unique_ptr<HeadlessExtensionHost>{new HeadlessExtensionHost(std::move(impl))});
    }

    HeadlessExtensionHost::HeadlessExtensionHost(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

    HeadlessExtensionHost::~HeadlessExtensionHost() noexcept {
        Shutdown();
    }

    /** @copydoc HeadlessExtensionHost::RegisterCapability */
    Result<void> HeadlessExtensionHost::RegisterCapability(ApplicationCapabilityProviderDescriptor descriptor) {
        const std::string subject = descriptor.capability.value;
        return impl_->Configure(subject, [&] {
            return impl_->Retain(impl_->capabilities.Register(std::move(descriptor)), impl_->capabilityRegistrations);
        });
    }

    /** @copydoc HeadlessExtensionHost::RegisterImporter */
    Result<void> HeadlessExtensionHost::RegisterImporter(Assets::AssetImporterContribution contribution) {
        const std::string subject = contribution.contributionId;
        return impl_->Configure(subject, [&] {
            return impl_->importers->Register(std::move(contribution));
        });
    }

    /** @copydoc HeadlessExtensionHost::RegisterCooker */
    Result<void> HeadlessExtensionHost::RegisterCooker(AssetCookerDescriptor descriptor, std::shared_ptr<const IAssetCooker> provider) {
        const std::string subject = descriptor.cookerId.value;
        return impl_->Configure(subject, [&] {
            return impl_->Retain(impl_->cookers.Register(std::move(descriptor), std::move(provider)), impl_->cookerRegistrations);
        });
    }

    /** @copydoc HeadlessExtensionHost::RegisterValidator */
    Result<void> HeadlessExtensionHost::RegisterValidator(ProjectValidatorProviderDescriptor descriptor,
                                                          std::shared_ptr<const IProjectValidator> provider) {
        const std::string subject = descriptor.validatorId.value;
        return impl_->Configure(subject, [&] {
            return impl_->Retain(impl_->validators.Register(std::move(descriptor), std::move(provider)), impl_->validatorRegistrations);
        });
    }

    /** @copydoc HeadlessExtensionHost::RegisterPipelineStep */
    Result<void> HeadlessExtensionHost::RegisterPipelineStep(PipelineStepDescriptor descriptor,
                                                             std::shared_ptr<const IPipelineStep> provider) {
        const std::string subject = descriptor.stepId.value;
        return impl_->Configure(subject, [&] {
            return impl_->Retain(impl_->pipeline.Register(std::move(descriptor), std::move(provider)), impl_->pipelineRegistrations);
        });
    }

    /** @copydoc HeadlessExtensionHost::RegisterToolchain */
    Result<ToolchainInvocationAuthority> HeadlessExtensionHost::RegisterToolchain(ToolchainProviderDescriptor descriptor) {
        const std::string subject = descriptor.contributionId;
        return impl_->Configure(subject, [&] {
            auto registered = impl_->toolchains.Register(std::move(descriptor));
            if (registered.HasError())
                return Result<ToolchainInvocationAuthority>::Failure(registered.ErrorValue());
            ToolchainInvocationAuthority authority = registered.Value().Authority();
            impl_->toolchainRegistrations.push_back(std::move(registered).Value());
            return Result<ToolchainInvocationAuthority>::Success(std::move(authority));
        });
    }

    /** @copydoc HeadlessExtensionHost::Start */
    Result<void> HeadlessExtensionHost::Start(const std::span<const Discovery::PackageLocation> locations) {
        std::scoped_lock lock{impl_->lifecycleMutex};
        if (Result<void> configuring = impl_->RequireConfiguring(); configuring.HasError()) {
            impl_->Record(HeadlessExtensionHostStage::Activation, "headless-host", configuring.ErrorValue());
            return configuring;
        }

        auto discovered = Discovery::DiscoverDeclaredPackages(impl_->configuration.roots, locations, impl_->configuration.discoveryPolicy);
        if (discovered.HasError()) {
            Error failure = WrapError(ExtensionErrors::HeadlessHostDiscoveryFailed, discovered.ErrorValue());
            return impl_->FailStartup(HeadlessExtensionHostStage::Discovery, "declared-packages", std::move(failure));
        }
        Discovery::DiscoveryPlan plan = std::move(discovered).Value();
        impl_->rootDiagnostics = plan.rootDiagnostics;
        impl_->discoveredPackages.reserve(plan.packages.size());
        for (const Discovery::DiscoveredPackage &package : plan.packages) {
            impl_->discoveredPackages.push_back(package.packageId);
            auto activated = impl_->manager->LoadExtension(package.canonicalPath.string());
            if (activated.HasError()) {
                Error failure = WrapError(ExtensionErrors::HeadlessHostActivationFailed, activated.ErrorValue(),
                                          "Headless package activation failed: " + package.packageId);
                return impl_->FailStartup(HeadlessExtensionHostStage::Activation, package.packageId, std::move(failure));
            }
        }

        auto published = impl_->importers->Publish();
        if (published.HasError()) {
            Error failure = WrapError(ExtensionErrors::HeadlessHostActivationFailed, published.ErrorValue(),
                                      "Headless importer catalog publication failed.");
            return impl_->FailStartup(HeadlessExtensionHostStage::Activation, "asset.importer", std::move(failure));
        }
        impl_->importerSnapshot = std::move(published).Value();
        impl_->state.store(HeadlessExtensionHostState::Ready, std::memory_order_release);
        return Result<void>::Success();
    }

    /** @copydoc HeadlessExtensionHost::Import */
    Result<Assets::PreparedAssetImport> HeadlessExtensionHost::Import(const std::string_view contributionId,
                                                                      const Assets::AssetImportInput &input,
                                                                      const CancellationToken &cancellation) const {
        if (const auto error = impl_->ReadyFailure())
            return impl_->RejectWork<Assets::PreparedAssetImport>(HeadlessExtensionHostStage::Import, contributionId, *error);
        std::shared_ptr<const Assets::AssetImporterCatalogSnapshot> snapshot;
        {
            std::scoped_lock lock{impl_->lifecycleMutex};
            snapshot = impl_->importerSnapshot;
        }
        const Assets::AssetImporterContribution *contribution = snapshot != nullptr ? snapshot->FindById(contributionId) : nullptr;
        if (contribution == nullptr || contribution->strategy == nullptr) {
            Error error = MakeError(ExtensionErrors::HeadlessImporterUnavailable,
                                    "No published importer matches contribution: " + std::string{contributionId});
            impl_->Record(HeadlessExtensionHostStage::Import, std::string{contributionId}, error);
            return Result<Assets::PreparedAssetImport>::Failure(std::move(error));
        }
        auto imported = contribution->strategy->Import(input, cancellation);
        if (imported.HasError())
            impl_->Record(HeadlessExtensionHostStage::Import, std::string{contributionId}, imported.ErrorValue());
        return imported;
    }

    /** @copydoc HeadlessExtensionHost::Cook */
    Result<AssetCookerResult> HeadlessExtensionHost::Cook(const AssetCookerRequest &request, const CancellationToken &cancellation) const {
        if (const auto error = impl_->ReadyFailure())
            return impl_->RejectWork<AssetCookerResult>(HeadlessExtensionHostStage::Cook, request.input.assetType.Value(), *error);
        auto result = impl_->cookers.Cook(request, cancellation);
        if (result.HasError())
            impl_->Record(HeadlessExtensionHostStage::Cook, request.input.assetType.Value(), result.ErrorValue());
        return result;
    }

    /** @copydoc HeadlessExtensionHost::Validate */
    Result<std::vector<AttributedProjectValidationResult>> HeadlessExtensionHost::Validate(const ProjectValidationSnapshot &snapshot,
                                                                                           const CancellationToken &cancellation) const {
        if (const auto error = impl_->ReadyFailure())
            return impl_->RejectWork<std::vector<AttributedProjectValidationResult>>(HeadlessExtensionHostStage::Validation,
                                                                                     snapshot.projectId, *error);
        auto result = impl_->validators.ValidateAll(snapshot, cancellation);
        if (result.HasError())
            impl_->Record(HeadlessExtensionHostStage::Validation, std::string{snapshot.projectId}, result.ErrorValue());
        return result;
    }

    /** @copydoc HeadlessExtensionHost::ExecutePipeline */
    Result<PipelineRunResult> HeadlessExtensionHost::ExecutePipeline(const std::span<const PipelineArtifactView> initialArtifacts,
                                                                     const CancellationToken &cancellation) const {
        if (const auto error = impl_->ReadyFailure())
            return impl_->RejectWork<PipelineRunResult>(HeadlessExtensionHostStage::Pipeline, "pipeline", *error);
        auto result = impl_->pipeline.Execute(initialArtifacts, cancellation);
        if (result.HasError())
            impl_->Record(HeadlessExtensionHostStage::Pipeline, "pipeline", result.ErrorValue());
        return result;
    }

    /** @copydoc HeadlessExtensionHost::InvokeToolchain */
    Result<ToolchainInvocationResult> HeadlessExtensionHost::InvokeToolchain(const ToolchainInvocationAuthority &authority,
                                                                             const ToolchainInvocationIntent &intent,
                                                                             const CancellationToken &cancellation) const {
        if (const auto error = impl_->ReadyFailure())
            return impl_->RejectWork<ToolchainInvocationResult>(HeadlessExtensionHostStage::Toolchain, authority.contributionId, *error);
        auto result = impl_->toolchains.Invoke(authority, intent, cancellation);
        if (result.HasError())
            impl_->Record(HeadlessExtensionHostStage::Toolchain, authority.contributionId, result.ErrorValue());
        return result;
    }

    /** @copydoc HeadlessExtensionHost::ResolveCapability */
    Result<ApplicationCapabilityProviderLease> HeadlessExtensionHost::ResolveCapability(const ExtensionCapabilityHandle &authority,
                                                                                        const ApplicationCapabilityVersionRange &versions,
                                                                                        const std::string_view extensionId,
                                                                                        const std::string_view moduleId,
                                                                                        const std::uint64_t activationGeneration) const {
        if (const auto error = impl_->ReadyFailure())
            return impl_->RejectWork<ApplicationCapabilityProviderLease>(HeadlessExtensionHostStage::CapabilityResolution,
                                                                         authority.Capability().value, *error);
        auto result = impl_->capabilities.Resolve(authority, versions, extensionId, moduleId, activationGeneration);
        if (result.HasError())
            impl_->Record(HeadlessExtensionHostStage::CapabilityResolution, authority.Capability().value, result.ErrorValue());
        return result;
    }

    /** @copydoc HeadlessExtensionHost::Inspect */
    HeadlessExtensionHostSnapshot HeadlessExtensionHost::Inspect() const {
        HeadlessExtensionHostSnapshot snapshot;
        std::scoped_lock lock{impl_->lifecycleMutex, impl_->diagnosticMutex};
        snapshot.state = impl_->state.load(std::memory_order_acquire);
        snapshot.discoveredPackages = impl_->discoveredPackages;
        snapshot.rootDiagnostics = impl_->rootDiagnostics;
        if (impl_->manager != nullptr)
            snapshot.loadedExtensions = impl_->manager->GetLoadedExtensionIds();
        snapshot.diagnostics = impl_->diagnostics;
        return snapshot;
    }

    /** @copydoc HeadlessExtensionHost::Shutdown */
    void HeadlessExtensionHost::Shutdown() noexcept {
        if (impl_ == nullptr)
            return;
        std::scoped_lock lock{impl_->lifecycleMutex};
        const HeadlessExtensionHostState current = impl_->state.load(std::memory_order_acquire);
        if (current == HeadlessExtensionHostState::Shutdown || current == HeadlessExtensionHostState::ShuttingDown)
            return;
        impl_->state.store(HeadlessExtensionHostState::ShuttingDown, std::memory_order_release);

        impl_->toolchains.BeginShutdown();
        impl_->pipeline.BeginShutdown();
        impl_->validators.BeginShutdown();
        impl_->cookers.BeginShutdown();
        impl_->capabilities.BeginShutdown();
        impl_->toolchainRegistrations.clear();
        impl_->pipelineRegistrations.clear();
        impl_->validatorRegistrations.clear();
        impl_->cookerRegistrations.clear();
        impl_->capabilityRegistrations.clear();
        if (impl_->manager != nullptr)
            impl_->manager->UnloadAll();
        impl_->importerSnapshot.reset();
        impl_->importers.reset();
        impl_->manager.reset();
        impl_->state.store(HeadlessExtensionHostState::Shutdown, std::memory_order_release);
    }
}  // namespace Horo::Extensions
