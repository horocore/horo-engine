#pragma once

#include "Horo/Application/NavigationSceneBakeGeometry.h"

namespace Horo::Application::NavigationBakeDetail {
    /** @brief Exact owned region intent and invertible affine frame, independent of a native provider. */
    struct SourceRegion final {
        Runtime::NavigationRegionComponent authored;
        Runtime::SceneObjectId owner;
        Math::Mat4 world;
        Math::Mat4 inverse;
    };

    /** @brief Stable-ID ordered applicable region closure and versioned descriptor digest. */
    struct SourceRegions final {
        std::vector<SourceRegion> values;
        Sha256Digest digest;
        bool hasIncludes{};
        bool hasExcludes{};
    };

    /** @brief Selection bounds and uniform source semantics copied into the synchronous boolean kernel. */
    struct RegionSelectionPolicy final {
        NavigationPrimitiveGeometryPolicy geometry;
        std::uint32_t maximumFragments{NavigationSceneGeometryPolicy::MaximumRegionFragments};
        Navigation::NavigationAreaId area;
        Navigation::NavigationSourceMaterialSlot material;
    };

    /** @brief Selected canonical geometry with a digest retaining both original geometry and exact region intent. */
    struct RegionSelection final {
        NavigationPrimitiveGeometry geometry;
        Sha256Digest contentDigest;
    };

    /** @brief Hashes complete stable region descriptors after canonical ordering. */
    [[nodiscard]] Sha256Digest SourceRegionsDigest(std::span<const SourceRegion> regions);
    /** @brief Applies include union then exclusions; buffers/work are bounded and no partial output escapes. */
    [[nodiscard]] Result<RegionSelection> SelectRegionGeometry(NavigationPrimitiveGeometry input, const SourceRegions &regions,
                                                               const RegionSelectionPolicy &policy, std::uint64_t &remainingWork,
                                                               CancellationToken cancellation);
}  // namespace Horo::Application::NavigationBakeDetail
