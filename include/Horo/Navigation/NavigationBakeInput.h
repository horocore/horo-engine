#pragma once

/**
 * @file NavigationBakeInput.h
 * @brief Deterministic, bounded and publication-fenced grounded tile-build input.
 */

#include "Horo/Foundation/Sha256.h"
#include "Horo/Math/SceneMath.h"
#include "Horo/Navigation/NavigationAgentProfiles.h"
#include "Horo/Navigation/NavigationAreas.h"
#include "Horo/Navigation/NavigationBakeScopeId.h"
#include "Horo/Navigation/NavigationProjectProfiles.h"
#include "Horo/Navigation/NavigationSourceGeometry.h"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace Horo::Navigation {
    struct NavigationBakeRequestGenerationTag;
    struct NavigationDefinitionRevisionTag;
    struct NavigationSceneDocumentRevisionTag;
    struct NavigationAreaRegistryRevisionTag;
    struct NavigationCoordinatePolicyRevisionTag;
    struct NavigationModifierIdentityTag;

    /** @brief Monotonic generation of one admitted bake request or its replacement. */
    using NavigationBakeRequestGeneration = NavigationIdentity<NavigationBakeRequestGenerationTag>;
    /** @brief Exact navigation-definition revision captured by a bake request. */
    using NavigationDefinitionRevision = NavigationIdentity<NavigationDefinitionRevisionTag>;
    /** @brief Exact immutable Scene document revision captured by a bake request. */
    using NavigationSceneDocumentRevision = NavigationIdentity<NavigationSceneDocumentRevisionTag>;
    /** @brief Exact area/filter registry revision captured by a bake request. */
    using NavigationAreaRegistryRevision = NavigationIdentity<NavigationAreaRegistryRevisionTag>;
    /** @brief Exact coordinate/origin policy revision captured by a bake request. */
    using NavigationCoordinatePolicyRevision = NavigationIdentity<NavigationCoordinatePolicyRevisionTag>;
    /** @brief Stable authored identity of one navigation modifier volume. */
    using NavigationModifierId = NavigationIdentity<NavigationModifierIdentityTag>;

    /** @brief Exact revision set that must remain current through final artifact adoption. */
    struct NavigationBakeInputRevisions final {
        NavigationBakeRequestGeneration requestGeneration;
        NavigationDefinitionRevision definition;
        NavigationSceneDocumentRevision scene;
        NavigationAreaRegistryRevision areaRegistry;
        NavigationProjectProfileRevision projectProfile;
        NavigationCoordinatePolicyRevision coordinates;
        NavigationSourceSnapshotRevision geometry;

        [[nodiscard]] constexpr auto operator<=>(const NavigationBakeInputRevisions &) const noexcept = default;
    };

    /** @brief Closed modifier semantics consumed by a grounded tile builder. */
    enum class NavigationBakeModifierMode : std::uint8_t {
        AssignArea,
        Exclude,
        Count,
    };

    /** @brief Closed operation state accepted by the final publication fence. */
    enum class NavigationBakePublicationState : std::uint8_t {
        Ready,
        Cancelled,
        Failed,
        Superseded,
        ShuttingDown,
        Count,
    };

    /** @brief One stable surface-to-source binding in arbitrary authoring order. */
    struct NavigationBakeSurfaceInput final {
        SurfaceId surface;
        NavigationAgentProfileId profile;
        NavigationFilterId filter;
        NavigationSourceProducerId producer;
        NavigationSourceContributionId contribution;
        NavigationBakeScopeId scope;
    };

    /** @brief One local modifier volume normalized to canonical space during capture. */
    struct NavigationBakeModifierInput final {
        NavigationModifierId id;
        SurfaceId surface;
        NavigationAgentProfileId profile;
        NavigationBakeModifierMode mode{NavigationBakeModifierMode::AssignArea};
        NavigationAreaId area;
        Math::Aabb localBounds;
        Math::Transform localToCanonicalMeters;
        NavigationBakeScopeId scope;
    };

    /** @brief Hard-bounded tile-input capture policy; callers may lower but not exceed qualified maxima. */
    struct NavigationBakeInputLimits final {
        static constexpr std::uint32_t MaximumProfiles = 64;
        static constexpr std::uint32_t MaximumAreas = 256;
        static constexpr std::uint32_t MaximumSurfaceBindings = NavigationSourceGeometryLimits::MaximumContributions;
        static constexpr std::uint32_t MaximumModifiers = 16'384;
        static constexpr std::uint64_t MaximumTileTriangles = NavigationSourceGeometryLimits::MaximumTriangles * 4ULL;
        static constexpr std::uint64_t MaximumWorkUnits = MaximumTileTriangles * 3ULL;
        static constexpr std::uint64_t MaximumOwnedBytes = 512ULL * 1024ULL * 1024ULL;

        std::uint32_t maxProfiles{MaximumProfiles};
        std::uint32_t maxAreas{MaximumAreas};
        std::uint32_t maxSurfaceBindings{MaximumSurfaceBindings};
        std::uint32_t maxModifiers{MaximumModifiers};
        std::uint64_t maxTileTriangles{MaximumTileTriangles};
        std::uint64_t maxWorkUnits{MaximumWorkUnits};
        std::uint64_t maxOwnedBytes{MaximumOwnedBytes};

        [[nodiscard]] constexpr auto operator<=>(const NavigationBakeInputLimits &) const noexcept = default;
    };

    /** @brief Bake-only profile values resolved from a validated stable profile identity. */
    struct NavigationResolvedBakeProfile final {
        NavigationAgentProfileId id;
        NavigationAgentBuildGeometry buildGeometry;
    };

    /** @brief Registry-owned area semantics copied into the immutable bake input. */
    struct NavigationResolvedBakeArea final {
        NavigationAreaId id;
        NavigationDescriptorSource source;
        float traversalCost{};
        NavigationAreaFlags flags;
    };

    /** @brief One canonical triangle selected for a surface/profile partition. */
    struct NavigationTileBuildTriangle final {
        std::array<Math::Vec3, 3> vertices{};
        NavigationAreaId area;
        float traversalCost{};
        NavigationSourceMaterialSlot materialSlot;
        NavigationTriangleProvenance provenance;
    };

    /** @brief One canonical modifier volume associated with an exact surface/profile partition. */
    struct NavigationTileBuildModifier final {
        NavigationModifierId id;
        SurfaceId surface;
        NavigationAgentProfileId profile;
        NavigationBakeModifierMode mode{NavigationBakeModifierMode::AssignArea};
        NavigationAreaId area;
        Math::Aabb canonicalBounds;
        NavigationBakeScopeId scope;
    };

    /** @brief Contiguous canonical tile-builder ranges for one stable surface/profile pair. */
    struct NavigationTileBuildPartition final {
        SurfaceId surface;
        NavigationAgentProfileId profile;
        NavigationFilterId filter;
        std::uint32_t firstTriangle{};
        std::uint32_t triangleCount{};
        std::uint32_t firstModifier{};
        std::uint32_t modifierCount{};
        NavigationBakeScopeId scope;
    };

    /**
     * @brief Immutable canonical tile-build input with exact source and publication provenance.
     *
     * Surface bindings are ordered by profile, surface and producer identity. Profiles, areas and modifiers are
     * identity-sorted. Geometry is already canonical right-handed Y-up metres; excluded filter areas do not enter
     * tile triangles. The retained source snapshot supports a final exact revision/digest publication check.
     */
    class NavigationBakeInputSnapshot final {
    public:
        NavigationBakeInputSnapshot(const NavigationBakeInputSnapshot &) = delete;
        NavigationBakeInputSnapshot &operator=(const NavigationBakeInputSnapshot &) = delete;
        NavigationBakeInputSnapshot(NavigationBakeInputSnapshot &&) noexcept = default;
        NavigationBakeInputSnapshot &operator=(NavigationBakeInputSnapshot &&) = delete;

        /**
         * @brief Resolves, filters, normalizes and owns one complete tile-build input transactionally.
         * @param revisions Exact non-zero capture and request revisions.
         * @param areas Immutable area/filter registry used for exact resolution.
         * @param profiles Complete available grounded profile descriptors in arbitrary order.
         * @param surfaces Complete surface/profile/source bindings in arbitrary order; empty requires no geometry triangles
         * and no modifiers, explicitly representing empty definition coverage. Empty contribution records remain owned
         * and publication still requires their complete freshness observations.
         * Bound contributions with no traversable triangles retain valid empty partitions and exact filter resolution.
         * @param modifiers Modifier volumes in arbitrary order.
         * @param geometry Unique immutable source snapshot; moved only after all validation succeeds.
         * @param limits Positive qualified count, work and byte bounds.
         * @return Canonical owned input or a typed invalid, missing-reference, conflict, capacity or source error.
         */
        [[nodiscard]] static Result<NavigationBakeInputSnapshot> Create(const NavigationBakeInputRevisions &revisions,
                                                                        const NavigationAreaRegistry &areas,
                                                                        std::span<const NavigationAgentProfileDescriptor> profiles,
                                                                        std::span<const NavigationBakeSurfaceInput> surfaces,
                                                                        std::span<const NavigationBakeModifierInput> modifiers,
                                                                        NavigationSourceGeometrySnapshot &&geometry,
                                                                        const NavigationBakeInputLimits &limits = {});

        /** @brief Returns the exact capture/publication revision set. @return Immutable retained revisions. */
        [[nodiscard]] const NavigationBakeInputRevisions &Revisions() const noexcept;
        /** @brief Returns the qualified capture bounds. @return Immutable retained limits. */
        [[nodiscard]] const NavigationBakeInputLimits &Limits() const noexcept;
        /** @brief Returns a deterministic digest over canonical tile-build semantics and source provenance, excluding operation generation.
         * @return Canonical digest. */
        [[nodiscard]] const Sha256Digest &Fingerprint() const noexcept;
        /** @brief Returns canonical profiles ordered by identity. @return Immutable owned profile view. */
        [[nodiscard]] std::span<const NavigationResolvedBakeProfile> Profiles() const noexcept;
        /** @brief Returns referenced area semantics ordered by identity. @return Immutable owned area view. */
        [[nodiscard]] std::span<const NavigationResolvedBakeArea> Areas() const noexcept;
        /** @brief Returns stable surface/profile partitions in canonical order. @return Immutable owned partition view. */
        [[nodiscard]] std::span<const NavigationTileBuildPartition> Partitions() const noexcept;
        /** @brief Returns partition-contiguous canonical triangles. @return Immutable owned triangle view. */
        [[nodiscard]] std::span<const NavigationTileBuildTriangle> Triangles() const noexcept;
        /** @brief Returns partition-contiguous canonical modifier volumes. @return Immutable owned modifier view. */
        [[nodiscard]] std::span<const NavigationTileBuildModifier> Modifiers() const noexcept;

        /**
         * @brief Enforces the final adoption barrier for revision, lifecycle and complete source freshness.
         * @param expectedRequestGeneration Generation retained by the publishing operation.
         * @param currentRevisions Complete current authoritative revision set.
         * @param currentSources Complete current source revision/digest observations.
         * @param state Current operation state; only Ready may publish.
         * @return Success only when every exact fence remains current and the operation is publishable.
         */
        [[nodiscard]] Result<void> ValidatePublication(NavigationBakeRequestGeneration expectedRequestGeneration,
                                                       const NavigationBakeInputRevisions &currentRevisions,
                                                       std::span<const NavigationSourceObservation> currentSources,
                                                       NavigationBakePublicationState state = NavigationBakePublicationState::Ready) const;

    private:
        struct ConstructionState;

        explicit NavigationBakeInputSnapshot(ConstructionState &&state) noexcept;

        NavigationBakeInputRevisions revisions_;
        NavigationBakeInputLimits limits_;
        Sha256Digest fingerprint_{};
        std::vector<NavigationResolvedBakeProfile> profiles_;
        std::vector<NavigationResolvedBakeArea> areas_;
        std::vector<NavigationTileBuildPartition> partitions_;
        std::vector<NavigationTileBuildTriangle> triangles_;
        std::vector<NavigationTileBuildModifier> modifiers_;
        NavigationSourceGeometrySnapshot geometry_;
    };
}  // namespace Horo::Navigation
