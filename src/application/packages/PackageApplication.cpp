#include "Horo/Application/PackageApplication.h"

#include "PackageApplicationInternal.h"

#include <limits>

namespace Horo::Application {
    namespace PackageOperations {
        /** @brief Stable application admission failures; lower-layer errors retain their original typed cause. */
        Error Failure(const std::string_view code) {
            return Error{ErrorCode{std::string{code}}, ErrorDomainId{"packages.application"}};
        }
    }  // namespace PackageOperations

    using PackageOperations::Failure;

    Result<void> PackageApplication::Impl::CheckLane() const {
        if (owner != std::this_thread::get_id())
            return Result<void>::Failure(Failure("packages.application.owner_lane"));
        return Result<void>::Success();
    }

    Result<void> PackageApplication::Impl::CheckAdmission() const {
        if (const auto lane = CheckLane(); lane.HasError())
            return lane;
        if (closing)
            return Result<void>::Failure(Failure("packages.application.closed"));
        if (operation && operation->outcome == PackageOperationOutcome::Running)
            return Result<void>::Failure(Failure("packages.application.busy"));
        if (const auto current = configuration.validateAdmission(); current.HasError())
            return current;
        if (nextId == std::numeric_limits<std::uint64_t>::max())
            return Result<void>::Failure(Failure("packages.application.generation_exhausted"));
        return Result<void>::Success();
    }

    Result<PackageOperationId> PackageApplication::Impl::Admit(const PackageOperationKind kind, const CancellationToken &parent) {
        if (const auto valid = CheckAdmission(); valid.HasError())
            return Result<PackageOperationId>::Failure(valid.ErrorValue());
        cancellation = CancellationSource{parent};
        operation = PackageOperationSnapshot{.id = {nextId++}, .revision = 1, .kind = kind};
        if (const auto installed = install.InstalledRecord())
            operation->installedRevision = installed->Revision();
        if (const auto active = lifecycle.Active())
            operation->activeGeneration = active->generation;
        restoreOperation.reset();
        installJob.reset();
        completion.reset();
        enabled.clear();
        return Result<PackageOperationId>::Success(operation->id);
    }

    void PackageApplication::Impl::Finish(Result<void> result, const bool cancelled) {
        operation->phase = PackageOperationPhase::Completed;
        using enum PackageOperationOutcome;
        if (result.HasValue())
            operation->outcome = Succeeded;
        else if (cancelled)
            operation->outcome = Cancelled;
        else
            operation->outcome = Failed;
        operation->completion = result.HasValue() ? 1.0F : operation->completion;
        if (result.HasError())
            operation->diagnostic = std::move(result).ErrorValue();
        ++operation->revision;
    }

    /** @copydoc PackageApplication::Create */
    Result<std::unique_ptr<PackageApplication>> PackageApplication::Create(JobSystem &jobs, Packages::PackageRestoreService &restore,
                                                                           Packages::PackageInstallService &install,
                                                                           Packages::PackageLifecycleService &lifecycle,
                                                                           PackageApplicationConfiguration configuration) {
        if (!configuration.validateAdmission || configuration.maximumEnabledDescriptors == 0 ||
            configuration.maximumEnabledDescriptors > 4096 || configuration.restore.projectRoot.empty() ||
            install.ProjectRoot() != configuration.restore.projectRoot || !lifecycle.UsesInstallService(install) ||
            (configuration.restore.requirePublisherVerification && !configuration.wallClock))
            return Result<std::unique_ptr<PackageApplication>>::Failure(Failure("packages.application.invalid_input"));
        if (configuration.lockedPlan) {
            const auto valid =
                configuration.lockedPlan->ValidateForRestore(configuration.restore.expectedRequestHash, configuration.restore.platform);
            if (valid.HasError())
                return Result<std::unique_ptr<PackageApplication>>::Failure(valid.ErrorValue());
        }
        auto owner = std::make_unique<PackageApplication>(ConstructionKey{}, std::make_unique<Impl>(jobs, restore, install, lifecycle,
                                                                                                    std::move(configuration)));
        if (owner->impl_->configuration.lockedPlan) {
            auto plan = std::make_shared<PackagePlanSnapshot>();
            plan->revision = 1;
            for (const auto &entry : owner->impl_->configuration.lockedPlan->Packages())
                plan->packages.push_back({.locked = entry});
            owner->impl_->plan = std::move(plan);
        }
        return Result<std::unique_ptr<PackageApplication>>::Success(std::move(owner));
    }

    PackageApplication::PackageApplication(const ConstructionKey &, std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

    PackageApplication::~PackageApplication() noexcept {
        if (Close().HasError())
            std::terminate();
    }

    Result<PackageOperationId> PackageApplication::Impl::BeginRestore(const PackageOperationKind kind, const CancellationToken &parent) {
        if (const auto valid = CheckAdmission(); valid.HasError())
            return Result<PackageOperationId>::Failure(valid.ErrorValue());
        auto request = configuration.restore;
        if (configuration.wallClock) {
            const auto now =
                std::chrono::duration_cast<std::chrono::milliseconds>(configuration.wallClock->UtcNow().time_since_epoch()).count();
            if (now < 0)
                return Result<PackageOperationId>::Failure(Failure("packages.application.clock_unavailable"));
            request.nowUnixMilliseconds = static_cast<std::uint64_t>(now);
        }
        auto admitted = Admit(kind, parent);
        if (admitted.HasError())
            return admitted;
        request.cancellation = cancellation.Token();
        if (auto started = restore.Start(request); started.HasError())
            Finish(Result<void>::Failure(started.ErrorValue()));
        else {
            restoreOperation = std::move(started).Value();
            operation->phase = PackageOperationPhase::Restoring;
        }
        return admitted;
    }

    /** @copydoc PackageApplication::Restore */
    Result<PackageOperationId> PackageApplication::Restore(const CancellationToken &cancellation) {
        return impl_->BeginRestore(PackageOperationKind::Restore, cancellation);
    }

    /** @copydoc PackageApplication::Install */
    Result<PackageOperationId> PackageApplication::Install(const CancellationToken &cancellation) {
        return impl_->BeginRestore(PackageOperationKind::Install, cancellation);
    }

    void PackageApplication::Impl::SubmitInstall(std::shared_ptr<const Packages::PackageRestoreGraph> graph) {
        if (!graph) {
            Finish(Result<void>::Failure(Failure("packages.application.graph_unavailable")));
            return;
        }
        auto completed = std::make_shared<PackageOperations::Completion>();
        auto *authority = &install;
        auto submitted = jobs.SubmitResult({.parentCancellation = cancellation.Token(), .resource = JobResource::Io},
                                           [authority, graph = std::move(graph), completed](const CancellationToken &token) {
            auto result = authority->Install(graph, token);
            std::lock_guard guard(completed->mutex);
            completed->result = std::move(result);
            return Result<void>::Success();
        });
        if (submitted.HasError())
            Finish(Result<void>::Failure(submitted.ErrorValue()));
        else {
            completion = std::move(completed);
            installJob = std::move(submitted).Value();
            operation->phase = PackageOperationPhase::Installing;
            ++operation->revision;
        }
    }

    /** @copydoc PackageApplication::Activate */
    Result<PackageOperationId> PackageApplication::Activate(const std::span<const Packages::EnabledPackageExtension> enabled,
                                                            const CancellationToken &cancellation) {
        if (const auto valid = impl_->CheckAdmission(); valid.HasError())
            return Result<PackageOperationId>::Failure(valid.ErrorValue());
        if (enabled.size() > impl_->configuration.maximumEnabledDescriptors)
            return Result<PackageOperationId>::Failure(Failure("packages.application.capacity"));
        std::vector<Packages::EnabledPackageExtension> owned(enabled.begin(), enabled.end());
        const auto installed = impl_->install.InstalledRecord();
        const bool needsAvailability =
            !owned.empty() && (!installed || installed->Graph()->requestHash != impl_->configuration.restore.expectedRequestHash);
        auto admitted = needsAvailability ? impl_->BeginRestore(PackageOperationKind::Activate, cancellation)
                                          : impl_->Admit(PackageOperationKind::Activate, cancellation);
        if (admitted.HasValue() && impl_->operation->outcome == PackageOperationOutcome::Running) {
            impl_->enabled = std::move(owned);
            if (!needsAvailability)
                impl_->operation->phase = PackageOperationPhase::Activating;
        }
        return admitted;
    }

    /** @copydoc PackageApplication::Disable */
    Result<PackageOperationId> PackageApplication::Disable(const CancellationToken &cancellation) {
        auto result = Activate({}, cancellation);
        if (result.HasValue())
            impl_->operation->kind = PackageOperationKind::Disable;
        return result;
    }
}  // namespace Horo::Application
