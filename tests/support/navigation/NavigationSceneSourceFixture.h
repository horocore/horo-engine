#pragma once

/** @file NavigationSceneSourceFixture.h
 * @brief Authored collider, surface and explicit source policy shared by source and document regressions.
 */

#include "Horo/Scene/SceneSourceModel.h"

#include <array>

namespace Horo::Tests::NavigationSources {
    template <typename T> T Id(const std::uint64_t value) {
        return T::Create(value).Value();
    }

    /** @brief Issues deterministic authoring asset identity without source-name or runtime-handle derivation. */
    inline Assets::AssetId Asset(const std::uint8_t value) {
        std::array<std::uint8_t, 16> bytes{};
        bytes.back() = value;
        return Assets::AssetId::FromBytes(bytes);
    }

    /** @brief Creates one coherent static source using only already-owned Physics and Navigation identities. */
    inline SceneSource::SceneObjectSnapshot Object(const std::uint64_t object = 1, const std::uint64_t surface = 10) {
        SceneSource::SceneObjectSnapshot result{.id = {object}, .name = "Navigation floor"};
        result.components.navigationSurface =
            Runtime::NavigationSurfaceComponent{.id = Id<Navigation::SurfaceId>(surface),
                                                .definition = Asset(1),
                                                .profiles = {Id<Navigation::NavigationAgentProfileId>(1)}};
        result.components.rigidBody = Runtime::RigidBodyComponent{.id = {1}, .body = {1}};
        result.components.colliders.push_back({.id = {2},
                                               .collider = {3},
                                               .body = {{object}, {1}},
                                               .collisionProfile = Physics::CollisionProfileId::FromBytes(Asset(1).Bytes()),
                                               .materials = {{Physics::PhysicsMaterialSlotId::FromValue(1), Asset(4)}}});
        result.components.navigationSources.push_back({.collider = {3},
                                                       .surface = Id<Navigation::SurfaceId>(surface),
                                                       .area = Id<Navigation::NavigationAreaId>(3),
                                                       .filter = Id<Navigation::NavigationFilterId>(4),
                                                       .generation = 7});
        return result;
    }

    /** @brief Borrows exact source and collider ownership from one immutable authored object. */
    inline Runtime::NavigationSceneComponentView View(const SceneSource::SceneObjectSnapshot &object) {
        const auto &c = object.components;
        return {.surface = c.navigationSurface ? &*c.navigationSurface : nullptr,
                .region = c.navigationRegion ? &*c.navigationRegion : nullptr,
                .modifier = c.navigationModifier ? &*c.navigationModifier : nullptr,
                .link = c.navigationLink ? &*c.navigationLink : nullptr,
                .agent = c.navigationAgent ? &*c.navigationAgent : nullptr,
                .sources = c.navigationSources,
                .colliders = c.colliders};
    }
}  // namespace Horo::Tests::NavigationSources
