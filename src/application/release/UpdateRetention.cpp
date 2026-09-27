#include "Horo/Release/UpdateRetention.h"

#include "Horo/Release/UpdateRetentionErrors.h"

#include <algorithm>
#include <limits>
#include <unordered_set>
#include <utility>

namespace Horo::Release {
    /** @copydoc PlanUpdateRetention */
    Result<UpdateRetentionPlan> PlanUpdateRetention(const std::span<const UpdateRetentionVersion> versions,
                                                    const std::uint64_t maximumBytes) {
        std::unordered_set<std::string> seen;
        seen.reserve(versions.size());
        std::vector<const UpdateRetentionVersion *> obsolete;
        obsolete.reserve(versions.size());
        std::uint64_t occupied{};
        std::uint64_t protectedBytes{};
        unsigned activeCount{};
        unsigned lastKnownGoodCount{};
        for (const auto &version : versions) {
            if (!IsValidDistributionIdentity(version.package.value) || !seen.insert(version.package.value).second ||
                version.occupiedBytes == 0U || version.occupiedBytes > std::numeric_limits<std::uint64_t>::max() - occupied)
                return Result<UpdateRetentionPlan>::Failure(MakeError(UpdateRetentionErrors::InvalidSnapshot));
            occupied += version.occupiedBytes;
            switch (version.role) {
                case UpdateRetentionRole::Active:
                    ++activeCount;
                    protectedBytes += version.occupiedBytes;
                    break;
                case UpdateRetentionRole::LastKnownGood:
                    ++lastKnownGoodCount;
                    protectedBytes += version.occupiedBytes;
                    break;
                case UpdateRetentionRole::Obsolete:
                    obsolete.push_back(&version);
                    break;
                default:
                    return Result<UpdateRetentionPlan>::Failure(MakeError(UpdateRetentionErrors::InvalidSnapshot));
            }
        }
        if (activeCount != 1U || lastKnownGoodCount != 1U)
            return Result<UpdateRetentionPlan>::Failure(MakeError(UpdateRetentionErrors::InvalidSnapshot));
        if (protectedBytes > maximumBytes)
            return Result<UpdateRetentionPlan>::Failure(MakeError(UpdateRetentionErrors::ProtectedBudgetExceeded));
        std::ranges::sort(obsolete, [](const auto *left, const auto *right) {
            if (left->lastUsedGeneration != right->lastUsedGeneration)
                return left->lastUsedGeneration < right->lastUsedGeneration;
            return left->package.value < right->package.value;
        });
        UpdateRetentionPlan plan;
        for (const auto *version : obsolete) {
            if (occupied <= maximumBytes)
                break;
            plan.remove.push_back(version->package);
            occupied -= version->occupiedBytes;
        }
        plan.remainingBytes = occupied;
        return Result<UpdateRetentionPlan>::Success(std::move(plan));
    }
}  // namespace Horo::Release
