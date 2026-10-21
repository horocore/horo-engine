#pragma once
/** @file NavigationViewportSnapshot.h
 * @brief Owned, bounded navigation viewport evidence, independent of provider and drawing lifetimes.
 */
#include "Horo/Application/NavigationProjectBakeSource.h"
#include "Horo/Navigation/NavigationTileArtifact.h"

#include <functional>
#include <memory>

namespace Horo::Editor {
    /** @brief Distinct geometry semantics; geometry shape also distinguishes categories without color. */
    enum class NavigationOverlayKind : std::uint8_t {
        Surface,
        Walkable,
        Tile,
        Modifier,
        Link
    };
    /** @brief Evidence classification, never an inferred runtime validity guarantee. */
    enum class NavigationOverlayValidity : std::uint8_t {
        Authored,
        Cooked,
        Disabled,
        Unvalidated
    };

    /** @brief Closed retained line/triangle primitive; every point and identity is owned by value. */
    struct NavigationOverlayPrimitive final {
        NavigationOverlayKind kind{};
        NavigationOverlayValidity validity{};
        Navigation::NavigationAgentProfileId profile;
        Navigation::SurfaceId surface;
        Navigation::SurfaceId otherSurface; /**< Link destination; surface filtering admits either endpoint. */
        Navigation::NavigationAreaId area;
        SceneSource::SceneObjectId object; /**< Saved authored owner, absent for generated geometry. */
        Navigation::NavigationModifierId modifier;
        Navigation::NavigationLinkId link;
        std::optional<Navigation::NavMeshTileKey> tile; /**< Exact generated tile, absent for authored geometry. */
        std::uint32_t polygonIndex{};                   /**< Tile-local canonical index for walkable faces. */
        std::array<Math::Vec3, 3> points{};
        bool triangle{};
        bool bidirectional{};
    };

    /** @brief One exact Scene namespace; surface IDs from different Scenes never alias. */
    struct NavigationOverlayScene final {
        std::filesystem::path path;
        Navigation::NavigationBakeScopeId scope;
        Sha256Digest sourceDigest{}; /**< Exact saved Scene byte identity. */
        std::vector<NavigationOverlayPrimitive> primitives;
    };

    /** @brief Hard capture and retained-geometry ceilings; truncation is explicit and deterministic. */
    struct NavigationOverlayBudget final {
        std::size_t maximumPrimitives{16'384};
        std::size_t maximumWork{262'144};
        static constexpr std::size_t HardMaximumPrimitives = 65'536;
        static constexpr std::size_t HardMaximumWork = 1'048'576;
    };

    /** @brief Filters are presentation intent and never alter provider, bake or authored state. */
    struct NavigationOverlayFilter final {
        std::optional<Navigation::NavigationAgentProfileId> profile;
        std::optional<Navigation::NavigationAreaId> area;
        std::optional<Navigation::SurfaceId> surface;
        std::optional<NavigationOverlayValidity> validity;
        std::array<bool, 5> kinds{true, true, true, true, true};
        /** @brief Tests exact typed identities and category visibility. @param primitive Owned geometry. @return True on match. */
        [[nodiscard]] bool Matches(const NavigationOverlayPrimitive &primitive) const noexcept;
    };

    /** @brief Immutable capture with no pointers, spans, provider leases or callbacks in its geometry. */
    class NavigationViewportSnapshot final {
    public:
        /** @brief Copies saved authored bounds, modifier volumes and links into canonical world geometry.
         * @param scenes Exact saved Scene captures. @param definition Explicit selected definition.
         * @param budget Positive hard-bounded work/storage policy.
         * @return Immutable owned evidence or typed invalid/capacity failure. Truncated captures remain inspectable.
         * @throws std::bad_alloc if bounded capture storage cannot be allocated.
         */
        [[nodiscard]] static Result<std::shared_ptr<const NavigationViewportSnapshot>> Capture(
            std::span<const Application::NavigationSceneBakeSource> scenes, Assets::AssetId definition,
            NavigationOverlayBudget budget = {});
        /** @brief Copies neutral cooked polygons, tile edges and generated links beside saved authored evidence.
         * @param authored Existing immutable saved capture, never modified. @param tiles Verified complete cooked publication.
         * @param budget Aggregate storage/work ceilings, including retained authored primitives.
         * @return Independent immutable geometry; all borrowed topology memory is released at return.
         * @throws std::bad_alloc if bounded copied geometry cannot be allocated.
         */
        [[nodiscard]] static Result<std::shared_ptr<const NavigationViewportSnapshot>> WithCooked(
            const NavigationViewportSnapshot &authored, const Navigation::NavigationCookedTileSet &tiles,
            NavigationOverlayBudget budget = {});

        /** @brief Reads owned Scene-qualified geometry. @return Immutable rows valid for this snapshot lifetime. */
        [[nodiscard]] std::span<const NavigationOverlayScene> Scenes() const noexcept {
            return scenes_;
        }

        /** @brief Reports capture work or storage truncation. @return True when evidence was omitted. */
        [[nodiscard]] bool Truncated() const noexcept {
            return truncated_;
        }

        /** @brief Reads exact cooked input evidence, independently of saved Scene digests.
         * @return Absent for authored-only evidence; never a runtime activation/currentness claim.
         */
        [[nodiscard]] const std::optional<Sha256Digest> &CookedFingerprint() const noexcept {
            return cookedFingerprint_;
        }

    private:
        std::optional<Sha256Digest> cookedFingerprint_;
        std::vector<NavigationOverlayScene> scenes_;
        bool truncated_{};
    };

    /** @brief Narrow owner-lane viewport read capability, outliving attachment.
     * @details Preparation schedules bounded source/cook reads on the existing owner. Query copies only
     * shared ownership of independent geometry; neither callback grants bake submission or cancellation.
     */
    struct NavigationViewportAccess final {
        std::function<Result<void>(const std::filesystem::path &)> prepare;
        std::function<std::shared_ptr<const NavigationViewportSnapshot>(const std::filesystem::path &)> query;
    };
}  // namespace Horo::Editor
