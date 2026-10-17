#pragma once

/** @file TriggerVolumeConversionTestValues.h
 * @brief Explicit project-owned fixture bindings; these identities are never engine defaults.
 */
#include "Horo/Scene/SceneRuntimeConversion.h"

namespace Horo::Test {
    inline SceneSource::TriggerVolumeConversionContext TriggerBinding(const std::uint64_t fingerprint = 874) {
        std::array<std::uint8_t, 16> profile{};
        profile.back() = 1;
        std::array<std::uint8_t, 16> material{};
        material.back() = 91;
        const std::array<std::uint8_t, 3> bytes{1, 2, 3};
        return {{fingerprint,
                 Physics::CollisionProfileId::FromBytes(profile),
                 {Assets::AssetId::FromBytes(material), Assets::AssetTypeId::Parse("core.physics_material").Value()},
                 1,
                 ComputeSha256(std::as_bytes(std::span{bytes}))},
                1};
    }
}  // namespace Horo::Test
