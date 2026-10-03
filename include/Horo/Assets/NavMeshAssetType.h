#pragma once

/** @file NavMeshAssetType.h
 * @brief Canonical AssetRegistry sidecar type shared by Scene dependency projection and NavMesh loading. */

#include <string_view>

namespace Horo::Assets {
    /** @brief One definition AssetId owns all generated surface/profile/tile partitions. */
    inline constexpr std::string_view NavMeshAssetTypeName = "core.navmesh";
}  // namespace Horo::Assets
