#pragma once
/** @file TerrainAssetCreation.h @brief Immutable actual-asset choices for the transient terrain resource creation modal. */
#include "Horo/Assets/AssetId.h"

#include <string>
#include <vector>

namespace Horo::Editor {
    enum class TerrainCreationChoiceKind : std::uint8_t {
        Material,
        Mesh
    };

    struct TerrainCreationAssetChoice final {
        Assets::AssetId asset;
        TerrainCreationChoiceKind kind;
        std::string label;
    };
}  // namespace Horo::Editor
