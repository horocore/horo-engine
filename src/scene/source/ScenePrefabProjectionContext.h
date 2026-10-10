#pragma once

#include "Horo/Scene/SceneRuntimeConversion.h"

#include <format>

namespace Horo::SceneSource::Detail {
    /** @brief Adds exact containing-instance context without replacing the resolver's typed error identity. */
    inline void AddInstanceContext(Error &error, const ScenePrefabInstanceProjection &projection) {
        error.message = std::format("Required prefab instance {} ({}) failed: {}", projection.authored.instanceId.Value(),
                                    projection.authored.sourcePrefab.Asset().ToString(), error.message);
    }
}  // namespace Horo::SceneSource::Detail
