#pragma once
#include "Horo/Assets/AssetRegistry.h"

namespace Horo::Editor {
    /** @brief Uses the sole Assets scanner to reject changed/degraded physical inventories without publishing a registry.
     * @param root Canonical authorized project root. @param expected Exact complete source set captured by review.
     * @return Original scanner error or typed stale inventory failure; no files or live registry are changed. */
    [[nodiscard]] Result<void> ValidateCollisionSourceInventory(const std::filesystem::path &root,
                                                                const Assets::AssetRegistrySnapshot &expected);
}  // namespace Horo::Editor
