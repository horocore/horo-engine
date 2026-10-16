#pragma once

/** @file TriggerVolumeCellTestValues.h @brief Portable project constraints shared by independent cell ownership/codec regressions. */
#include "Horo/Runtime/Scene/RuntimeSceneDefinition.h"

#include <array>

namespace Horo::Runtime::Test {
    struct TriggerCellValues final {
        std::array<RuntimeEntityDefinition, 1> entities;
        std::array<TriggerVolumeMigrationEvidence, 1> proof;

        TriggerCellValues() {
            std::array<std::uint8_t, 16> profileBytes{};
            profileBytes.back() = 1;
            const auto profile = Physics::CollisionProfileId::FromBytes(profileBytes);
            std::array<std::uint8_t, 16> materialBytes{};
            materialBytes.back() = 91;
            const auto material = Assets::AssetId::FromBytes(materialBytes);
            const auto type = Assets::AssetTypeId::Parse("core.physics_material").Value();
            const std::array<std::uint8_t, 3> bytes{1, 2, 3};
            const TriggerVolumeProjectBinding binding{874, profile, {material, type}, 1, ComputeSha256(std::as_bytes(std::span{bytes}))};
            entities.front() = {.object = {1},
                                .components = {.rigidBody = RigidBodyComponent{.id = {1}, .body = {1}},
                                               .colliders = {ColliderComponent{.id = {2},
                                                                               .collider = {1},
                                                                               .body = {{1}, {1}},
                                                                               .collisionProfile = profile,
                                                                               .materials = {{Physics::PhysicsMaterialSlotId::FromValue(1),
                                                                                              material}},
                                                                               .sensor = true}}}};
            proof.front() = {{1}, {1}, {1}, binding};
        }
    };
}  // namespace Horo::Runtime::Test
