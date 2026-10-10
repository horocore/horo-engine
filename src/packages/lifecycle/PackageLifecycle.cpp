#include "Horo/Packages/PackageLifecycle.h"

#include "Horo/Packages/PackageLifecycleErrors.h"
#include "PackageActivationComposition.h"

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
        Result<void> Reject(std::unique_ptr<Detail::PackageActivationComposition> candidate, Error error) {
            if (!candidate)
                return Fail(std::move(error));
            candidate->Retire();
            if (!candidate->Finalize()) {
                retired.push_back(std::move(candidate));
                journal.restartRequired = true;
            }
            return Fail(std::move(error));
        }

        /** @brief Reads live shutdown state after host callbacks can reenter the lifecycle service. */
        bool StopRequested() const noexcept {
            return shutdownRequested;
        }

        /** @brief Stages validated native owners and checks cancellation, install identity and leases before publication. */
        Result<void> PrepareStaged(std::shared_ptr<const VerifiedPackageInstallRecord> installed,
                                   std::span<const EnabledPackageExtension> enabled, std::uint64_t nextGeneration,
                                   const CancellationToken &cancellation, std::unique_ptr<Detail::PackageActivationComposition> &staged) {
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
            staged = std::make_unique<Detail::PackageActivationComposition>(configuration, installed, nextGeneration);
            if (auto prepared = staged->Prepare(candidates.Value(), configuration, cancellation); prepared.HasError())
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
        std::unique_ptr<Detail::PackageActivationComposition> current;
        std::shared_ptr<const PackageActivationSnapshot> active;
        std::vector<std::unique_ptr<Detail::PackageActivationComposition>> retired;
    };

    /** @copydoc PackageLifecycleService::Create */
    Result<std::unique_ptr<PackageLifecycleService>> PackageLifecycleService::Create(PackageInstallService &install,
                                                                                     const IPackageExtensionTrust &trust,
                                                                                     PackageLifecycleConfiguration configuration) {
        if (!IsPrivateTemporaryRoot(configuration.temporaryRoot) || !configuration.artifactGate || configuration.maximumExtensions == 0U ||
            configuration.maximumExtensions > 64U || configuration.maximumRetiredCompositions == 0U ||
            configuration.maximumRetiredCompositions > 64U || configuration.capabilities.size() > 256U)
            return Result<std::unique_ptr<PackageLifecycleService>>::Failure(MakeError(PackageLifecycleErrors::InvalidLifecycle));
        if (configuration.profile != Extensions::ExtensionHostProfile::Headless &&
            configuration.profile != Extensions::ExtensionHostProfile::Interactive)
            return Result<std::unique_ptr<PackageLifecycleService>>::Failure(MakeError(PackageLifecycleErrors::InvalidLifecycle));
        // Seal and validate the capability catalog rather than silently omitting invalid host policy.
        std::ranges::sort(configuration.capabilities);
        if (std::ranges::adjacent_find(configuration.capabilities) != configuration.capabilities.end())
            return Result<std::unique_ptr<PackageLifecycleService>>::Failure(MakeError(PackageLifecycleErrors::InvalidLifecycle));
        for (const auto &capability : configuration.capabilities)
            if (capability.empty() || capability.size() > 256U)
                return Result<std::unique_ptr<PackageLifecycleService>>::Failure(MakeError(PackageLifecycleErrors::InvalidLifecycle));
        // The validated factory retains the private constructor; make_unique cannot invoke it.
        return Result<std::unique_ptr<PackageLifecycleService>>::Success(std::unique_ptr<PackageLifecycleService>{
            new PackageLifecycleService{// NOSONAR(cpp:S5950) Private validated constructor.
                                        std::make_unique<Impl>(install, trust, std::move(configuration))}});
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

        std::unique_ptr<Detail::PackageActivationComposition> staged;
        try {
            if (auto prepared = impl_->PrepareStaged(install, enabled, generation, cancellation, staged); prepared.HasError())
                return impl_->Reject(std::move(staged), prepared.ErrorValue());
            const auto snapshot = staged->Snapshot();
            // All fallible work is complete. Publish the exact graph and registrations together before retiring prior owners.
            auto previous = std::move(impl_->current);
            impl_->current = std::move(staged);
            impl_->active = snapshot;
            impl_->journal.outcome = PackageActivationOutcome::Succeeded;
            if (previous) {
                previous->Retire();
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
        std::erase_if(impl_->retired, [](const auto &retired) {
            return retired->Finalize();
        });
        if (impl_->closed && impl_->current && impl_->current->Finalize())
            impl_->current.reset();
        impl_->journal.restartRequired = !impl_->retired.empty() || (impl_->closed && static_cast<bool>(impl_->current));
    }

    /** @copydoc PackageLifecycleService::Shutdown */
    void PackageLifecycleService::Shutdown() noexcept {
        impl_->shutdownRequested = true;
        if (impl_->closed || impl_->preparing)
            return;
        impl_->closed = true;
        impl_->active.reset();
        if (impl_->current)
            impl_->current->Retire();
        for (auto previous = impl_->retired.rbegin(); previous != impl_->retired.rend(); ++previous)
            (*previous)->Retire();
        impl_->journal.outcome = PackageActivationOutcome::Closed;
        // Native finalization is never forced by callback/worker last-owner release.
        try {
            FinalizeRetirements();
            impl_->journal.restartRequired = !impl_->retired.empty() || static_cast<bool>(impl_->current);
        } catch (...) {  // NOSONAR(cpp:S2738) Trusted host callbacks may throw arbitrary types; staged rollback must still run.
            impl_->journal.restartRequired = true;
        }
    }
}  // namespace Horo::Packages
