#include "Horo/Packages/PackageInstallErrors.h"
#include "Horo/Packages/PackageLifecycleErrors.h"
#include "PackageApplicationInternal.h"

namespace Horo::Application {
    using PackageOperations::Failure;

    void PackageApplication::Impl::PumpRestore() {
        const auto progress = restore.Query(restoreOperation->Id());
        if (!progress) {
            Finish(Result<void>::Failure(Failure("packages.application.restore_lost")));
            return;
        }
        if (operation->completion != progress->progress || operation->completedPackages != progress->completedPackages ||
            operation->totalPackages != progress->totalPackages) {
            operation->completion = progress->progress;
            operation->completedPackages = progress->completedPackages;
            operation->totalPackages = progress->totalPackages;
            ++operation->revision;
        }
        if (progress->outcome == Packages::PackageRestoreOutcome::Running)
            return;
        if (progress->outcome == Packages::PackageRestoreOutcome::Ready) {
            if (cancellation.Token().IsCancellationRequested()) {
                Finish(Result<void>::Failure(Failure("packages.application.cancelled")), true);
                restoreOperation.reset();
                return;
            }
            if (auto current = configuration.validateAdmission(); current.HasError()) {
                Finish(std::move(current));
                restoreOperation.reset();
                return;
            }
            if (auto published = PublishPlan(restore.ActiveGraph()); published.HasError()) {
                Finish(std::move(published));
                restoreOperation.reset();
                return;
            }
            if (operation->kind == PackageOperationKind::Install || operation->kind == PackageOperationKind::Activate)
                SubmitInstall(restore.ActiveGraph());
            else
                Finish(Result<void>::Success());
        } else
            Finish(Result<void>::Failure(progress->diagnostic.value_or(Failure("packages.application.restore_failed"))),
                   progress->outcome == Packages::PackageRestoreOutcome::Cancelled);
        restoreOperation.reset();
    }

    void PackageApplication::Impl::PumpInstall() {
        const auto job = installJob->Snapshot();
        if (!job || job->state == JobState::Queued || job->state == JobState::Running)
            return;
        std::optional<Result<void>> result;
        {
            std::lock_guard guard(completion->mutex);
            result = std::move(completion->result);
        }
        // The durable authority's commit result wins over cancellation observed after publication.
        if (result) {
            const bool cancelled =
                result->HasError() && result->ErrorValue().code.Value() == Packages::PackageInstallErrors::Cancelled.code.Value();
            if (result->HasValue()) {
                if (const auto installed = install.InstalledRecord())
                    operation->installedRevision = installed->Revision();
                if (operation->kind == PackageOperationKind::Activate) {
                    operation->phase = PackageOperationPhase::Activating;
                    ++operation->revision;
                } else
                    Finish(Result<void>::Success());
            } else
                Finish(std::move(*result), cancelled);
        } else
            Finish(Result<void>::Failure(job->error.value_or(Failure("packages.application.install_failed"))),
                   job->state == JobState::Cancelled);
        installJob.reset();
        completion.reset();
    }

    void PackageApplication::Impl::PumpActivation(const Packages::PackageActivationBoundary boundary) {
        if (cancellation.Token().IsCancellationRequested()) {
            Finish(Result<void>::Failure(Failure("packages.application.cancelled")), true);
            enabled.clear();
            return;
        }
        if (boundary == Packages::PackageActivationBoundary::Running)
            return;
        if (enabled.empty() && !lifecycle.Active()) {
            Finish(Result<void>::Success());
            return;
        }
        if (auto current = configuration.validateAdmission(); current.HasError()) {
            Finish(std::move(current));
            enabled.clear();
            return;
        }
        auto result = lifecycle.Activate(enabled, boundary, cancellation.Token());
        if (result.HasValue()) {
            const auto active = lifecycle.Active();
            operation->activeGeneration = active ? std::optional{active->generation} : std::nullopt;
        }
        const bool cancelled =
            result.HasError() && result.ErrorValue().code.Value() == Packages::PackageLifecycleErrors::Cancelled.code.Value();
        Finish(std::move(result), cancelled);
        enabled.clear();
    }

    /** @copydoc PackageApplication::Pump */
    Result<void> PackageApplication::Pump(const Packages::PackageActivationBoundary boundary) {
        if (const auto lane = impl_->CheckLane(); lane.HasError())
            return lane;
        if (impl_->closed)
            return Result<void>::Failure(Failure("packages.application.closed"));
        if (boundary != Packages::PackageActivationBoundary::Startup && boundary != Packages::PackageActivationBoundary::Quiescent &&
            boundary != Packages::PackageActivationBoundary::Running)
            return Result<void>::Failure(Failure("packages.application.invalid_input"));
        if (impl_->operation && impl_->operation->outcome == PackageOperationOutcome::Running) {
            if (impl_->restoreOperation)
                impl_->PumpRestore();
            else if (impl_->installJob)
                impl_->PumpInstall();
            else if (impl_->operation->kind == PackageOperationKind::Activate || impl_->operation->kind == PackageOperationKind::Disable)
                impl_->PumpActivation(boundary);
            else
                impl_->Finish(Result<void>::Failure(Failure("packages.application.incomplete_operation")));
        }
        impl_->lifecycle.FinalizeRetirements();
        return Result<void>::Success();
    }

    /** @copydoc PackageApplication::Query */
    Result<PackageOperationSnapshot> PackageApplication::Query(const PackageOperationId operation) const {
        if (const auto lane = impl_->CheckLane(); lane.HasError())
            return Result<PackageOperationSnapshot>::Failure(lane.ErrorValue());
        if (!impl_->operation || operation.value == 0 || impl_->operation->id != operation)
            return Result<PackageOperationSnapshot>::Failure(Failure("packages.application.stale_operation"));
        return Result<PackageOperationSnapshot>::Success(*impl_->operation);
    }

    /** @copydoc PackageApplication::RequestCancel */
    Result<void> PackageApplication::RequestCancel(const PackageOperationId operation) {
        if (const auto current = Query(operation); current.HasError())
            return Result<void>::Failure(current.ErrorValue());
        if (impl_->operation->outcome != PackageOperationOutcome::Running)
            return Result<void>::Success();
        impl_->cancellation.RequestCancellation();
        if (impl_->restoreOperation)
            return impl_->restore.RequestCancel(impl_->restoreOperation->Id());
        if (impl_->installJob)
            return impl_->installJob->RequestCancel();
        return Result<void>::Success();
    }

    /** @copydoc PackageApplication::Close */
    Result<void> PackageApplication::Close() {
        if (const auto lane = impl_->CheckLane(); lane.HasError())
            return lane;
        if (impl_->closed)
            return Result<void>::Success();
        impl_->closing = true;
        if (impl_->operation && impl_->operation->outcome == PackageOperationOutcome::Running) {
            if (const auto cancelled = RequestCancel(impl_->operation->id); cancelled.HasError())
                return cancelled;
            if (const auto pumped = Pump(Packages::PackageActivationBoundary::Running); pumped.HasError())
                return pumped;
            if (impl_->operation->outcome == PackageOperationOutcome::Running)
                return Result<void>::Failure(Failure("packages.application.busy"));
        }
        impl_->closed = true;
        return Result<void>::Success();
    }
}  // namespace Horo::Application
