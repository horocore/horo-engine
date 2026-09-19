#include "Horo/Extensions/HeadlessExtensionHost.h"

#include "Horo/Extensions/ExtensionErrors.h"

#include <atomic>
#include <deque>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <utility>

namespace Horo::Extensions {
    namespace {
        class HeadlessHostSynchronization final {
        public:
            [[nodiscard]] std::shared_mutex &Lifecycle() const noexcept {
                return lifecycle_;
            }

            [[nodiscard]] std::mutex &Shutdown() const noexcept {
                return shutdown_;
            }

        private:
            mutable std::shared_mutex lifecycle_;
            mutable std::mutex shutdown_;
        };
    }  // namespace

    class HeadlessExtensionHost::Impl final {
        friend class HeadlessExtensionHost;

        HeadlessExtensionHostConfiguration configuration;
        HeadlessHostSynchronization synchronization;
        mutable std::mutex diagnosticMutex;
        mutable std::deque<HeadlessExtensionHostDiagnostic> diagnostics;
        std::atomic<HeadlessExtensionHostState> state{HeadlessExtensionHostState::Configuring};
        std::vector<std::string> discoveredPackages;
        std::vector<Discovery::RootDiagnostic> rootDiagnostics;

        std::unique_ptr<Assets::AssetImporterCatalog> importers{std::make_unique<Assets::AssetImporterCatalog>()};
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

    public:
        Impl(HeadlessExtensionHostConfiguration hostConfiguration, ProjectValidatorRegistry validatorRegistry,
             const IToolchainInvocationPolicy &toolchainPolicy, IExternalProcessRunner &processes)
            : configuration(std::move(hostConfiguration)),
              manager(std::make_unique<ExtensionManager>(importers.get(), ExtensionHostProfile::Headless, configuration.capabilities,
                                                         configuration.artifactGate, configuration.libraryLoader)),
              validators(std::move(validatorRegistry)), toolchains(toolchainPolicy, processes) {}

        void Record(const HeadlessExtensionHostStage stage, std::string subject, const Error &error) const {
            std::scoped_lock lock{diagnosticMutex};
            if (diagnostics.size() >= configuration.maximumDiagnostics)
                diagnostics.pop_front();
            diagnostics.emplace_back(stage, std::move(subject), error);
        }

        [[nodiscard]] Result<void> RequireConfiguring() const {
            if (state.load() == HeadlessExtensionHostState::Configuring)
                return Result<void>::Success();
            return Result<void>::Failure(
                MakeError(ExtensionErrors::HeadlessHostStateInvalid, "Provider registration is closed after host startup begins."));
        }

        [[nodiscard]] std::optional<Error> ReadyFailure() const {
            if (state.load() == HeadlessExtensionHostState::Ready)
                return std::nullopt;
            return MakeError(ExtensionErrors::HeadlessHostStateInvalid, "Headless extension work requires a ready host.");
        }

        template <typename Registration>
        [[nodiscard]] Result<void> Retain(Result<Registration> registration, std::vector<Registration> &registrations) const {
            if (registration.HasError())
                return Result<void>::Failure(registration.ErrorValue());
            registrations.push_back(std::move(registration).Value());
            return Result<void>::Success();
        }

        template <typename Operation> [[nodiscard]] auto Configure(std::string subject, Operation &&operation) -> decltype(operation()) {
            using OperationResult = decltype(operation());
            std::unique_lock lock{synchronization.Lifecycle()};
            if (Result<void> configuring = RequireConfiguring(); configuring.HasError()) {
                Record(HeadlessExtensionHostStage::Registration, std::move(subject), configuring.ErrorValue());
                return OperationResult::Failure(configuring.ErrorValue());
            }
            OperationResult result = operation();
            if (result.HasError())
                Record(HeadlessExtensionHostStage::Registration, std::move(subject), result.ErrorValue());
            return result;
        }

        template <typename Operation>
        [[nodiscard]] auto ExecuteReady(const HeadlessExtensionHostStage stage, std::string subject, Operation &&operation) const
            -> decltype(operation()) {
            using OperationResult = decltype(operation());
            std::shared_lock lock{synchronization.Lifecycle()};
            if (const auto error = ReadyFailure()) {
                Record(stage, std::move(subject), *error);
                return OperationResult::Failure(*error);
            }
            OperationResult result = operation();
            if (result.HasError())
                Record(stage, std::move(subject), result.ErrorValue());
            return result;
        }

        [[nodiscard]] Result<void> FailStartup(const HeadlessExtensionHostStage stage, std::string subject, Error error) {
            Record(stage, std::move(subject), error);
            importerSnapshot.reset();
            importers->Reset();
            if (manager != nullptr)
                manager->UnloadAll();
            manager.reset();
            HeadlessExtensionHostState expected = HeadlessExtensionHostState::Configuring;
            static_cast<void>(state.compare_exchange_strong(expected, HeadlessExtensionHostState::Failed));
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
            std::unique_ptr<HeadlessExtensionHost>{new HeadlessExtensionHost(std::move(impl))});  // NOSONAR: constructor is private.
    }

    HeadlessExtensionHost::HeadlessExtensionHost(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

    HeadlessExtensionHost::~HeadlessExtensionHost() noexcept {
        Shutdown();
    }

    /** @copydoc HeadlessExtensionHost::RegisterCapability */
    Result<void> HeadlessExtensionHost::RegisterCapability(ApplicationCapabilityProviderDescriptor descriptor) {
        const std::string subject = descriptor.capability.value;
        return impl_->Configure(subject, [this, &descriptor] {
            return impl_->Retain(impl_->capabilities.Register(std::move(descriptor)), impl_->capabilityRegistrations);
        });
    }

    /** @copydoc HeadlessExtensionHost::RegisterImporter */
    Result<void> HeadlessExtensionHost::RegisterImporter(Assets::AssetImporterContribution contribution) {
        const std::string subject = contribution.contributionId;
        return impl_->Configure(subject, [this, &contribution] {
            return impl_->importers->Register(std::move(contribution));
        });
    }

    /** @copydoc HeadlessExtensionHost::RegisterCooker */
    Result<void> HeadlessExtensionHost::RegisterCooker(AssetCookerDescriptor descriptor, std::shared_ptr<const IAssetCooker> provider) {
        const std::string subject = descriptor.cookerId.value;
        return impl_->Configure(subject, [this, &descriptor, &provider] {
            return impl_->Retain(impl_->cookers.Register(std::move(descriptor), std::move(provider)), impl_->cookerRegistrations);
        });
    }

    /** @copydoc HeadlessExtensionHost::RegisterValidator */
    Result<void> HeadlessExtensionHost::RegisterValidator(ProjectValidatorProviderDescriptor descriptor,
                                                          std::shared_ptr<const IProjectValidator> provider) {
        const std::string subject = descriptor.validatorId.value;
        return impl_->Configure(subject, [this, &descriptor, &provider] {
            return impl_->Retain(impl_->validators.Register(std::move(descriptor), std::move(provider)), impl_->validatorRegistrations);
        });
    }

    /** @copydoc HeadlessExtensionHost::RegisterPipelineStep */
    Result<void> HeadlessExtensionHost::RegisterPipelineStep(PipelineStepDescriptor descriptor,
                                                             std::shared_ptr<const IPipelineStep> provider) {
        const std::string subject = descriptor.stepId.value;
        return impl_->Configure(subject, [this, &descriptor, &provider] {
            return impl_->Retain(impl_->pipeline.Register(std::move(descriptor), std::move(provider)), impl_->pipelineRegistrations);
        });
    }

    /** @copydoc HeadlessExtensionHost::RegisterToolchain */
    Result<ToolchainInvocationAuthority> HeadlessExtensionHost::RegisterToolchain(ToolchainProviderDescriptor descriptor) {
        const std::string subject = descriptor.contributionId;
        return impl_->Configure(subject, [this, &descriptor] {
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
        std::unique_lock lock{impl_->synchronization.Lifecycle()};
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
        if (HeadlessExtensionHostState expected = HeadlessExtensionHostState::Configuring;
            !impl_->state.compare_exchange_strong(expected, HeadlessExtensionHostState::Ready)) {
            Error failure =
                MakeError(ExtensionErrors::HeadlessHostStateInvalid, "Headless extension host shutdown began before startup completed.");
            impl_->Record(HeadlessExtensionHostStage::Activation, "headless-host", failure);
            return Result<void>::Failure(std::move(failure));
        }
        return Result<void>::Success();
    }

    /** @copydoc HeadlessExtensionHost::Import */
    Result<Assets::PreparedAssetImport> HeadlessExtensionHost::Import(const std::string_view contributionId,
                                                                      const Assets::AssetImportInput &input,
                                                                      const CancellationToken &cancellation) const {
        return impl_->ExecuteReady(HeadlessExtensionHostStage::Import, std::string{contributionId},
                                   [this, contributionId, &input, &cancellation] {
            const std::shared_ptr<const Assets::AssetImporterCatalogSnapshot> &snapshot = impl_->importerSnapshot;
            const Assets::AssetImporterContribution *contribution = snapshot != nullptr ? snapshot->FindById(contributionId) : nullptr;
            if (contribution == nullptr || contribution->strategy == nullptr)
                return Result<Assets::PreparedAssetImport>::Failure(
                    MakeError(ExtensionErrors::HeadlessImporterUnavailable,
                              "No published importer matches contribution: " + std::string{contributionId}));
            return contribution->strategy->Import(input, cancellation);
        });
    }

    /** @copydoc HeadlessExtensionHost::Cook */
    Result<AssetCookerResult> HeadlessExtensionHost::Cook(const AssetCookerRequest &request, const CancellationToken &cancellation) const {
        return impl_->ExecuteReady(HeadlessExtensionHostStage::Cook, request.input.assetType.Value(), [this, &request, &cancellation] {
            return impl_->cookers.Cook(request, cancellation);
        });
    }

    /** @copydoc HeadlessExtensionHost::Validate */
    Result<std::vector<AttributedProjectValidationResult>> HeadlessExtensionHost::Validate(const ProjectValidationSnapshot &snapshot,
                                                                                           const CancellationToken &cancellation) const {
        return impl_->ExecuteReady(HeadlessExtensionHostStage::Validation, std::string{snapshot.projectId},
                                   [this, &snapshot, &cancellation] {
            return impl_->validators.ValidateAll(snapshot, cancellation);
        });
    }

    /** @copydoc HeadlessExtensionHost::ExecutePipeline */
    Result<PipelineRunResult> HeadlessExtensionHost::ExecutePipeline(const std::span<const PipelineArtifactView> initialArtifacts,
                                                                     const CancellationToken &cancellation) const {
        return impl_->ExecuteReady(HeadlessExtensionHostStage::Pipeline, "pipeline", [this, initialArtifacts, &cancellation] {
            return impl_->pipeline.Execute(initialArtifacts, cancellation);
        });
    }

    /** @copydoc HeadlessExtensionHost::InvokeToolchain */
    Result<ToolchainInvocationResult> HeadlessExtensionHost::InvokeToolchain(const ToolchainInvocationAuthority &authority,
                                                                             const ToolchainInvocationIntent &intent,
                                                                             const CancellationToken &cancellation) const {
        return impl_->ExecuteReady(HeadlessExtensionHostStage::Toolchain, authority.contributionId,
                                   [this, &authority, &intent, &cancellation] {
            return impl_->toolchains.Invoke(authority, intent, cancellation);
        });
    }

    /** @copydoc HeadlessExtensionHost::ResolveCapability */
    Result<ApplicationCapabilityProviderLease> HeadlessExtensionHost::ResolveCapability(const ExtensionCapabilityHandle &authority,
                                                                                        const ApplicationCapabilityVersionRange &versions,
                                                                                        const std::string_view extensionId,
                                                                                        const std::string_view moduleId,
                                                                                        const std::uint64_t activationGeneration) const {
        return impl_->ExecuteReady(HeadlessExtensionHostStage::CapabilityResolution, authority.Capability().value,
                                   [this, &authority, &versions, extensionId, moduleId, activationGeneration] {
            return impl_->capabilities.Resolve(authority, versions, extensionId, moduleId, activationGeneration);
        });
    }

    /** @copydoc HeadlessExtensionHost::Inspect */
    HeadlessExtensionHostSnapshot HeadlessExtensionHost::Inspect() const {
        HeadlessExtensionHostSnapshot snapshot;
        std::shared_lock lifecycleLock{impl_->synchronization.Lifecycle()};
        snapshot.state = impl_->state.load();
        snapshot.discoveredPackages = impl_->discoveredPackages;
        snapshot.rootDiagnostics = impl_->rootDiagnostics;
        if (impl_->manager != nullptr)
            snapshot.loadedExtensions = impl_->manager->GetLoadedExtensionIds();
        std::scoped_lock diagnosticLock{impl_->diagnosticMutex};
        snapshot.diagnostics.assign(impl_->diagnostics.begin(), impl_->diagnostics.end());
        return snapshot;
    }

    /** @copydoc HeadlessExtensionHost::Shutdown */
    void HeadlessExtensionHost::Shutdown() noexcept {
        if (impl_ == nullptr)
            return;
        std::scoped_lock shutdownLock{impl_->synchronization.Shutdown()};
        if (impl_->state.load() == HeadlessExtensionHostState::Shutdown)
            return;
        impl_->state.store(HeadlessExtensionHostState::ShuttingDown);

        impl_->toolchains.BeginShutdown();
        impl_->pipeline.BeginShutdown();
        impl_->validators.BeginShutdown();
        impl_->cookers.BeginShutdown();
        impl_->capabilities.BeginShutdown();

        std::unique_lock lock{impl_->synchronization.Lifecycle()};
        impl_->toolchainRegistrations.clear();
        impl_->pipelineRegistrations.clear();
        impl_->validatorRegistrations.clear();
        impl_->cookerRegistrations.clear();
        impl_->capabilityRegistrations.clear();
        impl_->importerSnapshot.reset();
        if (impl_->importers != nullptr)
            impl_->importers->Reset();
        if (impl_->manager != nullptr)
            impl_->manager->UnloadAll();
        impl_->manager.reset();
        impl_->importers.reset();
        impl_->state.store(HeadlessExtensionHostState::Shutdown);
    }
}  // namespace Horo::Extensions
