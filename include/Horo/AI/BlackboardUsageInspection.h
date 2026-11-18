#pragma once

/** @file BlackboardUsageInspection.h @brief Typed Blackboard requirement evidence from Decision source. */

#include "Horo/AI/DecisionAssetValidation.h"

namespace Horo::AI {
    class DecisionNodeGeneration;
    struct DecisionGraphSource;

    /** @brief One statically declared key use in a source node or its current provider contract. */
    struct DecisionBlackboardSourceUse final {
        DecisionNodeId node;
        DecisionBlackboardRequirement requirement;
        std::optional<DecisionDescriptorOrigin> provider;
    };

    /** @brief Owned metadata projection. Unresolved providers make the source inventory incomplete. */
    struct DecisionBlackboardSourceUses final {
        std::vector<DecisionBlackboardSourceUse> uses;
        std::vector<DecisionNodeId> unresolvedProviders;
    };

    /** @brief Finds exact typed requirements through the canonical Decision lowering path.
     * @param source Decoded graph compatible with the caller's exact schema.
     * @param key Stable key identity to match. @param providers Optional exact current provider generation.
     * @return Declared uses and unresolved providers, or a typed source/limit error. No compiler, factory or arbitrary property is invoked.
     */
    [[nodiscard]] Result<DecisionBlackboardSourceUses> InspectDecisionBlackboardUses(const DecisionGraphSource &source, BlackboardKeyId key,
                                                                                     const DecisionNodeGeneration *providers);

}  // namespace Horo::AI
