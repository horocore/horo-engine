#pragma once

/**
 * @file UpdateRetention.h
 * @brief Deterministic disk-budget plan that protects active and last-known-good versions.
 */

#include "Horo/Release/DistributionModel.h"

#include <cstdint>
#include <span>
#include <vector>

namespace Horo::Release {
    /** @brief Installed-version role established from trusted active and rollback records. */
    enum class UpdateRetentionRole : std::uint8_t {
        Active,
        LastKnownGood,
        Obsolete
    };

    /** @brief Exact measured storage for one host-owned immutable installed version. */
    struct UpdateRetentionVersion final {
        DistributionPackageId package;
        std::uint64_t occupiedBytes{};
        std::uint64_t lastUsedGeneration{};
        UpdateRetentionRole role{UpdateRetentionRole::Obsolete};
    };

    /** @brief Obsolete versions to remove in order, and occupied bytes after their removal. */
    struct UpdateRetentionPlan final {
        std::vector<DistributionPackageId> remove;
        std::uint64_t remainingBytes{};
    };

    /**
     * @brief Selects oldest obsolete versions until the measured installation fits its disk budget.
     * @param versions Complete host-owned installed-version snapshot with one active and one distinct last-known-good version.
     * @param maximumBytes Maximum measured bytes retained across these immutable versions.
     * @return Deterministic deletion plan; failure if protected versions exceed budget or the snapshot is malformed.
     * @note The caller must recheck active/rollback records under the installation lock before applying this plan.
     */
    [[nodiscard]] Result<UpdateRetentionPlan> PlanUpdateRetention(std::span<const UpdateRetentionVersion> versions,
                                                                  std::uint64_t maximumBytes);
}  // namespace Horo::Release
