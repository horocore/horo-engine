#pragma once

#include "Horo/Application/BlackboardUsageReview.h"

namespace Horo::Application::BlackboardUsageSources {
    inline constexpr std::size_t MaximumFindings = AI::DecisionAssetValidationHardLimits::Diagnostics;

    /** @brief Identifies the optional authored Scene and Prefab asset scope. */
    [[nodiscard]] std::optional<BlackboardUsageGapKind> OwnerKind(std::string_view assetType) noexcept;

    /** @brief Adds one explicit owner gap, keeping all review output bounded. */
    [[nodiscard]] Result<void> AddGap(BlackboardUsageReview &review, BlackboardUsageGap gap);

    /** @brief Reviews one registered source for direct schema bindings and identifies undecoded ownership. */
    [[nodiscard]] Result<void> Inspect(BlackboardUsageReview &review, const Assets::AssetCookPinnedSource &file,
                                       const CancellationToken &cancellation);
}  // namespace Horo::Application::BlackboardUsageSources
