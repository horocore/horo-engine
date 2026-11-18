#pragma once

/** @file PrefabApplyPreparation.h @brief Detached prefab and Scene candidates for apply-to-source. */
#include "editor/document/PrefabOverrideScope.h"

namespace Horo::Editor {
    /** @brief Complete paired authored state after promotion and active Scene instance rebase. */
    struct PreparedPrefabApply final {
        Prefab::PrefabDocument source;
        std::vector<ScenePrefabInstance> placements;
    };

    /** @brief Resolves the selected exact source edits and restages every affected active Scene placement.
     * @param scene Exact current authored Scene snapshot. @param command Scope and optimistic revision/schema fences.
     * @param registry Current original Asset Registry publication. @param sources Complete pinned source closure.
     * @param schemas Captured value and topology registry.
     * @param limits Captured canonical bounds. @param cancellation Cooperative stop during detached preparation.
     * @return Both complete immutable-source and owned Scene candidates or typed rejection with no observable mutation.
     * @details Values and component topology are authored in the immediate variant or exact nested occurrence when inherited.
     * Older dependent source revisions remain fenced.
     */
    [[nodiscard]] Result<PreparedPrefabApply> PreparePrefabApply(
        const SceneDocumentSnapshot &scene, const RevertScenePrefabOverridesCommand &command, const Assets::AssetRegistrySnapshot &registry,
        const Prefab::PrefabSourceResolverSnapshot &sources, std::shared_ptr<const Prefab::PrefabValueSchemaSnapshot> schemas,
        const Prefab::PrefabLimitProfile &limits, const CancellationToken &cancellation = {});
}  // namespace Horo::Editor
