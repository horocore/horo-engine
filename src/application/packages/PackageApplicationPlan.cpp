#include "Horo/Packages/PackageLifecycleErrors.h"
#include "PackageApplicationInternal.h"

#include <algorithm>
#include <limits>
#include <map>
#include <new>
#include <stdexcept>

namespace Horo::Application {
    /** @copydoc PackageApplication::Impl::IndexPlanDependents */
    void PackageApplication::Impl::IndexPlanDependents(PackagePlanSnapshot &plan) {
        std::map<std::string_view, PackagePlanEntry *, std::less<>> entries;
        for (auto &entry : plan.packages)
            entries.try_emplace(entry.locked.package.Value(), &entry);
        for (const auto &indexed : entries) {
            const auto &entry = indexed.second->locked;
            for (const auto &dependency : entry.dependencies) {
                const auto provider = entries.find(dependency.package.Value());
                if (provider != entries.end() && provider->second->locked.version == dependency.version)
                    provider->second->requiredBy.emplace_back(entry.package, entry.version);
            }
        }
    }

    /** @brief Builds all presentation evidence before atomically replacing the borrowed plan view. */
    Result<void> PackageApplication::Impl::PublishPlan(std::shared_ptr<const Packages::PackageRestoreGraph> graph) {
        if (!graph || (plan && plan->revision == std::numeric_limits<std::uint64_t>::max()))
            return Result<void>::Failure(PackageOperations::Failure("packages.application.plan_unavailable"));
        try {
            auto candidate = std::make_shared<PackagePlanSnapshot>();
            candidate->revision = plan ? plan->revision + 1 : 1;
            candidate->packages.reserve(graph->packages.size());
            for (const auto &package : graph->packages) {
                PackagePlanEntry entry{.locked = package.lock};
                if (package.archive) {
                    entry.verification = PackagePlanVerification::ArchiveVerified;
                    entry.archiveBytes = package.archive->Bytes().size();
                    entry.verifiedFiles = package.archive->Manifest().Entries().size();
                    entry.expandedBytes = 0;
                    for (const auto &file : package.archive->Manifest().Entries())
                        *entry.expandedBytes += file.size;
                    entry.publisher = package.publisher;
                }
                candidate->packages.push_back(std::move(entry));
            }
            IndexPlanDependents(*candidate);
            plan = std::move(candidate);
            return Result<void>::Success();
        } catch (const std::bad_alloc &) {
            return Result<void>::Failure(PackageOperations::Failure("packages.application.resource_limit"));
        } catch (const std::length_error &) {
            return Result<void>::Failure(PackageOperations::Failure("packages.application.resource_limit"));
        }
    }

    /** @copydoc PackageApplication::Current */
    Result<std::optional<PackageOperationSnapshot>> PackageApplication::Current() const {
        if (const auto lane = impl_->CheckLane(); lane.HasError())
            return Result<std::optional<PackageOperationSnapshot>>::Failure(lane.ErrorValue());
        return Result<std::optional<PackageOperationSnapshot>>::Success(impl_->operation);
    }

    /** @copydoc PackageApplication::ActivationTrustRequired */
    Result<bool> PackageApplication::ActivationTrustRequired() const {
        if (const auto lane = impl_->CheckLane(); lane.HasError())
            return Result<bool>::Failure(lane.ErrorValue());
        const auto &current = impl_->operation;
        const auto &required = Packages::PackageLifecycleErrors::TrustRequired;
        if (impl_->closing || !current || current->kind != PackageOperationKind::Activate ||
            current->outcome == PackageOperationOutcome::Cancelled)
            return Result<bool>::Success(false);
        if (current->outcome == PackageOperationOutcome::Failed && current->diagnostic &&
            ErrorChainContains(*current->diagnostic, required.domain, required.code))
            return Result<bool>::Success(true);
        return Result<bool>::Success(std::ranges::any_of(current->moduleReports, [&required](const auto &report) {
            return report.outcome == Packages::PackageModuleOutcome::Denied && report.diagnostic &&
                   ErrorChainContains(*report.diagnostic, required.domain, required.code);
        }));
    }

    /** @copydoc PackageApplication::Plan */
    Result<std::shared_ptr<const PackagePlanSnapshot>> PackageApplication::Plan() const {
        if (const auto lane = impl_->CheckLane(); lane.HasError())
            return Result<std::shared_ptr<const PackagePlanSnapshot>>::Failure(lane.ErrorValue());
        if (!impl_->plan)
            return Result<std::shared_ptr<const PackagePlanSnapshot>>::Failure(
                PackageOperations::Failure("packages.application.plan_unavailable"));
        return Result<std::shared_ptr<const PackagePlanSnapshot>>::Success(impl_->plan);
    }

    /** @copydoc PackageApplication::PreviewRemoval */
    Result<Packages::PackageRemovalGraphPreview> PackageApplication::PreviewRemoval(const Packages::HoroPackageId &package) const {
        using Output = Result<Packages::PackageRemovalGraphPreview>;
        if (const auto lane = impl_->CheckLane(); lane.HasError())
            return Output::Failure(lane.ErrorValue());
        if (impl_->closing || !impl_->configuration.lockedPlan)
            return Output::Failure(PackageOperations::Failure("packages.application.plan_unavailable"));
        if (const auto current = impl_->configuration.validateAdmission(); current.HasError())
            return Output::Failure(current.ErrorValue());
        const auto &lock = *impl_->configuration.lockedPlan;
        const auto &request = impl_->configuration.portableRequest;
        if (!request || std::ranges::find(lock.Roots(), package, &Packages::LockedPackageReference::package) == lock.Roots().end())
            return lock.PreviewRemoval(package);
        auto replacement = Packages::PackageRemovalPlan::Prepare(*request, lock, package);
        if (replacement.HasError())
            return Output::Failure(replacement.ErrorValue());
        return Output::Success(std::move(replacement).Value().graph);
    }

    /** @copydoc PackageApplication::InspectRemoval */
    Result<PackageRemovalPreview> PackageApplication::InspectRemoval(const Packages::HoroPackageId &package) const {
        using Output = Result<PackageRemovalPreview>;
        auto graph = PreviewRemoval(package);
        if (graph.HasError())
            return Output::Failure(graph.ErrorValue());
        auto retained = impl_->lifecycle.RetainedRemovalPackages(graph.Value().removed);
        if (retained.HasError())
            return Output::Failure(retained.ErrorValue());
        return Output::Success({std::move(graph).Value(), std::move(retained).Value()});
    }

    /** @copydoc PackageApplication::PreviewUpdate */
    Result<Packages::PackageUpdatePreview> PackageApplication::PreviewUpdate(
        const Packages::ValidatedPackageRequest &candidate, const Packages::ValidatedPackageLockfileV1 &candidateLock) const {
        using Output = Result<Packages::PackageUpdatePreview>;
        if (const auto lane = impl_->CheckLane(); lane.HasError())
            return Output::Failure(lane.ErrorValue());
        const auto &configuration = impl_->configuration;
        if (impl_->closing || !configuration.portableRequest || !configuration.lockedPlan)
            return Output::Failure(PackageOperations::Failure("packages.application.plan_unavailable"));
        if (const auto current = configuration.validateAdmission(); current.HasError())
            return Output::Failure(current.ErrorValue());
        if (const auto valid = candidateLock.ValidateForRestore(candidate.Digest(), configuration.restore.platform); valid.HasError())
            return Output::Failure(valid.ErrorValue());
        return Packages::PackageUpdatePreview::Prepare(*configuration.portableRequest, *configuration.lockedPlan, candidate, candidateLock);
    }
}  // namespace Horo::Application
