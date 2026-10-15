#pragma once

#include "Horo/Scene/SceneRuntimeConversion.h"

#include <format>

namespace Horo::SceneSource::Detail {
    /** @brief Adds exact containing-instance context without replacing the resolver's typed error identity. */
    inline void AddInstanceContext(Error &error, const ScenePrefabInstanceProjection &projection) {
        error.message = std::format("Required prefab instance {} ({}) failed: {}", projection.authored.instanceId.Value(),
                                    projection.authored.sourcePrefab.Asset().ToString(), error.message);
    }

    /** @brief Apply instance values only after source-cache resolution; retain unresolved authoring evidence. */
    void ApplyInstanceValues(ScenePrefabInstanceProjection &entry, const Prefab::PrefabValueSchemaSnapshot *schemas,
                             const Prefab::PrefabLimitProfile &limits, const CancellationToken &cancellation = {});
    /** @brief Reject source-only/modified candidate substitution and stale schema metadata at runtime publication. */
    [[nodiscard]] Result<void> ValidateInstanceValues(const ScenePrefabInstanceProjection &entry,
                                                      const Prefab::PrefabValueSchemaSnapshot *schemas,
                                                      const Prefab::PrefabLimitProfile &limits);
}  // namespace Horo::SceneSource::Detail
