#pragma once

#include "Horo/Application/BlackboardUsageReview.h"

namespace Horo::Application::BlackboardUsageSources {
    inline constexpr std::size_t MaximumFindings = AI::DecisionAssetValidationHardLimits::Diagnostics;

    /** @brief Identifies the optional authored Scene and Prefab asset scope. */
    [[nodiscard]] std::optional<BlackboardUsageGapKind> OwnerKind(std::string_view assetType) noexcept;

    /** @brief Adds one explicit owner gap, keeping all review output bounded. */
    [[nodiscard]] Result<void> AddGap(BlackboardUsageReview &review, BlackboardUsageGap gap);

    /** @brief Reviews the frozen Scene/Prefab source batch and resolves supported authored placements. */
    [[nodiscard]] Result<void> Inspect(BlackboardUsageReview &review, const std::filesystem::path &root,
                                       const Assets::AssetCookInputSnapshot &owners, const CancellationToken &cancellation);
}  // namespace Horo::Application::BlackboardUsageSources
