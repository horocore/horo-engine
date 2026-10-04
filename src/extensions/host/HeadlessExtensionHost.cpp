#include "Horo/Extensions/HeadlessExtensionHost.h"

#include "Horo/Extensions/ExtensionErrors.h"

#include <algorithm>
#include <array>
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
        std::vector<std::shared_ptr<ExtensionRetirement>> retirementOwners;

        struct AdmittedWork final {
            std::size_t id;
            HeadlessExtensionOutstandingWork work;
        };

        mutable std::vector<AdmittedWork> admittedWork;
        mutable std::size_t nextWorkId{1};

        class WorkGuard final {
        public:
            WorkGuard(const Impl &owner, const std::size_t id) noexcept : owner_(owner), id_(id) {}

            ~WorkGuard() {
                std::scoped_lock lock{owner_.diagnosticMutex};
                std::erase_if(owner_.admittedWork, [id = id_](const auto &work) {
                    return work.id == id;
                });
            }

            WorkGuard(const WorkGuard &) = delete;
            WorkGuard &operator=(const WorkGuard &) = delete;

        private:
            const Impl &owner_;
            std::size_t id_;
        };

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
            std::size_t workId{};
            {
                std::scoped_lock workLock{diagnosticMutex};
                if (admittedWork.size() >= 4096U || nextWorkId == 0)
                    return OperationResult::Failure(
                        MakeError(ExtensionErrors::HeadlessHostStateInvalid, "Headless admitted-work retention bound exceeded."));
                workId = nextWorkId++;
                admittedWork.push_back({workId, {stage, subject}});
            }
            const WorkGuard work{*this, workId};
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

        /** @brief Retains discovery evidence independently of whether the later activation succeeds. */
        void RecordDiscovery(const Discovery::DiscoveryPlan &plan) {
            std::scoped_lock lock{diagnosticMutex};
            rootDiagnostics = plan.rootDiagnostics;
            discoveredPackages.reserve(plan.packages.size());
            for (const auto &package : plan.packages)
                discoveredPackages.push_back(package.packageId);
            retirementOwners.reserve(plan.packages.size());
        }

        /** @brief Activates dependency-ordered packages and retains only their successfully established retirement owners. */
        [[nodiscard]] Result<void> ActivatePackages(const std::vector<const Discovery::DiscoveredPackage *> &packages) {
            for (const auto *selected : packages) {
                if (auto configuring = RequireConfiguring(); configuring.HasError())
                    return FailStartup(HeadlessExtensionHostStage::Activation, selected->packageId, configuring.ErrorValue());
                const auto &package = *selected;
                auto activated = manager->LoadExtension(package.canonicalPath.string(), package.providerPackageIds);
                if (activated.HasError()) {
                    Error failure = WrapError(ExtensionErrors::HeadlessHostActivationFailed, activated.ErrorValue(),
                                              "Headless package activation failed: " + package.packageId);
                    return FailStartup(HeadlessExtensionHostStage::Activation, package.packageId, std::move(failure));
                }
                if (activated.Value() != package.packageId)
                    return FailStartup(HeadlessExtensionHostStage::Activation, package.packageId,
                                       MakeError(ExtensionErrors::HeadlessHostActivationFailed,
                                                 "Activated extension identity differs from its declared package identity."));
                std::scoped_lock lock{diagnosticMutex};
                retirementOwners.push_back(manager->Retirement(activated.Value()));
            }
            return Result<void>::Success();
        }
    };

    namespace {
        constexpr std::size_t MaximumRetainedHeadlessDiagnostics = 4096U;

        /** @brief Resolves package-authority provider edges before any native activation can become visible. */
        [[nodiscard]] Result<std::vector<const Discovery::DiscoveredPackage *>> ResolveActivationOrder(
            const Discovery::DiscoveryPlan &plan) {
            std::vector<const Discovery::DiscoveredPackage *> order;
            order.reserve(plan.packages.size());
            while (order.size() < plan.packages.size()) {
                const auto next = std::ranges::find_if(plan.packages, [&order](const auto &package) {
                    return std::ranges::find(order, &package) == order.end() &&
                           std::ranges::all_of(package.providerPackageIds, [&order](const auto &provider) {
                        return std::ranges::any_of(order, [&provider](const auto *activated) {
                            return activated->packageId == provider;
                        });
                    });
                });
                if (next == plan.packages.end())
                    return Result<std::vector<const Discovery::DiscoveredPackage *>>::Failure(
                        MakeError(ExtensionErrors::HeadlessHostActivationFailed,
                                  "Declared extension provider graph is cyclic or refers to an unavailable package."));
                order.push_back(&*next);
            }
            return Result<std::vector<const Discovery::DiscoveredPackage *>>::Success(std::move(order));
        }
    }  // namespace

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
        impl_->RecordDiscovery(plan);
        auto ordered = ResolveActivationOrder(plan);
        if (ordered.HasError())
            return impl_->FailStartup(HeadlessExtensionHostStage::Activation, "package-dependencies", ordered.ErrorValue());
        if (auto activated = impl_->ActivatePackages(ordered.Value()); activated.HasError())
            return activated;

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
        std::scoped_lock diagnosticLock{impl_->diagnosticMutex};
        snapshot.state = impl_->state.load();
        snapshot.discoveredPackages = impl_->discoveredPackages;
        snapshot.rootDiagnostics = impl_->rootDiagnostics;
        for (const auto &owner : impl_->retirementOwners) {
            auto report = owner->Inspect();
            if (report.disposition == ExtensionRetirementDisposition::Active)
                snapshot.loadedExtensions.push_back(report.extensionId);
            snapshot.retirements.push_back(std::move(report));
        }
        std::ranges::sort(snapshot.loadedExtensions);
        for (const auto &work : impl_->admittedWork)
            snapshot.outstandingWork.push_back(work.work);
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

        std::array<std::shared_ptr<ExtensionRetirement>, 1024> nativeOwners;
        std::size_t nativeOwnerCount{};
        {
            std::scoped_lock diagnosticLock{impl_->diagnosticMutex};
            for (const auto &owner : impl_->retirementOwners) {
                if (nativeOwnerCount < nativeOwners.size())
                    nativeOwners[nativeOwnerCount++] = owner;
            }
        }
        while (nativeOwnerCount > 0)
            nativeOwners[--nativeOwnerCount]->CloseAdmission();

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
