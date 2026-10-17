#pragma once
#include "Horo/Assets/AssetRegistry.h"

namespace Horo::Editor {
    /** @brief Uses the sole Assets scanner to reject changed/degraded physical inventories without publishing a registry.
     * @param source Root-bound native inventory authority. @param expected Exact complete source set captured by review.
     * @return Original scanner error or typed stale inventory failure; no files or live registry are changed. */
    [[nodiscard]] Result<void> ValidateCollisionSourceInventory(const Assets::IAssetRegistrySource &source,
                                                                const Assets::AssetRegistrySnapshot &expected);
}  // namespace Horo::Editor
