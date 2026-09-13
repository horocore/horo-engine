#pragma once

#include "Horo/Assets/AssetId.h"

#include <array>
#include <cstdint>

namespace Horo::Prefab::Test {
    [[nodiscard]] inline Assets::AssetId Asset(const std::uint16_t suffix = 1) {
        std::array<std::uint8_t, 16> bytes{};
        bytes[14] = static_cast<std::uint8_t>(suffix >> 8U);
        bytes[15] = static_cast<std::uint8_t>(suffix);
        return Assets::AssetId::FromBytes(bytes);
    }
}  // namespace Horo::Prefab::Test
