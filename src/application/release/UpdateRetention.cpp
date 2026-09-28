#include "Horo/Release/UpdateRetention.h"

#include "Horo/Release/UpdateRetentionErrors.h"

#include <algorithm>
#include <functional>
#include <limits>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace Horo::Release {
    namespace {
        /** @brief Hashes borrowed package IDs for the duration of one retention plan. */
        struct TransparentStringHash final {
            using is_transparent = void;

            [[nodiscard]] std::size_t operator()(const std::string_view value) const noexcept {
                return std::hash<std::string_view>{}(value);
            }
        };
    }  // namespace

    /** @copydoc PlanUpdateRetention */
    Result<UpdateRetentionPlan> PlanUpdateRetention(const std::span<const UpdateRetentionVersion> versions,
                                                    const std::uint64_t maximumBytes) {
        using enum UpdateRetentionRole;
        std::unordered_set<std::string_view, TransparentStringHash, std::equal_to<>> seen;
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
                case Active:
                    ++activeCount;
                    protectedBytes += version.occupiedBytes;
                    break;
                case LastKnownGood:
                    ++lastKnownGoodCount;
                    protectedBytes += version.occupiedBytes;
                    break;
                case Obsolete:
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
