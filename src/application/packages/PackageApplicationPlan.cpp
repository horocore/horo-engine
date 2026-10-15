#include "PackageApplicationInternal.h"

#include <limits>
#include <new>
#include <stdexcept>

namespace Horo::Application {
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

    /** @copydoc PackageApplication::Plan */
    Result<std::shared_ptr<const PackagePlanSnapshot>> PackageApplication::Plan() const {
        if (const auto lane = impl_->CheckLane(); lane.HasError())
            return Result<std::shared_ptr<const PackagePlanSnapshot>>::Failure(lane.ErrorValue());
        if (!impl_->plan)
            return Result<std::shared_ptr<const PackagePlanSnapshot>>::Failure(
                PackageOperations::Failure("packages.application.plan_unavailable"));
        return Result<std::shared_ptr<const PackagePlanSnapshot>>::Success(impl_->plan);
    }
}  // namespace Horo::Application
