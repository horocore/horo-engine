#pragma once

#include "Horo/Physics/PhysicsCollisionCooker.h"

#include <array>

namespace Horo::Physics::Detail {
    inline constexpr std::array<std::uint8_t, 4> CollisionArtifactMagic{'P', 'C', 'A', '1'};
    inline constexpr std::size_t CollisionArtifactHeaderBytes = 4 + 1 + 8 + 5 * 32;

    /** @brief Appends a fixed-width canonical little-endian scalar. */
    inline void AppendCollisionInteger(std::vector<std::uint8_t> &bytes, const std::uint64_t value, const unsigned width) {
        for (unsigned index = 0; index < width; ++index)
            bytes.push_back(static_cast<std::uint8_t>(value >> (index * 8U)));
    }

    /** @brief Appends one complete cryptographic identity. */
    inline void AppendCollisionDigest(std::vector<std::uint8_t> &bytes, const Sha256Digest &digest) {
        bytes.insert(bytes.end(), digest.bytes.begin(), digest.bytes.end());
    }
}  // namespace Horo::Physics::Detail
