#include "Horo/Packages/PackageLifecycle.h"

#include "Horo/Packages/PackageLifecycleErrors.h"
#include "PackageActivationComposition.h"
#include "PackageModuleComposition.h"

#include <algorithm>
#include <limits>
#include <thread>
#include <utility>

namespace Horo::Packages {
    namespace {
        /** @brief Validates the same absolute, canonical existing directory before lifecycle configuration is admitted. */
        bool IsPrivateTemporaryRoot(const std::filesystem::path &root) {
            std::error_code error;
            const auto canonical = std::filesystem::canonical(root, error);
            if (error || !root.is_absolute() || canonical != root.lexically_normal())
                return false;
            const bool directory = std::filesystem::is_directory(canonical, error);
            return directory && !error;
        }
    }  // namespace

    struct PackageLifecycleService::Impl final {
        Impl(PackageInstallService &installation, const IPackageExtensionTrust &approval, PackageLifecycleConfiguration policy)
            : install(installation), trust(approval), configuration(std::move(policy)) {}

        /** @brief Keeps the primary typed error and the previous published graph when preparation fails. */
        Result<void> Fail(Error error) {
            journal.outcome = error.code.Value() == PackageLifecycleErrors::Cancelled.code.Value() ? PackageActivationOutcome::Cancelled
                                                                                                   : PackageActivationOutcome::Failed;
            journal.diagnostic = error;
            return Result<void>::Failure(std::move(error));
        }

        /** @brief Retains failed detached native owners if actual callback leases still require draining. */
        Result<void> Reject(std::unique_ptr<Detail::PackageComposition> candidate, Error error) {
            if (!candidate)
                return Fail(std::move(error));
            candidate->Retire();
            journal.modules = candidate->Reports();
            if (candidate->HasRetirementReservation()) {
                configuration.moduleRetirements->Retain(std::move(candidate));
                journal.restartRequired = true;
                return Fail(std::move(error));
            }
            candidate->Retire();
            if (!candidate->Finalize()) {
                retired.push_back(std::move(candidate));
                journal.restartRequired = true;
            }
            return Fail(std::move(error));
        }

        /** @brief Completes fallible publication bookkeeping before moving the current executable composition. */
        void Publish(std::unique_ptr<Detail::PackageComposition> &staged) {
            const auto snapshot = staged->Snapshot();
            retired.reserve(retired.size() + 1);
            journal.modules = snapshot->modules;
            auto previous = std::move(current);
            current = std::move(staged);
            active = snapshot;
            journal.outcome = PackageActivationOutcome::Succeeded;
            if (previous) {
                previous->Retire();
                if (previous->HasRetirementReservation())
                    configuration.moduleRetirements->Retain(std::move(previous));
                else
                    retired.push_back(std::move(previous));
            }
        }

        /** @brief Reads live shutdown state after host callbacks can reenter the lifecycle service. */
        bool StopRequested() const noexcept {
            return shutdownRequested;
        }

        /** @brief Stages validated native owners and checks cancellation, install identity and leases before publication. */
        Result<void> PrepareStaged(std::shared_ptr<const VerifiedPackageInstallRecord> installed,
                                   std::span<const EnabledPackageExtension> enabled, std::uint64_t nextGeneration,
                                   const CancellationToken &cancellation, std::unique_ptr<Detail::PackageComposition> &staged) {
            if (cancellation.IsCancellationRequested() || StopRequested())
                return Result<void>::Failure(MakeError(PackageLifecycleErrors::Cancelled));
            auto candidates = Detail::PreparePackageCandidates(installed, enabled, trust, configuration);
            if (candidates.HasError())
                return Result<void>::Failure(candidates.ErrorValue());
            if (cancellation.IsCancellationRequested() || StopRequested())
                return Result<void>::Failure(MakeError(PackageLifecycleErrors::Cancelled));
            if (install.InstalledRecord() != installed)
                return Result<void>::Failure(MakeError(PackageLifecycleErrors::StaleInstall));
            retired.reserve(retired.size() + 1U);
            auto native = std::make_unique<Detail::PackageActivationComposition>(configuration, installed, nextGeneration);
            auto *preparingNative = native.get();
            staged = std::move(native);
            if (auto prepared = preparingNative->Prepare(candidates.Value(), configuration, cancellation); prepared.HasError())
                return Result<void>::Failure(prepared.ErrorValue());
            if (cancellation.IsCancellationRequested() || StopRequested())
                return Result<void>::Failure(MakeError(PackageLifecycleErrors::Cancelled));
            if (install.InstalledRecord() != installed)
                return Result<void>::Failure(MakeError(PackageLifecycleErrors::StaleInstall));
            if (const auto snapshot = staged->Snapshot(); !std::ranges::all_of(snapshot->activations, [nextGeneration](const auto &lease) {
                return lease.IsUsable() && lease.Activation().Generation() == nextGeneration;
            }))
                return Result<void>::Failure(MakeError(PackageLifecycleErrors::InvalidCandidate));
            return Result<void>::Success();
        }

        /** @brief Restores preparation state and services deferred reentrant shutdown only after staged rollback completes. */
        struct PreparationGuard final {
            PackageLifecycleService &service;
            Impl &state;

            PreparationGuard(PackageLifecycleService &lifecycle, Impl &lifecycleState) noexcept
                : service(lifecycle), state(lifecycleState) {}

            PreparationGuard(const PreparationGuard &) = delete;
            PreparationGuard &operator=(const PreparationGuard &) = delete;
            PreparationGuard(PreparationGuard &&) = delete;
            PreparationGuard &operator=(PreparationGuard &&) = delete;

            ~PreparationGuard() {
                state.preparing = false;
                if (state.shutdownRequested)
                    service.Shutdown();
            }
        };

        PackageInstallService &install;
        const IPackageExtensionTrust &trust;
        PackageLifecycleConfiguration configuration;
        std::thread::id owner{std::this_thread::get_id()};
        PackageLifecycleState journal;
        std::uint64_t generation{};
        bool preparing{};
        bool closed{};
        bool shutdownRequested{};
        std::unique_ptr<Detail::PackageComposition> current;
        std::shared_ptr<const PackageActivationSnapshot> active;
        std::vector<std::unique_ptr<Detail::PackageComposition>> retired;
    };

    /** @copydoc PackageLifecycleService::Create */
    Result<std::unique_ptr<PackageLifecycleService>> PackageLifecycleService::Create(PackageInstallService &install,
                                                                                     const IPackageExtensionTrust &trust,
                                                                                     PackageLifecycleConfiguration configuration) {
        if (!IsPrivateTemporaryRoot(configuration.temporaryRoot) || !configuration.artifactGate || configuration.maximumExtensions == 0U ||
            configuration.maximumExtensions > 64U || configuration.maximumRetiredCompositions == 0U ||
            configuration.maximumRetiredCompositions > 64U || configuration.capabilities.size() > 256U ||
            (configuration.moduleHosts.size() > 5) || (!configuration.moduleHosts.empty() && !configuration.moduleRetirements))
            return Result<std::unique_ptr<PackageLifecycleService>>::Failure(MakeError(PackageLifecycleErrors::InvalidLifecycle));
        if (configuration.profile != Extensions::ExtensionHostProfile::Headless &&
            configuration.profile != Extensions::ExtensionHostProfile::Interactive)
            return Result<std::unique_ptr<PackageLifecycleService>>::Failure(MakeError(PackageLifecycleErrors::InvalidLifecycle));
        for (std::size_t index = 0; index < configuration.moduleHosts.size(); ++index) {
            const auto &host = configuration.moduleHosts[index];
            if (!host || host->Identity().empty() || host->Identity().size() > 256)
                return Result<std::unique_ptr<PackageLifecycleService>>::Failure(MakeError(PackageLifecycleErrors::InvalidLifecycle));
            for (std::size_t other = 0; other < index; ++other)
                for (const auto role :
                     {Extensions::ExtensionModuleRole::BackendCapability, Extensions::ExtensionModuleRole::EditorPresentation,
                      Extensions::ExtensionModuleRole::HeadlessTooling, Extensions::ExtensionModuleRole::ScriptProvider,
                      Extensions::ExtensionModuleRole::RuntimeParticipant})
                    if (host->Identity() == configuration.moduleHosts[other]->Identity() ||
                        (host->Supports(role) && configuration.moduleHosts[other]->Supports(role)))
                        return Result<std::unique_ptr<PackageLifecycleService>>::Failure(
                            MakeError(PackageLifecycleErrors::InvalidLifecycle));
        }
        // Seal and validate the capability catalog rather than silently omitting invalid host policy.
        std::ranges::sort(configuration.capabilities);
        if (std::ranges::adjacent_find(configuration.capabilities) != configuration.capabilities.end())
            return Result<std::unique_ptr<PackageLifecycleService>>::Failure(MakeError(PackageLifecycleErrors::InvalidLifecycle));
        for (const auto &capability : configuration.capabilities)
            if (capability.empty() || capability.size() > 256U)
                return Result<std::unique_ptr<PackageLifecycleService>>::Failure(MakeError(PackageLifecycleErrors::InvalidLifecycle));
        // The validated factory retains the private constructor; make_unique cannot invoke it.
        auto service = std::unique_ptr<PackageLifecycleService>(  // NOSONAR(cpp:S5950) Sealed private constructor.
            new PackageLifecycleService{std::make_unique<Impl>(install, trust, std::move(configuration))});
        return Result<std::unique_ptr<PackageLifecycleService>>::Success(std::move(service));
    }

    /** @copydoc PackageLifecycleService::PackageLifecycleService */
    PackageLifecycleService::PackageLifecycleService(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

    /** @copydoc PackageLifecycleService::~PackageLifecycleService */
    PackageLifecycleService::~PackageLifecycleService() noexcept {
        Shutdown();
    }

    /** @copydoc PackageLifecycleService::Activate */
    Result<void> PackageLifecycleService::Activate(const std::span<const EnabledPackageExtension> enabled,
                                                   const PackageActivationBoundary boundary, const CancellationToken &cancellation) {
        if (std::this_thread::get_id() != impl_->owner || impl_->closed || impl_->shutdownRequested || impl_->preparing ||
            (boundary != PackageActivationBoundary::Startup && boundary != PackageActivationBoundary::Quiescent))
            return Result<void>::Failure(MakeError(PackageLifecycleErrors::InvalidLifecycle));
        FinalizeRetirements();
        if (enabled.size() > impl_->configuration.maximumExtensions ||
            impl_->retired.size() >= impl_->configuration.maximumRetiredCompositions ||
            impl_->generation == std::numeric_limits<std::uint64_t>::max())
            return impl_->Fail(MakeError(PackageLifecycleErrors::InvalidLifecycle));
        const auto install = impl_->install.InstalledRecord();
        if (!install)
            return impl_->Fail(MakeError(PackageLifecycleErrors::InvalidCandidate));
        const auto generation = ++impl_->generation;
        impl_->journal = {PackageActivationOutcome::Preparing, generation, install->Revision(), {}, !impl_->retired.empty()};
        impl_->preparing = true;

        Impl::PreparationGuard guard{*this, *impl_};

        std::unique_ptr<Detail::PackageComposition> staged;
        try {
            if (auto prepared = impl_->PrepareStaged(install, enabled, generation, cancellation, staged); prepared.HasError())
                return impl_->Reject(std::move(staged), prepared.ErrorValue());
            const auto snapshot = staged->Snapshot();
            // All fallible work is complete. Publish the exact graph and registrations together before retiring prior owners.
            impl_->journal.modules = snapshot->modules;
            auto previous = std::move(impl_->current);
            impl_->current = std::move(staged);
            impl_->active = snapshot;
            impl_->journal.outcome = PackageActivationOutcome::Succeeded;
            if (previous) {
                previous->Retire();
                if (previous->HasRetirementReservation())
                    impl_->configuration.moduleRetirements->Retain(std::move(previous));
                else
                    impl_->retired.push_back(std::move(previous));
            }
            return Result<void>::Success();
        } catch (const std::exception &exception) {  // NOSONAR(cpp:S1181) Trusted host callbacks form an exception containment boundary.
            auto error = MakeError(PackageLifecycleErrors::InvalidCandidate, exception.what());
            return impl_->Reject(std::move(staged), std::move(error));
        } catch (...) {  // NOSONAR(cpp:S2738) Trusted host callbacks may throw arbitrary types; staged rollback must still run.
            auto error = MakeError(PackageLifecycleErrors::InvalidCandidate);
            return impl_->Reject(std::move(staged), std::move(error));
        }
    }

    /** @copydoc PackageLifecycleService::ActivateModules */
    Result<void> PackageLifecycleService::ActivateModules(const std::span<const EnabledPackageModule> enabled,
                                                          const PackageActivationBoundary boundary, const CancellationToken &cancellation) {
        if (std::this_thread::get_id() != impl_->owner || impl_->closed || impl_->shutdownRequested || impl_->preparing ||
            !impl_->configuration.moduleRetirements ||
            (boundary != PackageActivationBoundary::Startup && boundary != PackageActivationBoundary::Quiescent))
            return Result<void>::Failure(MakeError(PackageLifecycleErrors::InvalidLifecycle));
        FinalizeRetirements();
        if (enabled.size() > impl_->configuration.maximumExtensions || impl_->generation == std::numeric_limits<std::uint64_t>::max())
            return impl_->Fail(MakeError(PackageLifecycleErrors::InvalidLifecycle));
        const auto install = impl_->install.InstalledRecord();
        if (!install)
            return impl_->Fail(MakeError(PackageLifecycleErrors::InvalidCandidate));
        const auto generation = ++impl_->generation;
        impl_->journal = {PackageActivationOutcome::Preparing, generation, install->Revision(), {}, false};
        // Empty enablement cannot invoke a host or publish a receipt; its metadata needs no executable retirement slot.
        const bool requiresRetirement = !enabled.empty();
        auto mixed = std::make_unique<Detail::PackageModuleComposition>(install, generation, requiresRetirement);
        if (requiresRetirement && !impl_->configuration.moduleRetirements->Reserve())
            return impl_->Fail(
                MakeError(PackageLifecycleErrors::InvalidLifecycle, "The explicit package retirement owner is full or unavailable."));
        std::unique_ptr<Detail::PackageComposition> staged = std::move(mixed);
        impl_->preparing = true;
        Impl::PreparationGuard guard{*this, *impl_};
        try {
            auto *modules = static_cast<Detail::PackageModuleComposition *>(staged.get());
            auto prepared = modules->Prepare(enabled, impl_->configuration, impl_->trust, cancellation, impl_->shutdownRequested);
            if (prepared.HasError())
                return impl_->Reject(std::move(staged), prepared.ErrorValue());
            if (cancellation.IsCancellationRequested() || impl_->shutdownRequested)
                return impl_->Reject(std::move(staged), MakeError(PackageLifecycleErrors::Cancelled));
            if (impl_->install.InstalledRecord() != install)
                return impl_->Reject(std::move(staged), MakeError(PackageLifecycleErrors::StaleInstall));
            const auto snapshot = staged->Snapshot();
            if (!std::ranges::all_of(snapshot->activations, [generation](const auto &lease) {
                return lease.IsUsable() && lease.Activation().Generation() == generation;
            }))
                return impl_->Reject(std::move(staged), MakeError(PackageLifecycleErrors::InvalidCandidate));
            impl_->Publish(staged);
            return Result<void>::Success();
        } catch (const std::exception &error) {
            return impl_->Reject(std::move(staged), MakeError(PackageLifecycleErrors::InvalidCandidate, error.what()));
        } catch (...) {
            return impl_->Reject(std::move(staged), MakeError(PackageLifecycleErrors::InvalidCandidate));
        }
    }

    /** @copydoc PackageLifecycleService::Active */
    std::shared_ptr<const PackageActivationSnapshot> PackageLifecycleService::Active() const {
        return impl_->active;
    }

    /** @copydoc PackageLifecycleService::State */
    PackageLifecycleState PackageLifecycleService::State() const {
        return impl_->journal;
    }

    /** @copydoc PackageLifecycleService::FinalizeRetirements */
    void PackageLifecycleService::FinalizeRetirements() {
        if (std::this_thread::get_id() != impl_->owner || impl_->preparing)
            return;
        if (impl_->configuration.moduleRetirements)
            (void)impl_->configuration.moduleRetirements->Drain();
        std::erase_if(impl_->retired, [](const auto &retired) {
            return retired->Finalize();
        });
        if (impl_->closed && impl_->current && impl_->current->Finalize())
            impl_->current.reset();
        impl_->journal.restartRequired = !impl_->retired.empty() || (impl_->closed && static_cast<bool>(impl_->current)) ||
                                         (impl_->configuration.moduleRetirements && impl_->configuration.moduleRetirements->Pending() != 0);
    }

    /** @copydoc PackageLifecycleService::Shutdown */
    void PackageLifecycleService::Shutdown() noexcept {
        impl_->shutdownRequested = true;
        if (impl_->closed || impl_->preparing)
            return;
        impl_->closed = true;
        impl_->active.reset();
        if (impl_->current) {
            impl_->current->Retire();
            if (impl_->current->HasRetirementReservation())
                impl_->configuration.moduleRetirements->Retain(std::move(impl_->current));
        }
        for (auto previous = impl_->retired.rbegin(); previous != impl_->retired.rend(); ++previous)
            (*previous)->Retire();
        impl_->journal.outcome = PackageActivationOutcome::Closed;
        // Native finalization is never forced by callback/worker last-owner release.
        try {
            FinalizeRetirements();
            impl_->journal.restartRequired =
                !impl_->retired.empty() || static_cast<bool>(impl_->current) ||
                (impl_->configuration.moduleRetirements && impl_->configuration.moduleRetirements->Pending() != 0);
        } catch (...) {  // NOSONAR(cpp:S2738) Trusted host callbacks may throw arbitrary types; staged rollback must still run.
            impl_->journal.restartRequired = true;
        }
    }
}  // namespace Horo::Packages
