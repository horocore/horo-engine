#pragma once

/**
 * @file NavigationSourceGeometry.h
 * @brief Immutable bounded navigation bake-source geometry and provenance snapshot.
 */

#include "Horo/Foundation/Sha256.h"
#include "Horo/Math/SceneMath.h"
#include "Horo/Navigation/NavigationAreas.h"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace Horo::Navigation {
    struct NavigationSourceProducerIdentityTag;
    struct NavigationSourceContributionIdentityTag;
    struct NavigationSourceRevisionIdentityTag;
    struct NavigationSourceSnapshotRevisionIdentityTag;

    /** @brief Stable authored geometry-producer identity, independent of entity slots and source paths. */
    using NavigationSourceProducerId = NavigationIdentity<NavigationSourceProducerIdentityTag>;
    /** @brief Stable identity for one producer contribution, independent of input and worker ordering. */
    using NavigationSourceContributionId = NavigationIdentity<NavigationSourceContributionIdentityTag>;
    /** @brief Monotonic revision of one exact authored geometry contribution. */
    using NavigationSourceRevision = NavigationIdentity<NavigationSourceRevisionIdentityTag>;
    /** @brief Monotonic revision for one complete immutable capture. */
    using NavigationSourceSnapshotRevision = NavigationIdentity<NavigationSourceSnapshotRevisionIdentityTag>;

    /** @brief Closed canonical producer set accepted by the grounded navigation bake boundary. */
    enum class NavigationSourceProducerKind : std::uint8_t {
        StaticCollider,
        Terrain,
        ProceduralGeneration,
        ApprovedCustom,
        Count,
    };

    /** @brief Closed source-coordinate conventions normalized at the navigation geometry boundary. */
    enum class NavigationSourceCoordinateSystem : std::uint8_t {
        RightHandedYUp,
        RightHandedZUp,
        LeftHandedYUp,
        Count,
    };

    /** @brief Explicit source units and axes converted to canonical right-handed Y-up metres before local transforms. */
    struct NavigationSourceCoordinateConvention final {
        NavigationSourceCoordinateSystem system{NavigationSourceCoordinateSystem::RightHandedYUp};
        float metersPerUnit{1.0F};

        [[nodiscard]] constexpr auto operator<=>(const NavigationSourceCoordinateConvention &) const noexcept = default;
    };

    /** @brief Producer-authored material slot retained for diagnostics without importing Physics or Renderer types. */
    struct NavigationSourceMaterialSlot final {
        std::uint32_t value{};

        [[nodiscard]] constexpr auto operator<=>(const NavigationSourceMaterialSlot &) const noexcept = default;
    };

    /** @brief Hard-bounded capture policy; projects may lower but not exceed these qualified maxima. */
    struct NavigationSourceGeometryLimits final {
        static constexpr std::uint32_t MaximumContributions = 4096;
        static constexpr std::uint64_t MaximumVertices = 1'000'000;
        static constexpr std::uint64_t MaximumTriangles = 2'000'000;
        static constexpr std::uint64_t MaximumOwnedBytes = 256ULL * 1024ULL * 1024ULL;

        std::uint32_t maxContributions{MaximumContributions};
        std::uint64_t maxVertices{MaximumVertices};
        std::uint64_t maxTriangles{MaximumTriangles};
        std::uint64_t maxOwnedBytes{MaximumOwnedBytes};

        [[nodiscard]] constexpr auto operator<=>(const NavigationSourceGeometryLimits &) const noexcept = default;
    };

    /** @brief One local-space indexed triangle carrying authored area and material-slot semantics. */
    struct NavigationSourceTriangleInput final {
        std::array<std::uint32_t, 3> vertexIndices{};
        NavigationAreaId area;
        NavigationSourceMaterialSlot materialSlot;
    };

    /** @brief Borrowed input from one immutable producer capture; Create owns and canonicalizes every value. */
    struct NavigationSourceContributionInput final {
        NavigationSourceProducerKind kind{NavigationSourceProducerKind::StaticCollider};
        NavigationSourceProducerId producer;
        NavigationSourceContributionId contribution;
        NavigationSourceRevision revision;
        Sha256Digest contentDigest{};
        NavigationSourceCoordinateConvention coordinates{};
        Math::Transform localToCanonicalMeters{};
        std::span<const Math::Vec3> vertices;
        std::span<const NavigationSourceTriangleInput> triangles;
    };

    /** @brief Stable diagnostic provenance attached to every canonical triangle. */
    struct NavigationTriangleProvenance final {
        NavigationSourceProducerKind kind{NavigationSourceProducerKind::StaticCollider};
        NavigationSourceProducerId producer;
        NavigationSourceContributionId contribution;
        NavigationSourceRevision revision;
        Sha256Digest contentDigest{};
        std::uint32_t sourceTriangleIndex{};

        [[nodiscard]] auto operator<=>(const NavigationTriangleProvenance &) const noexcept = default;
    };

    /** @brief Canonical-meter, right-handed Y-up triangle with complete authored provenance. */
    struct NavigationSourceTriangle final {
        std::array<std::uint32_t, 3> vertexIndices{};
        NavigationAreaId area;
        NavigationSourceMaterialSlot materialSlot;
        NavigationTriangleProvenance provenance;

        [[nodiscard]] auto operator<=>(const NavigationSourceTriangle &) const noexcept = default;
    };

    /** @brief Canonically ordered owned contribution range and exact source-freshness evidence. */
    struct NavigationSourceContribution final {
        NavigationSourceProducerKind kind{NavigationSourceProducerKind::StaticCollider};
        NavigationSourceProducerId producer;
        NavigationSourceContributionId contribution;
        NavigationSourceRevision revision;
        Sha256Digest contentDigest{};
        NavigationSourceCoordinateConvention coordinates{};
        Math::Transform localToCanonicalMeters{};
        std::uint32_t firstVertex{};
        std::uint32_t vertexCount{};
        std::uint32_t firstTriangle{};
        std::uint32_t triangleCount{};
    };

    /** @brief Current authoritative revision evidence used to reject stale bake input. */
    struct NavigationSourceObservation final {
        NavigationSourceProducerKind kind{NavigationSourceProducerKind::StaticCollider};
        NavigationSourceProducerId producer;
        NavigationSourceContributionId contribution;
        NavigationSourceRevision revision;
        Sha256Digest contentDigest{};

        [[nodiscard]] auto operator<=>(const NavigationSourceObservation &) const noexcept = default;
    };

    /**
     * @brief Immutable, owned and bounded source geometry valid for one complete bake attempt.
     *
     * The accepted producer enum intentionally has no renderer source. Render-derived geometry requires a future
     * explicit, versioned conversion policy and cannot be scraped through this contract when canonical geometry exists.
     */
    class NavigationSourceGeometrySnapshot final {
    public:
        /** @brief Snapshot storage has unique ownership and cannot be copied. */
        NavigationSourceGeometrySnapshot(const NavigationSourceGeometrySnapshot &) = delete;
        /** @brief Snapshot storage cannot be copy-assigned. */
        NavigationSourceGeometrySnapshot &operator=(const NavigationSourceGeometrySnapshot &) = delete;
        /** @brief Transfers all owned immutable capture storage. */
        NavigationSourceGeometrySnapshot(NavigationSourceGeometrySnapshot &&) noexcept = default;
        /** @brief Replacing a snapshot in place is forbidden while views may exist. */
        NavigationSourceGeometrySnapshot &operator=(NavigationSourceGeometrySnapshot &&) = delete;

        /**
         * @brief Validates and owns a complete canonical source snapshot transactionally.
         * @param revision Non-zero revision of the complete capture.
         * @param inputs Complete borrowed producer captures in arbitrary order; empty explicitly captures source removal.
         * A contribution may retain both empty vertex and triangle spans after clipping, preserving its identity/freshness.
         * One empty span with a nonempty counterpart remains invalid.
         * @param limits Positive qualified count and byte bounds.
         * @return Owned canonical snapshot or a typed invalid, unsupported, conflict, or capacity error.
         */
        [[nodiscard]] static Result<NavigationSourceGeometrySnapshot> Create(NavigationSourceSnapshotRevision revision,
                                                                             std::span<const NavigationSourceContributionInput> inputs,
                                                                             const NavigationSourceGeometryLimits &limits = {});

        /** @brief Returns the exact complete-capture revision. @return Non-zero capture revision. */
        [[nodiscard]] NavigationSourceSnapshotRevision Revision() const noexcept;
        /** @brief Returns the validated capture bounds. @return Qualified limits used during capture. */
        [[nodiscard]] NavigationSourceGeometryLimits Limits() const noexcept;
        /** @brief Returns contribution metadata in stable producer/contribution identity order. @return Immutable owned view. */
        [[nodiscard]] std::span<const NavigationSourceContribution> Contributions() const noexcept;
        /** @brief Returns owned vertices transformed to canonical metres in right-handed Y-up scene space. @return Immutable owned view. */
        [[nodiscard]] std::span<const Math::Vec3> Vertices() const noexcept;
        /** @brief Returns owned triangles whose indices address Vertices and each carry exact provenance. @return Immutable owned view. */
        [[nodiscard]] std::span<const NavigationSourceTriangle> Triangles() const noexcept;

        /**
         * @brief Revalidates the complete captured source set before later bake adoption.
         * @param expectedSnapshotRevision Revision retained by the bake attempt.
         * @param currentSources Complete current authoritative source observations in arbitrary order.
         * @return Success only for an exact revision/digest set; otherwise a typed invalid or stale error.
         */
        [[nodiscard]] Result<void> ValidateCurrent(NavigationSourceSnapshotRevision expectedSnapshotRevision,
                                                   std::span<const NavigationSourceObservation> currentSources) const;

    private:
        NavigationSourceGeometrySnapshot(NavigationSourceSnapshotRevision revision, const NavigationSourceGeometryLimits &limits,
                                         std::vector<NavigationSourceContribution> contributions, std::vector<Math::Vec3> vertices,
                                         std::vector<NavigationSourceTriangle> triangles) noexcept;

        NavigationSourceSnapshotRevision revision_;
        NavigationSourceGeometryLimits limits_;
        std::vector<NavigationSourceContribution> contributions_;
        std::vector<Math::Vec3> vertices_;
        std::vector<NavigationSourceTriangle> triangles_;
    };
}  // namespace Horo::Navigation
