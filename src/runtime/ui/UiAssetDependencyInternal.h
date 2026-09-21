#pragma once

/**
 * @file UiAssetDependencyInternal.h
 * @brief Target-private ordered dependency merge support for Runtime UI models.
 */

#include "Horo/Runtime/Ui/UiAssetDependency.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace Horo::Runtime::Ui::Internal {
    /** @brief Result of merging one validated-or-unvalidated dependency into an ordered manifest. */
    enum class UiAssetDependencyMergeResult : std::uint8_t {
        Inserted,
        Strengthened,
        Invalid,
        Conflict,
        CapacityExceeded,
    };

    /**
     * @brief Merges a dependency by stable asset identity while preserving canonical order.
     * @param dependencies Ordered manifest to update.
     * @param dependency Candidate dependency; invalid data is reported without mutation.
     * @param maximumSize Maximum manifest size for newly inserted identities.
     * @return Merge outcome; conflicting type and capacity failures leave the manifest unchanged.
     */
    [[nodiscard]] inline UiAssetDependencyMergeResult MergeUiAssetDependency(
        std::vector<UiAssetDependency> &dependencies, UiAssetDependency dependency,
        const std::size_t maximumSize = std::numeric_limits<std::size_t>::max()) {
        if (!dependency.asset.IsValid() || dependency.expectedType.Value().empty())
            return UiAssetDependencyMergeResult::Invalid;

        const auto position = std::ranges::lower_bound(dependencies, dependency.asset, {}, &UiAssetDependency::asset);
        if (position != dependencies.end() && position->asset == dependency.asset) {
            if (position->expectedType != dependency.expectedType)
                return UiAssetDependencyMergeResult::Conflict;
            position->required = position->required || dependency.required;
            return UiAssetDependencyMergeResult::Strengthened;
        }
        if (dependencies.size() >= maximumSize)
            return UiAssetDependencyMergeResult::CapacityExceeded;
        dependencies.insert(position, std::move(dependency));
        return UiAssetDependencyMergeResult::Inserted;
    }
}  // namespace Horo::Runtime::Ui::Internal
