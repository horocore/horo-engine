#pragma once

/** @file NavigationDefinition.h
 * @brief Immutable authored definition shared by Editor and headless navigation capture.
 */

#include "Horo/Navigation/NavigationAgentProfiles.h"
#include "Horo/Navigation/NavigationDataSerialization.h"
#include "Horo/Navigation/NavigationSourceGeometry.h"

namespace Horo::Navigation {
    /** @brief Production record allocation NAVDEF01, introduced by project contract 0.2.0. */
    inline constexpr std::uint64_t NavigationDefinitionRecordTypeValue = 0x4e41564445463031ULL;
    /** @brief Exact semantic payload version; independent of the unchanged HNAV 1.1 envelope. */
    inline constexpr NavigationSourceSchemaVersion NavigationDefinitionPayloadVersion{1, 0};

    /** @brief Definition's authoring scope, not an automation request's selected-surface filter. */
    enum class NavigationDefinitionScope : std::uint8_t {
        Scene,
        Project,
        Count,
    };

    /** @brief Authored grid policy; tile extent is tileSizeCells times each profile's cell size. */
    struct NavigationDefinitionTilePolicy final {
        std::uint32_t tileSizeCells{128}; /**< Positive integral voxel count, at most 4096. */
        std::uint32_t maximumTiles{1024}; /**< Complete replacement closure limit, at most 65536. */
        [[nodiscard]] constexpr auto operator<=>(const NavigationDefinitionTilePolicy &) const noexcept = default;
    };

    /** @brief Detached authored candidate; stable asset identity remains in the owning asset sidecar. */
    struct NavigationDefinitionInput final {
        std::vector<NavigationAgentProfileDescriptor> profiles;
        std::vector<NavigationAreaDescriptor> areas;
        std::vector<NavigationQueryFilterDescriptor> filters;
        NavigationSourceCoordinateConvention coordinates;
        NavigationDefinitionTilePolicy tiles;
        NavigationDefinitionScope scope{NavigationDefinitionScope::Scene};
    };

    /**
     * @brief One bounded, validated, identity-ordered durable definition.
     * @details Owns authored values only: no Scene, provider, generated topology, cache or host policy.
     * Borrowed views remain valid until destruction; all admission uses an explicit complete candidate.
     */
    class NavigationDefinition final {
    public:
        static constexpr std::size_t MaximumProfiles = 64;
        static constexpr std::size_t MaximumAreas = 256;
        static constexpr std::size_t MaximumFilters = 256;
        static constexpr std::size_t MaximumProfileNameBytes = 256;

        NavigationDefinition(const NavigationDefinition &) = delete;
        NavigationDefinition &operator=(const NavigationDefinition &) = delete;
        NavigationDefinition(NavigationDefinition &&) noexcept = default;
        NavigationDefinition &operator=(NavigationDefinition &&) = delete;

        /** @brief Validates and owns one complete authoring candidate, without host side effects.
         * @param input Explicit profiles, descriptors, coordinates and policy.
         * @return Immutable definition or a typed invalid, duplicate-identity or capacity diagnostic.
         */
        [[nodiscard]] static Result<NavigationDefinition> Create(NavigationDefinitionInput input);
        /** @brief Returns stable-identity ordered grounded profiles. @return Borrowed immutable profiles. */
        [[nodiscard]] std::span<const NavigationAgentProfileDescriptor> Profiles() const noexcept;
        /** @brief Returns the one validated descriptor registry. @return Borrowed immutable registry. */
        [[nodiscard]] const NavigationAreaRegistry &Registry() const noexcept;
        /** @brief Returns explicit source units and axes. @return Authored coordinate convention. */
        [[nodiscard]] NavigationSourceCoordinateConvention Coordinates() const noexcept;
        /** @brief Returns complete-closure grid limits. @return Authored tile policy. */
        [[nodiscard]] NavigationDefinitionTilePolicy Tiles() const noexcept;
        /** @brief Returns definition source scope. @return Authored scope, never a request filter. */
        [[nodiscard]] NavigationDefinitionScope Scope() const noexcept;

    private:
        NavigationDefinition(std::vector<NavigationAgentProfileDescriptor> profiles, NavigationAreaRegistry registry,
                             NavigationSourceCoordinateConvention coordinates, NavigationDefinitionTilePolicy tiles,
                             NavigationDefinitionScope scope) noexcept;
        std::vector<NavigationAgentProfileDescriptor> profiles_;
        NavigationAreaRegistry registry_;
        NavigationSourceCoordinateConvention coordinates_;
        NavigationDefinitionTilePolicy tiles_;
        NavigationDefinitionScope scope_;
    };

    /** @brief Returns the exact production support entry, with no synthetic/legacy payload reinterpretation.
     * @return NAVDEF01 support for payload 1.0 only.
     */
    [[nodiscard]] NavigationAuthoredRecordSupport NavigationDefinitionRecordSupport();
}  // namespace Horo::Navigation
