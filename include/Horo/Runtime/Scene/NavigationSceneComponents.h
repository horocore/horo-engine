#pragma once

/**
 * @file NavigationSceneComponents.h
 * @brief Typed authored navigation surface, region, modifier-volume, link, and agent Scene components.
 */

#include "Horo/Assets/AssetId.h"
#include "Horo/Foundation/Result.h"
#include "Horo/Math/SceneMath.h"
#include "Horo/Navigation/NavigationAgentProfiles.h"
#include "Horo/Navigation/NavigationAreas.h"
#include "Horo/Navigation/NavigationIdentity.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace Horo::Runtime {
    /** @brief Maximum grounded profiles selected by one authored navigation surface. */
    inline constexpr std::size_t MaximumNavigationSurfaceProfiles = 64;

    /** @brief Closed authored scope choices for one navigation surface. */
    enum class NavigationBakeScope : std::uint8_t {
        ObjectSubtree,
        LocalBounds,
        Count,
    };

    /** @brief Closed source-selection policies for one bounded navigation region. */
    enum class NavigationRegionSourceSelection : std::uint8_t {
        ExplicitContributors,
        StaticCollisionInBounds,
        Count,
    };

    /** @brief Semantic contribution of one bounded region to its referenced surface. */
    enum class NavigationRegionMode : std::uint8_t {
        Include,
        Exclude,
        Count,
    };

    /** @brief Finite positive object-local axis-aligned bounds. */
    struct NavigationLocalBounds final {
        Math::Vec3 center{};
        Math::Vec3 halfExtents{1.0F, 1.0F, 1.0F};

        [[nodiscard]] constexpr bool operator==(const NavigationLocalBounds &) const noexcept = default;
    };

    /**
     * @brief Stable authored navigation surface intent owned by one committed Scene object.
     * @details The definition and profile references are durable authoring identities. No runtime or provider handle is serialized.
     */
    struct NavigationSurfaceComponent final {
        Navigation::SurfaceId id;
        Assets::AssetId definition;
        std::uint32_t schemaVersion{1};
        std::uint64_t generation{1};
        NavigationBakeScope bakeScope{NavigationBakeScope::ObjectSubtree};
        std::optional<NavigationLocalBounds> localBounds;
        std::vector<Navigation::NavigationAgentProfileId> profiles;
        bool enabled{true};

        [[nodiscard]] bool operator==(const NavigationSurfaceComponent &) const noexcept = default;
    };

    /** @brief Stable authored bounded region that includes or excludes sources for one exact surface. */
    struct NavigationRegionComponent final {
        Navigation::NavigationRegionId id;
        Navigation::SurfaceId surface;
        std::uint32_t schemaVersion{1};
        std::uint64_t generation{1};
        NavigationLocalBounds localBounds;
        NavigationRegionSourceSelection sourceSelection{NavigationRegionSourceSelection::ExplicitContributors};
        NavigationRegionMode mode{NavigationRegionMode::Include};
        bool enabled{true};

        [[nodiscard]] constexpr bool operator==(const NavigationRegionComponent &) const noexcept = default;
    };

    /** @brief Closed semantic operations contributed by one authored modifier volume. */
    enum class NavigationModifierOperation : std::uint8_t {
        Exclude,
        OverrideArea,
        OverrideAreaAndCost,
        Count,
    };

    /** @brief Finite positive object-local Y-axis cylinder used by a navigation modifier. */
    struct NavigationCylinderVolume final {
        Math::Vec3 center{};
        float radius{1.0F};
        float halfHeight{1.0F};

        [[nodiscard]] constexpr bool operator==(const NavigationCylinderVolume &) const noexcept = default;
    };

    /** @brief Closed object-local shape payload for an authored navigation modifier. */
    using NavigationModifierVolume = std::variant<NavigationLocalBounds, NavigationCylinderVolume>;

    /**
     * @brief Stable authored area, exclusion, or cost-volume intent for one exact surface.
     * @details Exclusion carries no area or cost. Area override carries an area only. Area-and-cost override carries both.
     */
    struct NavigationModifierComponent final {
        Navigation::NavigationModifierId id;
        Navigation::SurfaceId surface;
        std::uint32_t schemaVersion{1};
        std::uint64_t generation{1};
        NavigationModifierVolume volume{NavigationLocalBounds{}};
        NavigationModifierOperation operation{NavigationModifierOperation::Exclude};
        std::optional<Navigation::NavigationAreaId> area;
        std::optional<float> traversalCost;
        bool enabled{true};

        [[nodiscard]] constexpr bool operator==(const NavigationModifierComponent &) const noexcept = default;
    };

    /** @brief Closed traversal semantics for one explicit grounded transition. */
    enum class NavigationLinkKind : std::uint8_t {
        Jump,
        Ladder,
        Door,
        Teleport,
        Count,
    };

    /** @brief Direction of a link relative to its explicitly named start and end endpoints. */
    enum class NavigationLinkDirection : std::uint8_t {
        StartToEnd,
        Bidirectional,
        Count,
    };

    /** @brief One finite object-local endpoint attached to an exact authored navigation surface. */
    struct NavigationLinkEndpoint final {
        Navigation::SurfaceId surface;
        Math::Vec3 localPosition{};
        float connectionRadiusMeters{0.5F};

        [[nodiscard]] constexpr bool operator==(const NavigationLinkEndpoint &) const noexcept = default;
    };

    /**
     * @brief Stable authored grounded transition owned by the containing committed Scene object.
     * @details Endpoint order is semantic: StartToEnd permits traversal only from start to end; Bidirectional permits both ways.
     */
    struct NavigationLinkComponent final {
        Navigation::NavigationLinkId id;
        std::uint32_t schemaVersion{1};
        std::uint64_t generation{1};
        NavigationLinkEndpoint start;
        NavigationLinkEndpoint end;
        NavigationLinkKind kind{NavigationLinkKind::Jump};
        NavigationLinkDirection direction{NavigationLinkDirection::StartToEnd};
        std::vector<Navigation::NavigationAgentProfileId> profiles;
        float traversalCost{1.0F};
        bool enabled{true};

        [[nodiscard]] bool operator==(const NavigationLinkComponent &) const noexcept = default;
    };

    /**
     * @brief Provider-neutral authored navigation-agent intent owned by one Scene object.
     * @details The profile, filter, radius override, and movement capability are durable authoring values. Runtime crowd
     * handles and provider state are created only for the exact active entity generation.
     */
    struct NavigationAgentComponent final {
        std::uint32_t schemaVersion{1};
        Navigation::NavigationAgentProfileId profile;
        Navigation::NavigationFilterId filter;
        std::optional<float> radiusOverride;
        Navigation::NavigationAgentMovementCapability movementCapability{Navigation::NavigationAgentMovementCapability::Grounded};
        bool enabled{true};

        [[nodiscard]] constexpr bool operator==(const NavigationAgentComponent &) const noexcept = default;
    };

    /** @brief Borrowed Scene-object projection used to validate navigation components without copying payload storage. */
    struct NavigationSceneComponentView final {
        const NavigationSurfaceComponent *surface{};   /**< Optional surface owned by the immutable source snapshot. */
        const NavigationRegionComponent *region{};     /**< Optional region owned by the immutable source snapshot. */
        const NavigationModifierComponent *modifier{}; /**< Optional modifier owned by the immutable source snapshot. */
        const NavigationLinkComponent *link{};         /**< Optional grounded link owned by the immutable source snapshot. */
        const NavigationAgentComponent *agent{};       /**< Optional agent intent owned by the immutable source snapshot. */
    };

    /** @brief Validates one surface payload independently of Scene-wide identity references.
     * @param component Authored component value.
     * @return Success or NavigationErrors::SceneComponentInvalid.
     */
    [[nodiscard]] Result<void> ValidateNavigationSurfaceComponent(const NavigationSurfaceComponent &component);

    /** @brief Validates one region payload independently of its referenced surface's presence.
     * @param component Authored component value.
     * @return Success or NavigationErrors::SceneComponentInvalid.
     */
    [[nodiscard]] Result<void> ValidateNavigationRegionComponent(const NavigationRegionComponent &component);

    /** @brief Validates one modifier payload independently of its referenced surface's presence.
     * @param component Authored component value.
     * @return Success or NavigationErrors::SceneComponentInvalid.
     */
    [[nodiscard]] Result<void> ValidateNavigationModifierComponent(const NavigationModifierComponent &component);

    /** @brief Validates one grounded-link payload independently of endpoint surface/profile presence.
     * @param component Authored component value.
     * @return Success or NavigationErrors::SceneComponentInvalid.
     */
    [[nodiscard]] Result<void> ValidateNavigationLinkComponent(const NavigationLinkComponent &component);

    /**
     * @brief Validates one navigation-agent payload independently of runtime entity ownership.
     * @param component Authored component value.
     * @return Success or NavigationErrors::AgentDescriptorInvalid.
     */
    [[nodiscard]] Result<void> ValidateNavigationAgentComponent(const NavigationAgentComponent &component);

    /**
     * @brief Validates unique identities and exact region-to-surface references in one committed Scene snapshot.
     * @param surfaces Surface components in arbitrary Scene-object order.
     * @param regions Region components in arbitrary Scene-object order.
     * @param modifiers Modifier components in arbitrary Scene-object order.
     * @param links Grounded-link components in arbitrary Scene-object order.
     * @param agents Agent components in arbitrary Scene-object order.
     * @return Success, or a typed invalid, conflict, missing-surface, or profile-mismatch diagnostic.
     */
    [[nodiscard]] Result<void> ValidateNavigationSceneComponents(std::span<const NavigationSurfaceComponent> surfaces,
                                                                 std::span<const NavigationRegionComponent> regions,
                                                                 std::span<const NavigationModifierComponent> modifiers = {},
                                                                 std::span<const NavigationLinkComponent> links = {},
                                                                 std::span<const NavigationAgentComponent> agents = {});

    /**
     * @brief Validates unique identities and references through borrowed component projections without payload copies.
     * @param components Views whose pointers remain valid for the duration of this call.
     * @return Success, or a typed invalid, conflict, or missing-surface diagnostic.
     */
    [[nodiscard]] Result<void> ValidateNavigationSceneComponentViews(std::span<const NavigationSceneComponentView> components);
}  // namespace Horo::Runtime
