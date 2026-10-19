#pragma once

/** @file NavigationBakeScopeId.h
 * @brief Exact Scene coordinate-space identity for canonical bake partitions, independent of AssetPipeline types.
 */

#include "Horo/Navigation/NavigationIdentity.h"

#include <algorithm>
#include <array>
#include <tuple>

namespace Horo::Navigation {
    /** @brief Stable Scene asset UUID or an explicit definition-local non-Scene coordinate space.
     * @details Default construction is unscoped for standalone non-Scene producers and legacy content.
     * It never selects or infers a Scene. Scene producers preserve all 128 authored asset identity bits.
     */
    class NavigationBakeScopeId final {
    public:
        /** @brief Creates an explicit definition-local, non-Scene scope. */
        constexpr NavigationBakeScopeId() noexcept = default;

        /** @brief Creates a Scene scope without hashing or narrowing its persistent UUID.
         * @param bytes Exact canonical owning Scene asset UUID bytes, not an object/runtime identity.
         * @return Owned scope or IdentityInvalid for the reserved all-zero Scene identity.
         */
        [[nodiscard]] static Result<NavigationBakeScopeId> ForScene(std::array<std::uint8_t, 16> bytes) {
            NavigationBakeScopeId scope;
            scope.scene_ = bytes;
            if (!scope.IsSceneScoped())
                return Result<NavigationBakeScopeId>::Failure(MakeError(NavigationErrors::IdentityInvalid));
            return Result<NavigationBakeScopeId>::Success(scope);
        }

        /** @brief Distinguishes an exact Scene from the definition-local non-Scene domain. @return Whether Scene bytes are present. */
        [[nodiscard]] constexpr bool IsSceneScoped() const noexcept {
            return std::ranges::any_of(scene_, [](const auto byte) {
                return byte != 0;
            });
        }

        /** @brief Returns canonical Scene UUID bytes; all zero only for an explicit unscoped domain.
         * @return Immutable identity bytes; no native padding or paths participate.
         */
        [[nodiscard]] constexpr const std::array<std::uint8_t, 16> &SceneBytes() const noexcept {
            return scene_;
        }

        [[nodiscard]] constexpr auto operator<=>(const NavigationBakeScopeId &) const noexcept = default;

    private:
        std::array<std::uint8_t, 16> scene_{};
    };

    /** @brief Scope-qualified authored surface selection, independent of profile and generated tile coordinates. */
    struct NavigationBakeSurfaceKey final {
        SurfaceId surface;
        NavigationBakeScopeId scope;

        [[nodiscard]] constexpr auto operator<=>(const NavigationBakeSurfaceKey &other) const noexcept {
            return std::tie(scope, surface) <=> std::tie(other.scope, other.surface);
        }

        [[nodiscard]] constexpr bool operator==(const NavigationBakeSurfaceKey &) const noexcept = default;
    };
}  // namespace Horo::Navigation
