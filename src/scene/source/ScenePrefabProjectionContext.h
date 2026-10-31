#pragma once

#include "Horo/Scene/SceneRuntimeConversion.h"

#include <format>

namespace Horo::SceneSource::Detail {
    /** @brief Adds exact containing-instance context without replacing the resolver's typed error identity. */
    inline void AddInstanceContext(Error &error, const ScenePrefabInstanceProjection &projection) {
        error.message = std::format("Required prefab instance {} ({}) failed: {}", projection.authored.instanceId.Value(),
                                    projection.authored.sourcePrefab.Asset().ToString(), error.message);
    }

    /** @brief Operation-local aggregate inspection admission; independent of canonical expansion success. */
    struct ScenePrefabDebugCaptureBudget final {
        std::size_t remainingEntries{Prefab::PrefabDebugCaptureLimits::MaximumEntries};
        std::size_t remainingSnapshots{64};
        std::size_t remainingValueEntries{4096};
        std::size_t remainingValueBytes{4 * 1024 * 1024};
    };

    /** @brief Apply instance values only after source-cache resolution; retain unresolved authoring evidence. */
    void ApplyInstanceValues(ScenePrefabInstanceProjection &entry, const Prefab::PrefabValueSchemaSnapshot *schemas,
                             const Prefab::PrefabLimitProfile &limits, ScenePrefabDebugCaptureBudget &debugBudget,
                             const CancellationToken &cancellation = {});
    /** @brief Reject source-only/modified candidate substitution and stale schema metadata at runtime publication. */
    [[nodiscard]] Result<void> ValidateInstanceValues(const ScenePrefabInstanceProjection &entry,
                                                      const Prefab::PrefabValueSchemaSnapshot *schemas,
                                                      const Prefab::PrefabLimitProfile &limits);
}  // namespace Horo::SceneSource::Detail
