#include "PackageProjectSession.h"

#include "Horo/Packages/GameplayPackageModuleHost.h"
#include "Horo/Packages/NativePackageModuleHost.h"
#include "PackageBehaviorReferences.h"

#include <algorithm>
#include <new>
#include <tuple>

namespace Horo::Application::Internal {
    /** @copydoc FindPackageImporterOwner */
    const Packages::NativePackageModuleHost *FindPackageImporterOwner(
        const Packages::PackageLifecycleConfiguration &configuration) noexcept {
        for (const auto &host : configuration.moduleHosts) {
            const auto *native = dynamic_cast<const Packages::NativePackageModuleHost *>(host.get());
            if (native && native->Supports(Extensions::ExtensionModuleRole::BackendCapability))
                return native;
        }
        return nullptr;
    }

    namespace {
        using Registration = Packages::GameplayPackageModuleHost::RegistrationInventory;
        using RemovalEvidence = Result<std::shared_ptr<const PackageRemovalRegistrationEvidence>>;
        constexpr std::size_t MaximumObservedRoles = 4'096;

        /** @brief Explicit owner lifetime failures, distinct from portable package diagnostics. */
        Error SessionError(const char *code) {
            return Error{ErrorCode{code}, ErrorDomainId{"packages.host"}};
        }

        /** @brief Opens bounded root-owned inputs on the I/O lane and publishes through the journaled authority. */
        Result<void> RemoveSavedPackage(const PackageProjectPolicy &policy, const PreparedPackageProject &expected,
                                        const Packages::HoroPackageId &package, DurableFileSystem &files,
                                        PackageRemovalReadAuthorities reads,
                                        std::shared_ptr<const Packages::PackageRemovalLease> reservation,
                                        const RemovalEvidence &evidence,
                                        const CancellationToken &token) {
            const Platform::ProjectReadContext context{.projectIdentity = "editor-package-removal",
                                                      .projectGeneration = 1,
                                                      .cancellation = token,
                                                      .deadline = std::chrono::steady_clock::now() + std::chrono::seconds{30}};
            const auto read = reads.openFiles(policy.projectRoot, context.projectIdentity, context.projectGeneration);
            if (read.HasError())
                return Result<void>::Failure(read.ErrorValue());
            const auto assets = reads.openAssets(read.Value(), context, {});
            if (assets.HasError())
                return Result<void>::Failure(assets.ErrorValue());
            const std::span<const Registration> registrations =
                evidence.HasValue() && evidence.Value() ? std::span<const Registration>{evidence.Value()->registrations}
                                                        : std::span<const Registration>{};
            const PackageRemovalSources sources{.assets = *assets.Value(),
                                                .files = *read.Value(),
                                                .context = context,
                                                .registrations = registrations,
                                                .live = std::move(reservation),
                                                .requireGameplayOwner = true,
                                                .registrationFailure = evidence.HasError() ? &evidence.ErrorValue() : nullptr};
            const auto published = PublishPackageProjectRemoval(policy, expected, package, files, sources);
            if (published.HasError())
                return Result<void>::Failure(published.ErrorValue());
            return Result<void>::Success();
        }

        /** @brief Captures only the approved project and metadata digests in the worker's immutable policy. */
        auto MakeRemovalPreparation(const PackageProjectPolicy &policy, const PreparedPackageProject &prepared,
                                    const PackageProjectHostServices &services,
                                    std::function<RemovalEvidence()> snapshot) {
            PackageProjectPolicy removalPolicy{.projectRoot = policy.projectRoot, .limits = policy.limits};
            PreparedPackageProject removalBase{.application = {.restore = {.projectRoot = policy.projectRoot}},
                                               .lockDigest = prepared.lockDigest,
                                               .requestDocumentDigest = prepared.requestDocumentDigest};
            return [removalPolicy = std::move(removalPolicy), removalBase = std::move(removalBase),
                    &files = services.files, reads = services.removal, snapshot = std::move(snapshot)](
                       const Packages::HoroPackageId &package, std::shared_ptr<const Packages::PackageRemovalLease> reservation) {
                return Result<PackageRemovalWork>::Success([removalPolicy, removalBase, package, &files, reads,
                                                           reservation = std::move(reservation), evidence = snapshot()](
                                                               const CancellationToken &cancellation) {
                    return RemoveSavedPackage(removalPolicy, removalBase, package, files, reads, reservation, evidence, cancellation);
                });
            };
        }

        /** @brief Preserves a role's historical typed identities across reactivation of the same artifact. */
        void MergeObservedIds(auto &current, const auto &previous) {
            current.insert(current.end(), previous.begin(), previous.end());
            std::ranges::sort(current);
            const auto duplicates = std::ranges::unique(current);
            current.erase(duplicates.begin(), duplicates.end());
        }

        /** @brief Keeps all previously observed descriptor identities and their service dependencies. */
        template <typename Descriptor, typename Ids>
        void MergeObservedServiceDependencies(std::vector<Descriptor> &current, const std::vector<Descriptor> &previous, Ids dependencies) {
            std::ranges::sort(current, {}, &Descriptor::id);
            for (const auto &old : previous) {
                const auto match = std::ranges::lower_bound(current, old.id, {}, &Descriptor::id);
                if (match == current.end() || match->id != old.id)
                    current.insert(match, old);
                else
                    MergeObservedIds((*match).*dependencies, old.*dependencies);
            }
        }

        /** @brief Caps the aggregate service and system history before publishing the observation. */
        bool FitsServiceEvidence(const Registration &observed) {
            constexpr auto fits = [](const auto &dependencies) { return dependencies.size() <= Gameplay::MaximumGameplayDependencies; };
            return observed.services.size() <= Gameplay::MaximumGameplayServices &&
                   observed.systems.size() <= Gameplay::MaximumGameplaySystems &&
                   std::ranges::all_of(observed.services, fits, &Gameplay::GameplayServiceDescriptor::dependencies) &&
                   std::ranges::all_of(observed.systems, fits, &Gameplay::GameplaySystemDescriptor::requiredServices);
        }
    }  // namespace

    /** @copydoc PackageRemovalRegistrationEvidence::Observe */
    Result<void> PackageRemovalRegistrationEvidence::Observe(Registration observed) {
        auto match = std::ranges::find_if(registrations, [&observed](const auto &previous) {
            return std::tie(previous.package, previous.version, previous.artifact, previous.moduleId, previous.descriptorPath) ==
                   std::tie(observed.package, observed.version, observed.artifact, observed.moduleId, observed.descriptorPath);
        });
        if (match != registrations.end()) {
            MergeObservedIds(observed.behaviors, match->behaviors);
            MergeObservedIds(observed.decisionNodes, match->decisionNodes);
            MergeObservedIds(observed.components, match->components);
            MergeObservedIds(observed.componentUses, match->componentUses);
            MergeObservedServiceDependencies(observed.services, match->services, &Gameplay::GameplayServiceDescriptor::dependencies);
            MergeObservedServiceDependencies(observed.systems, match->systems, &Gameplay::GameplaySystemDescriptor::requiredServices);
            observed.hasCustomSenses = observed.hasCustomSenses || match->hasCustomSenses;
            observed.hasAdditionalReferenceClasses = observed.hasAdditionalReferenceClasses || match->hasAdditionalReferenceClasses;
        }
        if (observed.behaviors.size() > Gameplay::MaximumGeneratedBehaviorDescriptors ||
            observed.components.size() > Registration::MaximumComponentIds ||
            observed.componentUses.size() > Registration::MaximumComponentUses ||
            observed.decisionNodes.size() > AI::DecisionNodeGeneration::MaximumNodes || !FitsServiceEvidence(observed))
            return Result<void>::Failure(SessionError("packages.host.registration_evidence_limit"));
        if (match != registrations.end())
            *match = std::move(observed);
        else {
            if (registrations.size() == MaximumObservedRoles)
                return Result<void>::Failure(SessionError("packages.host.registration_evidence_limit"));
            registrations.push_back(std::move(observed));
        }
        return Result<void>::Success();
    }

    struct PackageProjectSession::Impl final {
        std::thread::id owner{std::this_thread::get_id()};
        bool closed{};
        std::optional<Packages::PackageCacheStore> cache;
        std::optional<Packages::PackageRestoreService> restore;
        std::optional<Packages::PackageInstallService> install;
        std::unique_ptr<Packages::PackageLifecycleService> lifecycle;
        std::unique_ptr<PackageApplication> application;
        Packages::GameplayPackageModuleHost *gameplayModules{};
        const Packages::NativePackageModuleHost *importerModules{}; /**< Borrowed from the lifecycle configuration. */
        std::shared_ptr<const PackageRemovalRegistrationEvidence> removalEvidence;
        std::optional<Error> registrationFailure;
        std::uint64_t observedActivation{};

        /** @brief Retains only owned descriptors from the exact published activation; a failed capture blocks evidence admission. */
        void ObserveRegistrations() {
            if (!gameplayModules || registrationFailure)
                return;
            const auto active = lifecycle->Active();
            if (!active || active->generation == observedActivation)
                return;
            try {
                auto observed = gameplayModules->CaptureActiveRegistrations(*lifecycle);
                if (observed.HasError()) {
                    registrationFailure = std::move(observed).ErrorValue();
                    return;
                }
                auto next = removalEvidence ? std::make_shared<PackageRemovalRegistrationEvidence>(*removalEvidence)
                                            : std::make_shared<PackageRemovalRegistrationEvidence>();
                for (auto &entry : std::move(observed).Value()) {
                    if (auto merged = next->Observe(std::move(entry)); merged.HasError()) {
                        registrationFailure = std::move(merged).ErrorValue();
                        return;
                    }
                }
                removalEvidence = std::move(next);
                observedActivation = active->generation;
            } catch (const std::bad_alloc &) {
                registrationFailure = SessionError("packages.host.registration_evidence_storage");
            }
        }
    };

    /** @copydoc PackageProjectSession::Create */
    Result<std::shared_ptr<PackageProjectSession>> PackageProjectSession::Create(
        PackageProjectHostServices services, PackageProjectPolicy policy, PreparedPackageProject prepared,
        Packages::IPackageRestoreSource *source, Packages::PackagePublisherVerificationService *publishers) {
        if (const auto &request = prepared.application.restore; request.projectRoot != policy.projectRoot ||
                                                                request.platform != policy.platform || request.mode != policy.mode ||
                                                                request.requirePublisherVerification != policy.requirePublisherVerification)
            return Result<std::shared_ptr<PackageProjectSession>>::Failure(SessionError("packages.host.policy_mismatch"));
        if (policy.gameplayModules && !std::ranges::any_of(policy.lifecycle.moduleHosts, [&policy](const auto &host) {
            return host.get() == policy.gameplayModules;
        }))
            return Result<std::shared_ptr<PackageProjectSession>>::Failure(SessionError("packages.host.policy_mismatch"));
        auto impl = std::make_unique<Impl>();
        impl->gameplayModules = policy.gameplayModules;
        impl->importerModules = FindPackageImporterOwner(policy.lifecycle);
        auto cache = Packages::PackageCacheStore::Create(services.files, policy.cacheRoot, policy.limits.validation);
        if (cache.HasError())
            return Result<std::shared_ptr<PackageProjectSession>>::Failure(cache.ErrorValue());
        impl->cache.emplace(std::move(cache).Value());
        auto restore =
            Packages::PackageRestoreService::Create(services.jobs, services.files, *impl->cache, source, publishers, policy.limits);
        if (restore.HasError())
            return Result<std::shared_ptr<PackageProjectSession>>::Failure(restore.ErrorValue());
        impl->restore.emplace(std::move(restore).Value());
        auto install = Packages::PackageInstallService::Create(services.files, policy.projectRoot);
        if (install.HasError())
            return Result<std::shared_ptr<PackageProjectSession>>::Failure(install.ErrorValue());
        impl->install.emplace(std::move(install).Value());
        auto lifecycle = Packages::PackageLifecycleService::Create(*impl->install, services.trust, std::move(policy.lifecycle));
        if (lifecycle.HasError())
            return Result<std::shared_ptr<PackageProjectSession>>::Failure(lifecycle.ErrorValue());
        impl->lifecycle = std::move(lifecycle).Value();
        prepared.application.wallClock = &services.wallClock;
        if (services.removal.openFiles && services.removal.openAssets) {
            prepared.application.prepareRemoval = MakeRemovalPreparation(policy, prepared, services, [session = impl.get()] {
                if (session->registrationFailure)
                    return RemovalEvidence::Failure(*session->registrationFailure);
                return RemovalEvidence::Success(session->removalEvidence);
            });
        }
        auto application =
            PackageApplication::Create(services.jobs, *impl->restore, *impl->install, *impl->lifecycle, std::move(prepared.application));
        if (application.HasError())
            return Result<std::shared_ptr<PackageProjectSession>>::Failure(application.ErrorValue());
        impl->application = std::move(application).Value();
        return Result<std::shared_ptr<PackageProjectSession>>::Success(
            std::make_shared<PackageProjectSession>(ConstructionKey{}, std::move(impl)));
    }

    /** @copydoc PackageProjectSession::PackageProjectSession */
    PackageProjectSession::PackageProjectSession(const ConstructionKey &, std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

    /** @copydoc PackageProjectSession::~PackageProjectSession */
    PackageProjectSession::~PackageProjectSession() noexcept {
        try {
            // Reuse the existing final-owner affinity/lifetime violation policy; normal roots cancel, pump and Close explicitly.
            if (Close().HasError())
                std::terminate();
        } catch (...) {
            std::terminate();
        }
    }

    /** @copydoc PackageProjectSession::Application */
    Result<std::shared_ptr<PackageApplication>> PackageProjectSession::Application() {
        if (std::this_thread::get_id() != impl_->owner)
            return Result<std::shared_ptr<PackageApplication>>::Failure(SessionError("packages.host.owner_lane"));
        if (impl_->closed)
            return Result<std::shared_ptr<PackageApplication>>::Failure(SessionError("packages.host.closed"));
        return Result<std::shared_ptr<PackageApplication>>::Success({shared_from_this(), impl_->application.get()});
    }

    /** @copydoc PackageProjectSession::RemovalPublished */
    bool PackageProjectSession::RemovalPublished() const noexcept {
        return impl_->application->RemovalPublished();
    }

    /** @copydoc PackageProjectSession::InstalledRecord */
    Result<std::shared_ptr<const Packages::VerifiedPackageInstallRecord>> PackageProjectSession::InstalledRecord() const {
        using Outcome = Result<std::shared_ptr<const Packages::VerifiedPackageInstallRecord>>;
        if (std::this_thread::get_id() != impl_->owner)
            return Outcome::Failure(SessionError("packages.host.owner_lane"));
        if (impl_->closed)
            return Outcome::Failure(SessionError("packages.host.closed"));
        return Outcome::Success(impl_->install->InstalledRecord());
    }

    /** @copydoc PackageProjectSession::RemovalRegistrations */
    Result<std::shared_ptr<const PackageRemovalRegistrationEvidence>> PackageProjectSession::RemovalRegistrations() const {
        using Evidence = Result<std::shared_ptr<const PackageRemovalRegistrationEvidence>>;
        if (std::this_thread::get_id() != impl_->owner)
            return Evidence::Failure(SessionError("packages.host.owner_lane"));
        if (impl_->closed)
            return Evidence::Failure(SessionError("packages.host.closed"));
        if (impl_->registrationFailure)
            return Evidence::Failure(*impl_->registrationFailure);
        return Evidence::Success(impl_->removalEvidence);
    }

    /** @copydoc PackageProjectSession::DecisionCatalogGeneration */
    std::uint64_t PackageProjectSession::DecisionCatalogGeneration() const noexcept {
        if (std::this_thread::get_id() != impl_->owner || impl_->closed || !impl_->gameplayModules)
            return 0;
        const auto active = impl_->lifecycle->Active();
        if (!active)
            return 0;
        const auto host = impl_->gameplayModules->Identity();
        const bool selected = std::ranges::any_of(active->modules, [host](const auto &module) {
            return module.role == Extensions::ExtensionModuleRole::RuntimeParticipant &&
                   module.outcome == Packages::PackageModuleOutcome::Active && module.host == host;
        });
        return selected ? active->generation : 0;
    }

    /** @copydoc PackageProjectSession::ComposeImporters */
    Result<PackageImporterCatalog> PackageProjectSession::ComposeImporters(PackageImporterCatalog host) const {
        using Outcome = Result<PackageImporterCatalog>;
        if (std::this_thread::get_id() != impl_->owner)
            return Outcome::Failure(SessionError("packages.host.owner_lane"));
        if (impl_->closed)
            return Outcome::Failure(SessionError("packages.host.closed"));
        const auto active = impl_->lifecycle->Active();
        if (!active || !impl_->importerModules)
            return Outcome::Success(std::move(host));

        std::vector<PackageImporterCatalog> importers;
        const auto owner = impl_->importerModules->Identity();
        for (const auto &module : active->modules) {
            if (module.role != Extensions::ExtensionModuleRole::BackendCapability ||
                module.outcome != Packages::PackageModuleOutcome::Active || module.host != owner)
                continue;
            auto catalog = impl_->importerModules->Importers(module.package, module.module, active->generation);
            if (!catalog)
                return Outcome::Failure(SessionError("packages.host.importer_catalog_unavailable"));
            importers.push_back(std::move(catalog));
        }
        return Assets::AssetImporterCatalogSnapshot::Compose(std::move(host), importers);
    }

    /** @copydoc PackageProjectSession::AcquireDecisionNodes */
    Result<PackageDecisionNodeLeaves> PackageProjectSession::AcquireDecisionNodes(std::uint64_t activation) const {
        using Outcome = Result<PackageDecisionNodeLeaves>;
        if (activation == 0 || activation != DecisionCatalogGeneration())
            return Outcome::Failure(SessionError("packages.host.decision_catalog_stale"));
        auto acquired = impl_->gameplayModules->AcquireActiveDecisionNodes(*impl_->lifecycle);
        if (acquired.HasError())
            return Outcome::Failure(std::move(acquired).ErrorValue());
        try {
            PackageDecisionNodeLeaves leaves;
            auto contributions = std::move(acquired).Value();
            leaves.reserve(contributions.size());
            for (auto &entry : contributions)
                leaves.push_back(std::move(entry.nodes));
            return Outcome::Success(std::move(leaves));
        } catch (const std::bad_alloc &) {
            return Outcome::Failure(SessionError("packages.host.decision_catalog_storage"));
        }
    }

    /** @copydoc PackageProjectSession::Pump */
    Result<void> PackageProjectSession::Pump(const Packages::PackageActivationBoundary boundary) {
        if (std::this_thread::get_id() != impl_->owner)
            return Result<void>::Failure(SessionError("packages.host.owner_lane"));
        if (impl_->closed)
            return Result<void>::Failure(SessionError("packages.host.closed"));
        impl_->ObserveRegistrations();
        auto pumped = impl_->application->Pump(boundary);
        impl_->ObserveRegistrations();
        return pumped;
    }

    /** @copydoc PackageProjectSession::Close */
    Result<void> PackageProjectSession::Close() {
        if (std::this_thread::get_id() != impl_->owner)
            return Result<void>::Failure(SessionError("packages.host.owner_lane"));
        if (impl_->closed)
            return Result<void>::Success();
        if (auto closed = impl_->application->Close(); closed.HasError())
            return closed;
        if (auto closed = impl_->restore->Shutdown(); closed.HasError())
            return closed;
        impl_->lifecycle->Shutdown();
        impl_->lifecycle->FinalizeRetirements();
        if (impl_->lifecycle->State().restartRequired)
            return Result<void>::Failure(SessionError("packages.host.retirement_busy"));
        impl_->closed = true;
        return Result<void>::Success();
    }
}  // namespace Horo::Application::Internal
