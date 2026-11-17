#pragma once
/** @file PrefabSourceExpansionInternal.h @brief One shared bounded source/composition expansion operation. */
#include "Horo/Prefab/PrefabSourceResolver.h"
#include "Horo/Prefab/PrefabValueOverrides.h"

namespace Horo::Prefab::Detail {
    /** @brief Operation-local shared budget, scoped identities and detached output; never a second publication authority. */
    struct SourceExpansionState final {
        std::span<const PrefabDependencySource> sources;
        PrefabInstanceId instance;
        std::vector<LocalObjectId> &scope;
        std::vector<Assets::AssetId> &active;
        std::vector<ResolvedPrefabObject> &objects;
        std::vector<Assets::AssetId> &failureChain;
        PrefabExpansionBudget &budget;
        const PrefabLimitProfile &limits;
        const CancellationToken &cancellation;
        std::vector<PrefabCompositionOriginStep> originChain;
        const PrefabValueSchemaSnapshot *schemas{};
        std::vector<PrefabLayerOverrideEvidence> &layers;
        bool unresolved{};
    };

    /** @brief Rejects the complete combined graph cycle/depth/object/work failure before any hierarchy is materialized. */
    [[nodiscard]] Result<void> PreflightSourceExpansion(SourceExpansionState &state, const PrefabDependencySource &root);

    /** @brief Applies an existing typed override set to one fully expanded source slice before its owning composition returns. */
    [[nodiscard]] Result<void> ApplySourceOverrideLayer(SourceExpansionState &state, const std::optional<PrefabValueOverrideSet> &layer,
                                                        const PrefabDependencySource &source, Assets::AssetId owner,
                                                        std::optional<LocalObjectId> placement, std::size_t begin);
}  // namespace Horo::Prefab::Detail
