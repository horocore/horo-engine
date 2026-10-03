#pragma once

#include "Horo/Prefab/CookedPrefab.h"

namespace Horo::Prefab::Detail {
    /** @brief Encodes validated typed tables within the exact cooked payload ceiling. */
    [[nodiscard]] Result<std::vector<std::byte>> EncodeCookedPrefabPayload(const CookedPrefabData &data, std::size_t maximumBytes);
    /** @brief Decodes only already integrity-verified payload bytes with captured collection ceilings. */
    [[nodiscard]] Result<CookedPrefabData> DecodeCookedPrefabPayload(std::span<const std::byte> payload, Assets::AssetId asset,
                                                                     std::uint32_t objectCount, const PrefabProjectPolicy &policy);
}  // namespace Horo::Prefab::Detail
